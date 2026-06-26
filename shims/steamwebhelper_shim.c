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
    WCHAR newCmd[32768];
    wcscpy(newCmd, L"\""); wcscat(newCmd, realExe);
    wcscat(newCmd, L"\" --use-angle=vulkan --ignore-gpu-blocklist --in-process-gpu");
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
