#!/usr/bin/env python3
import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import time

SCREENSHOT_DIR = pathlib.Path(
    "/Users/vol/Library/Application Support/Steam/steamapps/common/dota 2 beta/game/dota/screenshots"
)
IMAGE_EXTS = {".png", ".jpg", ".jpeg"}
OVERLAY_DURATION_SECONDS = 0
SAY_RATE_WPM = 350
ANALYSIS_PROMPT_TEMPLATE = (
    "Look only at the image I attached in this request, and do not inspect any other images or files. "
    "Identify what hero the game user is playing. Identify five heroes on each side. "
    "Then suggest an anti-lineup item build for the user's hero. List items, and for each item add a short "
    "note saying which enemy hero(es) it is good against and quickly why. Keep each note brief. Use this format:\n"
    "Suggested <Hero Name> anti-lineup item build:\n"
    "- <item>: good vs <Enemy Hero>, <Enemy Hero> - <very short why>\n"
    "- <item>: good vs <Enemy Hero> - <very short why>\n"
    "Return only the analysis in plain text, with no preface and no mention of saving files. "
    "Save the result to {output_file}."
)
OVERLAY_SCRIPT = r"""
import pathlib
import sys
import tkinter as tk

path = pathlib.Path(sys.argv[1])
duration_ms = int(float(sys.argv[2]) * 1000)
text = path.read_text(encoding="utf-8", errors="replace").strip()
if not text:
    sys.exit(0)

root = tk.Tk()
root.withdraw()
overlay = tk.Toplevel(root)
overlay.overrideredirect(True)
overlay.attributes("-topmost", True)
overlay.attributes("-alpha", 0.78)
overlay.configure(bg="black")

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


def run_osascript(script):
    result = subprocess.run(
        ["osascript", "-e", script],
        check=False,
        capture_output=True,
        text=True,
    )
    return result.returncode, result.stdout.strip(), result.stderr.strip()


def run_codex_analysis(image_path, output_path):
    prompt = ANALYSIS_PROMPT_TEMPLATE.format(output_file=output_path)
    subprocess.run(
        [
            "codex",
            "exec",
            "--skip-git-repo-check",
            "-i",
            image_path,
            "-o",
            output_path,
            "--",
            prompt,
        ],
        check=True,
    )


def speak_result(output_path):
    subprocess.run(["say", "-r", str(SAY_RATE_WPM), "-f", output_path], check=False)


def show_overlay(output_path):
    subprocess.Popen(
        [sys.executable, "-c", OVERLAY_SCRIPT, output_path, str(OVERLAY_DURATION_SECONDS)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def next_timestamp_files():
    timestamp = int(time.time())
    while pathlib.Path(f"{timestamp}.png").exists() or pathlib.Path(
        f"{timestamp}_gamestate.txt"
    ).exists():
        timestamp += 1
    return f"{timestamp}.png", f"{timestamp}_gamestate.txt"


def process_existing_screenshot(source_path):
    filename, output_file = next_timestamp_files()
    shutil.copy2(source_path, filename)
    print(filename)
    run_codex_analysis(filename, output_file)
    show_overlay(output_file)
    speak_result(output_file)


def screenshot_files_in_dir():
    if not SCREENSHOT_DIR.is_dir():
        return []
    return [
        path
        for path in SCREENSHOT_DIR.iterdir()
        if path.is_file() and path.suffix.lower() in IMAGE_EXTS
    ]


def watch_screenshots():
    if not SCREENSHOT_DIR.is_dir():
        raise SystemExit("Screenshot directory does not exist.")

    known = {path: path.stat().st_mtime_ns for path in screenshot_files_in_dir()}
    print(f"Watching {SCREENSHOT_DIR} for new screenshots...")
    while True:
        time.sleep(1)
        current = {}
        for path in screenshot_files_in_dir():
            try:
                mtime_ns = path.stat().st_mtime_ns
            except FileNotFoundError:
                continue
            current[path] = mtime_ns
            if path not in known or mtime_ns > known[path]:
                # Give Steam a moment to finish writing before copying.
                time.sleep(0.5)
                process_existing_screenshot(path)
        known = current


def get_window_id_for_process(process_name):
    script = (
        'tell application "System Events" to tell process "{name}" '
        'to if (count of windows) > 0 then get id of first window'
    ).format(name=process_name)
    code, out, _ = run_osascript(script)
    if code == 0 and re.fullmatch(r"\d+", out):
        return out
    return None


def find_dota_process_name():
    code, out, _ = run_osascript(
        'tell application "System Events" to get name of (processes where background only is false)'
    )
    if code != 0 or not out:
        return None
    names = [name.strip() for name in out.split(",") if name.strip()]
    for name in names:
        if "dota" in name.lower():
            return name
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--watch",
        action="store_true",
        help="Monitor the Dota screenshot directory and process new screenshots.",
    )
    args = parser.parse_args()

    if args.watch:
        watch_screenshots()
        return

    filename, output_file = next_timestamp_files()
    candidates = ["Dota 2", "Dota", "dota2", "dota"]
    window_id = None
    for name in candidates:
        window_id = get_window_id_for_process(name)
        if window_id:
            break

    if not window_id:
        guessed = find_dota_process_name()
        if guessed:
            window_id = get_window_id_for_process(guessed)

    if not window_id:
        if not SCREENSHOT_DIR.is_dir():
            raise SystemExit(
                "Could not find a Dota window, and the screenshot directory does not exist."
            )

        latest = None
        latest_mtime = -1.0
        for path in screenshot_files_in_dir():
            mtime = path.stat().st_mtime
            if mtime > latest_mtime:
                latest = path
                latest_mtime = mtime

        if not latest:
            raise SystemExit(
                "Could not find a Dota window, and no screenshots were found in the screenshot directory."
            )

        shutil.copy2(latest, filename)
        print(filename)
        run_codex_analysis(filename, output_file)
        show_overlay(output_file)
        speak_result(output_file)
        return

    subprocess.run(["screencapture", "-l", window_id, filename], check=True)
    print(filename)
    run_codex_analysis(filename, output_file)
    show_overlay(output_file)
    speak_result(output_file)


if __name__ == "__main__":
    main()
