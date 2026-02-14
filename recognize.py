#!/usr/bin/env python3
import argparse
import os
import sys
import threading
import time
from typing import Any, Dict, List, Optional

import numpy as np
import torch

# Use vendored Whisper implementation.
ROOT = os.path.dirname(os.path.abspath(__file__))
VENDOR_WHISPER = os.path.join(ROOT, "vendor", "whisper")
if VENDOR_WHISPER not in sys.path:
    sys.path.insert(0, VENDOR_WHISPER)

import whisper
from whisper.audio import N_FRAMES, N_SAMPLES, SAMPLE_RATE, log_mel_spectrogram, pad_or_trim
from whisper.decoding import DecodingOptions
from whisper.tokenizer import LANGUAGES, get_tokenizer


def str2bool(v: str) -> bool:
    if isinstance(v, bool):
        return v
    v = v.lower()
    if v in {"1", "true", "t", "yes", "y", "on"}:
        return True
    if v in {"0", "false", "f", "no", "n", "off"}:
        return False
    raise argparse.ArgumentTypeError(f"invalid bool value: {v}")


def detect_language_if_needed(model: whisper.Whisper, mel: torch.Tensor, language: Optional[str]) -> str:
    if language:
        return language
    if not model.is_multilingual:
        return "en"

    mel_segment = pad_or_trim(mel, N_FRAMES).to(model.device)
    if model.device.type != "cpu":
        mel_segment = mel_segment.half()
    _, probs = model.detect_language(mel_segment)
    return max(probs, key=probs.get)


def decode_window(
    *,
    model: whisper.Whisper,
    tokenizer,
    language: str,
    task: str,
    temperature: float,
    dtype: torch.dtype,
    audio_chunk: np.ndarray,
):
    mel = log_mel_spectrogram(pad_or_trim(audio_chunk, N_SAMPLES), model.dims.n_mels).to(model.device)
    mel = mel.to(dtype)

    options = DecodingOptions(
        task=task,
        language=language,
        temperature=temperature,
        fp16=(dtype == torch.float16),
    )
    result = model.decode(mel, options)

    text_tokens = [tok for tok in result.tokens if tok < tokenizer.eot]
    text = tokenizer.decode(text_tokens).strip()
    return result, text


def run_microphone(args, model: whisper.Whisper, dtype: torch.dtype) -> None:
    import sounddevice as sd

    window_samples = int(args.window_seconds * SAMPLE_RATE)
    stride_samples = int(args.stride_seconds * SAMPLE_RATE)
    if window_samples <= 0 or stride_samples <= 0:
        raise ValueError("window-seconds and stride-seconds must be > 0")

    max_buffer_samples = window_samples + (4 * stride_samples)
    audio_buffer = np.zeros(0, dtype=np.float32)
    buffer_lock = threading.Lock()
    overflow_flag = False
    windows: List[Dict[str, Any]] = []
    i = 0

    language: Optional[str] = args.language
    tokenizer = (
        get_tokenizer(
            model.is_multilingual,
            num_languages=model.num_languages,
            language=language,
            task=args.task,
        )
        if language is not None
        else None
    )

    if language is not None:
        print(f"language={language} ({LANGUAGES.get(language, language)})")

    print("listening from microphone... press Ctrl+C to stop")

    def on_audio(indata, frames, _time_info, status):
        nonlocal audio_buffer, overflow_flag
        if status.input_overflow:
            overflow_flag = True
        mono = indata[:, 0].astype(np.float32, copy=False)
        with buffer_lock:
            audio_buffer = np.concatenate([audio_buffer, mono])
            if audio_buffer.shape[0] > max_buffer_samples:
                audio_buffer = audio_buffer[-max_buffer_samples:]

    with sd.InputStream(
        samplerate=SAMPLE_RATE,
        channels=1,
        dtype="float32",
        blocksize=0,
        device=args.mic_device,
        latency="high",
        callback=on_audio,
    ):
        next_tick = time.monotonic() + args.stride_seconds
        while True:
            sleep_for = next_tick - time.monotonic()
            if sleep_for > 0:
                time.sleep(sleep_for)
            next_tick += args.stride_seconds

            if overflow_flag:
                print("warning: microphone overflow detected", file=sys.stderr)
                overflow_flag = False

            with buffer_lock:
                history = audio_buffer[-window_samples:].copy()
            if history.size == 0:
                continue

            mel_for_lang = log_mel_spectrogram(history, model.dims.n_mels)
            if language is None:
                language = detect_language_if_needed(model, mel_for_lang, language)
                tokenizer = get_tokenizer(
                    model.is_multilingual,
                    num_languages=model.num_languages,
                    language=language,
                    task=args.task,
                )
                print(f"language={language} ({LANGUAGES.get(language, language)})")

            assert language is not None
            assert tokenizer is not None

            result, text = decode_window(
                model=model,
                tokenizer=tokenizer,
                language=language,
                task=args.task,
                temperature=args.temperature,
                dtype=dtype,
                audio_chunk=history,
            )

            start_sec = i * args.stride_seconds
            end_sec = start_sec + args.window_seconds
            print(f"[{start_sec:8.2f}s - {end_sec:8.2f}s] {text}")

            windows.append(
                {
                    "window_index": i,
                    "start": start_sec,
                    "end": end_sec,
                    "text": text,
                    "avg_logprob": result.avg_logprob,
                    "no_speech_prob": result.no_speech_prob,
                    "compression_ratio": result.compression_ratio,
                }
            )
            i += 1


def run_file(args, model: whisper.Whisper, dtype: torch.dtype) -> None:
    audio = whisper.load_audio(args.audio)

    mel_full = log_mel_spectrogram(audio, model.dims.n_mels)
    language = detect_language_if_needed(model, mel_full, args.language)
    tokenizer = get_tokenizer(
        model.is_multilingual,
        num_languages=model.num_languages,
        language=language,
        task=args.task,
    )

    window_samples = int(args.window_seconds * SAMPLE_RATE)
    stride_samples = int(args.stride_seconds * SAMPLE_RATE)
    if window_samples <= 0 or stride_samples <= 0:
        raise ValueError("window-seconds and stride-seconds must be > 0")

    windows: List[Dict[str, Any]] = []

    print(f"language={language} ({LANGUAGES.get(language, language)})")

    i = 0
    start_sample = 0
    while start_sample < len(audio) or i == 0:
        end_sample = start_sample + window_samples
        audio_chunk = audio[start_sample:end_sample]

        result, text = decode_window(
            model=model,
            tokenizer=tokenizer,
            language=language,
            task=args.task,
            temperature=args.temperature,
            dtype=dtype,
            audio_chunk=audio_chunk,
        )

        start_sec = start_sample / SAMPLE_RATE
        end_sec = end_sample / SAMPLE_RATE
        print(f"[{start_sec:8.2f}s - {end_sec:8.2f}s] {text}")

        windows.append(
            {
                "window_index": i,
                "start": start_sec,
                "end": end_sec,
                "text": text,
                "avg_logprob": result.avg_logprob,
                "no_speech_prob": result.no_speech_prob,
                "compression_ratio": result.compression_ratio,
            }
        )

        i += 1
        start_sample += stride_samples

def main() -> None:
    parser = argparse.ArgumentParser(description="Sliding-window Whisper recognizer with 1s delay")
    parser.add_argument("audio", nargs="?", help="Optional input audio file path; omit to use live microphone")
    parser.add_argument("--model", default="turbo", help="Whisper model name/path")
    parser.add_argument("--device", default=None, help="Torch device, e.g. cpu, cuda")
    parser.add_argument("--mic-device", default=None, help="Microphone device id/name for sounddevice")
    parser.add_argument("--language", default="en", help="Language code, e.g. en")
    parser.add_argument("--task", default="transcribe", choices=["transcribe", "translate"])
    parser.add_argument("--temperature", type=float, default=0.0)
    parser.add_argument("--fp16", type=str2bool, default=True)
    parser.add_argument("--window-seconds", type=float, default=30.0)
    parser.add_argument("--stride-seconds", type=float, default=1.0)
    args = parser.parse_args()

    model = whisper.load_model(args.model, device=args.device)
    dtype = torch.float16 if args.fp16 and model.device.type != "cpu" else torch.float32

    try:
        if args.audio:
            run_file(args, model, dtype)
        else:
            run_microphone(args, model, dtype)
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
