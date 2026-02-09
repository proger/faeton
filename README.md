# faeton

faeton is a legendary dota coach.

[![Demo](exp/demo_preview.png)](exp/demo.mp4)

## Install

1. Install dependencies:
   - `ffmpeg`
   - `python3`
   - `swift` / `swiftc` (for the HUD overlay)
   - `go` (for the replay decoder, optional)
   - `codex` CLI (required for coaching calls)
   - `BlackHole` (for system audio loopback capture)

```bash
brew install ffmpeg go
brew install blackhole-2ch
python3 -m pip install --user numpy openai-whisper
npm i -g @openai/codex
```

## Run

Start coaching:

```bash
python -m faeton --tag mysession
```

What it does:
- records audio and screen in multiple resolutions
- asks Codex for advice
- puts advice on a HUD
- speaks advice out lud

Game logs are written to:

```text
exp/<timestamp>_<tag>/
```

## Audio notes:

- `faeton` expects a system-audio loopback device and looks for names like:
  - `BlackHole`, `Loopback`, `Soundflower`, `VB-Cable`
- If no loopback device is found, it exits with:
  - `No system-audio loopback device found`
- For best results on macOS, create a Multi-Output/Aggregate setup in Audio MIDI Setup so game audio routes to BlackHole while you can still monitor output.

## Replay Decode

Dump raw replay events with Manta (you need to download replay after the game):

```bash
./manta_run_decoder <replay.dem>  #  they are in ~/Library/Application Support/Steam/steamapps/common/dota 2 beta/game/dota/replays
```

Optional flags:
- `-eclipse`: only ticks where Luna casts Eclipse
- `-include-binary`: include low-level packet families (`CNETMsg_`, `CSVCMsg_`, `CDemo*` etc)
