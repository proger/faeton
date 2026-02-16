#!/usr/bin/env python3
import argparse
import importlib
import numpy as np
import pathlib
import random
import re
import signal
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field

SYSTEM_DEVICE_HINTS = ("blackhole", "loopback", "soundflower", "vb-cable")
MIC_DEVICE_HINTS = ("microphone", "mic", "built-in")
PREFERRED_MIC_HINTS = ("steinberg ur22c", "ur22c")
CHUNK_SECONDS = 30
SCREEN_INPUT_PIXEL_FORMAT = "nv12"
SAY_RATE_WPM = 350
DOTA_REPLAY_DIR = pathlib.Path.home() / "Library/Application Support/Steam/steamapps/common/dota 2 beta/game/dota/replays"
ADVICE_PROMPT_TEMPLATE = """You are coaching a Dota 2 player.
Use the attached screenshot plus the speech transcript context.
Explain what is happening right now and the single next best action.
Include item advice when appropriate.
Identify two heroes visible on screen and discuss their most likely interaction in this moment.
Follow the player's spoken instructions from the latest speech chunk, and incorporate them into your advice.
Prioritize answering the player's most recent spoken question first.
If there is no direct question in the current chunk, give a brief game-situation read plus directional next-step advice.
Advice takes at least 15 seconds to return, so complete real-time commentary is not necessary.
Prioritize guidance that stays useful over the next minute: durable principles, likely next decisions, and fallback plans.
Avoid repeating previous advice unless there is a strong new reason to repeat it.
Do not repeat your previous response unless game state changed meaningfully.
Treat short filler follow-ups (e.g., "thank you", "let's go", "nice", "ok") as likely speech-recognition false positives unless clearly tied to a real gameplay question.
Vary your situation modeling and phrasing across chunks; avoid repeating the same framing too often.
Keep the response very short: exactly 1 sentence.
Be extremely concise: cap ADVICE to about 8-14 words.
Use light Gen Z phrasing naturally (e.g., "nah", "lowkey", "hard commit"), but keep it clear and actionable.
Think fast, latency is important.
Output format is mandatory:
ADVICE: <exactly 1 sentence actionable coaching response>
NEW GAME STATE: <only new game-state facts not already in Known game state; concise semicolon-separated facts, or 'none'>
When adding NEW GAME STATE, assume it will be appended after existing Known game state; do not repeat old facts.
In NEW GAME STATE, always include the current in-game time (or best visible time estimate) in each new fact when available.

Current chunk speech:
{chunk_text}

All speech so far:
{history_text}

Known game state:
{known_game_state}
"""


@dataclass
class LiveRecognizerState:
    emitted_words: list[str] = field(default_factory=list)
    next_advice_idx: int = 0
    pending_question_text: str = ""
    chunk_prefix: str = "mic"
    next_random_trigger_ts: float = 0.0


KNOWN_GAME_STATE_PATH = "_known_game_state.txt"


def load_whisper_model():
    import torch
    import whisper
    from whisper.audio import N_FRAMES, log_mel_spectrogram, pad_or_trim

    device = "cuda" if torch.cuda.is_available() else "cpu"
    model = whisper.load_model("turbo", device=device)
    use_fp16 = device == "cuda"
    return model, torch, use_fp16, log_mel_spectrogram, pad_or_trim, N_FRAMES


def list_avfoundation_audio_devices():
    result = subprocess.run(
        ["ffmpeg", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    devices = []
    in_audio_section = False
    for line in result.stderr.splitlines():
        if "AVFoundation audio devices" in line:
            in_audio_section = True
            continue
        if "AVFoundation video devices" in line:
            in_audio_section = False
            continue
        if not in_audio_section:
            continue
        match = re.search(r"\[(\d+)\]\s+(.+)$", line)
        if match:
            devices.append((int(match.group(1)), match.group(2).strip()))
    return devices


def list_avfoundation_video_devices():
    result = subprocess.run(
        ["ffmpeg", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    devices = []
    in_video_section = False
    for line in result.stderr.splitlines():
        if "AVFoundation video devices" in line:
            in_video_section = True
            continue
        if "AVFoundation audio devices" in line:
            in_video_section = False
            continue
        if not in_video_section:
            continue
        match = re.search(r"\[(\d+)\]\s+(.+)$", line)
        if match:
            devices.append((int(match.group(1)), match.group(2).strip()))
    return devices


def pick_device(devices, hints, exclude_hints=()):
    for device_id, name in devices:
        lower_name = name.lower()
        if any(h in lower_name for h in hints) and not any(
            x in lower_name for x in exclude_hints
        ):
            return device_id, name
    return None


def _normalize_word(word):
    return re.sub(r"[^\w']+", "", word.lower())


def _split_words(text):
    return re.findall(r"\S+", text or "")


def stitch_new_text(emitted_words, current_text, lookback_words=120):
    curr_words = _split_words(current_text)
    if not curr_words:
        return ""
    if not emitted_words:
        emitted_words.extend(curr_words)
        return " ".join(curr_words)

    a = emitted_words[-lookback_words:]
    b = curr_words
    max_k = min(len(a), len(b))
    overlap = 0
    for k in range(max_k, 0, -1):
        ok = True
        for i in range(k):
            if _normalize_word(a[len(a) - k + i]) != _normalize_word(b[i]):
                ok = False
                break
        if ok:
            overlap = k
            break
    new_words = b[overlap:]
    if not new_words:
        return ""
    emitted_words.extend(new_words)
    return " ".join(new_words)


def transcribe_live_wav_window(model, torch, use_fp16, log_mel_spectrogram, pad_or_trim, n_frames, wav_path):
    if not wav_path.exists() or wav_path.stat().st_size <= 44:
        return "", 0.0
    try:
        import whisper
        audio = whisper.load_audio(str(wav_path))
    except Exception:
        return "", 0.0
    if audio.size == 0:
        return "", 0.0

    sample_rate = 16000
    window_samples = CHUNK_SECONDS * sample_rate
    if audio.shape[0] > window_samples:
        audio = audio[-window_samples:]

    mel = log_mel_spectrogram(audio, model.dims.n_mels)
    mel = pad_or_trim(mel, n_frames).to(model.device)
    if use_fp16:
        mel = mel.half()
    result = model.decode(mel, whisper.DecodingOptions(language="en", task="transcribe", fp16=use_fp16))
    text = (result.text or "").strip()
    return text, float(audio.shape[0]) / sample_rate


def process_live_audio_file(
    model,
    torch,
    use_fp16,
    log_mel_spectrogram,
    pad_or_trim,
    n_frames,
    chunks_dir,
    overlay_text_path,
    live_wav_path,
    state: LiveRecognizerState,
    with_screen_advice=False,
):
    if state.next_random_trigger_ts <= 0:
        state.next_random_trigger_ts = time.time() + 5.0

    full_text, seen_seconds = transcribe_live_wav_window(
        model, torch, use_fp16, log_mel_spectrogram, pad_or_trim, n_frames, live_wav_path
    )
    if not full_text:
        return

    delta_text = stitch_new_text(state.emitted_words, full_text)
    if delta_text:
        interrupt_speech_playback(chunks_dir)
        stream_path = chunks_dir / "_live_stream.txt"
        with stream_path.open("a", encoding="utf-8") as f:
            f.write(delta_text + "\n")
        print(f"[live +{int(seen_seconds)}s] {delta_text}", flush=True)
        pending = state.pending_question_text.strip()
        if pending:
            pending = f"{pending} {delta_text.strip()}"
        else:
            pending = delta_text.strip()
        state.pending_question_text = pending

        # Trigger advice only when Whisper output contains complete question(s).
        while "?" in state.pending_question_text:
            before, after = state.pending_question_text.split("?", 1)
            question_text = (before.strip() + "?").strip()
            state.pending_question_text = after.strip()
            if not question_text:
                continue
            advice_idx = state.next_advice_idx
            chunk_stem = f"{state.chunk_prefix}_{advice_idx:06d}"
            chunk_path = chunks_dir / f"{chunk_stem}.wav"
            txt_path = chunks_dir / f"{chunk_stem}.txt"
            txt_path.write_text(question_text + "\n", encoding="utf-8")
            print(f"[{chunk_stem}] {question_text}", flush=True)
            if with_screen_advice:
                run_reloaded_generate_chunk_advice(
                    chunks_dir, chunk_path, question_text, overlay_text_path
                )
            state.next_advice_idx += 1

    # Fallback: if no explicit question is asked, trigger advice randomly
    # with probability 1/4 every 5 seconds using buffered speech context.
    now = time.time()
    if now >= state.next_random_trigger_ts:
        state.next_random_trigger_ts = now + 5.0
        buffered = state.pending_question_text.strip()
        if buffered and random.random() < 0.25:
            advice_idx = state.next_advice_idx
            chunk_stem = f"{state.chunk_prefix}_{advice_idx:06d}"
            chunk_path = chunks_dir / f"{chunk_stem}.wav"
            txt_path = chunks_dir / f"{chunk_stem}.txt"
            txt_path.write_text(buffered + "\n", encoding="utf-8")
            print(f"[{chunk_stem}] {buffered}", flush=True)
            if with_screen_advice:
                run_reloaded_generate_chunk_advice(
                    chunks_dir, chunk_path, buffered, overlay_text_path
                )
            state.pending_question_text = ""
            state.next_advice_idx += 1

def collect_speech_history(chunks_dir):
    transcript_paths = sorted(
        p
        for p in chunks_dir.glob("*.txt")
        if re.fullmatch(r"(?:\d{6}|mic_\d{6}|loopback_\d{6})\.txt", p.name)
    )
    lines = []
    for path in transcript_paths:
        text = path.read_text(encoding="utf-8", errors="replace").strip()
        if text:
            lines.append(f"{path.stem}: {text}")
    return "\n".join(lines) if lines else "(no speech yet)"


def load_known_game_state(chunks_dir):
    path = chunks_dir / KNOWN_GAME_STATE_PATH
    if not path.exists():
        return "(none yet)"
    text = path.read_text(encoding="utf-8", errors="replace").strip()
    return text if text else "(none yet)"


def update_known_game_state(chunks_dir, new_state_text):
    cleaned = (new_state_text or "").strip()
    if not cleaned or cleaned.lower() == "none":
        return
    path = chunks_dir / KNOWN_GAME_STATE_PATH
    existing = []
    if path.exists():
        existing = [ln.strip() for ln in path.read_text(encoding="utf-8", errors="replace").splitlines() if ln.strip()]
    seen = {ln.lower(): True for ln in existing}
    additions = []
    for part in re.split(r"[;\n]+", cleaned):
        item = part.strip()
        if not item:
            continue
        key = item.lower()
        if key in seen:
            continue
        seen[key] = True
        additions.append(item)
    if additions:
        # Keep prior state in-place and append newly discovered facts last.
        merged = existing + additions
        path.write_text("\n".join(merged) + "\n", encoding="utf-8")


def extract_section(text, label):
    pattern = rf"(?ims)^\s*{re.escape(label)}\s*:\s*(.*?)(?=^\s*[A-Z][A-Z _-]*\s*:|\Z)"
    m = re.search(pattern, text or "")
    if not m:
        return ""
    return m.group(1).strip()


def wait_for_video_chunk(path, timeout_seconds=8):
    deadline = time.time() + timeout_seconds
    while time.time() < deadline:
        if path.exists() and path.stat().st_size > 0:
            return True
        time.sleep(0.2)
    return path.exists() and path.stat().st_size > 0


def extract_last_frame(video_path, png_path):
    commands = [
        [
            "ffmpeg",
            "-sseof",
            "-3",
            "-i",
            str(video_path),
            "-frames:v",
            "1",
            "-q:v",
            "2",
            "-y",
            str(png_path),
        ],
        [
            "ffmpeg",
            "-i",
            str(video_path),
            "-frames:v",
            "1",
            "-q:v",
            "2",
            "-y",
            str(png_path),
        ],
    ]
    for cmd in commands:
        result = subprocess.run(
            cmd,
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if result.returncode == 0 and png_path.exists() and png_path.stat().st_size > 0:
            return True
    return False


def run_codex_advice(image_path, prompt, response_path):
    input_bytes = len(prompt.encode("utf-8")) + image_path.stat().st_size
    started = time.time()
    print("=== Codex prompt begin ===", flush=True)
    print(prompt, flush=True)
    print("=== Codex prompt end ===", flush=True)
    result = subprocess.run(
        [
            "codex",
            "exec",
            "-m",
            "gpt-5.3-codex",
            "-c",
            "model_reasoning_effort=low",
            "--skip-git-repo-check",
            "-i",
            str(image_path),
            "-o",
            str(response_path),
            "--",
            prompt,
        ],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    elapsed = time.time() - started
    if result.returncode != 0:
        return False, (result.stderr or "codex failed").strip(), elapsed, input_bytes
    response = response_path.read_text(encoding="utf-8", errors="replace").strip()
    if not response:
        return False, "codex returned empty advice", elapsed, input_bytes
    return True, response, elapsed, input_bytes


def get_overlay_binary_path():
    repo_root = pathlib.Path(__file__).resolve().parent
    cache_dir = repo_root / ".cache"
    cache_dir.mkdir(parents=True, exist_ok=True)
    return cache_dir / "record_overlay_nonactivating"


def build_overlay_binary():
    swiftc = shutil.which("swiftc")
    if swiftc is None:
        print("swiftc not found; overlay disabled.", flush=True)
        return None

    source_path = pathlib.Path(__file__).resolve().parent / "hud.swift"
    if not source_path.exists():
        print(f"Overlay source not found: {source_path}", flush=True)
        return None

    binary_path = get_overlay_binary_path()
    if binary_path.exists() and binary_path.stat().st_mtime >= source_path.stat().st_mtime:
        return binary_path

    result = subprocess.run(
        [swiftc, "-O", str(source_path), "-o", str(binary_path)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        print(f"Failed to build overlay binary: {result.stderr.strip()}", flush=True)
        return None
    return binary_path


def get_image_resolution(image_path):
    try:
        result = subprocess.run(
            [
                "ffprobe",
                "-v",
                "error",
                "-select_streams",
                "v:0",
                "-show_entries",
                "stream=width,height",
                "-of",
                "csv=p=0:s=x",
                str(image_path),
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        value = (result.stdout or "").strip()
        return value if value else "unknown"
    except Exception:
        return "unknown"


def start_persistent_overlay(chunks_dir, current_microphone):
    overlay_binary = build_overlay_binary()
    if overlay_binary is None:
        return None, None

    overlay_text_path = chunks_dir / "_overlay.txt"
    overlay_text_path.write_text(
        (
            f"Ask me how to play. I can see your screen. "
            f"Listening: {current_microphone}"
        ),
        encoding="utf-8",
    )
    proc = subprocess.Popen(
        [
            str(overlay_binary),
            "-i",
            str(overlay_text_path),
            "-o",
            str(chunks_dir / "_pub.txt"),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return proc, overlay_text_path


def set_overlay_text(overlay_text_path, text):
    if overlay_text_path is None:
        return
    overlay_text_path.write_text(text.strip() + "\n", encoding="utf-8")


def stop_persistent_overlay(overlay_proc):
    if overlay_proc is None or overlay_proc.poll() is not None:
        return
    overlay_proc.terminate()
    try:
        overlay_proc.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        overlay_proc.kill()
        overlay_proc.wait()


def speak_text_for_session(text, chunks_dir):
    stop_flag = None
    if chunks_dir is not None:
        stop_flag = chunks_dir / "_stop_playback.flag"
        if stop_flag.exists():
            stop_flag.unlink(missing_ok=True)
    proc = subprocess.Popen(["say", "-r", str(SAY_RATE_WPM), text])
    while True:
        if proc.poll() is not None:
            break
        if stop_flag is not None and stop_flag.exists():
            proc.terminate()
            try:
                proc.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            stop_flag.unlink(missing_ok=True)
            break
        time.sleep(0.05)


def interrupt_speech_playback(chunks_dir):
    if chunks_dir is None:
        return
    (chunks_dir / "_stop_playback.flag").write_text("1\n", encoding="utf-8")


def generate_chunk_advice(chunks_dir, chunk_path, chunk_text, overlay_text_path):
    advice_path = chunk_path.with_name(f"{chunk_path.stem}_advice.txt")
    if advice_path.exists():
        return

    latest_live_png = chunks_dir / "latest_down4_1fps.png"
    png_path = chunk_path.with_name(f"{chunk_path.stem}_down4_1fps_last.png")
    if wait_for_video_chunk(latest_live_png, timeout_seconds=2):
        shutil.copy2(latest_live_png, png_path)
    else:
        down4_video_path = chunk_path.with_name(f"{chunk_path.stem}_down4_1fps.mp4")
        if not wait_for_video_chunk(down4_video_path):
            candidates = sorted(
                chunks_dir.glob("*_down4_1fps.mp4"),
                key=lambda p: p.stem.split("_", 1)[0],
            )
            if candidates:
                down4_video_path = candidates[-1]
        if not wait_for_video_chunk(down4_video_path):
            print(f"[{chunk_path.stem}] down4 video chunk missing, skipping advice", flush=True)
            return
        video_stem = down4_video_path.stem.replace("_down4_1fps", "")
        png_path = chunk_path.with_name(f"{video_stem}_down4_1fps_last.png")
        if not extract_last_frame(down4_video_path, png_path):
            print(f"[{chunk_path.stem}] failed to extract last frame, skipping advice", flush=True)
            return

    history_text = collect_speech_history(chunks_dir)
    known_game_state = load_known_game_state(chunks_dir)
    safe_chunk_text = chunk_text.strip() if chunk_text and chunk_text.strip() else "(empty)"
    prompt = ADVICE_PROMPT_TEMPLATE.format(
        chunk_text=safe_chunk_text,
        history_text=history_text,
        known_game_state=known_game_state,
    )
    input_bytes = len(prompt.encode("utf-8")) + png_path.stat().st_size
    input_kib = input_bytes / 1024.0
    screenshot_resolution = get_image_resolution(png_path)
    latest_question = safe_chunk_text
    set_overlay_text(
        overlay_text_path,
        (
            f"Thinking...\n"
            f"meta: {latest_question} | step: input {input_kib:.1f}KiB, output --.-s | res: {screenshot_resolution}"
        ),
    )

    response_path = chunk_path.with_name(f"{chunk_path.stem}_advice_response.txt")
    ok, response_or_error, elapsed, input_bytes = run_codex_advice(
        png_path, prompt, response_path
    )
    if not ok:
        print(f"[{chunk_path.stem}] advice failed: {response_or_error}", flush=True)
        return

    response = response_or_error
    advice_text = extract_section(response, "ADVICE")
    if not advice_text:
        advice_text = response.strip()
    new_game_state = extract_section(response, "NEW GAME STATE")
    update_known_game_state(chunks_dir, new_game_state)
    print("==== Codex advice begin ====", flush=True)
    print(response, flush=True)
    print("==== Codex advice end ====", flush=True)
    print(f"[{chunk_path.stem}] advice: {advice_text}", flush=True)
    input_kib = input_bytes / 1024.0
    overlay_meta = f"step: input {input_kib:.1f}KiB, output {elapsed:.2f}s | res: {screenshot_resolution}"
    advice_path.write_text(
        "Prompt:\n"
        f"{prompt.strip()}\n\n"
        "Response:\n"
        f"{response.strip()}\n\n"
        "Parsed advice:\n"
        f"{advice_text}\n\n"
        "Parsed new game state:\n"
        f"{new_game_state or 'none'}\n\n"
        f"{overlay_meta}\n",
        encoding="utf-8",
    )
    (chunks_dir / "_current_advice_chunk.txt").write_text(
        f"{chunk_path.stem}\n", encoding="utf-8"
    )
    set_overlay_text(
        overlay_text_path,
        f"{advice_text}\nmeta: {latest_question} | {overlay_meta}",
    )
    speak_text_for_session(advice_text, chunks_dir)


def run_reloaded_generate_chunk_advice(chunks_dir, chunk_path, chunk_text, overlay_text_path):
    module_name = pathlib.Path(__file__).resolve().stem
    module = sys.modules.get(module_name)
    if module is None:
        module = importlib.import_module(module_name)
    else:
        module = importlib.reload(module)
    reloaded_generate = getattr(module, "generate_chunk_advice")
    reloaded_generate(chunks_dir, chunk_path, chunk_text, overlay_text_path)


def copy_session_replay_to_exp(session_start_unix, chunks_dir):
    replay_dir = DOTA_REPLAY_DIR
    if not replay_dir.is_dir():
        print(f"Replay copy: replay directory not found: {replay_dir}", flush=True)
        return

    # Replay files may finalize shortly after capture ends; poll briefly.
    latest = None
    deadline = time.time() + 20
    while time.time() < deadline:
        candidates = [
            p
            for p in replay_dir.glob("*.dem")
            if p.is_file() and p.stat().st_mtime >= (session_start_unix - 2)
        ]
        if candidates:
            latest = max(candidates, key=lambda p: p.stat().st_mtime)
        if latest is not None:
            break
        time.sleep(1)

    if latest is None:
        print(
            "Replay copy: no .dem file found in replay folder for this session.",
            flush=True,
        )
        return

    dest = chunks_dir / latest.name
    if dest.exists():
        stem = latest.stem
        suffix = latest.suffix
        n = 1
        while True:
            candidate = chunks_dir / f"{stem}_{n}{suffix}"
            if not candidate.exists():
                dest = candidate
                break
            n += 1
    shutil.copy2(latest, dest)
    print(f"Replay copy: copied {latest} -> {dest}", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--tag",
        type=str,
        default="",
        help="Optional suffix appended to the output directory name.",
    )
    parser.add_argument(
        "--whisper-loopback",
        action="store_true",
        help="Use loopback_%06d.opus files as Whisper input (default uses mic_%06d.opus).",
    )
    args = parser.parse_args()

    if shutil.which("ffmpeg") is None:
        raise SystemExit("ffmpeg is required but was not found in PATH.")
    model, torch, use_fp16, log_mel_spectrogram, pad_or_trim, n_frames = load_whisper_model()

    devices = list_avfoundation_audio_devices()
    if not devices:
        raise SystemExit("No AVFoundation audio devices found.")

    system_device = pick_device(devices, SYSTEM_DEVICE_HINTS)
    mic_device = pick_device(
        devices, PREFERRED_MIC_HINTS, exclude_hints=SYSTEM_DEVICE_HINTS
    ) or pick_device(devices, MIC_DEVICE_HINTS, exclude_hints=SYSTEM_DEVICE_HINTS)

    if system_device is None:
        raise SystemExit(
            "No system-audio loopback device found (expected names like BlackHole/Loopback/Soundflower)."
        )
    if mic_device is None:
        raise SystemExit("No microphone device found (Steinberg UR22C not detected).")

    start_ts = int(time.time())
    out_dir = pathlib.Path("exp")
    out_dir.mkdir(parents=True, exist_ok=True)
    tag_suffix = f"_{args.tag}" if args.tag else ""
    chunks_dir = out_dir / f"{start_ts}{tag_suffix}"
    chunks_dir.mkdir(parents=True, exist_ok=True)

    system_id, system_name = system_device
    mic_id, mic_name = mic_device

    mic_segment_output = str(chunks_dir / "mic_%06d.opus")
    loopback_segment_output = str(chunks_dir / "loopback_%06d.opus")
    mic_live_wav = str(chunks_dir / "mic_live.wav")
    loopback_live_wav = str(chunks_dir / "loopback_live.wav")
    whisper_live_wav = pathlib.Path(loopback_live_wav if args.whisper_loopback else mic_live_wav)
    if system_id == mic_id:
        audio_command = [
            "ffmpeg",
            "-f",
            "avfoundation",
            "-i",
            f":{system_id}",
            "-map",
            "0:a",
            "-ar",
            "48000",
            "-c:a",
            "libopus",
            "-b:a",
            "160k",
            "-f",
            "segment",
            "-segment_time",
            str(CHUNK_SECONDS),
            "-reset_timestamps",
            "1",
            mic_segment_output,
            "-map",
            "0:a",
            "-ar",
            "48000",
            "-c:a",
            "libopus",
            "-b:a",
            "160k",
            "-f",
            "segment",
            "-segment_time",
            str(CHUNK_SECONDS),
            "-reset_timestamps",
            "1",
            loopback_segment_output,
            "-map",
            "0:a",
            "-ar",
            "16000",
            "-ac",
            "1",
            "-c:a",
            "pcm_s16le",
            "-f",
            "wav",
            mic_live_wav,
            "-map",
            "0:a",
            "-ar",
            "16000",
            "-ac",
            "1",
            "-c:a",
            "pcm_s16le",
            "-f",
            "wav",
            loopback_live_wav,
        ]
        print(
            f"Recording aggregate device '{system_name}' to {chunks_dir} "
            "(duplicated into mic_*.opus and loopback_*.opus)"
        )
    else:
        audio_command = [
            "ffmpeg",
            "-f",
            "avfoundation",
            "-i",
            f":{mic_id}",
            "-f",
            "avfoundation",
            "-i",
            f":{system_id}",
            "-map",
            "0:a",
            "-ar",
            "48000",
            "-c:a",
            "libopus",
            "-b:a",
            "160k",
            "-f",
            "segment",
            "-segment_time",
            str(CHUNK_SECONDS),
            "-reset_timestamps",
            "1",
            mic_segment_output,
            "-map",
            "1:a",
            "-ar",
            "48000",
            "-c:a",
            "libopus",
            "-b:a",
            "160k",
            "-f",
            "segment",
            "-segment_time",
            str(CHUNK_SECONDS),
            "-reset_timestamps",
            "1",
            loopback_segment_output,
            "-map",
            "0:a",
            "-ar",
            "16000",
            "-ac",
            "1",
            "-c:a",
            "pcm_s16le",
            "-f",
            "wav",
            mic_live_wav,
            "-map",
            "1:a",
            "-ar",
            "16000",
            "-ac",
            "1",
            "-c:a",
            "pcm_s16le",
            "-f",
            "wav",
            loopback_live_wav,
        ]
        print(
            f"Recording mic '{mic_name}' -> mic_*.opus and "
            f"system '{system_name}' -> loopback_*.opus in {chunks_dir}"
        )

    print(f"Chunk length: {CHUNK_SECONDS}s")
    print(f"Whisper input source: {'loopback' if args.whisper_loopback else 'mic'}")
    print(f"Live WAV input: {whisper_live_wav}")
    print("Transcribing live WAV input with Whisper Turbo (Torch API).")
    overlay_proc, overlay_text_path = start_persistent_overlay(chunks_dir, mic_name)
    screen_proc = None
    screen_segment_pattern_30fps = str(chunks_dir / "%06d_down8_30fps.mp4")
    screen_segment_pattern_1fps = str(chunks_dir / "%06d_down4_1fps.mp4")
    latest_frame_path = str(chunks_dir / "latest_down4_1fps.png")
    video_devices = list_avfoundation_video_devices()
    if not video_devices:
        raise SystemExit("No AVFoundation video devices found for screen recording.")
    screen_device = pick_device(video_devices, ("capture screen", "screen"))
    if screen_device is None:
        screen_device = video_devices[0]
    screen_id, screen_name = screen_device
    screen_command = [
        "ffmpeg",
        "-f",
        "avfoundation",
        "-pixel_format",
        SCREEN_INPUT_PIXEL_FORMAT,
        "-framerate",
        "30",
        "-i",
        f"{screen_id}:none",
        "-filter_complex",
        (
            "[0:v]split=3[v30][v1][v1live];"
            "[v30]fps=30,scale=trunc(iw/8):trunc(ih/8)[v30out];"
            "[v1]fps=1,scale=trunc(iw/4):trunc(ih/4)[v1out];"
            "[v1live]fps=1,scale=trunc(iw/4):trunc(ih/4)[v1liveout]"
        ),
        "-map",
        "[v30out]",
        "-c:v",
        "h264_videotoolbox",
        "-b:v",
        "8M",
        "-maxrate",
        "12M",
        "-bufsize",
        "24M",
        "-g",
        "60",
        "-force_key_frames",
        f"expr:gte(t,n_forced*{CHUNK_SECONDS})",
        "-f",
        "segment",
        "-segment_time",
        str(CHUNK_SECONDS),
        "-reset_timestamps",
        "1",
        screen_segment_pattern_30fps,
        "-map",
        "[v1out]",
        "-c:v",
        "h264_videotoolbox",
        "-b:v",
        "2M",
        "-maxrate",
        "3M",
        "-bufsize",
        "6M",
        "-g",
        "30",
        "-force_key_frames",
        f"expr:gte(t,n_forced*{CHUNK_SECONDS})",
        "-f",
        "segment",
        "-segment_time",
        str(CHUNK_SECONDS),
        "-reset_timestamps",
        "1",
        screen_segment_pattern_1fps,
        "-map",
        "[v1liveout]",
        "-f",
        "image2",
        "-update",
        "1",
        "-q:v",
        "2",
        latest_frame_path,
    ]
    print(
        "Screen chunks "
        f"({screen_name}): {chunks_dir} "
        "(*_down8_30fps.mp4, *_down4_1fps.mp4, latest_down4_1fps.png)"
    )
    screen_proc = subprocess.Popen(screen_command)
    print("Press Ctrl-C to stop.")
    proc = subprocess.Popen(audio_command)
    live_state = LiveRecognizerState(
        chunk_prefix="loopback" if args.whisper_loopback else "mic",
    )
    try:
        while proc.poll() is None:
            process_live_audio_file(
                model,
                torch,
                use_fp16,
                log_mel_spectrogram,
                pad_or_trim,
                n_frames,
                chunks_dir,
                overlay_text_path,
                whisper_live_wav,
                live_state,
                with_screen_advice=True,
            )
            time.sleep(1)
    except KeyboardInterrupt:
        proc.send_signal(signal.SIGINT)
        if screen_proc is not None and screen_proc.poll() is None:
            screen_proc.send_signal(signal.SIGINT)
        proc.wait()
        if screen_proc is not None:
            screen_proc.wait()
    finally:
        if screen_proc is not None and screen_proc.poll() is None:
            screen_proc.send_signal(signal.SIGINT)
            screen_proc.wait()
        process_live_audio_file(
            model,
            torch,
            use_fp16,
            log_mel_spectrogram,
            pad_or_trim,
            n_frames,
            chunks_dir,
            overlay_text_path,
            whisper_live_wav,
            live_state,
            with_screen_advice=True,
        )
        copy_session_replay_to_exp(start_ts, chunks_dir)
        stop_persistent_overlay(overlay_proc)


if __name__ == "__main__":
    main()
