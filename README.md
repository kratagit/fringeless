# Fringeless

Fringeless removes the colour fringes that ClearType text shows on QD-OLED monitors.

ClearType assumes the red, green and blue subpixels of a pixel sit side by side in a
row. QD-OLED panels arrange them as a triangle (green on top, red and blue below), so
the colour ClearType adds to glyph edges lands in the wrong place and text gets pink
and green edges. Fringeless shifts each colour channel of the whole screen by a
fraction of a pixel so the channels line up with the physical subpixels again.

![Fringeless icon](src/Fringeless.ico)

## How it works

- The selected display is captured with the Desktop Duplication API.
- A pixel shader resamples each channel at its own sub-pixel offset (bilinear or
  Catmull-Rom), then applies a small Laplacian that restores the contrast the
  interpolation would otherwise soften.
- The result is shown in a click-through, always-on-top window that covers the
  display and is excluded from screen capture, so the capture never sees it. The
  window has its own thread that only handles window messages, so it stays responsive
  while rendering waits on the GPU; if rendering stalls for more than 1.5 s, the overlay
  is hidden until it recovers, so a frozen image never covers the screen.
- Correction pauses automatically while a fullscreen game or video player is in
  front; browsers, editors and terminals in full screen keep it.

### Staying light on the GPU

- Only the rectangles the desktop reports as changed are copied and re-shaded (grown
  by the 4 pixels a corrected pixel reads from its neighbourhood). The corrected
  image is kept between frames.
- Desktop Duplication also reports frames in which nothing visible changed, most
  notably the recomposition caused by the overlay's own presentation. Each changed
  rectangle is compared with the previous image on the GPU, and only rectangles that
  really differ are redrawn.
- The shift is the same for every pixel, so filter offsets and weights are computed
  once per frame on the CPU ([`src/shift_constants.h`](src/shift_constants.h)). Taps
  with zero weight are skipped: a shift along one axis needs only a 1-D filter.

GPU engine utilisation measured with [`tests/measure-gpu.ps1`](tests/measure-gpu.ps1)
on a 2560x1440 @ 280 Hz display (Radeon RX 7600 XT), bilinear, 10 s averages:

| Scenario                         | Fringeless | dwm.exe |
| -------------------------------- | ---------- | ------- |
| Idle desktop                     | 0.1 %      | 7 %     |
| Typing-sized updates (520x60)    | 5 %        | 13 %    |
| Scrolling text (1800x1000)       | 11 %       | 18 %    |

`dwm.exe` composites the full-screen overlay whenever it updates.

## Using it

Run `Fringeless.exe`. The main window has three cards:

- **Display**: the monitor to correct and the main on/off switch.
- **Channel shift**: per-channel X/Y offsets in pixels, tuned while looking at text.
  Drag a slider (hold Shift for fine control), scroll on it or use the arrow keys for
  0.001 px steps (Ctrl+scroll or Shift+arrow: 0.01 px), click a value to type it, or
  double-click a slider to reset it. Below them, bilinear or bicubic interpolation.
- **Behaviour**: automatic pause for fullscreen apps.

The gear button opens **Settings**:

- **Shortcut**: the global on/off shortcut, Win+Shift+F by default (Win+Ctrl+Alt+F
  if another app owns that). Click it and press a new combination; Backspace removes
  it. Ctrl+Alt with a character key is refused because it works like AltGr and would
  block typing.
- **Startup**: start with Windows, in the tray.
- **Settings file**: where settings are stored, `%APPDATA%\Fringeless\settings.ini`.
  Slow or stalled rendering steps are recorded in `diagnostics.log` in the same folder.

Closing the window keeps Fringeless running in the tray; click the tray icon to bring
it back, right-click for the menu.
Requirements: Windows 10 or 11, a Direct3D 11 GPU, an SDR desktop (8 or 10 bits per
channel; turn HDR off for the corrected display).

## Building

Requires MinGW-w64, e.g. `winget install BrechtSanders.WinLibs.POSIX.UCRT`.

```powershell
.\build.ps1
```

Produces `build\Fringeless.exe`, statically linked. The shader is embedded as source
and compiled at startup with the system's `d3dcompiler_47.dll`.

Every push is built by GitHub Actions ([`.github/workflows/build.yml`](.github/workflows/build.yml));
the executable is attached to the run as the `Fringeless` artifact, and pushing a
`v*` tag publishes it as a release.

## Tests

```powershell
.\tests\run-tests.ps1                                   # shader, partial redraw, change detection
.\tests\measure-gpu.ps1 -Exe build\Fringeless.exe       # GPU load (opens a test window)
```

[`tests/verify.cpp`](tests/verify.cpp) checks the shader against a per-pixel CPU
implementation of the same filter (within 1/255 for rounding), that redrawing only the
changed area gives a bit-identical image, and that change detection counts exactly the
pixels that differ.

## Layout

| File                     | Contents                                              |
| ------------------------ | ----------------------------------------------------- |
| `src/main.cpp`           | Startup, tray icon, the on/off shortcut               |
| `src/panel.cpp`          | The window (Direct2D/DirectWrite, light/dark, Mica)   |
| `src/renderer.cpp`       | Capture, change tracking, correction, overlay         |
| `src/shader.hlsl`        | Correction and change-detection shaders               |
| `src/shift_constants.h`  | Per-frame filter constants, shared with the tests     |
| `src/settings.cpp`       | Settings file and start with Windows                  |
| `src/displays.cpp`       | Display enumeration and names                         |
