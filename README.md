# Steam-Win-Silicon

Patches and shims to get Steam (Windows) running well on Apple Silicon (M1/M2/M3) via Wine/GPTK.

Tested on: M1 MacBook, macOS 15, Wine 11.10 (GPTK), Steam build ~1782257239.

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
| Unity (D3D11) | DXVK if bundled | Some Unity games ship their own DXVK; use `d3d11=n,b` in WINEDLLOVERRIDES |
| D3D9 games | Auto (wined3d) | Generally works |
| D3D11/D3D12 games | DXVK if bundled | Use `d3d11=n,b` in WINEDLLOVERRIDES to load the game's own DXVK |

---

## Related

- [Aether](https://github.com/wisnuub/Aether) - macOS launcher app that automates all of the above for Steam PC gaming on Apple Silicon
- [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) - DirectDraw wrapper used for old 2D games
- [GPTK (Game Porting Toolkit)](https://developer.apple.com/games/) - Apple's Wine distribution with Metal D3D translation
