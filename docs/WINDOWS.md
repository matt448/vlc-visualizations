# Building For Your Windows VLC

Your VLC is installed at:

```text
C:\Program Files\VideoLAN\VLC
```

That is the 64-bit VLC install location, so the plugin must also be a 64-bit Windows DLL. A 32-bit DLL will not load in that VLC, and a Linux `.so` built in WSL will not load either.

## Recommended Toolchain

Use MSYS2's **MINGW64** environment. The important detail is the shell: open **MSYS2 MinGW x64**, not UCRT64, CLANG64, or MINGW32.

Install build tools:

```sh
pacman -S --needed mingw-w64-x86_64-cmake mingw-w64-x86_64-gcc mingw-w64-x86_64-pkg-config mingw-w64-x86_64-vlc make
```

You also need matching VLC development files:

- VLC headers, including `vlc_plugin.h`.
- Import libraries, usually `libvlccore.dll.a` and `libvlc.dll.a`.
- Preferably `vlc-plugin.pc` for `pkg-config`.

The normal VLC installer does not include these development files.

## Configure

If your VLC development package provides `vlc-plugin.pc`:

```sh
cmake -S . -B build -G "MinGW Makefiles" -DVLC_TARGET_BITS=64
```

If you have headers and import libraries but no `vlc-plugin.pc`:

```sh
cmake -S . -B build -G "MinGW Makefiles" \
  -DVLC_TARGET_BITS=64 \
  -DVLC_INCLUDE_DIR=/path/to/vlc/include \
  -DVLC_LIB_DIR=/path/to/vlc/lib \
  -DVLC_PLUGIN_LIBRARIES="vlccore;vlc"
```

## Build

```sh
cmake --build build
```

The plugin DLLs should be:

```text
build/libtrackinfo_visualizer_plugin.dll
build/libled_segment_visualizer_plugin.dll
```

## Install

From normal Windows PowerShell in this repo:

```powershell
.\scripts\install-windows.ps1 -BuildDir build -VlcDir "C:\Program Files\VideoLAN\VLC"
```

The script installs the plugins to:

```text
%APPDATA%\vlc\plugins\visualization\
```

If your VLC build does not scan the user plugin folder, install into VLC's application plugin folder from an Administrator PowerShell. Run this from the repository root, or adjust the DLL path to match your build directory:

```powershell
Copy-Item ".\build\libtrackinfo_visualizer_plugin.dll" "C:\Program Files\VideoLAN\VLC\plugins\visualization\" -Force
Copy-Item ".\build\libled_segment_visualizer_plugin.dll" "C:\Program Files\VideoLAN\VLC\plugins\visualization\" -Force
& "C:\Program Files\VideoLAN\VLC\vlc-cache-gen.exe" "C:\Program Files\VideoLAN\VLC\plugins"
```

You can also test without administrator rights by setting `VLC_PLUGIN_PATH` for that launch:

```powershell
$env:VLC_PLUGIN_PATH = "$env:APPDATA\vlc\plugins"
& "C:\Program Files\VideoLAN\VLC\vlc.exe" --audio-visual=led_segments path\to\song.mp3
```

## Run

Spectrum Info:

```powershell
& "C:\Program Files\VideoLAN\VLC\vlc.exe" --audio-visual=spectrum_info path\to\song.mp3
```

LED Segments:

```powershell
& "C:\Program Files\VideoLAN\VLC\vlc.exe" --audio-visual=led_segments path\to\song.mp3
```

VLC's audio visualization menu is hard-coded and may not show third-party visualization plugins. Use `--audio-visual=spectrum_info` or `--audio-visual=led_segments` from the command line to start playback with these visualizers. The older `trackinfo_visualizer` shortcut is still accepted for command-line compatibility.
