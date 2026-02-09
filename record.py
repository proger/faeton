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
CHUNK_SECONDS = 30
SCREEN_INPUT_PIXEL_FORMAT = "nv12"
SAY_RATE_WPM = 350
OVERLAY_DURATION_SECONDS = 20
ADVICE_PROMPT_TEMPLATE = """You are coaching a Dota 2 player.
Use the attached screenshot plus the speech transcript context.
Explain what is happening right now and the single next best action.
Keep the response very short: at most 2 sentences.
Think fast, latency is important.

Current chunk speech:
{chunk_text}

All speech so far:
{history_text}
"""
OVERLAY_SCRIPT = r"""
import sys
import tkinter as tk

text = sys.argv[1]
duration_ms = int(float(sys.argv[2]) * 1000)
if not text.strip():
    sys.exit(0)

root = tk.Tk()
root.withdraw()
overlay = tk.Toplevel(root)
overlay.overrideredirect(True)
overlay.attributes("-topmost", True)
overlay.attributes("-alpha", 0.78)
overlay.configure(bg="black")
try:
    # macOS: make the overlay non-activating so it does not steal app focus.
    overlay.tk.call("::tk::unsupported::MacWindowStyle", "style", overlay._w, "help", "noActivates")
except tk.TclError:
    pass

screen_h = overlay.winfo_screenheight()
screen_w = overlay.winfo_screenwidth()
pad = 24
window_w = 620

label = tk.Label(
    overlay,
    text=text,
    justify="left",
    anchor="nw",
    bg="black",
    fg="white",
    padx=18,
    pady=14,
    wraplength=window_w - 36,
    font=("Menlo", 14),
)
label.pack(fill="both", expand=True)

overlay.update_idletasks()
needed_h = label.winfo_reqheight()
max_h = max(200, screen_h - (pad * 2))
window_h = min(needed_h, max_h)
x = screen_w - window_w - pad
y = pad
overlay.geometry(f"{window_w}x{window_h}+{x}+{y}")

if duration_ms > 0:
    overlay.after(duration_ms, root.destroy)
root.mainloop()
"""


def load_whisper_model():
    vendor_dir = pathlib.Path(__file__).resolve().parent / "vendor" / "whisper"
    if str(vendor_dir) not in sys.path:
        sys.path.insert(0, str(vendor_dir))
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
    model, torch, use_fp16, log_mel_spectrogram, pad_or_trim, n_frames, chunk_path
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

    result = model.transcribe(str(chunk_path), fp16=use_fp16)
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
    if result.returncode != 0:
        return False, (result.stderr or "codex failed").strip()
    response = response_path.read_text(encoding="utf-8", errors="replace").strip()
    if not response:
        return False, "codex returned empty advice"
    return True, response


def show_overlay_text(text):
    subprocess.Popen(
        [sys.executable, "-c", OVERLAY_SCRIPT, text, str(OVERLAY_DURATION_SECONDS)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def speak_text(text):
    subprocess.run(["say", "-r", str(SAY_RATE_WPM), text], check=False)


def generate_chunk_advice(chunks_dir, chunk_path, chunk_text):
    advice_path = chunk_path.with_name(f"{chunk_path.stem}_advice.txt")
    if advice_path.exists():
        return

    down4_video_path = chunk_path.with_name(f"{chunk_path.stem}_down4_1fps.mp4")
    if not wait_for_video_chunk(down4_video_path):
        print(f"[{chunk_path.stem}] down4 video chunk missing, skipping advice", flush=True)
        return

    png_path = chunk_path.with_name(f"{chunk_path.stem}_down4_1fps_last.png")
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
    ok, response_or_error = run_codex_advice(png_path, prompt, response_path)
    if not ok:
        print(f"[{chunk_path.stem}] advice failed: {response_or_error}", flush=True)
        return

    response = response_or_error
    advice_path.write_text(
        "Prompt:\n"
        f"{prompt.strip()}\n\n"
        "Response:\n"
        f"{response.strip()}\n",
        encoding="utf-8",
    )
    print(f"[{chunk_path.stem}] advice: {response}", flush=True)
    show_overlay_text(response)
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

    for chunk in chunks:
        if chunk in processed:
            continue
        chunk_text = transcribe_chunk(
            model, torch, use_fp16, log_mel_spectrogram, pad_or_trim, n_frames, chunk
        )
        if with_screen_advice:
            generate_chunk_advice(chunks_dir, chunk, chunk_text)
        processed.add(chunk)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--screen",
        action="store_true",
        help="Also record screen video chunks with hardware acceleration.",
    )
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
    screen_proc = None
    if args.screen:
        screen_segment_pattern_30fps = str(chunks_dir / "%06d_down8_30fps.mp4")
        screen_segment_pattern_1fps = str(chunks_dir / "%06d_down4_1fps.mp4")
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
                "[0:v]split=2[v30][v1];"
                "[v30]fps=30,scale=trunc(iw/8):trunc(ih/8)[v30out];"
                "[v1]fps=1,scale=trunc(iw/4):trunc(ih/4)[v1out]"
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
        ]
        print(
            "Screen chunks "
            f"({screen_name}): {chunks_dir} "
            "(*_down8_30fps.mp4, *_down4_1fps.mp4)"
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
                with_screen_advice=args.screen,
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
            with_screen_advice=args.screen,
            final_pass=True,
        )


if __name__ == "__main__":
    main()
