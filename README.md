# Steam-Win-Silicon

> Started because my wife wanted to play Plants vs. Zombies on her Mac and I refused to accept "it doesn't run on Apple Silicon" as a valid answer.

Run Windows Steam games on Apple Silicon (M1/M2/M3) Macs via Wine/GPTK -- patches and shims that fix the common rendering failures.

> **Keywords:** steam on mac m1, windows games on apple silicon, wine macos apple silicon, game porting toolkit games, play steam games mac m1, gptk wine moltenvk fix, windows games m1 mac, steam windows mac silicon

Tested on: M1 MacBook, macOS 15, Wine 11.10 (GPTK), Steam build ~1782257239.

---

## Tested / Playable Games

Games confirmed working on M1 Mac with this setup. All tested via [Aether](https://github.com/wisnuub/Aether).

| Game | App ID | Engine | Renderer | Required Fixes | Notes |
|------|--------|--------|----------|---------------|-------|
| [Taskbar Hero](https://store.steampowered.com/app/3678970) | 3678970 | Unity 6 | MoltenVK wrapper | moltenvk_wrap.c v13, gameoverlay64_stub, DirectComposition enabled | Transparent taskbar overlay idle game; DComp required for transparency |
| [Plants vs. Zombies GOTY](https://store.steampowered.com/app/3590) | 3590 | PopCap | cnc-ddraw + D3D9 | gameoverlay_stub, cnc-ddraw (renderer=opengl) | Classic 2D DDraw game; no d3d11 needed |

The easiest way to run these is **[Aether](https://github.com/wisnuub/Aether)** -- a macOS launcher that applies all the fixes automatically per-game.

---

## The Problem

Running Windows Steam through Wine on Apple Silicon hits several rendering bugs:

- **Steam UI black screen**: Steam launches but the store/library shows nothing
- **CEF/WebView black screen**: The browser-based Steam UI never renders
- **DirectX 11 failures**: Games using D3D11 fail to create a device
- **DirectDraw games blank window**: Old games using ddraw get a white/black empty window

These are caused by three separate bugs in the Wine + MoltenVK + ANGLE stack on Apple Silicon.

---

## Fixes

### 1. Steam UI Black Screen + Menu/Icon Flicker (`shims/steamwebhelper_shim.c`)

**Root cause (black screen):** Wine 11.x uses cross-process `client_surface_present` IPC to deliver rendered frames from the GPU subprocess to the browser process's NSWindow. This silently fails on Apple Silicon, frames are rendered but never displayed.

**Root cause (flicker):** Steam's CEF spawns separate renderer/GPU/utility OS processes, each owning native windows that Wine's macOS driver has to composite together. That cross-process window handoff is where a hide/show race lives: macOS window-server occlusion notifications get translated back into `WM_SHOWWINDOW` messages that Steam's own UI reacts to by re-hiding, producing rapid visible flicker in menus, popups, and icons. Confirmed via Steam's own `webhelper.txt` log, showing a context menu toggling `WasHidden 0/1/0/1` within the same second. This is a window-management race, not a GPU rendering bug, so fixes at the MoltenVK/Vulkan layer (shadow buffers, feature masking, etc.) don't touch it.

**Fix:** Replace `steamwebhelper.exe` with a shim that injects CEF flags via the `AETHER_CEF_FLAGS` environment variable (falls back to a built-in default if unset). Two flag sets, pick based on what you're fixing:
- `--disable-gpu --single-process` (default): folds CEF's renderer/GPU/utility processes into one, eliminating the cross-process window state that causes the flicker. Steam's own UI renders via CPU instead of GPU (a bit slower to redraw, not a big deal for a store/library UI). Games are unaffected, they launch in their own separate Wine process. Credit to [notpop/steam-on-m1-wine](https://github.com/notpop/steam-on-m1-wine) for this combination.
- `--use-angle=vulkan --ignore-gpu-blocklist --in-process-gpu`: the original black-screen-only fix. Keeps Steam's UI GPU-accelerated but does not address the flicker.

**Build:**
```bash
x86_64-w64-mingw32-gcc -O2 -mwindows -o steamwebhelper.exe shims/steamwebhelper_shim.c
```

**Install:**
```bash
STEAM="$HOME/Library/Application Support/<your-bottle>/drive_c/Program Files (x86)/Steam"
cp "$STEAM/bin/cef/cef.win64/steamwebhelper.exe" "$STEAM/bin/cef/cef.win64/steamwebhelper_real.exe"
cp steamwebhelper.exe "$STEAM/bin/cef/cef.win64/steamwebhelper.exe"
# Lock it so Steam can't overwrite it
chflags uchg "$STEAM/bin/cef/cef.win64/steamwebhelper.exe"
```

Also add `-noverifyfiles` to your Steam launch args to stop the bootstrapper from reverting the shim.

Override the flags at launch time if needed:
```bash
AETHER_CEF_FLAGS="--use-angle=vulkan --ignore-gpu-blocklist --in-process-gpu" wine Steam.exe ...
```

---

### 2. MoltenVK BDA + Shadow Buffer (`shims/moltenvk_wrap.c`)

**Root cause:**
- MoltenVK exposes `VK_KHR_buffer_device_address` but it does not work correctly on Apple Silicon. ANGLE's VMA allocator enables BDA mode and gets `VK_ERROR_FEATURE_NOT_PRESENT (-8)`.
- CAMetalLayer drawables rotate with undefined content. Chrome/CEF's compositor assumes the previous frame is still in the swapchain image for partial redraws (damage tracking). It is not, so unredrawing regions go black.

**Fix:** A `dylib` wrapper that sits in front of `libMoltenVK.dylib` and:
- Strips BDA extension advertisements and feature bits
- Maintains a shadow image **per swap-image-index** (not per swapchain), copies the presented image into its own shadow slot, then restores that same index's shadow into it before returning to ANGLE. A swapchain double/triple-buffers across N independent images; sharing one shadow across all of them restores the wrong generation of content into 2-out-of-3 (or 1-out-of-2) acquisitions -- this was misdiagnosed as flicker before the real cause (see Fix 1) was found, but is a real correctness bug in its own right and worth keeping.
- Tracks up to 64 concurrent swapchains (Steam's CEF UI can have 30+ live surfaces: main window, popups, toasts, tooltips) and evicts least-recently-used on overflow instead of always evicting slot 0, so an active window's shadow state doesn't get silently clobbered

**Build:**
```bash
MVKLIB="$HOME/Library/Application Support/<your-bottle>/../Wine/Contents/Resources/wine/lib"
clang -arch x86_64 -dynamiclib -O2 \
  -install_name "@rpath/libMoltenVK.dylib" \
  -rpath "@loader_path" \
  -Wl,-reexport_library,"$MVKLIB/libMoltenVK_real.dylib" \
  -o libMoltenVK.dylib shims/moltenvk_wrap.c \
  -framework Foundation -ldl
```

**Install:**
```bash
# Back up real MoltenVK and patch its install name
cp "$MVKLIB/libMoltenVK.dylib" "$MVKLIB/libMoltenVK_real.dylib"
install_name_tool -id "@rpath/libMoltenVK_real.dylib" "$MVKLIB/libMoltenVK_real.dylib"
cp libMoltenVK.dylib "$MVKLIB/libMoltenVK.dylib"
```

---

### 3. DirectDraw Game Overlay Stub (`shims/gameoverlay_stub.c`)

**Root cause:** Old DirectDraw games (e.g. Plants vs Zombies) use `cnc-ddraw` as a ddraw-to-D3D9 bridge. Steam's `steam_api.dll` loads `GameOverlayRenderer.dll` via hardcoded full path and hooks `IDirect3DDevice9::Present`, then calls `Reset()`, which destroys cnc-ddraw's D3D9 surfaces, leaving a white or black window.

**Fix:** Compile a stub `GameOverlayRenderer.dll` that exports all 14 expected functions as no-ops and replace Steam's copy directly. Wine's `=b` builtin override does not work for MinGW-compiled DLLs (Wine 11+ rejects them as "not a builtin"), so replacing the file at the path Steam loads is the only reliable approach.

**Build:**
```bash
i686-w64-mingw32-gcc -O2 -shared -o GameOverlayRenderer.dll shims/gameoverlay_stub.c \
  -ld3d9 -lgdi32 -luser32
```

**Install:**
```bash
STEAM="$HOME/Library/Application Support/<your-bottle>/drive_c/Program Files (x86)/Steam"
cp "$STEAM/GameOverlayRenderer.dll" "$STEAM/GameOverlayRenderer_real.dll"  # backup
cp GameOverlayRenderer.dll "$STEAM/GameOverlayRenderer.dll"
chflags uchg "$STEAM/GameOverlayRenderer.dll"  # prevent Steam from reverting it
```

Add `-noverifyfiles` to your Steam launch args to prevent Steam from reverting it on start.

For the DirectDraw game itself, configure `cnc-ddraw`'s `ddraw.ini`:
```ini
[ddraw]
renderer=opengl
windowed=true
nonexclusive=true
width=800
height=600
maxfps=60
```

---

### 4. D3D11/DXVK Game Overlay Stub (`shims/gameoverlay64_stub.c`)

**Root cause:** 64-bit games using DXVK (e.g. Unity 6 games with bundled DXVK) crash with a page fault in `dxgi.dll`. Steam's `steam_api64.dll` loads `GameOverlayRenderer64.dll` via hardcoded full path and hooks `IDXGISwapChain::Present`. On Wine + DXVK, this hook corrupts the swap chain, causing an immediate crash: `page fault on read access to 0x0000000000000020` in dxgi.

Setting `GameOverlayRenderer64.dll=d` in WINEDLLOVERRIDES does not work because `=d` does not intercept full-path `LoadLibraryA` calls. The `=b` flag forces Wine's builtin search first, which does intercept it.

**Fix:** Compile a 64-bit stub `GameOverlayRenderer64.dll` that exports all 13 expected functions as no-ops and replace Steam's copy directly (same reason as the 32-bit stub -- `=b` doesn't work for MinGW DLLs).

**Build:**
```bash
x86_64-w64-mingw32-gcc -O2 -shared -o GameOverlayRenderer64.dll shims/gameoverlay64_stub.c
```

**Install:**
```bash
STEAM="$HOME/Library/Application Support/<your-bottle>/drive_c/Program Files (x86)/Steam"
cp "$STEAM/GameOverlayRenderer64.dll" "$STEAM/GameOverlayRenderer64_real.dll"  # backup
cp GameOverlayRenderer64.dll "$STEAM/GameOverlayRenderer64.dll"
chflags uchg "$STEAM/GameOverlayRenderer64.dll"
```

Note: if the game ships its own DXVK (common for Unity games), also set `dxgi=n,b;d3d11=n,b;d3d10core=n,b` so Wine loads the game's bundled DXVK instead of wined3d. If the bundled DXVK is old and doesn't support `D3D_FEATURE_LEVEL_11_1`, replace it with DXVK 3.0 (download `dxvk-X.X.tar.gz` from the DXVK GitHub releases and copy `x64/d3d11.dll`, `x64/dxgi.dll`, `x64/d3d10core.dll` into the game directory).

---

## Required Wine Environment

These env vars are needed for Steam + games to work:

```bash
WINEDLLOVERRIDES="steamservice=d;winemenubuilder.exe=d;dxgi=b;d3d11=b;d3d10core=b;dcomp=n"
DYLD_FALLBACK_LIBRARY_PATH="<wine-lib-dir>"   # routes libMoltenVK.dylib through the wrapper
MVK_CONFIG_LOG_LEVEL=3
WINEESYNC=1
```

Steam launch args:
```
Steam.exe -no-cef-sandbox -forcedesktopscaling 1 -noverifyfiles
```

---

## Compatibility Notes

| Game type | Renderer | Notes |
|---|---|---|
| DirectDraw (old 2D games) | cnc-ddraw + OpenGL | Use gameoverlay_stub + `ddraw=n,b;GameOverlayRenderer.dll=b` in WINEDLLOVERRIDES |
| Unity 6 / D3D11 (64-bit) | DXVK 3.0 | Replace bundled DXVK if old; use gameoverlay64_stub + `GameOverlayRenderer64.dll=b;dxgi=n,b;d3d11=n,b` |
| D3D9 games | Auto (wined3d) | Generally works |
| D3D11/D3D12 games (other) | DXVK if bundled | Use `d3d11=n,b;d3d10core=n,b` in WINEDLLOVERRIDES to load the game's own DXVK |

---

## Related

- [Aether](https://github.com/wisnuub/Aether) - macOS launcher app that automates all of the above for Steam PC gaming on Apple Silicon
- [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) - DirectDraw wrapper used for old 2D games
- [GPTK (Game Porting Toolkit)](https://developer.apple.com/games/) - Apple's Wine distribution with Metal D3D translation
- [notpop/steam-on-m1-wine](https://github.com/notpop/steam-on-m1-wine) - independent Steam-on-Apple-Silicon project; source of the `--disable-gpu --single-process` CEF flag combination used in Fix 1
- [MelonForAll/vineport](https://github.com/MelonForAll/vineport) - another Wine + GPTK launcher for Steam/Epic on Apple Silicon
