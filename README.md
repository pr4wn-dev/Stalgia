# Stalgia

Lightweight animated desktop wallpapers for Windows 10 and 11. One portable 64-bit exe draws behind your icons with Direct3D 11: Matrix rain, Code Ape, Slow Myco, Stars, Aurora, and Lava, plus a desktop widget and a gallery.

![Stars](docs/stars.png)

![Aurora](docs/aurora.png)

![Lava lamp](docs/lava.png)

Stills above are from the CPU reference renderer (`tools/render_sky.py` and `tools/render_lava_preview.py`), not screenshots of a desktop.

## Features

- **Matrix** — falling katakana, instanced on the GPU.
- **Code Ape** — the same rain, brighter where a knuckle-walking silverback sits. The silhouette is a baked luminance mask, not a pasted photo.
- **Slow Myco** — rain that reveals a mushroom turtle. Color mode tints the rain from a baked palette (cap, spots, stem, shell, legs).
- **Stars** — a dark sky, crisp stars, and a Milky Way. Still night, Milky Way, and Nebula stay at 30 fps. Warp follows the global cap.
- **Aurora** — one folded ribbon with a green hem and magenta rays, and an optional ridgeline with a reflection.
- **Lava** — opaque wax inside the lamp. Blobs pinch off the bottom pool and drift up. Default is 30 fps.
- **Widget** — a frameless card on the desktop: live preview, wallpaper picker, play/pause, FPS, hide-icons, presets, and a System strip (CPU, RAM, GPU, disk, network). It collapses to a small pill. It is a top-level window, not a child of Explorer.
- **Gallery** — groups, live cards, favorites, and the same knobs as the widget.
- **Tray and control panel** — show or hide the wallpaper, pause, pick an effect, open the config, or exit. Closing the panel leaves the wallpaper running.
- Frame rate is capped (default 30). Fullscreen, a covered desktop, and battery saver can pause rendering. Start with Windows is off until you turn it on.

Every wallpaper also has brightness, saturation, vignette, an FPS cap, a night palette, and optional mouse parallax.

## Install

Download `Stalgia-<version>.exe` from [GitHub Releases](https://github.com/pr4wn-dev/Stalgia/releases). No installer. Nothing is written to `HKCU\...\Run` until you opt in.

1. Double-click the exe. The widget appears over the wallpaper, and a tray icon appears near the clock.
2. Matrix rain should fill the desktop behind the icons. Switch wallpapers from the widget, the tray, or `Stalgia.exe --panel`.

Optional per-user copy (still does not enable autostart):

```text
Stalgia.exe --install
```

That copies the exe to `%LOCALAPPDATA%\Programs\Stalgia\Stalgia.exe`, writes a desktop shortcut, and launches the copy. `Stalgia.exe --uninstall` removes that copy, the Run key, and the shortcut, and restores desktop icons. `%APPDATA%\Stalgia\config.json` is left in place.

Preview in a window without touching the desktop:

```text
Stalgia.exe --window --wallpaper myco
```

### Unsigned builds

Release builds are unsigned. Windows SmartScreen may say **Windows protected your PC**. Choose **More info**, then **Run anyway**. If the file was downloaded from the internet, unblock it in **Properties**.

Windows 11 Smart App Control, when it is on and enforced, can block an unsigned exe with Code Integrity events such as 3077, 3033, and 3118 (`An Application Control policy has blocked this file`). That is a signing-policy block, not a fault in the wallpaper. Code signing via [SignPath](https://signpath.io/) is planned. The GitHub Actions artifact is named `Stalgia-unsigned` so a signing step can be inserted before the release is published.

## Command line

| Option | Meaning |
| --- | --- |
| `--window`, `-w` | Preview window. Does not attach to the desktop. |
| `--wallpaper NAME`, `--effect NAME` | Start with `matrix`, `starfield`, `aurora`, `lava`, `ape`, or `myco`. A running instance switches to that wallpaper. |
| `--panel` | Open the control panel. A running instance opens its own panel. |
| `--widget` | Show the desktop widget. |
| `--hide-widget` | Hide the desktop widget on a running instance. |
| `--install` | Copy to `%LOCALAPPDATA%\Programs\Stalgia`, add a desktop shortcut, and launch. Does not enable Start with Windows. |
| `--uninstall` | Quit a running instance, remove the Run key and shortcuts, restore icons, and delete the per-user install folder. |
| `--diag` | Log the Progman/WorkerW/DefView tree, monitors, and DPI, then exit. Read-only. |
| `--exit` | Tell a running instance to quit and restore the static wallpaper. |
| `--icons hide\|show` | Hide or show desktop icons, the same command as Explorer’s View menu. Restored on exit. |
| `--selftest` | Attach, check visibility, exercise the gallery and widget settings, and cycle presets. Writes `%APPDATA%\Stalgia\selftest.txt`. Exits nonzero on failure. |
| `--capture` | With `--selftest`, write a PNG of every preset to `%APPDATA%\Stalgia\capture`. |
| `--console` | Attach a console. |
| `--help`, `-h` | Short help. |

A second desktop instance is refused. Flags sent while Stalgia is already running are delivered to that instance. Preview mode can run beside the desktop wallpaper.

Logs: `%APPDATA%\Stalgia\stalgia.log` (rotated to `stalgia.log.1` at about 2 MB). A crash writes a minidump and the last log lines under `%APPDATA%\Stalgia\crash`.

## Config

On first launch Stalgia writes `%APPDATA%\Stalgia\config.json`. If `%APPDATA%\DesktopRain\config.json` exists and Stalgia’s does not, it is copied over (the app used to be named Desktop Rain). A `config.json` next to the exe is used instead, as a portable file.

See `config.example.json`. `version` is the JSON format (currently **2**). `effect` is one of the wallpaper ids. Colors are `#RRGGBB`. `tune.*` is a generation number per wallpaper: when a release changes that wallpaper’s defaults, an older saved block is reset. Custom glyphs, photo paths, and the parallax toggle are kept.

`showWidget`, `widgetMini`, `widgetX`, and `widgetY` place the widget. `hideDesktopIcons` uses the same Explorer command as **View → Show desktop icons**. Start with Windows is off on a new config.

## Build from source

Requirements: Windows 10 or 11, 64-bit, and a Direct3D 11 GPU. A CJK font is optional; Matrix falls back to Latin glyphs. There is no .NET runtime, no VC++ redistributable, and no Python at runtime.

The GitHub workflow (`.github/workflows/build.yml`) cross-compiles the same way.

### MinGW-w64

On Debian or Ubuntu, including a GitHub `ubuntu-latest` runner:

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  cmake make clang \
  gcc-mingw-w64-x86-64-win32 g++-mingw-w64-x86-64-win32 \
  mingw-w64-x86-64-dev binutils-mingw-w64-x86-64
cmake -S . -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw -j
```

The toolchain file selects the win32-thread compilers (`x86_64-w64-mingw32-g++-win32`). The link line is `-static -static-libgcc -static-libstdc++ -municode -mwindows`, so the result is one portable exe: `build-mingw/Stalgia.exe`. Clang compiles `src/host/SehGuard.c` because MinGW g++ has no `__try`.

### MSVC

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The exe is `build\Release\Stalgia.exe`. Release uses the static CRT (`/MT`).

## License

Stalgia is free software under the GNU General Public License v3.0 only. See [LICENSE](LICENSE). The version resource reports `LegalCopyright` as `GPL-3.0`.

No third-party source is vendored. The exe links only to Windows system libraries (Direct3D 11, DXGI, Direct2D, DirectWrite, DirectComposition, the shell, GDI, PDH, and the rest). Matrix glyphs are drawn with a font already installed on the PC. The app icon (`resources/app.ico`, `resources/stalgia-256.png`) is the project’s own art.

Code Ape and Slow Myco ship as generated mask tables in `src/effects/ApeMaskData.cpp` and `src/effects/MycoMaskData.cpp`. The reference photographs those masks were baked from are not included. `tools/gen_ape_mask.py` and `tools/gen_myco_mask.py` can rebuild the tables if you supply your own images. Matrix rain is an homage to a film look, not an official product.
