/* Stub for GameOverlayRenderer64.dll -- 64-bit Steam overlay for Wine/Apple Silicon.
 * steam_api64.dll loads this via full-path LoadLibraryA; it hooks DXGI/D3D swap chains
 * and crashes DXVK on Wine. This no-op stub exports the same 13 symbols so Wine's
 * builtin search intercepts the load before the real DLL is reached.
 * Use: GameOverlayRenderer64.dll=b in WINEDLLOVERRIDES.
 */

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    return TRUE;
}

/* Returns whether the overlay needs a present call -- always false, we do nothing. */
__declspec(dllexport) BOOL BOverlayNeedsPresent(void) { return FALSE; }

__declspec(dllexport) BOOL IsOverlayEnabled(void) { return FALSE; }

/* D3D3 hook entry -- no-op, returns 0 (not hooked). */
__declspec(dllexport) BOOL OverlayHookD3D3(void *pDevice) { return FALSE; }

__declspec(dllexport) void SetNotificationInset(int x, int y) {}

__declspec(dllexport) void SetNotificationPosition(int position) {}

__declspec(dllexport) BOOL SteamOverlayIsUsingGamepad(void) { return FALSE; }

__declspec(dllexport) BOOL SteamOverlayIsUsingKeyboard(void) { return FALSE; }

__declspec(dllexport) BOOL SteamOverlayIsUsingMouse(void) { return FALSE; }

__declspec(dllexport) void ValveHookScreenshots(void *pDevice) {}

__declspec(dllexport) BOOL ValveIsScreenshotsHooked(void) { return FALSE; }

/* Vulkan overlay hooks -- no-ops, DXVK handles presentation itself. */
__declspec(dllexport) void VulkanSteamOverlayPresent(void) {}

__declspec(dllexport) void VulkanSteamOverlayProcessCapturedFrame(void) {}

__declspec(dllexport) void VulkanSteamOverlaySetWindowType(int type) {}
