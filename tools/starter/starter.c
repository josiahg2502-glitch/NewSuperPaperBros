// The download's top-level "New Super Paper Bros.exe": starts the game in engine\ (nspb-engine.exe, which picks
// the disc region's module and, on first start, sets the game up from the player's disc image). Settings, mods
// and updates are all in the game's own Options menu, so there is no separate launcher.
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR args, int show) {
    (void)instance, (void)previous, (void)args, (void)show;
    wchar_t dir[MAX_PATH + 8], command[MAX_PATH + 64];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    while (n > 0 && dir[n - 1] != L'\\') --n;
    wcscpy(dir + n, L"engine");
    swprintf(command, sizeof command / sizeof command[0], L"\"%ls\\nspb-engine.exe\"", dir);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        MessageBoxW(NULL, L"The game files (the engine folder next to this program) are missing. Unzip the whole download again.",
                    L"New Super Paper Bros.", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
