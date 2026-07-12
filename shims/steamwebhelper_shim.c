#include <windows.h>
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    WCHAR myPath[MAX_PATH];
    GetModuleFileNameW(NULL, myPath, MAX_PATH);
    WCHAR *lastSlash = wcsrchr(myPath, L'\\');
    if (!lastSlash) return 1;
    *(lastSlash + 1) = L'\0';
    WCHAR realExe[MAX_PATH];
    wcscpy(realExe, myPath);
    wcscat(realExe, L"steamwebhelper_real.exe");
    LPWSTR origCmd = GetCommandLineW();
    LPWSTR args = origCmd;
    if (*args == L'"') { args++; while (*args && *args != L'"') args++; if (*args == L'"') args++; }
    else { while (*args && *args != L' ') args++; }
    /* Default: --disable-gpu --single-process. Steam's CEF normally spawns
     * separate renderer/GPU/utility OS processes, each owning native windows
     * that Wine's macOS driver has to composite together -- that cross-process
     * window handoff is where a hide/show race lives (macOS window-server
     * occlusion notifications get translated back into WM_SHOWWINDOW messages
     * that Steam's own UI reacts to by re-hiding, producing rapid visible
     * flicker in menus/popups). --single-process removes the cross-process
     * window state entirely; --disable-gpu avoids Steam's own GPU compositor
     * thread (which runs inline under single-process) fighting the Vulkan/
     * Metal path used by actual games. Trades Steam's own UI smoothness
     * (now CPU-rendered) for eliminating the flicker; games are unaffected,
     * they launch in their own separate Wine process.
     * Override via AETHER_CEF_FLAGS for other configurations (e.g. GPTK,
     * which has no Vulkan driver and needs ANGLE on D3D11 instead). */
    WCHAR flags[4096];
    DWORD flagLen = GetEnvironmentVariableW(L"AETHER_CEF_FLAGS", flags, 4096);
    WCHAR newCmd[32768];
    wcscpy(newCmd, L"\""); wcscat(newCmd, realExe);
    wcscat(newCmd, L"\" ");
    if (flagLen > 0 && flagLen < 4096)
        wcscat(newCmd, flags);
    else
        wcscat(newCmd, L"--disable-gpu --single-process");
    if (*args) wcscat(newCmd, args);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(realExe, newCmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)exitCode;
}
