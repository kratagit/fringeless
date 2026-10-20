# Fringeless

Fringeless removes the colour fringes that ClearType text shows on QD-OLED monitors.

ClearType assumes the red, green and blue subpixels of a pixel sit side by side in a row.
QD-OLED panels arrange them as a triangle (green on top, red and blue below), so the colour
ClearType adds to glyph edges lands in the wrong place and text gets pink and green edges.
Fringeless shifts each colour channel of the composited desktop by a fraction of a pixel so
the channels line up with the physical subpixels again.

![Fringeless icon](src/Fringeless.ico)

## How it works

The correction runs **inside the Desktop Window Manager** (`dwm.exe`), the Windows component
that composites every window into the final image sent to the display:

- A small engine DLL is injected into `dwm.exe` and hooks `COverlayContext::Present`, the
  point where the composited frame is ready. A pixel shader resamples each colour channel at
  its own sub-pixel offset, cancelling the fringe, and the shaded frame is what reaches the
  screen. Hardware overlays / MPO / DirectFlip are disabled while correction is active so the
  frame always goes through this path.
- Because the correction happens at composition time, it covers **every** window, including
  ones that opt out of screen capture (password managers, messengers with a screen-security
  option) — the app never captures or reads the screen.
- Full-screen games and video that bypass DWM composition are not corrected, so correction
  effectively pauses for them.
- Only the rectangles DWM recomposed each frame are reshaded, so a static desktop costs
  almost nothing on the GPU.

The approach, the `COverlayContext::Present` hook and the MPO/DirectFlip handling come from
**dwm_lut** (see [Credits and licence](#credits-and-licence)); Fringeless replaces its 3D
colour LUT with the per-channel sub-pixel shift and feeds the shift from live settings.

## Using it

Fringeless is a single `Fringeless.exe` — no installer and no extra files. Run it and:

- **Turn correction on** from the main switch, the tray icon or the global shortcut
  (Win+Shift+F by default). The correction runs inside `dwm.exe`, which needs administrator
  rights, so the **first time you turn it on after signing in Windows shows one UAC prompt**.
  After that, toggling it and moving the sliders take effect immediately, and closing the
  window leaves the correction running in the tray.
- **Pick the display** to correct with the monitor selector; only that monitor is touched, so
  a second screen that does not need the shift is left alone.
- **Channel shift**: per-channel X/Y offsets in pixels, tuned while looking at text. Drag a
  slider (hold Shift for fine control), scroll or use the arrow keys for 0.001 px steps, click
  a value to type it, or double-click a slider to reset it.
- **Test pattern** (button on the main view, or the tray menu): real GDI/ClearType text in
  many fonts and sizes with its own Correction switch, for judging the fringe on and off.

Settings are stored in `%APPDATA%\Fringeless\settings.ini`.

Requirements: Windows 11 24H2 (build 26100) or 25H2 (build 26200) — the engine matches those
DWM versions and does nothing on others; a Direct3D 11 GPU; an SDR desktop (turn HDR off for
the corrected display).

## Building

Requires MinGW-w64, e.g. `winget install BrechtSanders.WinLibs.POSIX.UCRT`.

```powershell
.\build.ps1
```

Produces a single `build\Fringeless.exe`, statically linked. The DWM engine is built first as
a DLL and embedded in the executable as a resource; at runtime the app extracts it to a
temporary folder and injects it, re-launching itself elevated, when correction is turned on.

Every push is built by GitHub Actions
([`.github/workflows/build.yml`](.github/workflows/build.yml)); `Fringeless.exe` is attached to
the run, and pushing a `v*` tag publishes it as a release.

## Layout

| Path                      | Contents                                                     |
| ------------------------- | ------------------------------------------------------------ |
| `src/main.cpp`            | Startup, tray icon, the on/off shortcut                      |
| `src/panel.cpp`           | The window (Direct2D/DirectWrite, light/dark, Mica)          |
| `src/dwm_engine.cpp`      | Controls the engine and publishes live parameters            |
| `src/inject.cpp`          | The elevated `--inject`/`--eject` mode built into the exe     |
| `src/testpattern.cpp`     | The built-in test pattern window                             |
| `src/settings.cpp`        | Settings file and start with Windows                         |
| `src/displays.cpp`        | Display enumeration and names                                 |
| `src/dwm/`                | The engine DLL embedded in the exe (GPL-3.0; from dwm_lut)    |
| `third_party/minhook/`    | MinHook, the function-hooking library (BSD-2-Clause)          |

## Credits and licence

Fringeless is licensed under the **GNU General Public License v3.0** (see
[`LICENSE`](LICENSE)), because its correction engine is derived from **dwm_lut**:

- [lauralex/dwm_lut](https://github.com/lauralex/dwm_lut) and
  [ledoge/dwm_lut](https://github.com/ledoge/dwm_lut) — the original DWM LUT injector.
- [0x401gg/dwm_lut-windows-25h2](https://github.com/0x401gg/dwm_lut-windows-25h2) — the
  Windows 11 24H2 / 25H2 compatibility fork the engine's signatures are based on.

`src/dwm/dllmain.cpp` keeps dwm_lut's injection and `COverlayContext::Present` hook and
replaces the 3D colour LUT with the per-channel sub-pixel shift; the file header lists the
changes. It bundles [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu
(BSD-2-Clause, see [`third_party/minhook/LICENSE.txt`](third_party/minhook/LICENSE.txt)).
