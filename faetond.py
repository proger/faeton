import os
import time
import asyncio
import re
from pathlib import Path

import modal

app = modal.App("faetond")
image = modal.Image.debian_slim().pip_install("fastapi")
volume = modal.Volume.from_name("faetond-store", create_if_missing=True)

MOUNT_PATH = "/data"
EVENTS_DIR = Path(MOUNT_PATH) / "events"
TEXT_DIR = Path(MOUNT_PATH) / "blobs" / "text"
PNG_DIR = Path(MOUNT_PATH) / "blobs" / "png"


def _ensure_dirs() -> None:
    EVENTS_DIR.mkdir(parents=True, exist_ok=True)
    TEXT_DIR.mkdir(parents=True, exist_ok=True)
    PNG_DIR.mkdir(parents=True, exist_ok=True)


def _now_ts() -> str:
    return f"{time.time():.6f}"


def _safe_filename(filename: str) -> str:
    base = os.path.basename(filename)
    if not base or base in {".", ".."}:
        raise ValueError("invalid filename")
    return base


def _format_kv_lines(payload: dict[str, object]) -> str:
    lines = []
    for key, value in payload.items():
        text = str(value).replace("\n", "\\n")
        lines.append(f"{key}: {text}")
    return "\n".join(lines) + "\n"


def _parse_kv_lines(text: str) -> dict[str, str]:
    payload: dict[str, str] = {}
    for raw_line in text.splitlines():
        if not raw_line.strip():
            continue
        if ":" not in raw_line:
            continue
        key, value = raw_line.split(":", 1)
        payload[key.strip()] = value.lstrip().replace("\\n", "\n")
    return payload


def _write_event(event: dict) -> str:
    ts = _now_ts()
    while (EVENTS_DIR / ts).exists():
        ts = _now_ts()
    event_path = EVENTS_DIR / ts
    event["ts"] = ts
    event_path.write_text(_format_kv_lines(event), encoding="utf-8")
    return ts


def _write_event_at_ts(event: dict, ts: str) -> str:
    event_path = EVENTS_DIR / ts
    event["ts"] = ts
    event_path.write_text(_format_kv_lines(event), encoding="utf-8")
    return ts


def _iter_events_after(ts: float):
    for path in sorted(EVENTS_DIR.iterdir(), key=lambda p: float(p.name)):
        try:
            event_ts = float(path.name)
        except ValueError:
            continue
        if event_ts <= ts:
            continue
        try:
            payload = _parse_kv_lines(path.read_text(encoding="utf-8"))
        except Exception:
            continue
        yield event_ts, payload


@app.function(image=image, volumes={MOUNT_PATH: volume})
@modal.asgi_app()
def web():
    from fastapi import FastAPI, HTTPException, Request
    from fastapi.responses import PlainTextResponse, Response, StreamingResponse

    api = FastAPI()
    updates_ready = asyncio.Event()

    @api.post("/pub")
    async def post_pub(req: Request):
        content_type = (req.headers.get("content-type") or "").split(";")[0].strip().lower()
        if content_type != "text/plain":
            raise HTTPException(status_code=415, detail="content-type must be text/plain")

        body = await req.body()
        try:
            text = body.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise HTTPException(status_code=400, detail="text/plain must be utf-8") from exc

        volume.reload()
        _ensure_dirs()
        ts = _now_ts()

        text_path = TEXT_DIR / f"{ts}.txt"
        text_path.write_text(text, encoding="utf-8")

        event = {
            "type": "text",
            "text": text,
            "blob": f"blobs/text/{ts}.txt",
        }
        event_ts = _write_event(event)
        volume.commit()
        updates_ready.set()
        return PlainTextResponse(_format_kv_lines({"ok": "true", "ts": event_ts}))

    @api.post("/pub/{ts}")
    async def post_pub_with_ts(ts: str, req: Request):
        if not re.fullmatch(r"\d+(?:\.\d+)?", ts):
            raise HTTPException(status_code=400, detail="ts must be numeric unix timestamp")

        content_type = (req.headers.get("content-type") or "").split(";")[0].strip().lower()
        if content_type != "text/plain":
            raise HTTPException(status_code=415, detail="content-type must be text/plain")

        body = await req.body()
        try:
            text = body.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise HTTPException(status_code=400, detail="text/plain must be utf-8") from exc

        volume.reload()
        _ensure_dirs()
        if (EVENTS_DIR / ts).exists():
            raise HTTPException(status_code=409, detail="event ts already exists")

        text_path = TEXT_DIR / f"{ts}.txt"
        text_path.write_text(text, encoding="utf-8")

        event = {
            "type": "text",
            "text": text,
            "blob": f"blobs/text/{ts}.txt",
        }
        event_ts = _write_event_at_ts(event, ts)
        volume.commit()
        updates_ready.set()
        return PlainTextResponse(_format_kv_lines({"ok": "true", "ts": event_ts}))

    @api.post("/png/{filename}")
    async def post_png(filename: str, req: Request):
        content_type = (req.headers.get("content-type") or "").split(";")[0].strip().lower()
        if content_type != "image/png":
            raise HTTPException(status_code=415, detail="content-type must be image/png")

        try:
            safe_name = _safe_filename(filename)
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc

        body = await req.body()
        if not body:
            raise HTTPException(status_code=400, detail="empty body")

        volume.reload()
        _ensure_dirs()

        png_path = PNG_DIR / safe_name
        png_path.write_bytes(body)

        event = {
            "type": "png",
            "filename": safe_name,
            "url": f"/png/{safe_name}",
            "blob": f"blobs/png/{safe_name}",
        }
        ts = _write_event(event)
        volume.commit()
        updates_ready.set()
        return PlainTextResponse(_format_kv_lines({"ok": "true", "ts": ts, "filename": safe_name}))

    @api.get("/png/{filename}")
    async def get_png(filename: str):
        try:
            safe_name = _safe_filename(filename)
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc

        volume.reload()
        png_path = PNG_DIR / safe_name
        if not png_path.exists():
            raise HTTPException(status_code=404, detail="not found")

        return Response(content=png_path.read_bytes(), media_type="image/png")

    async def _sub_impl(start_ts: float):
        async def event_stream():
            cursor = start_ts
            while True:
                volume.reload()
                _ensure_dirs()
                sent_any = False
                for event_ts, payload in _iter_events_after(cursor):
                    cursor = max(cursor, event_ts)
                    sent_any = True
                    yield f"id: {payload.get('ts', event_ts)}\n"
                    for line in _format_kv_lines(payload).splitlines():
                        yield f"data: {line}\n"
                    yield "\n"
                if not sent_any:
                    try:
                        await asyncio.wait_for(updates_ready.wait(), timeout=15.0)
                        updates_ready.clear()
                    except TimeoutError:
                        yield ": keepalive\n\n"

        return StreamingResponse(event_stream(), media_type="text/event-stream")

    @api.get("/")
    async def sub_root():
        return await _sub_impl(0.0)

    @api.get("/{ts}")
    async def sub(ts: str):
        if not re.fullmatch(r"\d+(?:\.\d+)?", ts):
            raise HTTPException(status_code=400, detail="ts must be numeric unix timestamp")
        start_ts = float(ts)
        return await _sub_impl(start_ts)

    return api
