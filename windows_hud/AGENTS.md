# Developer Notes

## Purpose

`windows_hud` contains a Windows-native C++ overlay executable used by faeton.
Current binary name: `faeton.exe`.

## Build

Primary build path (works over SSH/non-dev shell):

```bat
run_hud.bat C:\path\to\overlay.txt
```

What it does:
- Auto-initializes MSVC environment via `vcvars64.bat` if `cl` is not already in `PATH`.
- Compiles `main.cpp` with `cl` and links:
  - `user32.lib`
  - `gdi32.lib`
  - `d2d1.lib`
  - `dwrite.lib`
  - `shell32.lib`

Output:
- `windows_hud\faeton.exe`

## Run

Direct:

```bat
faeton.exe C:\path\to\overlay.txt
```

CLI contract:
- `argv[1]` is the text file path.

## Runtime behavior

- Poll interval: `100ms`
- Position: top-right
- Window styles: topmost, layered, click-through, non-activating
- Background: black rounded rectangle
- Opacity: 85% at window layer (`LWA_ALPHA`, alpha=217)
- Text rendering: DirectWrite (`Consolas`), main + optional trailing `meta:`/`step:` line

## Common remote workflow (from macOS/Linux)

Build remotely:

```bash
ssh volo@steam.local "cmd /c C:\\Users\\volo\\windows_hud\\run_hud.bat C:\\Users\\volo\\windows_hud\\overlay.txt"
```

Run remotely:

```bash
ssh volo@steam.local "cmd /c start \"\" C:\\Users\\volo\\windows_hud\\faeton.exe C:\\Users\\volo\\windows_hud\\overlay.txt"
```

Kill all HUDs:

```bash
ssh volo@steam.local "cmd /c taskkill /F /IM faeton.exe"
```

## Sync note

If you are editing from another machine, sync sources both ways before/after build to avoid drift.
