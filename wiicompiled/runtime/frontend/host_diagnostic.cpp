#include "frontend/runtime_contract.h"
#include "frontend/runtime_mode.h"
#include <aurora/external.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

namespace {
// SPM_FRONTEND_INPUT="3000:TWO:6,3200:DOWN+A:10": hold the named buttons (screen directions, as the
// libretro adapter reports them) from frame N for D frames.
struct ScriptedPress { unsigned start = 0, frames = 0; RuntimeFrontend::InputSnapshot input; };
std::vector<ScriptedPress> ParseInputScript(const char* text) {
    using namespace RuntimeFrontend;
    std::vector<ScriptedPress> presses;
    std::stringstream list(text ? text : "");
    for (std::string item; std::getline(list, item, ',');) {
        std::stringstream fields(item);
        std::string start, names, frames;
        std::getline(fields, start, ':'); std::getline(fields, names, ':'); std::getline(fields, frames, ':');
        ScriptedPress press{static_cast<unsigned>(std::stoul(start)), frames.empty() ? 6u : static_cast<unsigned>(std::stoul(frames))};
        std::stringstream buttons(names);
        for (std::string name; std::getline(buttons, name, '+');) {
            const uint32_t bit = name == "TWO" ? Two : name == "ONE" ? One : name == "A" ? A : name == "B" ? B :
                                 name == "PLUS" ? Plus : name == "MINUS" ? Minus : name == "HOME" ? Home :
                                 name == "UP" ? Up : name == "DOWN" ? Down : name == "LEFT" ? Left :
                                 name == "RIGHT" ? Right : 0u;
            if (name == "SHAKE") press.input.shake = true;
            else if (!bit) throw std::runtime_error("Unknown scripted button " + name);
            press.input.buttons |= bit;
        }
        presses.push_back(press);
    }
    return presses;
}
}  // namespace

// The libretro core's runtime path (boot, fibers, VI, GX) without Android's presentation or UI.
int RunFrontendDiagnostic(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: --frontend-diagnostic DISC_DATA SYSTEM OUTPUT FRAMES\n");
        return 2;
    }
    const std::filesystem::path output = std::filesystem::absolute(argv[4]);
    const unsigned count = static_cast<unsigned>(std::stoul(argv[5]));
    if (!count || count > 36000) return 2;
    std::filesystem::create_directories(output);
    RuntimeFrontend::ContentPaths paths{std::filesystem::absolute(argv[2]), output / "saves",
                                       std::filesystem::absolute(argv[3])};
    uint64_t pcmFrames = 0;
    RuntimeFrontend::Callbacks sinks{
        [](void* opaque, const int16_t*, size_t frames, uint32_t) {
            *static_cast<uint64_t*>(opaque) += frames;
            return frames;
        }, &pcmFrames};
    int status = 0;
    try {
        const unsigned cycles = std::getenv("SPM_FRONTEND_CYCLES") ? std::stoul(std::getenv("SPM_FRONTEND_CYCLES")) : 1;
        if (!cycles || cycles > 100) throw std::runtime_error("Invalid lifecycle cycle count");
        for (unsigned cycle = 0; cycle < cycles; ++cycle) {
            const auto cycleOutput = cycles == 1 ? output : output / ("cycle-" + std::to_string(cycle + 1));
            std::filesystem::create_directories(cycleOutput);
            std::fprintf(stderr, "HOST lifecycle cycle=%u/%u begin\n", cycle + 1, cycles);
            if (const char* step = std::getenv("SPM_FRAME_STEP")) RuntimeFrontend::SetFrameStep(std::stoul(step));
            if (!RuntimeFrontend::Initialize(paths, sinks))
                throw std::runtime_error(RuntimeFrontend::LastError());
            const auto presses = ParseInputScript(std::getenv("SPM_FRONTEND_INPUT"));
            const auto start = std::chrono::steady_clock::now();
            for (unsigned frame = 0; frame < count; ++frame) {
                RuntimeFrontend::InputSnapshot input;
                for (const auto& press : presses)
                    if (frame >= press.start && frame < press.start + press.frames) {
                        input.buttons |= press.input.buttons;
                        input.shake |= press.input.shake;
                    }
                if (input.buttons || input.shake) std::fprintf(stderr, "HOST input frame=%u buttons=0x%x\n", frame, input.buttons);
                const auto result = RuntimeFrontend::AdvanceOneFrame(input, {});
                if (result == RuntimeFrontend::FrameResult::Failed || result == RuntimeFrontend::FrameResult::Stopped)
                    throw std::runtime_error(RuntimeFrontend::LastError());
                if (!aurora_external_healthy()) throw std::runtime_error("Dawn device lost");
                if ((frame + 1) % 600 == 0)
                    std::fprintf(stderr, "HOST frame=%u pcmFrames=%llu elapsed=%.3f\n", frame + 1,
                        static_cast<unsigned long long>(pcmFrames),
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            }
            RuntimeFrontend::Unload();
            RuntimeFrontend::Unload();  // must be idempotent
            std::fprintf(stderr, "HOST lifecycle cycle=%u/%u end pcmFrames=%llu\n", cycle + 1, cycles, static_cast<unsigned long long>(pcmFrames));
            size_t openFiles = 0;
            for (const auto& entry : std::filesystem::directory_iterator("/dev/fd")) ++openFiles;
            std::fprintf(stderr, "HOST resources cycle=%u openFiles=%zu\n", cycle + 1, openFiles);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "HOST FAILED: %s\n", error.what());
        status = 1;
    }
    RuntimeFrontend::Unload();
    return status;
}
