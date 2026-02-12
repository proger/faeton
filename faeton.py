#!/usr/bin/env python3
import argparse
import importlib
import numpy as np
import pathlib
import re
import signal
import shutil
import subprocess
import sys
import time

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
Advice takes at least 15 seconds to return, so complete real-time commentary is not necessary.
Prioritize guidance that stays useful over the next minute: durable principles, likely next decisions, and fallback plans.
Avoid repeating previous advice unless there is a strong new reason to repeat it.
Vary your situation modeling and phrasing across chunks; avoid repeating the same framing too often.
Keep the response very short: exactly 1 sentence.
Think fast, latency is important.

Current chunk speech:
{chunk_text}

All speech so far:
{history_text}
"""


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


def transcribe_chunk(
    model, torch, use_fp16, log_mel_spectrogram, pad_or_trim, n_frames, chunks_dir, chunk_path
):
    txt_path = chunk_path.with_suffix(".txt")
    npy_path = chunk_path.with_name(f"{chunk_path.stem}_wenc.npy")
    if txt_path.exists() and npy_path.exists():
        return txt_path.read_text(encoding="utf-8", errors="replace").strip()
    mel = log_mel_spectrogram(str(chunk_path), model.dims.n_mels)
    mel = pad_or_trim(mel, n_frames).to(model.device)
    if use_fp16:
        mel = mel.half()
    with torch.no_grad():
        features = model.embed_audio(mel.unsqueeze(0))
    np.save(npy_path, features.squeeze(0).detach().cpu().numpy())

    result = model.transcribe(
        str(chunk_path),
        fp16=use_fp16,
        language="en",
    )
    text = (result.get("text") or "").strip()
    txt_path.write_text(text + "\n", encoding="utf-8")
    print(f"[{chunk_path.stem}] {text}", flush=True)
    return text


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


def start_persistent_overlay(chunks_dir):
    overlay_binary = build_overlay_binary()
    if overlay_binary is None:
        return None, None

    overlay_text_path = chunks_dir / "_overlay.txt"
    overlay_text_path.write_text(
        "Recording active.\nWaiting for chunk advice...",
        encoding="utf-8",
    )
    proc = subprocess.Popen(
        [str(overlay_binary), "--text-file", str(overlay_text_path)],
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


def speak_text(text):
    subprocess.run(["say", "-r", str(SAY_RATE_WPM), text], check=False)


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
    safe_chunk_text = chunk_text.strip() if chunk_text and chunk_text.strip() else "(empty)"
    prompt = ADVICE_PROMPT_TEMPLATE.format(
        chunk_text=safe_chunk_text,
        history_text=history_text,
    )
    input_bytes = len(prompt.encode("utf-8")) + png_path.stat().st_size
    input_kib = input_bytes / 1024.0
    set_overlay_text(
        overlay_text_path,
        f"Thinking...\nstep: input {input_kib:.1f}KiB, output --.-s",
    )

    response_path = chunk_path.with_name(f"{chunk_path.stem}_advice_response.txt")
    ok, response_or_error, elapsed, input_bytes = run_codex_advice(
        png_path, prompt, response_path
    )
    if not ok:
        print(f"[{chunk_path.stem}] advice failed: {response_or_error}", flush=True)
        return

    response = response_or_error
    input_kib = input_bytes / 1024.0
    overlay_meta = f"step: input {input_kib:.1f}KiB, output {elapsed:.2f}s"
    advice_path.write_text(
        "Prompt:\n"
        f"{prompt.strip()}\n\n"
        "Response:\n"
        f"{response.strip()}\n\n"
        f"{overlay_meta}\n",
        encoding="utf-8",
    )
    print(f"[{chunk_path.stem}] advice: {response}", flush=True)
    set_overlay_text(overlay_text_path, f"{response}\n{overlay_meta}")
    speak_text(response)


def run_reloaded_generate_chunk_advice(chunks_dir, chunk_path, chunk_text, overlay_text_path):
    module_name = pathlib.Path(__file__).resolve().stem
    module = sys.modules.get(module_name)
    if module is None:
        module = importlib.import_module(module_name)
    else:
        module = importlib.reload(module)
    reloaded_generate = getattr(module, "generate_chunk_advice")
    reloaded_generate(chunks_dir, chunk_path, chunk_text, overlay_text_path)


def process_finished_chunks(
    model,
    torch,
    use_fp16,
    log_mel_spectrogram,
    pad_or_trim,
    n_frames,
    chunks_dir,
    processed,
    overlay_text_path,
    audio_glob,
    with_screen_advice=False,
    final_pass=False,
):
    chunks = sorted(chunks_dir.glob(audio_glob))
    if not chunks:
        return
    if not final_pass and len(chunks) > 1:
        chunks = chunks[:-1]
    elif not final_pass and len(chunks) == 1:
        return

    chunk_texts = {}
    latest_unprocessed_chunk = None
    for chunk in chunks:
        if chunk in processed:
            continue
        latest_unprocessed_chunk = chunk
        chunk_text = transcribe_chunk(
            model,
            torch,
            use_fp16,
            log_mel_spectrogram,
            pad_or_trim,
            n_frames,
            chunks_dir,
            chunk,
        )
        chunk_texts[chunk] = chunk_text
        processed.add(chunk)
    if with_screen_advice and latest_unprocessed_chunk is not None:
        run_reloaded_generate_chunk_advice(
            chunks_dir,
            latest_unprocessed_chunk,
            chunk_texts.get(latest_unprocessed_chunk, ""),
            overlay_text_path,
        )


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
    whisper_audio_glob = "loopback_*.opus" if args.whisper_loopback else "mic_*.opus"

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
        ]
        print(
            f"Recording mic '{mic_name}' -> mic_*.opus and "
            f"system '{system_name}' -> loopback_*.opus in {chunks_dir}"
        )

    print(f"Chunk length: {CHUNK_SECONDS}s")
    print(f"Whisper input source: {'loopback' if args.whisper_loopback else 'mic'}")
    print("Transcribing each completed chunk with Whisper Turbo (Torch API).")
    overlay_proc, overlay_text_path = start_persistent_overlay(chunks_dir)
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
    processed_chunks = set()
    try:
        while proc.poll() is None:
            process_finished_chunks(
                model,
                torch,
                use_fp16,
                log_mel_spectrogram,
                pad_or_trim,
                n_frames,
                chunks_dir,
                processed_chunks,
                overlay_text_path,
                whisper_audio_glob,
                with_screen_advice=True,
                final_pass=False,
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
        process_finished_chunks(
            model,
            torch,
            use_fp16,
            log_mel_spectrogram,
            pad_or_trim,
            n_frames,
            chunks_dir,
            processed_chunks,
            overlay_text_path,
            whisper_audio_glob,
            with_screen_advice=True,
            final_pass=True,
        )
        copy_session_replay_to_exp(start_ts, chunks_dir)
        stop_persistent_overlay(overlay_proc)


if __name__ == "__main__":
    main()
