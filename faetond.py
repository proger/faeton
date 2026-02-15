"""faetond HTTP service.

Keep route changes in sync with `faetond.conf` (Nginx locations):
- /sub and /sub/{ts}
- /pub and /pub/{ts}
- /png/{filename}
"""

import argparse
import asyncio
import os
import re
import time
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import PlainTextResponse, Response, StreamingResponse

DEFAULT_DATA_DIR = os.environ.get("FAETOND_DATA_DIR", "./faetond_data")


class Store:
    def __init__(self, base_dir: Path):
        self.base_dir = base_dir
        self.events_dir = self.base_dir / "events"
        self.text_dir = self.base_dir / "blobs" / "text"
        self.png_dir = self.base_dir / "blobs" / "png"
        self._write_lock = asyncio.Lock()

    def ensure_dirs(self) -> None:
        self.events_dir.mkdir(parents=True, exist_ok=True)
        self.text_dir.mkdir(parents=True, exist_ok=True)
        self.png_dir.mkdir(parents=True, exist_ok=True)


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
        if not raw_line.strip() or ":" not in raw_line:
            continue
        key, value = raw_line.split(":", 1)
        payload[key.strip()] = value.lstrip().replace("\\n", "\n")
    return payload


def _write_event_at_path(event_path: Path, event: dict) -> str:
    ts = event_path.name
    event["ts"] = ts
    event_path.write_text(_format_kv_lines(event), encoding="utf-8")
    return ts


def _iter_events_after(events_dir: Path, ts: float):
    for path in sorted(events_dir.iterdir(), key=lambda p: float(p.name)):
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


def create_app(data_dir: str = DEFAULT_DATA_DIR) -> FastAPI:
    store = Store(Path(data_dir).resolve())
    store.ensure_dirs()
    updates_ready = asyncio.Event()

    api = FastAPI(title="faetond")

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

        async with store._write_lock:
            store.ensure_dirs()
            ts = _now_ts()
            while (store.events_dir / ts).exists():
                ts = _now_ts()

            text_path = store.text_dir / f"{ts}.txt"
            text_path.write_text(text, encoding="utf-8")

            event = {
                "type": "text",
                "text": text,
                "blob": f"blobs/text/{ts}.txt",
            }
            event_ts = _write_event_at_path(store.events_dir / ts, event)

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

        async with store._write_lock:
            store.ensure_dirs()
            event_path = store.events_dir / ts
            if event_path.exists():
                raise HTTPException(status_code=409, detail="event ts already exists")

            text_path = store.text_dir / f"{ts}.txt"
            text_path.write_text(text, encoding="utf-8")

            event = {
                "type": "text",
                "text": text,
                "blob": f"blobs/text/{ts}.txt",
            }
            event_ts = _write_event_at_path(event_path, event)

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

        async with store._write_lock:
            store.ensure_dirs()
            png_path = store.png_dir / safe_name
            png_path.write_bytes(body)

            ts = _now_ts()
            while (store.events_dir / ts).exists():
                ts = _now_ts()

            event = {
                "type": "png",
                "filename": safe_name,
                "url": f"/png/{safe_name}",
                "blob": f"blobs/png/{safe_name}",
            }
            event_ts = _write_event_at_path(store.events_dir / ts, event)

        updates_ready.set()
        return PlainTextResponse(_format_kv_lines({"ok": "true", "ts": event_ts, "filename": safe_name}))

    @api.get("/png/{filename}")
    async def get_png(filename: str):
        try:
            safe_name = _safe_filename(filename)
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc

        store.ensure_dirs()
        png_path = store.png_dir / safe_name
        if not png_path.exists():
            raise HTTPException(status_code=404, detail="not found")

        return Response(content=png_path.read_bytes(), media_type="image/png")

    async def _sub_impl(start_ts: float, req: Request):
        async def event_stream():
            cursor = start_ts
            try:
                while True:
                    if await req.is_disconnected():
                        break

                    store.ensure_dirs()
                    sent_any = False
                    for event_ts, payload in _iter_events_after(store.events_dir, cursor):
                        if await req.is_disconnected():
                            return
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
                        except asyncio.TimeoutError:
                            yield ": keepalive\n\n"
            except asyncio.CancelledError:
                return

        return StreamingResponse(event_stream(), media_type="text/event-stream")

    @api.get("/sub")
    async def sub_root_alias(req: Request):
        return await _sub_impl(0.0, req)

    @api.get("/sub/{ts}")
    async def sub_alias(ts: str, req: Request):
        if not re.fullmatch(r"\d+(?:\.\d+)?", ts):
            raise HTTPException(status_code=400, detail="ts must be numeric unix timestamp")
        return await _sub_impl(float(ts), req)

    return api


app = create_app()


def main() -> None:
    parser = argparse.ArgumentParser(description="faetond local filesystem server")
    parser.add_argument("--host", default=os.environ.get("FAETOND_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("FAETOND_PORT", "8008")))
    parser.add_argument("--data-dir", default=DEFAULT_DATA_DIR)
    args = parser.parse_args()

    import uvicorn

    uvicorn.run(
        create_app(args.data_dir),
        host=args.host,
        port=args.port,
        proxy_headers=True,
        forwarded_allow_ips=os.environ.get("FAETOND_FORWARDED_ALLOW_IPS", "127.0.0.1"),
    )


if __name__ == "__main__":
    main()
