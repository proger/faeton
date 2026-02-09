#!/usr/bin/env python3
import argparse
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
CHUNK_SECONDS = 10
SCREEN_INPUT_PIXEL_FORMAT = "nv12"
SAY_RATE_WPM = 350
SAY_VOICE = "Grandpa"
WHISPER_CONTEXT_SECONDS = 30
ADVICE_PROMPT_TEMPLATE = """You are coaching a Dota 2 player.
Use the attached screenshot plus the speech transcript context.
Explain what is happening right now and the single next best action.
Include item advice when appropriate.
Keep the response very short: at most 2 sentences.
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

    context_audio_path, temp_files = build_whisper_context_audio(chunks_dir, chunk_path)
    initial_prompt = collect_prior_transcripts_for_chunk(chunks_dir, chunk_path)
    try:
        result = model.transcribe(
            str(context_audio_path),
            fp16=use_fp16,
            language="en",
            initial_prompt=initial_prompt or None,
        )
    finally:
        for path in temp_files:
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass
    text = (result.get("text") or "").strip()
    txt_path.write_text(text + "\n", encoding="utf-8")
    print(f"[{chunk_path.stem}] {text}", flush=True)
    return text


def collect_speech_history(chunks_dir):
    transcript_paths = sorted(
        p for p in chunks_dir.glob("*.txt") if re.fullmatch(r"\d{6}\.txt", p.name)
    )
    lines = []
    for path in transcript_paths:
        text = path.read_text(encoding="utf-8", errors="replace").strip()
        if text:
            lines.append(f"{path.stem}: {text}")
    return "\n".join(lines) if lines else "(no speech yet)"


def collect_prior_transcripts_for_chunk(chunks_dir, chunk_path):
    try:
        chunk_index = int(chunk_path.stem)
    except ValueError:
        return ""
    lines = []
    for i in range(chunk_index):
        txt_path = chunks_dir / f"{i:06d}.txt"
        if not txt_path.exists():
            continue
        text = txt_path.read_text(encoding="utf-8", errors="replace").strip()
        if text:
            lines.append(text)
    return "\n".join(lines)


def build_whisper_context_audio(chunks_dir, chunk_path):
    window_chunks = max(1, (WHISPER_CONTEXT_SECONDS + CHUNK_SECONDS - 1) // CHUNK_SECONDS)
    try:
        chunk_index = int(chunk_path.stem)
    except ValueError:
        return chunk_path, []

    start_index = max(0, chunk_index - window_chunks + 1)
    context_paths = []
    for i in range(start_index, chunk_index + 1):
        p = chunks_dir / f"{i:06d}.opus"
        if p.exists():
            context_paths.append(p)
    if not context_paths:
        return chunk_path, []
    if len(context_paths) == 1:
        return context_paths[0], []

    list_path = chunks_dir / f".{chunk_path.stem}_whisper_context.txt"
    output_path = chunks_dir / f".{chunk_path.stem}_whisper_context.opus"
    list_lines = []
    for p in context_paths:
        escaped = str(p.resolve()).replace("'", "'\\''")
        list_lines.append(f"file '{escaped}'")
    list_path.write_text("\n".join(list_lines) + "\n", encoding="utf-8")

    result = subprocess.run(
        [
            "ffmpeg",
            "-f",
            "concat",
            "-safe",
            "0",
            "-i",
            str(list_path),
            "-c",
            "copy",
            "-y",
            str(output_path),
        ],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if result.returncode == 0 and output_path.exists() and output_path.stat().st_size > 0:
        return output_path, [list_path, output_path]
    return chunk_path, [list_path]


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
    subprocess.run(["say", "-v", SAY_VOICE, "-r", str(SAY_RATE_WPM), text], check=False)


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
    with_screen_advice=False,
    final_pass=False,
):
    chunks = sorted(chunks_dir.glob("*.opus"))
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
        generate_chunk_advice(
            chunks_dir,
            latest_unprocessed_chunk,
            chunk_texts.get(latest_unprocessed_chunk, ""),
            overlay_text_path,
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--tag",
        type=str,
        default="",
        help="Optional suffix appended to the output directory name.",
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

    segment_output = (
        f"[f=segment:segment_time={CHUNK_SECONDS}:reset_timestamps=1]"
        f"{chunks_dir}/%06d.opus"
    )

    if system_id == mic_id:
        command = [
            "ffmpeg",
            "-f",
            "avfoundation",
            "-i",
            f":{system_id}",
            "-ar",
            "48000",
            "-c:a",
            "libopus",
            "-b:a",
            "160k",
            "-f",
            "tee",
            segment_output,
        ]
        print(f"Recording from aggregate device '{system_name}' to {chunks_dir}")
    else:
        command = [
            "ffmpeg",
            "-f",
            "avfoundation",
            "-i",
            f":{mic_id}",
            "-f",
            "avfoundation",
            "-i",
            f":{system_id}",
            "-filter_complex",
            "[0:a][1:a]amix=inputs=2:duration=longest:dropout_transition=0[a]",
            "-map",
            "[a]",
            "-ar",
            "48000",
            "-c:a",
            "libopus",
            "-b:a",
            "160k",
            "-f",
            "tee",
            segment_output,
        ]
        print(
            f"Recording mic '{mic_name}' + system '{system_name}' to {chunks_dir}"
        )

    print(f"Chunk length: {CHUNK_SECONDS}s")
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
    proc = subprocess.Popen(command)
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
            with_screen_advice=True,
            final_pass=True,
        )
        stop_persistent_overlay(overlay_proc)


if __name__ == "__main__":
    main()
