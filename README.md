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

### 1. Steam UI Black Screen (`shims/steamwebhelper_shim.c`)

**Root cause:** Wine 11.x uses cross-process `client_surface_present` IPC to deliver rendered frames from the GPU subprocess to the browser process's NSWindow. This silently fails on Apple Silicon, frames are rendered but never displayed.

**Fix:** Replace `steamwebhelper.exe` with a shim that injects `--in-process-gpu` into the CEF command line. This eliminates the cross-process GPU path entirely so Metal composites directly to the NSWindow.

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

---

### 2. MoltenVK BDA + Shadow Buffer (`shims/moltenvk_wrap.c`)

**Root cause:**
- MoltenVK exposes `VK_KHR_buffer_device_address` but it does not work correctly on Apple Silicon. ANGLE's VMA allocator enables BDA mode and gets `VK_ERROR_FEATURE_NOT_PRESENT (-8)`.
- CAMetalLayer drawables rotate with undefined content. Chrome/CEF's compositor assumes the previous frame is still in the swapchain image for partial redraws (damage tracking). It is not, so unredrawing regions go black.

**Fix:** A `dylib` wrapper that sits in front of `libMoltenVK.dylib` and:
- Strips BDA extension advertisements and feature bits
- Maintains a per-swapchain shadow image, copies the presented frame into it, then restores it into the next acquired image before returning to ANGLE

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

**Fix:** Compile a stub `GameOverlayRenderer.dll` that exports all 14 expected functions as no-ops. Place it in Wine's i386-windows builtin directory. Set `GameOverlayRenderer.dll=b` in `WINEDLLOVERRIDES` and Wine's builtin search intercepts even the full-path `LoadLibraryA` call.

**Build:**
```bash
i686-w64-mingw32-gcc -O2 -shared -o GameOverlayRenderer.dll shims/gameoverlay_stub.c \
  -ld3d9 -lgdi32 -luser32
```

**Install:**
```bash
WINE_I386="$HOME/Library/Application Support/<your-bottle>/../Wine/Contents/Resources/wine/lib/wine/i386-windows"
cp GameOverlayRenderer.dll "$WINE_I386/GameOverlayRenderer.dll"
```

Then add to `WINEDLLOVERRIDES`:
```
GameOverlayRenderer.dll=b
```

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

**Fix:** Compile a 64-bit stub `GameOverlayRenderer64.dll` that exports all 13 expected functions as no-ops. Place it in Wine's x86_64-windows builtin directory and set `GameOverlayRenderer64.dll=b` in WINEDLLOVERRIDES.

**Build:**
```bash
x86_64-w64-mingw32-gcc -O2 -shared -o GameOverlayRenderer64.dll shims/gameoverlay64_stub.c
```

**Install:**
```bash
WINE_X64="$HOME/Library/Application Support/<your-bottle>/../Wine/Contents/Resources/wine/lib/wine/x86_64-windows"
cp GameOverlayRenderer64.dll "$WINE_X64/GameOverlayRenderer64.dll"
```

Then add to `WINEDLLOVERRIDES`:
```
GameOverlayRenderer64.dll=b
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
