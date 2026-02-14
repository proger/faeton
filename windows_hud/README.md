# Windows HUD (C++)

A minimal always-on-top transparent overlay for faeton.

## Features

- Transparent, top-right overlay window
- Click-through (`WS_EX_TRANSPARENT`)
- Non-activating (`WS_EX_NOACTIVATE`)
- Polls `_overlay.txt` every 100ms
- Parses optional final `meta:`/`step:` line as secondary text

## Build

```powershell
cd windows_hud
run_hud.bat C:\path\to\_overlay.txt
```

Binary:

- `windows_hud/faeton.exe`

## Run

```powershell
faeton.exe C:\path\to\_overlay.txt
```
