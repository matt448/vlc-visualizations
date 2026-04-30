# VLC Track Info Visualizer

A native VLC visualization plugin that draws a Spectrum-style frequency visualization inside VLC's video area and keeps the current track/stream title visible.

![Spectrum Info visualization running in VLC](docs/screenshots/trackinfo_visualizer_screenshot.png)

This scaffold targets VLC 3.x on Windows. It uses VLC's visualization/audio-filter plugin API for audio samples and VLC's video-output request API for rendering, matching the built-in Spectrum visualizer's placement inside the VLC window.

Your current VLC path is assumed to be:

```text
C:\Program Files\VideoLAN\VLC
```

That means you need a 64-bit Windows build of this plugin. See [docs/WINDOWS.md](docs/WINDOWS.md) for the Windows-specific build flow.

## What It Shows

- Spectrum-style frequency bars derived from the current audio buffer.
- Persistent current track/stream text inside the visualization.
- A fallback message when metadata is not available.

## Prerequisites

- VLC 3.x development headers and plugin import libraries.
- CMake 3.20 or newer.
- A 64-bit Windows C compiler for your installed VLC, preferably MSYS2 MinGW64.
- `pkg-config` metadata for `vlc-plugin`, or equivalent include/library paths supplied manually.

On many systems, VLC runtime installers do not include the headers needed to build plugins. If `pkg-config --cflags vlc-plugin` fails, install or build the VLC SDK/development package first.

## Build

```powershell
cmake -S . -B build -DVLC_TARGET_BITS=64
cmake --build build
```

The output plugin DLL is named:

```text
libtrackinfo_visualizer_plugin.dll
```

## Install

Install the DLL into your VLC user plugin folder and refresh VLC's plugin cache:

```text
%APPDATA%\vlc\plugins\visualization\
```

```powershell
.\scripts\install-windows.ps1 -BuildDir build -VlcDir "C:\Program Files\VideoLAN\VLC"
```

If VLC does not discover the user plugin folder, copy the built DLL into `C:\Program Files\VideoLAN\VLC\plugins\visualization\` from an Administrator PowerShell and run `vlc-cache-gen.exe`.

The visualization appears as `Spectrum Info`. The command-line shortcuts are `spectrum_info` and `trackinfo_visualizer`.

## Run

From the command line:

```powershell
& "C:\Program Files\VideoLAN\VLC\vlc.exe" --audio-visual=spectrum_info path\to\song.mp3
```

Or open VLC and select the visualization from the audio visualization menu after the plugin cache has been refreshed.

## Notes

VLC's native plugin ABI is version-sensitive. This project is intentionally small so it can be adjusted for the exact VLC SDK you build against. The metadata code is compiled only when `vlc_playlist_legacy.h` is available, which is the normal VLC 3.x route for accessing the current playlist input.

This plugin is inspired by VLC's built-in Spectrum visualization and is licensed under GPL-2.0-or-later.
