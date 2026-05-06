# Project Instructions

This repository contains native VLC visualization plugins for Windows. These notes are for coding agents working in this repo.

## Workflow

- Use feature branches for code changes.
- Open a pull request for review instead of pushing feature work directly to `main`.
- Only merge to `main` and create a release when the user explicitly asks.
- Keep changes focused. Avoid unrelated refactors while tuning a visualizer.
- If the user says they want to manually test before pushing, do not push until they confirm.

## Build

- The active Windows build directory is `build64`.
- Build with:

```powershell
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
C:\msys64\mingw64\bin\cmake.exe --build build64 --verbose
```

- Current active plugin DLLs are:

```text
build64\libtrackinfo_visualizer_plugin.dll
build64\libled_segment_visualizer_plugin.dll
build64\libled_peak_visualizer_plugin.dll
build64\libbreakout_chill_visualizer_plugin.dll
build64\libbreakout_advanced_visualizer_plugin.dll
```

- Do not release stale local build artifacts such as:

```text
build64\libbreakout_visualizer_plugin.dll
build64\libtempest_visualizer_plugin.dll
```

## Local VLC Testing

- VLC is assumed to be installed at:

```text
C:\Program Files\VideoLAN\VLC
```

- VLC's visualization menu is hard-coded and does not reliably show third-party visualizers. Use command-line shortcuts such as:

```powershell
& "C:\Program Files\VideoLAN\VLC\vlc.exe" --audio-visual=breakout_advanced path\to\song.mp3
```

- For tasks that require an Administrator PowerShell, give the user the command to run instead of trying to perform the admin install directly.
- When giving a copy command, include all current active DLLs unless the user asks for only one.

## Release Process

- Use semantic version tags like `v0.7.2`.
- For focused fixes after a minor release, use a patch version.
- Build before tagging.
- Push `main`, push the tag, create a GitHub release, and upload the five active Windows x64 DLLs.
- Name release assets with the version suffix, for example:

```text
libbreakout_advanced_visualizer_plugin-v0.7.2-win64.dll
```

- Release notes should mention that downloaded DLLs need the version suffix removed before copying into VLC's `plugins\visualization` folder.

## Visualizer Behavior Notes

- Track metadata is shared through `visualizer_common.c`.
- Stream metadata may contain HTML entities such as `&#8217;`; keep entity decoding in the shared metadata path so all visualizers benefit.
- Breakout Advanced resets bricks and score on metadata track changes, not on silence gaps.
- Breakout Advanced alternates `PLAYER 1` / `PLAYER 2` when the metadata track changes.
- Breakout Advanced score rolls over after `99999` and refreshes the brick wall.
- Breakout Advanced brick restoration begins at each 100-point milestone and restores random broken bricks every 250 ms until the wall is full again.

