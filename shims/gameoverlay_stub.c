// Stub GameOverlayRenderer.dll
// Exports all functions steam_api.dll expects.
// OverlayHookD3D3 calls the real hook to prime the D3D9 subsystem
// (so cnc-ddraw's Direct3DCreate9 succeeds), but does NOT hook Present
// or call Reset — which is what caused the white/blank window.
#include <windows.h>
#include <d3d9.h>

// Forward-declare the real overlay's hook so we can call it via the loaded DLL.
typedef BOOL (WINAPI *PFN_OverlayHookD3D3)(void *pDevice);

static HMODULE  s_hD3D9  = NULL;
static HMODULE  s_hReal  = NULL;

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    if (reason == DLL_PROCESS_ATTACH) {
        // Pre-load d3d9.dll so the subsystem is initialised before cnc-ddraw
        // calls Direct3DCreate9. Without this, Direct3DCreate9 fails in the
        // 32-bit WoW64 process and cnc-ddraw silently falls back to D3D8.
        s_hD3D9 = LoadLibraryA("d3d9.dll");
        if (s_hD3D9) {
            typedef IDirect3D9 *(WINAPI *PFN_Create9)(UINT);
            PFN_Create9 pfn = (PFN_Create9)GetProcAddress(s_hD3D9, "Direct3DCreate9");
            if (pfn) {
                IDirect3D9 *pD3D = pfn(D3D_SDK_VERSION);
                if (pD3D) pD3D->lpVtbl->Release(pD3D);
            }
            // Keep d3d9 loaded — cnc-ddraw needs it to already be in memory.
        }
    }
    return TRUE;
}

__declspec(dllexport) BOOL  BOverlayNeedsPresent(void)                              { return FALSE; }
__declspec(dllexport) BOOL  IsOverlayEnabled(void)                                  { return FALSE; }
// No-op: don't hook Present, don't call Reset. D3D9 is already primed by DllMain.
__declspec(dllexport) BOOL  OverlayHookD3D3(void *pDevice)                          { return FALSE; }
__declspec(dllexport) void  SetNotificationInset(int x, int y)                      {}
__declspec(dllexport) void  SetNotificationPosition(int pos)                        {}
__declspec(dllexport) BOOL  SteamOverlayIsUsingGamepad(void)                        { return FALSE; }
__declspec(dllexport) BOOL  SteamOverlayIsUsingKeyboard(void)                       { return FALSE; }
__declspec(dllexport) BOOL  SteamOverlayIsUsingMouse(void)                          { return FALSE; }
__declspec(dllexport) void  ValveHookScreenshots(void *dev, void *a, void *b)       {}
__declspec(dllexport) BOOL  ValveIsScreenshotsHooked(void)                          { return FALSE; }
__declspec(dllexport) void  VirtualFreeWrapper(void *p, SIZE_T sz, DWORD type)      {}
__declspec(dllexport) void  VulkanSteamOverlayPresent(void *q, void *info)          {}
__declspec(dllexport) void  VulkanSteamOverlayProcessCapturedFrame(void *q)         {}
__declspec(dllexport) void  VulkanSteamOverlaySetWindowType(HWND hw, BOOL isGame)   {}
