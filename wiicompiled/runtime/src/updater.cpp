#include "updater.h"

#include "aurora_events.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <SDL3/SDL_filesystem.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace Updater {
namespace {
constexpr const char* kApi = "https://api.github.com/repos/josiahg2502-glitch/NewSuperPaperBros/releases/latest";
constexpr const char* kDownload = "https://github.com/josiahg2502-glitch/NewSuperPaperBros/releases/download/";
#if defined(_WIN32)
constexpr const char* kAsset = "NewSuperPaperBros.zip";
#else
constexpr const char* kAsset = "NewSuperPaperBros-linux.tar.gz";
#endif

std::atomic<State> g_state{State::Off};
std::mutex g_mutex;
std::string g_current, g_latest;
std::once_flag g_started;

fs::path EngineDir() {
    const char* base = SDL_GetBasePath();
    fs::path dir = base ? fs::u8path(base) : fs::current_path();
    if (!dir.has_filename()) dir = dir.parent_path();  // SDL's base path ends in a separator
    return dir;
}

// Runs a program without a console window and waits for it. Returns its exit code (-1 if it could not start).
int RunHidden(const std::string& commandLine) {
#if defined(_WIN32)
    std::wstring cmd(commandLine.size() + 1, L'\0');
    cmd.resize(MultiByteToWideChar(CP_UTF8, 0, commandLine.c_str(), -1, cmd.data(), int(cmd.size())) - 1);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
#else
    const int status = std::system(commandLine.c_str());
    return status == -1 ? -1 : WEXITSTATUS(status);
#endif
}

std::string Quote(const fs::path& p) { return "\"" + RuntimeConfigFile::PathToUtf8(p) + "\""; }

// "v1.2.3" -> {1,2,3}; anything unparsable compares as 0.
std::vector<int> Parts(const std::string& tag) {
    std::vector<int> parts;
    std::stringstream in(tag.substr(tag.find_first_of("0123456789") == std::string::npos ? tag.size() : tag.find_first_of("0123456789")));
    for (std::string part; std::getline(in, part, '.');) parts.push_back(std::atoi(part.c_str()));
    return parts;
}

bool Newer(const std::string& latest, const std::string& current) {
    const auto a = Parts(latest), b = Parts(current);
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

void Check() {
    const fs::path json = EngineDir() / "UserData" / "update_check.json";
    std::error_code ec;
    fs::create_directories(json.parent_path(), ec);
    const int code = RunHidden("curl -sfL -m 20 -H \"Accept: application/vnd.github+json\" -o " + Quote(json) + " " + kApi);
    std::ifstream in(json);
    const std::string body((std::istreambuf_iterator<char>(in)), {});
    in.close();
    fs::remove(json, ec);
    const size_t key = body.find("\"tag_name\"");
    const size_t open = key == std::string::npos ? key : body.find('"', body.find(':', key) + 1);
    const size_t close = open == std::string::npos ? open : body.find('"', open + 1);
    if (code != 0 || close == std::string::npos) {
        RT_LOGF(RT_TAG_RUNTIME, "update check failed (curl %d)\n", code);
        g_state = State::Failed;
        return;
    }
    const std::string tag = body.substr(open + 1, close - open - 1);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_latest = tag;
    g_state = Newer(tag, g_current) ? State::Available : State::UpToDate;
    RT_LOGF(RT_TAG_RUNTIME, "update check: running %s, latest %s\n", g_current.c_str(), tag.c_str());
}

void Download() {
    std::string tag;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        tag = g_latest;
    }
    const fs::path engine = EngineDir();
    const fs::path root = engine.parent_path();  // the game folder: engine/ sits inside it
    if (engine.filename() != "engine") {
        g_state = State::Failed;  // not laid out like the download, so there is nothing safe to copy over
        return;
    }
    const fs::path work = root / "_update";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work / "files", ec);
    const fs::path archive = work / kAsset;
    bool ok = RunHidden("curl -sfL -m 1800 -o " + Quote(archive) + " " + kDownload + tag + "/" + kAsset) == 0;
#if defined(_WIN32)
    ok = ok && RunHidden("tar -xf " + Quote(archive) + " -C " + Quote(work / "files")) == 0;  // Windows' bsdtar reads zips
#else
    ok = ok && RunHidden("tar -xzf " + Quote(archive) + " -C " + Quote(work / "files")) == 0;
#endif
    // The archive holds one folder (NewSuperPaperBros/) with the launcher and engine/ in it.
    fs::path unpacked;
    for (const auto& entry : fs::directory_iterator(work / "files", ec))
        if (entry.is_directory() && fs::exists(entry.path() / "engine")) unpacked = entry.path();
    if (!ok || unpacked.empty()) {
        RT_LOGF(RT_TAG_RUNTIME, "update download failed\n");
        fs::remove_all(work, ec);
        g_state = State::Failed;
        return;
    }
#if defined(_WIN32)
    const fs::path script = work / "update.cmd";
    const std::string pid = std::to_string(GetCurrentProcessId());
    std::ofstream out(script);
    out << "@echo off\r\n"
        << ":wait\r\n"
        << "tasklist /FI \"PID eq " << pid << "\" | find \"" << pid << "\" >nul && (timeout /t 1 /nobreak >nul & goto wait)\r\n"
        << "robocopy " << Quote(unpacked) << " " << Quote(root) << " /E /R:30 /W:1 >nul\r\n"
        << "start \"\" " << Quote(root / "New Super Paper Bros.exe") << "\r\n"
        << "cd /d " << Quote(root) << "\r\n"
        << "rmdir /s /q " << Quote(work) << "\r\n";
    out.close();
    std::wstring cmd = L"cmd /c \"" + script.wstring() + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        g_state = State::Failed;
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    const fs::path script = work / "update.sh";
    std::ofstream out(script);
    out << "#!/bin/sh\n"
        << "while kill -0 " << getpid() << " 2>/dev/null; do sleep 1; done\n"
        << "cp -a " << Quote(unpacked) << "/. " << Quote(root) << "/\n"
        << "cd " << Quote(root) << " && rm -rf " << Quote(work) << "\n"
        << "exec " << Quote(root / "New Super Paper Bros") << "\n";
    out.close();
    if (std::system(("sh " + Quote(script) + " >/dev/null 2>&1 &").c_str()) != 0) {
        g_state = State::Failed;
        return;
    }
#endif
    RT_LOGF(RT_TAG_RUNTIME, "update %s downloaded; quitting so it can be copied in\n", tag.c_str());
    ExitForAuroraWindowClose();
}
} // namespace

void Start() {
    std::call_once(g_started, [] {
        std::ifstream in(EngineDir() / "version.txt");
        std::getline(in, g_current);
        while (!g_current.empty() && (g_current.back() == '\r' || g_current.back() == ' ')) g_current.pop_back();
        if (g_current.empty() || !CheckEnabled()) return;  // a development build, or checks turned off
        g_state = State::Checking;
        std::thread(Check).detach();
    });
}

State GetState() { return g_state.load(); }

std::string Current() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_current;
}

std::string Latest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_latest;
}

void Install() {
    State expected = State::Available;
    if (!g_state.compare_exchange_strong(expected, State::Downloading)) {
        expected = State::Failed;  // a failed download or check: try again
        if (!g_state.compare_exchange_strong(expected, State::Checking)) return;
        std::thread(Check).detach();
        return;
    }
    std::thread(Download).detach();
}

bool CheckEnabled() { return RuntimeConfigFile::CheckUpdates(true); }

void SetCheckEnabled(bool on) {
    RuntimeConfigFile::SetCheckUpdates(on);
    if (on && g_state == State::Off && !Current().empty()) {
        g_state = State::Checking;
        std::thread(Check).detach();
    }
}
} // namespace Updater
