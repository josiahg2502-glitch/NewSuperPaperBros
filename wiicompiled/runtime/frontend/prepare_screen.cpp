#include "prepare_screen.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace RuntimeFrontend {
namespace {
struct TextRect { int x, y, width, height; };
#include "prepare_text.inc"
enum Line { kTitle, kReady, kSubtitle, kDigits, kFps, kAudio, kLineCount };
constexpr uint64_t kUniformBytes = 48 + 16 + kLineCount * 16;
constexpr uint64_t kStatsBytes = FrameStats::kHistory * 16;

constexpr double kPi = 3.14159265358979323846;
constexpr double kFlipRest = 0.55, kFlipMove = 0.95;  // seconds per flip: pause, then turn
constexpr double kSettle = 0.42;                      // card snaps face-up after completion
constexpr double kHold = 1.6, kFadeOut = 0.4;         // stay on "Ready", then fade to black

double EaseInOutCubic(double x) { return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2; }
double EaseOutBack(double x) {
    constexpr double c1 = 1.70158, c3 = c1 + 1;
    return 1 + c3 * std::pow(x - 1, 3) + c1 * std::pow(x - 1, 2);
}

// One bell-like note: a fast attack, then partials that decay at different rates.
double Note(double t, double frequency, double gain) {
    if (t < 0) return 0;
    const double attack = 1 - std::exp(-t / 0.0015), decay = std::exp(-t / 0.17);
    const double w = 2 * kPi * frequency * t;
    return gain * attack * (std::sin(w) * decay + 0.28 * std::sin(2 * w) * decay * decay +
                            0.10 * std::sin(3.02 * w) * decay * decay * decay);
}

std::vector<uint8_t> DecodeBase64(const char* text) {
    std::vector<uint8_t> out;
    uint32_t buffer = 0;
    int bits = 0;
    for (const char* c = text; *c && *c != '='; ++c) {
        const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const char* found = std::strchr(table, *c);
        if (!found) continue;
        buffer = buffer << 6 | uint32_t(found - table);
        if ((bits += 6) >= 8) { bits -= 8; out.push_back(uint8_t(buffer >> bits)); }
    }
    return out;
}

constexpr const char* kShader = R"wgsl(
struct Uniforms {
    size: vec2f, time: f32, progress: f32,
    done: f32, fade: f32, angle: f32, lift: f32,
    atlas: vec4f,            // width, height, em, distance units per atlas pixel
    meter: vec4f,            // padding, digit advance (atlas px), samples shown (0 hides), fps
    lines: array<vec4f, 6>,  // title, ready, subtitle, digits, "fps", "audio" in atlas pixels
}
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var atlasTex: texture_2d<f32>;
@group(0) @binding(2) var atlasSampler: sampler;
// Oldest first: interval ms, work ms, audio occupancy (negative unknown), underrun flag.
@group(0) @binding(3) var<uniform> samples: array<vec4f, 128>;

@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let xy = array<vec2f, 3>(vec2f(-1, -1), vec2f(3, -1), vec2f(-1, 3));
    return vec4f(xy[i], 0, 1);
}

fn roundBox(p: vec2f, halfSize: vec2f, r: f32) -> f32 {
    let q = abs(p) - halfSize + vec2f(r);
    return length(max(q, vec2f(0))) + min(max(q.x, q.y), 0.0) - r;
}
fn segment(p: vec2f, a: vec2f, b: vec2f) -> f32 {
    let pa = p - a; let ba = b - a;
    return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0));
}
fn over(base: vec3f, color: vec3f, alpha: f32) -> vec3f { return mix(base, color, clamp(alpha, 0.0, 1.0)); }

// Coverage of one atlas line drawn with its em at `em` (fraction of screen height), centred at `centre`.
fn textAlpha(p: vec2f, line: vec4f, centre: vec2f, em: f32, px: f32) -> f32 {
    let scale = em / u.atlas.z;  // screen units per atlas pixel
    let uvLocal = (p - centre) / scale + line.zw * 0.5;
    if (any(uvLocal < vec2f(0)) || any(uvLocal > line.zw)) { return 0.0; }
    let v = textureSampleLevel(atlasTex, atlasSampler, (line.xy + uvLocal) / u.atlas.xy, 0.0).r;
    let width = u.atlas.w * px / scale;  // one screen pixel in distance units
    return clamp((v - 0.502) / width + 0.5, 0.0, 1.0);
}
// Coverage of atlas rectangle `r` drawn with its top-left at `origin`.
fn rectAlpha(p: vec2f, r: vec4f, origin: vec2f, em: f32, px: f32) -> f32 {
    let scale = em / u.atlas.z;
    let uvLocal = (p - origin) / scale;
    if (any(uvLocal < vec2f(0)) || any(uvLocal > r.zw)) { return 0.0; }
    let v = textureSampleLevel(atlasTex, atlasSampler, (r.xy + uvLocal) / u.atlas.xy, 0.0).r;
    return clamp((v - 0.502) / (u.atlas.w * px / scale) + 0.5, 0.0, 1.0);
}
// A label line, vertically centred on `y`, starting at `x`; returns its right edge in .y.
fn labelAlpha(p: vec2f, line: vec4f, x: f32, y: f32, em: f32, px: f32) -> vec2f {
    let scale = em / u.atlas.z;
    let r = vec4f(line.xy + vec2f(u.meter.x, 0.0), line.z - 2.0 * u.meter.x, line.w);
    return vec2f(rectAlpha(p, r, vec2f(x, y - line.w * scale * 0.5), em, px), x + r.z * scale);
}
// Right-aligned unsigned number ending at `right`.
fn numberAlpha(p: vec2f, value: u32, right: f32, y: f32, em: f32, px: f32) -> f32 {
    let scale = em / u.atlas.z;
    let digits = u.lines[3];
    let advance = u.meter.y * scale;
    let slot = i32(floor((right - p.x) / advance));  // 0 = ones digit
    if (slot < 0 || slot > 2) { return 0.0; }
    var v = value;
    for (var i = 0; i < slot; i++) { v = v / 10u; }
    if (slot > 0 && v == 0u) { return 0.0; }
    let d = f32(v % 10u);
    let r = vec4f(digits.x + u.meter.x + d * u.meter.y, digits.y, u.meter.y, digits.w);
    return rectAlpha(p, r, vec2f(right - f32(slot + 1) * advance, y - digits.w * scale * 0.5), em, px);
}
fn barColour(ms: f32) -> vec3f {
    if (ms <= 18.0) { return vec3f(0.45, 0.86, 0.56); }
    if (ms <= 26.0) { return vec3f(1.00, 0.76, 0.35); }
    return vec3f(1.00, 0.38, 0.36);
}
// Frame-timing panel: fps, a graph of frame intervals (line = 60 fps) and the audio buffer.
fn drawMeter(base: vec3f, p: vec2f, px: f32) -> vec3f {
    var c = base;
    let aspect = u.size.x / u.size.y;
    let size = vec2f(0.27, 0.118);
    let topLeft = vec2f(0.5 * aspect - 0.025 - size.x, -0.475);
    let centre = topLeft + size * 0.5;
    let box = roundBox(p - centre, size * 0.5, 0.012);
    if (box > 0.01) { return c; }
    c = over(c, vec3f(0.02, 0.025, 0.05), 0.55 * clamp(0.5 - box / px, 0.0, 1.0));
    let inner = topLeft + vec2f(0.012, 0.0);
    let width = size.x - 0.024;
    // Header: "57 fps" on the left, "audio NN%" on the right.
    let rowY = topLeft.y + 0.020;
    let fpsRight = inner.x + 0.052;
    c = over(c, vec3f(0.96), numberAlpha(p, u32(round(u.meter.w)), fpsRight, rowY, 0.030, px));
    c = over(c, vec3f(0.62, 0.65, 0.78), labelAlpha(p, u.lines[4], fpsRight + 0.006, rowY + 0.002, 0.018, px).x);
    let last = samples[max(i32(u.meter.z) - 1, 0)];
    let audioPct = u32(round(clamp(last.z, 0.0, 1.0) * 100.0));
    let pctRight = inner.x + width;
    if (last.z >= 0.0) {
        c = over(c, vec3f(0.96), numberAlpha(p, audioPct, pctRight, rowY, 0.022, px));
    }
    c = over(c, vec3f(0.62, 0.65, 0.78), labelAlpha(p, u.lines[5], pctRight - 0.090, rowY + 0.002, 0.018, px).x);
    // Frame-interval bars, newest on the right; full height = 50 ms.
    let graphTop = topLeft.y + 0.040; let graphHeight = 0.046;
    let graphBottom = graphTop + graphHeight;
    let shown = u.meter.z;
    let column = (p.x - inner.x) / width * 128.0;
    let msToY = graphHeight / 50.0;
    if (p.y >= graphTop && p.y <= graphBottom && column >= 0.0 && column < 128.0) {
        c = over(c, vec3f(1.0), 0.05);
        let index = i32(floor(column)) - (128 - i32(shown));
        if (index >= 0) {
            let s = samples[index];
            if (p.y >= graphBottom - min(s.x, 50.0) * msToY) {
                c = over(c, barColour(s.x), 0.9);
            }
            if (s.y > 0.0 && p.y >= graphBottom - min(s.y, 50.0) * msToY) {
                c = over(c, vec3f(1.0), 0.18);  // part of the interval spent inside the core
            }
        }
        for (var ms = 16.667; ms < 40.0; ms += 16.667) {
            let ly = graphBottom - ms * msToY;
            c = over(c, vec3f(1.0), (0.35 - ms * 0.006) * clamp(1.0 - abs(p.y - ly) / px, 0.0, 1.0));
        }
    }
    // Audio buffer strip: fill = occupancy, red where the frontend predicted an underrun.
    let stripTop = graphBottom + 0.010; let stripHeight = 0.013;
    if (p.y >= stripTop && p.y <= stripTop + stripHeight && column >= 0.0 && column < 128.0) {
        c = over(c, vec3f(1.0), 0.05);
        let index = i32(floor(column)) - (128 - i32(shown));
        if (index >= 0) {
            let s = samples[index];
            if (s.w > 0.5) {
                c = over(c, vec3f(1.00, 0.38, 0.36), 0.95);
            } else if (s.z >= 0.0 && p.y >= stripTop + stripHeight * (1.0 - s.z)) {
                c = over(c, vec3f(0.45, 0.62, 1.00), 0.85);
            }
        }
    }
    return c;
}

fn hash(p: vec2f) -> f32 { return fract(sin(dot(p, vec2f(12.9898, 78.233))) * 43758.5453); }

@fragment fn fs(@builtin(position) frag: vec4f) -> @location(0) vec4f {
    let s = u.size.y;
    let p = (frag.xy - u.size * 0.5) / s;  // y down, height spans -0.5..0.5
    let px = 1.0 / s;
    let t = u.time;
    let done = u.done;
    let ready = select(0.0, 1.0, done >= 0.0);

    // Deep blue backdrop with two slow, soft glows and a gentle vignette.
    var c = mix(vec3f(0.086, 0.094, 0.180), vec3f(0.031, 0.035, 0.075), smoothstep(0.05, 0.85, length(p * vec2f(0.85, 1.15))));
    let g1 = vec2f(0.30 * sin(t * 0.23), -0.10 + 0.07 * cos(t * 0.19));
    let g2 = vec2f(-0.28 * cos(t * 0.17), 0.16 + 0.06 * sin(t * 0.29));
    c += vec3f(0.95, 0.55, 0.32) * 0.075 * exp(-dot(p - g1, p - g1) / 0.07);
    c += vec3f(0.36, 0.46, 1.00) * 0.085 * exp(-dot(p - g2, p - g2) / 0.09);

    // The paper card: turns about its vertical axis, lifts while it moves, pops when ready.
    let flip = cos(u.angle);
    let front = flip >= 0.0;
    let pop = 1.0 + 0.07 * ready * sin(clamp((done - 0.38) / 0.30, 0.0, 1.0) * 3.14159);
    let cardHalf = vec2f(0.072 * max(abs(flip), 0.015), 0.072) * pop;
    let centre = vec2f(0.0, -0.095 - 0.022 * u.lift);
    let shadowY = -0.095 + 0.072 + 0.024;
    let shadowW = 0.072 * max(abs(flip), 0.15) * (1.0 - 0.18 * u.lift) + 0.012;
    let sp = (p - vec2f(0.0, shadowY)) / vec2f(shadowW, 0.010);
    c = over(c, vec3f(0.0), 0.30 * (1.0 - 0.35 * u.lift) * exp(-dot(sp, sp) * 1.6));
    let d = roundBox(p - centre, cardHalf, 0.014 * pop);
    let shade = 0.78 + 0.22 * abs(flip);
    let light = 1.0 - 0.10 * smoothstep(-0.07, 0.07, p.y - centre.y);  // top-lit paper
    var face = select(vec3f(0.910, 0.345, 0.310), vec3f(0.975, 0.945, 0.870), front) * shade * light;
    let rim = smoothstep(-0.0115 - px, -0.0115, d) * (1.0 - smoothstep(-0.0085, -0.0085 + px, d));
    face = mix(face, face * select(1.18, 0.90, front), rim);
    // Check mark drawn onto the face once the card has landed.
    let draw = clamp((done - 0.40) / 0.32, 0.0, 1.0) * ready;
    if (draw > 0.0) {
        let q = (p - centre) / pop;
        let a = vec2f(-0.031, 0.002); let b = vec2f(-0.009, 0.024); let e = vec2f(0.034, -0.022);
        let l1 = length(b - a); let l2 = length(e - b);
        let reach = draw * (l1 + l2);
        var stroke = segment(q, a, a + (b - a) * min(reach / l1, 1.0));
        if (reach > l1) { stroke = min(stroke, segment(q, b, b + (e - b) * ((reach - l1) / l2))); }
        face = over(face, vec3f(0.180, 0.600, 0.420), 1.0 - smoothstep(0.0072 - px, 0.0072 + px, stroke));
    }
    c = over(c, face, clamp(0.5 - d / px, 0.0, 1.0));

    // Title crossfades to "Ready"; the subtitle bows out.
    let swap = smoothstep(0.05, 0.40, done) * ready;
    let titleY = 0.075;
    let title = textAlpha(p, u.lines[0], vec2f(0.0, titleY - 0.012 * swap), 0.050, px) * (1.0 - swap);
    let readyText = textAlpha(p, u.lines[1], vec2f(0.0, titleY + 0.012 * (1.0 - swap)), 0.050, px) * swap;
    c = over(c, vec3f(0.955, 0.950, 0.975), title + readyText);
    let subtitle = textAlpha(p, u.lines[2], vec2f(0.0, 0.135), 0.029, px) * (1.0 - swap);
    c = over(c, vec3f(0.62, 0.65, 0.78), subtitle * 0.85);

    // Thin progress bar with a travelling shimmer and a glowing leading edge.
    let barY = 0.205; let barHalf = vec2f(0.20, 0.0042);
    let barFade = 1.0 - smoothstep(0.15, 0.55, done) * ready;
    let bd = roundBox(p - vec2f(0.0, barY), barHalf, barHalf.y);
    let inBar = clamp(0.5 - bd / px, 0.0, 1.0);
    c = over(c, vec3f(1.0), 0.10 * inBar * barFade);
    let edge = -barHalf.x + 2.0 * barHalf.x * u.progress;
    let filled = inBar * clamp(0.5 - (p.x - edge) / px, 0.0, 1.0);
    let along = clamp((p.x + barHalf.x) / (2.0 * barHalf.x), 0.0, 1.0);
    var fill = mix(vec3f(1.00, 0.80, 0.38), vec3f(1.00, 0.50, 0.40), along);
    let band = -barHalf.x + fract(t * 0.55) * (2.0 * barHalf.x + 0.16) - 0.08;
    fill += vec3f(0.40, 0.32, 0.25) * exp(-pow((p.x - band) / 0.035, 2.0));
    c = over(c, fill, filled * barFade);
    let glow = p - vec2f(edge, barY);
    c += vec3f(1.0, 0.62, 0.40) * 0.35 * exp(-dot(glow, glow) / 0.00012) * barFade * step(0.002, u.progress);

    if (u.meter.z > 0.0) { c = drawMeter(c, p, px); }
    c += (hash(frag.xy + fract(t)) - 0.5) / 255.0;  // dither away gradient banding
    return vec4f(c * (1.0 - u.fade), 1.0);
}
)wgsl";
}  // namespace

float FrameStats::Fps(size_t window) const {
    const size_t n = std::min(window, count);
    double total = 0;
    for (size_t i = count - n; i < count; ++i) total += At(i).intervalMs;
    return total > 0 ? float(1000.0 * n / total) : 0.f;
}

void PrepareTimeline::Advance(double fraction, bool finished) {
    time += kFrameSeconds;
    const double target = finished ? 1.0 : std::clamp(fraction, 0.0, 1.0);
    progress += (target - progress) * (1 - std::exp(-kFrameSeconds * 5));
    if (finished) progress = std::min(1.0, progress + 0.012);
    if (done < 0 && finished && progress >= 0.999) {
        done = 0;
        progress = 1;
        settleFrom = angle;
    } else if (done >= 0) {
        done += kFrameSeconds;
    }
    double lift = 0;
    if (done < 0) {
        const double period = kFlipRest + kFlipMove;
        const double turn = std::floor(time / period), within = std::fmod(time, period);
        const double move = std::clamp((within - kFlipRest) / kFlipMove, 0.0, 1.0);
        angle = kPi * (turn + EaseInOutCubic(move));
        lift = std::sin(kPi * move);
    } else {
        // Land face up: the next whole turn at or beyond where the card was.
        const double target = 2 * kPi * std::ceil(settleFrom / (2 * kPi) - 1e-6);
        const double x = std::clamp(done / kSettle, 0.0, 1.0);
        angle = settleFrom + (target - settleFrom) * EaseOutBack(x);
        lift = target > settleFrom + 1e-6 ? std::sin(kPi * x) * 0.8 : 0;
        if (!sounding && done >= kSettle - 0.02) { sounding = true; soundSample = 0; }
    }
    frame.time = float(time);
    frame.progress = float(progress);
    frame.done = float(done);
    frame.fade = done < kHold ? 0.f : float(std::clamp((done - kHold) / kFadeOut, 0.0, 1.0));
    frame.cardAngle = float(angle);
    frame.cardLift = float(lift);
}

size_t PrepareTimeline::Audio(int16_t* stereo, size_t maxFrames) {
    audioDebt += kSampleRate * kFrameSeconds;
    const size_t count = std::min(maxFrames, size_t(audioDebt));
    audioDebt -= double(count);
    for (size_t i = 0; i < count; ++i) {
        double left = 0, right = 0;
        if (sounding) {
            const double t = double(soundSample++) / kSampleRate;
            const double low = Note(t, 1567.98, 0.26), high = Note(t - 0.072, 2349.32, 0.30);  // G6 then D7
            left = low + high * 0.82;
            right = low * 0.82 + high;
        }
        stereo[i * 2] = int16_t(std::clamp(left, -1.0, 1.0) * 32767);
        stereo[i * 2 + 1] = int16_t(std::clamp(right, -1.0, 1.0) * 32767);
    }
    return count;
}

PrepareScreen::PrepareScreen(wgpu::Device existing) : device(std::move(existing)) {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kShader;
    wgpu::ShaderModuleDescriptor shaderDesc{};
    shaderDesc.nextInChain = &wgsl;
    const auto shader = device.CreateShaderModule(&shaderDesc);
    wgpu::ColorTargetState color{};
    color.format = wgpu::TextureFormat::RGBA8Unorm;
    wgpu::FragmentState fragment{};
    fragment.module = shader;
    fragment.entryPoint = "fs";
    fragment.targetCount = 1;
    fragment.targets = &color;
    wgpu::RenderPipelineDescriptor pipelineDesc{};
    pipelineDesc.vertex.module = shader;
    pipelineDesc.vertex.entryPoint = "vs";
    pipelineDesc.fragment = &fragment;
    pipelineDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pipeline = device.CreateRenderPipeline(&pipelineDesc);

    wgpu::BufferDescriptor bufferDesc{};
    bufferDesc.size = kUniformBytes;
    bufferDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    uniforms = device.CreateBuffer(&bufferDesc);
    bufferDesc.size = kStatsBytes;
    statsBuffer = device.CreateBuffer(&bufferDesc);

    const auto atlas = DecodeBase64(kAtlasBase64);
    if (atlas.size() != size_t(kAtlasWidth) * kAtlasHeight) throw std::runtime_error("Preparing screen text atlas is corrupt");
    wgpu::TextureDescriptor textureDesc{};
    textureDesc.size = {kAtlasWidth, kAtlasHeight, 1};
    textureDesc.format = wgpu::TextureFormat::R8Unorm;
    textureDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    const auto texture = device.CreateTexture(&textureDesc);
    wgpu::TexelCopyTextureInfo destination{};
    destination.texture = texture;
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = kAtlasWidth;
    layout.rowsPerImage = kAtlasHeight;
    device.GetQueue().WriteTexture(&destination, atlas.data(), atlas.size(), &layout, &textureDesc.size);

    wgpu::SamplerDescriptor samplerDesc{};
    samplerDesc.magFilter = samplerDesc.minFilter = wgpu::FilterMode::Linear;
    const auto sampler = device.CreateSampler(&samplerDesc);
    wgpu::BindGroupEntry entries[4]{};
    entries[0].binding = 0;
    entries[0].buffer = uniforms;
    entries[0].size = kUniformBytes;
    entries[3].binding = 3;
    entries[3].buffer = statsBuffer;
    entries[3].size = kStatsBytes;
    entries[1].binding = 1;
    entries[1].textureView = texture.CreateView();
    entries[2].binding = 2;
    entries[2].sampler = sampler;
    wgpu::BindGroupDescriptor groupDesc{};
    groupDesc.layout = pipeline.GetBindGroupLayout(0);
    groupDesc.entryCount = 4;
    groupDesc.entries = entries;
    bindings = device.CreateBindGroup(&groupDesc);
}

void PrepareScreen::Draw(const wgpu::Texture& target, const PrepareFrame& frame, const FrameStats* stats) {
    const size_t shown = stats ? stats->Count() : 0;
    // Distance units per atlas pixel: the generator maps its 6 px spread onto 128 of 255 levels.
    float values[kUniformBytes / 4] = {
        float(target.GetWidth()), float(target.GetHeight()), frame.time, frame.progress,
        frame.done, frame.fade, frame.cardAngle, frame.cardLift,
        float(kAtlasWidth), float(kAtlasHeight), kAtlasEm, 128.f / kAtlasPadding / 255.f,
        kAtlasPadding, kDigitAdvance, float(shown), stats ? stats->Fps() : 0.f,
    };
    for (int i = 0; i < kLineCount; ++i) {
        float* line = &values[16 + i * 4];
        line[0] = float(kLines[i].x); line[1] = float(kLines[i].y);
        line[2] = float(kLines[i].width); line[3] = float(kLines[i].height);
    }
    device.GetQueue().WriteBuffer(uniforms, 0, values, sizeof(values));
    if (shown) {
        std::array<float, FrameStats::kHistory * 4> history{};
        for (size_t i = 0; i < shown; ++i) {
            const auto& s = stats->At(i);
            history[i * 4] = s.intervalMs; history[i * 4 + 1] = s.workMs;
            history[i * 4 + 2] = s.audio; history[i * 4 + 3] = s.underrun;
        }
        device.GetQueue().WriteBuffer(statsBuffer, 0, history.data(), sizeof(history));
    }
    auto encoder = device.CreateCommandEncoder();
    wgpu::RenderPassColorAttachment attachment{};
    attachment.view = target.CreateView();
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = {0, 0, 0, 1};
    wgpu::RenderPassDescriptor passDesc{};
    passDesc.colorAttachmentCount = 1;
    passDesc.colorAttachments = &attachment;
    auto pass = encoder.BeginRenderPass(&passDesc);
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, bindings);
    pass.Draw(3);
    pass.End();
    const auto commands = encoder.Finish();
    device.GetQueue().Submit(1, &commands);
}
}  // namespace RuntimeFrontend
