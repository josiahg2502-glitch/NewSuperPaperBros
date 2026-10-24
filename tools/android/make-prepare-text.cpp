// Generates wiicompiled/runtime/frontend/prepare_text.inc (run by make-prepare-text.py): the Preparing
// screen's text as one signed-distance atlas (R8, edge = 128), so it stays crisp at any output size.
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
constexpr float kPixelHeight = 34.f;  // em height the distance field is sampled at
constexpr int kPadding = 6;
constexpr float kDistanceScale = 128.f / kPadding;

struct Line {
    std::string text;
    int x = 0, y = 0, width = 0, height = 0;
    float advance = 0;  // pen advance of the first glyph (digits are tabular in Roboto)
    std::vector<unsigned char> pixels;
};

std::vector<unsigned char> ReadFile(const char* path) {
    std::vector<unsigned char> bytes;
    if (FILE* file = std::fopen(path, "rb")) {
        unsigned char buffer[65536];
        size_t n;
        while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
        std::fclose(file);
    }
    return bytes;
}

// Decodes the few non-ASCII characters the lines use.
std::vector<int> Codepoints(const std::string& text) {
    std::vector<int> out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = text[i];
        if (c < 0x80) { out.push_back(c); ++i; }
        else if ((c >> 5) == 6) { out.push_back(((c & 0x1F) << 6) | (text[i + 1] & 0x3F)); i += 2; }
        else { out.push_back(((c & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F)); i += 3; }
    }
    return out;
}

void Render(const stbtt_fontinfo& font, Line& line) {
    const float scale = stbtt_ScaleForPixelHeight(&font, kPixelHeight);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    const int baseline = static_cast<int>(std::ceil(ascent * scale)) + kPadding;
    line.height = baseline + static_cast<int>(std::ceil(-descent * scale)) + kPadding;
    const auto codepoints = Codepoints(line.text);
    float pen = kPadding;
    std::vector<float> advances;
    for (size_t i = 0; i < codepoints.size(); ++i) {
        int advance, bearing;
        stbtt_GetCodepointHMetrics(&font, codepoints[i], &advance, &bearing);
        advances.push_back(pen);
        if (i == 0) line.advance = advance * scale;
        pen += advance * scale;
        if (i + 1 < codepoints.size()) pen += stbtt_GetCodepointKernAdvance(&font, codepoints[i], codepoints[i + 1]) * scale;
    }
    line.width = static_cast<int>(std::ceil(pen)) + kPadding;
    line.pixels.assign(static_cast<size_t>(line.width) * line.height, 0);
    for (size_t i = 0; i < codepoints.size(); ++i) {
        int w, h, xoff, yoff;
        unsigned char* sdf = stbtt_GetCodepointSDF(&font, scale, codepoints[i], kPadding, 128,
                                                   kDistanceScale, &w, &h, &xoff, &yoff);
        if (!sdf) continue;
        const int ox = static_cast<int>(std::lround(advances[i])) + xoff, oy = baseline + yoff;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int tx = ox + x, ty = oy + y;
                if (tx < 0 || ty < 0 || tx >= line.width || ty >= line.height) continue;
                unsigned char& out = line.pixels[static_cast<size_t>(ty) * line.width + tx];
                out = std::max(out, sdf[y * w + x]);  // union of neighbouring glyph fields
            }
        stbtt_FreeSDF(sdf, nullptr);
    }
}

std::string Base64(const std::vector<unsigned char>& bytes) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned v = bytes[i] << 16 | (i + 1 < bytes.size() ? bytes[i + 1] << 8 : 0) |
                           (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        out += table[v >> 18 & 63];
        out += table[v >> 12 & 63];
        out += i + 1 < bytes.size() ? table[v >> 6 & 63] : '=';
        out += i + 2 < bytes.size() ? table[v & 63] : '=';
    }
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: %s font.ttf output.inc\n", argv[0]); return 2; }
    const auto ttf = ReadFile(argv[1]);
    stbtt_fontinfo font;
    if (ttf.empty() || !stbtt_InitFont(&font, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) {
        std::fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    // Order matches the Line enum in prepare_screen.cpp.
    std::vector<Line> lines = {{"Preparing game data"}, {"Ready"}, {"First launch only · this takes a moment"},
                               {"0123456789"}, {"fps"}, {"audio"}};
    for (Line& line : lines) Render(font, line);
    // Shelf packing: left to right, wrapping at the subtitle's width.
    const int width = lines[2].width;
    int x = 0, y = 0, shelf = 0;
    for (Line& line : lines) {
        if (x + line.width > width) { x = 0; y += shelf; shelf = 0; }
        line.x = x; line.y = y;
        x += line.width;
        shelf = std::max(shelf, line.height);
    }
    const int height = y + shelf;
    std::vector<unsigned char> atlas(static_cast<size_t>(width) * height, 0);
    for (const Line& line : lines)
        for (int y = 0; y < line.height; ++y)
            std::copy_n(&line.pixels[static_cast<size_t>(y) * line.width], line.width,
                        &atlas[static_cast<size_t>(line.y + y) * width + line.x]);
    FILE* out = std::fopen(argv[2], "w");
    if (!out) return 1;
    std::fprintf(out, "// Generated by tools/android/make-prepare-text.py from Roboto Medium (Apache License 2.0).\n"
                      "// Signed distance field, R8, edge value 128, %g px em, %d px spread. Do not edit.\n",
                 kPixelHeight, kPadding);
    std::fprintf(out, "constexpr uint32_t kAtlasWidth = %d, kAtlasHeight = %d;\n", width, height);
    std::fprintf(out, "constexpr float kAtlasEm = %g;\n", kPixelHeight);
    std::fprintf(out, "constexpr float kAtlasPadding = %d, kDigitAdvance = %g;\n", kPadding, lines[3].advance);
    std::fprintf(out, "constexpr TextRect kLines[] = {\n");
    for (const Line& line : lines)
        std::fprintf(out, "    {%d, %d, %d, %d},  // %s\n", line.x, line.y, line.width, line.height, line.text.c_str());
    std::fprintf(out, "};\nconstexpr const char* kAtlasBase64 =\n");
    const std::string encoded = Base64(atlas);
    for (size_t i = 0; i < encoded.size(); i += 120) std::fprintf(out, "    \"%s\"\n", encoded.substr(i, 120).c_str());
    std::fprintf(out, "    ;\n");
    std::fclose(out);
    std::printf("atlas %dx%d, %zu bytes\n", width, height, atlas.size());
    return 0;
}
