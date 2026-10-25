#include "frontend/runtime_contract.h"
#include "frontend/runtime_mode.h"
#include <aurora/external.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <fstream>
#include <iterator>
#include <map>
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
            // SPM_REBUILD_AT="3000,6000": rebuild every guest thread's native stack from its continuation
            // after those frames.
            std::set<unsigned> rebuilds;
            if (const char* list = std::getenv("SPM_REBUILD_AT")) {
                std::stringstream items(list);
                for (std::string item; std::getline(items, item, ',');) rebuilds.insert(static_cast<unsigned>(std::stoul(item)));
            }
            // SPM_STATE_AT="3000:4000": capture the state after frame 3000, restore it after frame 4000 and
            // carry on from frame 3001.
            std::map<unsigned, unsigned> stateRestores;  // capture frame -> restore frame
            if (const char* list = std::getenv("SPM_STATE_AT")) {
                std::stringstream items(list);
                for (std::string item; std::getline(items, item, ',');) {
                    const auto colon = item.find(':');
                    if (colon != std::string::npos)
                        stateRestores[static_cast<unsigned>(std::stoul(item))] = static_cast<unsigned>(std::stoul(item.substr(colon + 1)));
                }
            }
            // SPM_STATE_SAVE="4000:/path": write the state after that frame. SPM_STATE_LOAD="60:/path:4000": after
            // frame 60 of this run, load the file and carry on as frame 4001.
            unsigned saveFileFrame = ~0u, loadFileFrame = ~0u, loadAsFrame = 0;
            std::string saveFilePath, loadFilePath;
            if (const char* spec = std::getenv("SPM_STATE_SAVE")) {
                const std::string text = spec;
                saveFileFrame = static_cast<unsigned>(std::stoul(text));
                saveFilePath = text.substr(text.find(':') + 1);
            }
            if (const char* spec = std::getenv("SPM_STATE_LOAD")) {
                const std::string text = spec;
                const auto first = text.find(':'), last = text.rfind(':');
                loadFileFrame = static_cast<unsigned>(std::stoul(text));
                loadFilePath = text.substr(first + 1, last - first - 1);
                loadAsFrame = static_cast<unsigned>(std::stoul(text.substr(last + 1)));
            }
            std::vector<uint8_t> savedState;
            unsigned savedFrame = 0, restoreFrame = ~0u;
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
                if (rebuilds.count(frame)) {
                    std::string error;
                    const bool rebuilt = RuntimeFrontend::RebuildContinuationsInPlace(error);
                    std::fprintf(stderr, "HOST rebuild frame=%u %s%s\n", frame, rebuilt ? "ok" : "refused: ", error.c_str());
                }
                if (const auto it = stateRestores.find(frame); it != stateRestores.end()) {
                    std::string error;
                    const auto began = std::chrono::steady_clock::now();
                    const bool captured = RuntimeFrontend::CaptureState(savedState, error);
                    std::fprintf(stderr, "HOST state capture frame=%u %s%s bytes=%zu ms=%.1f\n", frame, captured ? "ok" : "refused: ",
                        error.c_str(), savedState.size(),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
                    if (captured) { savedFrame = frame; restoreFrame = it->second; }
                    stateRestores.erase(it);
                } else if (frame == restoreFrame) {
                    std::string error;
                    const auto began = std::chrono::steady_clock::now();
                    const bool restored = RuntimeFrontend::RestoreState(savedState, error);
                    std::fprintf(stderr, "HOST state restore frame=%u to=%u %s%s ms=%.1f\n", frame, savedFrame, restored ? "ok" : "refused: ",
                        error.c_str(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
                    restoreFrame = ~0u;
                    if (restored) frame = savedFrame;
                }
                if (frame == saveFileFrame) {
                    std::vector<uint8_t> state;
                    std::string error;
                    const bool captured = RuntimeFrontend::CaptureState(state, error);
                    if (captured) std::ofstream(saveFilePath, std::ios::binary).write(reinterpret_cast<const char*>(state.data()), state.size());
                    std::fprintf(stderr, "HOST state save frame=%u %s%s\n", frame, captured ? "ok" : "refused: ", error.c_str());
                } else if (frame == loadFileFrame) {
                    std::ifstream file(loadFilePath, std::ios::binary);
                    std::vector<uint8_t> state((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                    std::string error;
                    const auto began = std::chrono::steady_clock::now();
                    const bool restored = RuntimeFrontend::RestoreState(state, error);
                    std::fprintf(stderr, "HOST state load frame=%u as=%u %s%s ms=%.1f\n", frame, loadAsFrame, restored ? "ok" : "refused: ",
                        error.c_str(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
                    if (restored) frame = loadAsFrame;
                }
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
