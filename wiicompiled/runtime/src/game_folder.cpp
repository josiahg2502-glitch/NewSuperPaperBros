#include "game_folder.h"

#include "disc_image.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_messagebox.h>

#include <cctype>
#include <fstream>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace GameFolder {
namespace {

namespace fs = std::filesystem;

bool IsExtractedDisc(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path / "files", ec) && fs::is_regular_file(path / "sys" / "fst.bin", ec);
}

#if defined(_WIN32)
constexpr const wchar_t* kTitle = L"Super Paper Mario";

constexpr const wchar_t* kSteps =
    L"This port runs your own copy of Super Paper Mario (USA, R8PE01, revision 0, or Europe, R8PP01).\n\n"
    L"Click OK and choose your disc image (.iso, .ciso or .wbfs, dumped from your Wii with CleanRip or "
    L"USB Loader GX). The game is set up from it once; you can delete the image afterwards.\n\n"
    L"(Or put the image in the game folder and start the game again.)";

// The Windows file picker for a disc image; empty when cancelled.
fs::path PickImage() {
    fs::path result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        const COMDLG_FILTERSPEC types[] = {{L"Wii disc images (*.iso, *.ciso, *.wbfs)", L"*.iso;*.ciso;*.wbfs"},
                                           {L"All files", L"*.*"}};
        dialog->SetFileTypes(2, types);
        dialog->SetTitle(L"Choose your Super Paper Mario disc image");
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                    result = fs::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}

std::wstring Widen(const std::string& utf8) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}
#endif

// The folder the program is in, and the download's top folder (the program sits in its engine/ folder).
fs::path EngineDirectory() {
    const char* base = SDL_GetBasePath();
    fs::path dir = base ? fs::u8path(base) : fs::current_path();
    if (!dir.has_filename()) dir = dir.parent_path();
    return dir;
}

fs::path GameRoot() {
    const fs::path engine = EngineDirectory();
    return engine.filename() == "engine" ? engine.parent_path() : engine;
}

bool IsImage(const fs::path& p) {
    std::string ext = RuntimeConfigFile::PathToUtf8(p.extension());
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".iso" || ext == ".ciso" || ext == ".wbfs";
}

// A disc image the player put in the game folder (or beside the program).
fs::path ImageInGameFolder() {
    std::error_code ec;
    for (const fs::path& dir : {GameRoot(), EngineDirectory()})
        for (const auto& entry : fs::directory_iterator(dir, ec))
            if (entry.is_regular_file(ec) && IsImage(entry.path())) return entry.path();
    return {};
}

void ShowMessage(const std::string& text) {
#if defined(_WIN32)
    MessageBoxW(nullptr, Widen(text).c_str(), kTitle, MB_OK | MB_ICONINFORMATION);
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Super Paper Mario", text.c_str(), nullptr);
#endif
    RT_LOGF(RT_TAG_RUNTIME, "game folder: %s\n", text.c_str());
}

// Remembers the game at `root`, relative to UserData/Config.toml (one level below the game folder) so the folder can move.
void SaveDvdRoot(const fs::path& root) {
    std::error_code ec;
    const fs::path rel = fs::relative(root, RuntimeConfigFile::ResolveConfigPath().parent_path(), ec);
    RuntimeConfigFile::SetDvdRoot(ec || rel.empty() ? root : rel);
}

#if defined(_WIN32)
// The shell progress window's ids (shlobj.h); spelled out because mingw has no __uuidof for it.
constexpr GUID kClsidProgressDialog = {0xF8383852, 0xFCD3, 0x11D1, {0xA6, 0xB9, 0x00, 0x60, 0x97, 0xDF, 0x5B, 0xD4}};
constexpr GUID kIidProgressDialog = {0xEBBC7C04, 0x315E, 0x11D2, {0xB6, 0x2F, 0x00, 0x60, 0x97, 0xDF, 0x5B, 0xD4}};
#endif

// Unpacks `image` into <game folder>/Disc/DATA (a progress window on Windows). "" on success, else why not.
std::string SetUpFromImage(const fs::path& image) {
    DiscImage::Info info;
    std::string error;
    if (!DiscImage::Probe(image, info, error)) return RuntimeConfigFile::PathToUtf8(image.filename()) + ": " + error;
    if (info.id != "R8PE01" && info.id != "R8PP01")
        return "That is " + info.id + " (" + info.title + "), not Super Paper Mario for the USA (R8PE01) or Europe (R8PP01).";
    if (info.id == "R8PE01" && info.revision != 0)
        return "That is Super Paper Mario USA revision " + std::to_string(info.revision) + "; only revision 0 (the first print) works.";
    const fs::path target = GameRoot() / "Disc" / "DATA";
    const fs::path staging = GameRoot() / "Disc" / "DATA.new";
    std::error_code ec;
    fs::remove_all(staging, ec);
#if defined(_WIN32)
    IProgressDialog* dialog = nullptr;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(CoCreateInstance(kClsidProgressDialog, nullptr, CLSCTX_INPROC_SERVER, kIidProgressDialog,
                                   reinterpret_cast<void**>(&dialog)))) {
        dialog->SetTitle(kTitle);
        dialog->SetLine(1, L"Setting up the game from your disc (once only)...", FALSE, nullptr);
        dialog->StartProgressDialog(nullptr, nullptr, PROGDLG_NORMAL | PROGDLG_AUTOTIME | PROGDLG_NOCANCEL, nullptr);
    }
    const auto progress = [&](int done, int total) {
        if (dialog) dialog->SetProgress(DWORD(done), DWORD(total));
    };
#else
    const auto progress = [](int done, int total) {
        if (done % 200 == 0 || done == total) RT_LOGF(RT_TAG_RUNTIME, "setting up the game: %d / %d files\n", done, total);
    };
#endif
    const bool ok = DiscImage::Extract(image, staging, progress, error);
#if defined(_WIN32)
    if (dialog) {
        dialog->StopProgressDialog();
        dialog->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
#endif
    if (!ok) {
        fs::remove_all(staging, ec);
        return "The disc could not be set up: " + error;
    }
    fs::remove_all(target, ec);
    fs::rename(staging, target, ec);
    if (ec) return "The disc could not be set up: " + ec.message();
    SaveDvdRoot(target);
    RT_LOGF(RT_TAG_RUNTIME, "game folder: set up %s from %s\n", info.id.c_str(), RuntimeConfigFile::PathToUtf8(image).c_str());
    return {};
}

// The disc is the other region than this engine: start the program again so its region's engine runs.
void RestartForRegion(const std::string& id) {
#if defined(_WIN32)
    (void)id;  // the loader (product/engine_loader.cpp) picks the module from the disc
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       EngineDirectory().wstring().c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
#else
    const std::string path = RuntimeConfigFile::PathToUtf8(EngineDirectory() / (id == "R8PP01" ? "nspb-engine-pal" : "nspb-engine-usa"));
    execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
#endif
}

std::string DiscIdAt(const fs::path& root) {
    std::ifstream boot(root / "sys" / "boot.bin", std::ios::binary);
    char id[6] = {};
    return boot.read(id, 6) ? std::string(id, 6) : std::string();
}

} // namespace

fs::path Find(const fs::path& path) {
    if (path.empty()) {
        return {};
    }
    if (IsExtractedDisc(path)) {
        return path;
    }
    if (IsExtractedDisc(path / "DATA")) {
        return path / "DATA";
    }
    return {};
}

std::string Problem(const fs::path& root) {
    if (!IsExtractedDisc(root)) {
        return "That folder isn't an extracted game: it needs the files and sys folders Dolphin's Extract "
               "Entire Disc makes.";
    }
    std::ifstream boot(root / "sys" / "boot.bin", std::ios::binary);
    char header[8] = {};
    if (!boot.read(header, sizeof(header))) {
        return "That folder has no sys/boot.bin, so the game can't be identified.";
    }
    const std::string id(header, 6);
    const int revision = static_cast<unsigned char>(header[7]);
    // projects/spm/pal/tools/make_pal_runtime.py rewrites these two for the PAL engine (R8PP01, any revision).
    constexpr const char* kGameId = "R8PE01";
    constexpr int kRevision = 0;
    if (id != kGameId) {
        return "That is " + id + ", not Super Paper Mario " + kGameId + ". This engine runs only that disc.";
    }
    if (kRevision >= 0 && revision != kRevision) {
        return "That is Super Paper Mario revision " + std::to_string(revision) +
               ". This port runs only revision 0 (Dolphin shows it under Properties > Info).";
    }
    return {};
}

bool EnsureConfigured() {
#if defined(_WIN32)
    // A "game" folder next to the program needs no questions, and wins over the saved folder: every
    // install shares Documents\MarioWiiSaveData, so this is how two installs play two games (say the
    // original and a mod), each with its own saves (nand_path.h keeps saves per game).
    if (const fs::path beside = Find(EngineDirectory() / "game"); !beside.empty() && Problem(beside).empty()) {
        if (RuntimeConfigFile::DvdRoot().empty()) {
            RuntimeConfigFile::SetDvdRoot(beside);
        } else {
            RuntimeConfigFile::UseDvdRootForThisRun(beside);
        }
        RT_LOGF(RT_TAG_RUNTIME, "game folder: %s\n", RuntimeConfigFile::PathToUtf8(beside).c_str());
        return true;
    }
#endif
    fs::path configured = Find(RuntimeConfigFile::ResolvedDvdRoot());
    if (configured.empty()) {
        // The download's own Disc/DATA (set up earlier, or by the old launcher).
        if (const fs::path own = Find(GameRoot() / "Disc" / "DATA"); !own.empty()) {
            SaveDvdRoot(own);
            configured = own;
        }
    }
    if (!configured.empty() && Problem(configured).empty()) {
        return true;
    }
    if (!configured.empty()) {
        const std::string id = DiscIdAt(configured);
        if (id == "R8PE01" || id == "R8PP01") {  // the other region's disc: its engine runs it
            RestartForRegion(id);
            return false;
        }
    }
    // Set the game up from a disc image: one in the game folder, else (Windows) one the player picks.
    std::string problem = configured.empty() ? std::string() : Problem(configured);
    fs::path image = ImageInGameFolder();
    for (;;) {
        if (image.empty()) {
#if defined(_WIN32)
            const std::wstring message = problem.empty() ? std::wstring(kSteps) : Widen(problem) + L"\n\n" + kSteps;
            if (MessageBoxW(nullptr, message.c_str(), kTitle, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return false;
            image = PickImage();
            if (image.empty()) continue;
#else
            ShowMessage((problem.empty() ? std::string() : problem + "\n\n") +
                        "Put your Super Paper Mario disc image (.iso, .ciso or .wbfs) in " + RuntimeConfigFile::PathToUtf8(GameRoot()) +
                        " and start the game again. It is set up from the image once.");
            return false;
#endif
        }
        problem = SetUpFromImage(image);
        image.clear();
        if (!problem.empty()) {
#if defined(_WIN32)
            continue;
#else
            ShowMessage(problem);
            return false;
#endif
        }
        configured = Find(RuntimeConfigFile::ResolvedDvdRoot());
        if (!configured.empty() && Problem(configured).empty()) return true;
        RestartForRegion(DiscIdAt(configured));
        return false;
    }
}

} // namespace GameFolder
