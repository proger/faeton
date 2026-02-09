# faeton

faeton is a legendary dota coach.

[![Demo](exp/demo_preview.png)](exp/demo.mp4)

## Install

1. Install system dependencies:
   - `ffmpeg`
   - `python3`
   - `swift` / `swiftc` (for the HUD overlay)
   - `go` (for the replay decoder)
   - `codex` CLI (required for coaching calls)
   - `BlackHole` (for system audio loopback capture)

On macOS with Homebrew:

```bash
brew install ffmpeg go
```

Install BlackHole (example):

```bash
brew install blackhole-2ch
```

2. Install Python packages used by `faeton.py`:

```bash
python3 -m pip install --user numpy openai-whisper
```

3. Install Codex CLI and verify it is on `PATH`:

```bash
npm i -g @openai/codex
```

```bash
codex --version
```

## Run

Start recording + coaching:

```bash
python3 faeton.py --tag mysession
```

What it does:
- records audio in 10s chunks
- records screen streams (`down8_30fps`, `down4_1fps`)
- keeps a live HUD overlay on top-right
- calls Codex for short coaching advice
- speaks advice with `say`

## BlackHole Notes

- `faeton.py` expects a system-audio loopback device and looks for names like:
  - `BlackHole`, `Loopback`, `Soundflower`, `VB-Cable`
- If no loopback device is found, it exits with:
  - `No system-audio loopback device found`
- For best results on macOS, create a Multi-Output/Aggregate setup in Audio MIDI Setup so game audio routes to BlackHole while you can still monitor output.

Outputs are written to:

```text
exp/<timestamp>_<tag>/
```

## Replay Decode

Dump raw replay events with Manta:

```bash
./manta_run_decoder <replay.dem>
```

Optional flags:
- `-eclipse`: only ticks where Luna casts Eclipse
- `-include-binary`: include low-level packet families (`CNETMsg_`, `CSVCMsg_`, `CDemo*`)
