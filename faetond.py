"""faetond HTTP service.

Keep route changes in sync with `faetond.conf` (Nginx locations):
- /sub and /sub/{ts}
- /pub and /pub/{ts}
- /png/{filename}
"""

import argparse
import asyncio
import html
import os
import re
import shutil
import subprocess
import time
import uuid
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import HTMLResponse, PlainTextResponse, Response, StreamingResponse

DEFAULT_DATA_DIR = os.environ.get("FAETOND_DATA_DIR", "./faetond_data")
DEFAULT_CODEX_MODEL = os.environ.get("FAETOND_CODEX_MODEL", "gpt-5.3-codex")
CODEX_LOOP_INTERVAL_SECONDS = float(os.environ.get("FAETOND_CODEX_INTERVAL", "2.0"))
KNOWN_GAME_STATE_FILE = "_known_game_state.txt"

MULTI_HOST_PROMPT = """You are supporting a Dota 2 team.
Use the attached screenshot context from multiple hosts.
You may see your past advice in the top-right HUD overlay; avoid repeating it unless game state changed.
React directly to the latest player requests recorded in Known game state; prioritize the newest explicit ask.
Look at the team composition and propose a team tactic to try given items and abilities that they have.
Prioritise advice how to position the team on the map given information from each team member and current fighting activity.
Prioritise early-game positioning guidance (lanes, rune control, rotations, vision posture) before mid/late-game macro plans.
When in pregame screens, advise 5-hero combos that fit together and say why.
Include item advice when appropriate.
Identify two heroes visible on screen and discuss their most likely interaction in this moment.
Prioritize guidance that stays useful over the next minute: durable principles, likely next decisions, and fallback plans.
Avoid repeating previous advice unless there is a strong new reason to repeat it.
Do not repeat your previous response unless game state changed meaningfully.
Vary your situation modeling and phrasing across updates; avoid repeating the same framing too often.
Keep the response very short: exactly 1 sentence.
Be extremely concise: cap ADVICE to about 8-14 words.
Use mostly plain language; casual slang is optional and should be rare.
Think fast, latency is important.
Output format is mandatory:
ADVICE: <exactly 1 sentence actionable coaching response>
NEW GAME STATE: <only new game-state facts not already in Known game state; concise semicolon-separated facts, or 'none'>
When adding NEW GAME STATE, assume it will be appended after existing Known game state; do not repeat old facts.
In NEW GAME STATE, always include the current in-game time (or best visible time estimate) in each new fact when available.
"""


class Store:
    def __init__(self, base_dir: Path):
        self.base_dir = base_dir
        self.games_dir = self.base_dir / "games"
        self.current_game_file = self.base_dir / "_current_game.txt"
        self.active_game_id = ""
        self.game_dir = self.base_dir
        self.events_dir = self.base_dir / "events"
        self.text_dir = self.base_dir / "blobs" / "text"
        self.png_dir = self.base_dir / "blobs" / "png"
        self._write_lock = asyncio.Lock()

    def ensure_dirs(self) -> None:
        self.base_dir.mkdir(parents=True, exist_ok=True)
        self.games_dir.mkdir(parents=True, exist_ok=True)
        if not self.active_game_id:
            game_id = self._read_current_game_id()
            if not game_id:
                game_id = _new_game_id()
            self._set_active_game(game_id)
            self._write_current_game_id(game_id)
        self.game_dir.mkdir(parents=True, exist_ok=True)
        self.events_dir.mkdir(parents=True, exist_ok=True)
        self.text_dir.mkdir(parents=True, exist_ok=True)
        self.png_dir.mkdir(parents=True, exist_ok=True)

    def _set_active_game(self, game_id: str) -> None:
        self.active_game_id = game_id
        self.game_dir = self.games_dir / game_id
        self.events_dir = self.game_dir / "events"
        self.text_dir = self.game_dir / "blobs" / "text"
        self.png_dir = self.game_dir / "blobs" / "png"

    def _read_current_game_id(self) -> str:
        if not self.current_game_file.exists():
            return ""
        try:
            game_id = self.current_game_file.read_text(encoding="utf-8").strip()
        except Exception:
            return ""
        if not re.fullmatch(r"[A-Za-z0-9._-]+", game_id):
            return ""
        return game_id

    def _write_current_game_id(self, game_id: str) -> None:
        self.current_game_file.write_text(game_id + "\n", encoding="utf-8")

    def rotate_game(self) -> str:
        self.ensure_dirs()
        game_id = _new_game_id()
        while (self.games_dir / game_id).exists():
            game_id = _new_game_id()
        self._set_active_game(game_id)
        self._write_current_game_id(game_id)
        self.ensure_dirs()
        return game_id


def _now_ts() -> str:
    return f"{time.time():.6f}"


def _new_game_id() -> str:
    return f"game_{time.strftime('%Y%m%d_%H%M%S')}_{uuid.uuid4().hex[:8]}"


def _safe_filename(filename: str) -> str:
    base = os.path.basename(filename)
    if not base or base in {".", ".."}:
        raise ValueError("invalid filename")
    return base


def _client_host(req: Request) -> str:
    client = req.client
    if client and client.host:
        return str(client.host)
    return ""


def _format_kv_lines(payload: dict[str, object]) -> str:
    lines = []
    for key, value in payload.items():
        text = str(value).replace("\n", "\\n")
        lines.append(f"{key}: {text}")
    return "\n".join(lines) + "\n"


def _blob_relpath(store: "Store", rel: str) -> str:
    if store.active_game_id:
        return f"games/{store.active_game_id}/{rel}"
    return rel


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


def _uuid_v1_machine_from_filename(filename: str) -> str | None:
    name = filename
    if name.lower().endswith(".png"):
        name = name[:-4]
    try:
        u = uuid.UUID(name)
    except ValueError:
        return None
    if u.version != 1:
        return None
    return f"{u.node:012x}"


def _latest_png_rows_by_node(store: "Store") -> list[dict[str, str]]:
    latest_by_node: dict[str, dict[str, str]] = {}
    for _, payload in _iter_events_after(store.events_dir, 0.0):
        if payload.get("type") != "png":
            continue
        filename = payload.get("filename", "")
        if not filename:
            continue
        node = _uuid_v1_machine_from_filename(filename)
        if not node:
            continue
        latest_by_node[node] = {
            "node": node,
            "ts": payload.get("ts", ""),
            "filename": filename,
            "url": payload.get("url", f"/png/{filename}"),
        }
    return sorted(latest_by_node.values(), key=lambda x: x.get("node", ""))


def _run_codex_for_rows(rows: list[dict[str, str]], store: "Store") -> str | None:
    codex_bin = shutil.which("codex")
    if not codex_bin:
        return None

    image_paths: list[Path] = []
    context_lines: list[str] = []
    for r in rows:
        filename = r.get("filename", "")
        if not filename:
            continue
        path = store.png_dir / filename
        if not path.exists():
            continue
        image_paths.append(path)
        context_lines.append(f"host={r.get('node','?')} ts={r.get('ts','?')} file={filename}")
    if not image_paths:
        return None

    known_game_state = _load_known_game_state(store)
    pregame_hint = (
        "\n\nParty size hint:\n"
        f"{len(rows)} players in party.\n"
        "Keep advice diverse across updates; include pregame suggestions about picking characters and trying skills.\n"
    )
    prompt = (
        MULTI_HOST_PROMPT
        + "\n\nMultiplayer host screenshots:\n"
        + "\n".join(context_lines)
        + pregame_hint
        + f"\n\nKnown game state:\n{known_game_state}\n"
    )
    codex_dir = store.game_dir / "codex"
    codex_dir.mkdir(parents=True, exist_ok=True)
    codex_log = store.game_dir / "codex.log"
    log_ts = _now_ts()
    prompt_log = codex_dir / f"{log_ts}_prompt.txt"
    response_log = codex_dir / f"{log_ts}_response.txt"
    prompt_log.write_text(prompt, encoding="utf-8")
    cmd = [
        codex_bin,
        "-a",
        "never",
        "exec",
        "-m",
        DEFAULT_CODEX_MODEL,
        "-c",
        "model_reasoning_effort=low",
        "--skip-git-repo-check",
    ]
    for p in image_paths:
        cmd.extend(["-i", str(p)])
    cmd.extend(["--", prompt])

    try:
        result = subprocess.run(
            cmd,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=120,
        )
    except Exception:
        with codex_log.open("a", encoding="utf-8") as f:
            f.write(f"{log_ts}\tstatus=exception\thosts={len(rows)}\n")
        response_log.write_text("codex invocation exception", encoding="utf-8")
        print("codex error: invocation exception", flush=True)
        return None
    if result.returncode != 0:
        err = (result.stderr or "codex failed").strip()
        with codex_log.open("a", encoding="utf-8") as f:
            f.write(f"{log_ts}\tstatus=error\thosts={len(rows)}\n")
        response_log.write_text(err, encoding="utf-8")
        print(f"codex error stderr: {err}", flush=True)
        return None

    out = (result.stdout or "").strip()
    if not out:
        with codex_log.open("a", encoding="utf-8") as f:
            f.write(f"{log_ts}\tstatus=empty\thosts={len(rows)}\n")
        response_log.write_text("(empty response)", encoding="utf-8")
        return None
    response_log.write_text(out + "\n", encoding="utf-8")
    advice = _extract_section(out, "ADVICE")
    if not advice:
        advice = out.strip()
    new_game_state = _extract_section(out, "NEW GAME STATE")
    _update_known_game_state(store, new_game_state)
    with codex_log.open("a", encoding="utf-8") as f:
        f.write(
            f"{log_ts}\tstatus=ok\thosts={len(rows)}\tadvice={advice.replace(chr(10), ' ').strip()}\n"
        )
    return advice or None


def _known_game_state_path(store: "Store") -> Path:
    return store.game_dir / KNOWN_GAME_STATE_FILE


def _find_png_path(store: "Store", safe_name: str) -> Path | None:
    current = store.png_dir / safe_name
    if current.exists():
        return current
    legacy = store.base_dir / "blobs" / "png" / safe_name
    if legacy.exists():
        return legacy
    if not store.games_dir.exists():
        return None
    for game_dir in sorted(store.games_dir.iterdir(), key=lambda p: p.name, reverse=True):
        if not game_dir.is_dir():
            continue
        candidate = game_dir / "blobs" / "png" / safe_name
        if candidate.exists():
            return candidate
    return None


def _latest_user_requests(store: "Store", limit: int = 6) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for _, payload in _iter_events_after(store.events_dir, 0.0):
        if payload.get("type") != "text":
            continue
        if payload.get("source") != "user":
            continue
        text = (payload.get("text") or "").strip()
        if not text:
            continue
        node = (payload.get("node") or "").strip().lower() or "unknown"
        rows.append({"ts": payload.get("ts", ""), "text": text, "node": node})
    if limit <= 0:
        return rows
    return rows[-limit:]


def _latest_png_node_for_client(store: "Store", client_host: str) -> str | None:
    host = (client_host or "").strip()
    if not host:
        return None
    best: str | None = None
    for _, payload in _iter_events_after(store.events_dir, 0.0):
        if payload.get("type") != "png":
            continue
        if (payload.get("client") or "").strip() != host:
            continue
        node = (payload.get("node") or "").strip().lower()
        if not node:
            filename = payload.get("filename", "")
            node = _uuid_v1_machine_from_filename(filename) or ""
        if node:
            best = node
    return best


def _resolve_user_node_tag(req: Request, store: "Store") -> str:
    by_client = _latest_png_node_for_client(store, _client_host(req))
    if by_client:
        return by_client
    return "unknown"


def _latest_user_request_marker(store: "Store") -> str:
    rows = _latest_user_requests(store, limit=1)
    if not rows:
        return ""
    node = rows[0].get("node", "unknown")
    ts = rows[0].get("ts", "")
    text = rows[0].get("text", "").replace("\n", " ").strip()
    return f"{ts}:{node}:{text[:160]}"


def _load_last_full_prompt(store: "Store") -> str:
    codex_dir = store.game_dir / "codex"
    if not codex_dir.exists():
        return "(none yet)"
    latest_prompt: Path | None = None
    for path in codex_dir.glob("*_prompt.txt"):
        if latest_prompt is None or path.name > latest_prompt.name:
            latest_prompt = path
    if latest_prompt is None:
        return "(none yet)"
    try:
        text = latest_prompt.read_text(encoding="utf-8", errors="replace").strip()
    except Exception:
        return "(failed to read latest prompt)"
    return text if text else "(empty prompt)"


def _load_known_game_state(store: "Store") -> str:
    path = _known_game_state_path(store)
    if not path.exists():
        return "(none yet)"
    text = path.read_text(encoding="utf-8", errors="replace").strip()
    return text if text else "(none yet)"


def _update_known_game_state(store: "Store", new_state_text: str) -> None:
    cleaned = (new_state_text or "").strip()
    if not cleaned or cleaned.lower() == "none":
        return
    path = _known_game_state_path(store)
    existing: list[str] = []
    if path.exists():
        existing = [
            ln.strip()
            for ln in path.read_text(encoding="utf-8", errors="replace").splitlines()
            if ln.strip()
        ]
    seen = {ln.lower() for ln in existing}
    additions: list[str] = []
    for part in re.split(r"[;\n]+", cleaned):
        item = part.strip()
        if not item:
            continue
        key = item.lower()
        if key in seen:
            continue
        seen.add(key)
        additions.append(item)
    if additions:
        merged = existing + additions
        path.write_text("\n".join(merged) + "\n", encoding="utf-8")


def _extract_section(text: str, label: str) -> str:
    pattern = rf"(?ims)^\s*{re.escape(label)}\s*:\s*(.*?)(?=^\s*[A-Z][A-Z _-]*\s*:|\Z)"
    m = re.search(pattern, text or "")
    if not m:
        return ""
    return m.group(1).strip()


def _scrub_player_png_history(store: "Store", node: str) -> int:
    if not re.fullmatch(r"[0-9a-fA-F]{12}", node):
        return 0
    removed = 0
    referenced_pngs: set[str] = set()
    for path in sorted(store.events_dir.iterdir(), key=lambda p: float(p.name)):
        try:
            payload = _parse_kv_lines(path.read_text(encoding="utf-8"))
        except Exception:
            continue
        if payload.get("type") != "png":
            continue
        filename = payload.get("filename", "")
        if not filename:
            continue
        file_node = _uuid_v1_machine_from_filename(filename)
        if file_node and file_node.lower() == node.lower():
            try:
                path.unlink()
                removed += 1
            except FileNotFoundError:
                pass
            referenced_pngs.add(filename)
    for name in referenced_pngs:
        png_path = store.png_dir / name
        try:
            png_path.unlink()
        except FileNotFoundError:
            pass
    return removed


def create_app(data_dir: str = DEFAULT_DATA_DIR) -> FastAPI:
    store = Store(Path(data_dir).resolve())
    store.ensure_dirs()
    updates_ready = asyncio.Event()
    codex_task: asyncio.Task | None = None

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
            user_node = _resolve_user_node_tag(req, store)
            client_host = _client_host(req)
            ts = _now_ts()
            while (store.events_dir / ts).exists():
                ts = _now_ts()

            text_path = store.text_dir / f"{ts}.txt"
            text_path.write_text(text, encoding="utf-8")

            event = {
                "type": "text",
                "text": text,
                "source": "user",
                "node": user_node,
                "client": client_host,
                "blob": _blob_relpath(store, f"blobs/text/{ts}.txt"),
            }
            _update_known_game_state(store, f"user[{user_node}] request: {text}")
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
            user_node = _resolve_user_node_tag(req, store)
            client_host = _client_host(req)
            event_path = store.events_dir / ts
            if event_path.exists():
                raise HTTPException(status_code=409, detail="event ts already exists")

            text_path = store.text_dir / f"{ts}.txt"
            text_path.write_text(text, encoding="utf-8")

            event = {
                "type": "text",
                "text": text,
                "source": "user",
                "node": user_node,
                "client": client_host,
                "blob": _blob_relpath(store, f"blobs/text/{ts}.txt"),
            }
            _update_known_game_state(store, f"user[{user_node}] request: {text}")
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
            png_node = _uuid_v1_machine_from_filename(safe_name) or ""
            client_host = _client_host(req)

            ts = _now_ts()
            while (store.events_dir / ts).exists():
                ts = _now_ts()

            event = {
                "type": "png",
                "filename": safe_name,
                "url": f"/png/{safe_name}",
                "blob": _blob_relpath(store, f"blobs/png/{safe_name}"),
            }
            if png_node:
                event["node"] = png_node
            if client_host:
                event["client"] = client_host
            event_ts = _write_event_at_path(store.events_dir / ts, event)

        updates_ready.set()
        return PlainTextResponse(_format_kv_lines({"ok": "true", "ts": event_ts, "filename": safe_name}))

    @api.get("/png")
    async def list_latest_pngs_by_machine():
        store.ensure_dirs()
        rows = _latest_png_rows_by_node(store)
        if not rows:
            return PlainTextResponse("")
        lines = [f"{r['node']} {r['ts']} {r['url']}" for r in rows]
        return PlainTextResponse("\n".join(lines) + "\n")

    @api.get("/png/{filename}")
    async def get_png(filename: str):
        try:
            safe_name = _safe_filename(filename)
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc

        store.ensure_dirs()
        png_path = _find_png_path(store, safe_name)
        if png_path is None:
            raise HTTPException(status_code=404, detail="not found")

        return Response(content=png_path.read_bytes(), media_type="image/png")

    @api.get("/state")
    async def get_state_page():
        store.ensure_dirs()
        rows = _latest_png_rows_by_node(store)
        last_full_prompt = _load_last_full_prompt(store)
        current_game_id = store.active_game_id
        current_game_dir = str(store.game_dir)
        rows.sort(key=lambda r: r.get("ts", ""), reverse=True)

        cards = []
        for r in rows:
            url = html.escape(r["url"])
            cards.append(
                (
                    "<div class='card'>"
                    f"<div><b>ts:</b> {html.escape(r['ts'])} "
                    f"<b>node:</b> {html.escape(r['node'])} "
                    f"<b>file:</b> {html.escape(r['filename'])}</div>"
                    f"<div><a href='{url}' target='_blank'>{url}</a></div>"
                    f"<img src='{url}' class='card-image' />"
                    "</div>"
                )
            )
        cards_html = "".join(cards) if cards else "<p>No PNG events yet.</p>"

        page = f"""<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="color-scheme" content="light dark" />
  <title>faetond state</title>
  <style>
    :root {{
      color-scheme: light dark;
      --bg: #ffffff;
      --fg: #111111;
      --muted: #555555;
      --card-bg: #ffffff;
      --card-border: #dddddd;
      --panel-bg: #f6f8fa;
      --img-border: #cccccc;
      --link: #0a58ca;
    }}
    @media (prefers-color-scheme: dark) {{
      :root {{
        --bg: #0f1115;
        --fg: #e9edf3;
        --muted: #a9b3c1;
        --card-bg: #171b22;
        --card-border: #2b3240;
        --panel-bg: #141922;
        --img-border: #3a4457;
        --link: #8ab4ff;
      }}
    }}
    body {{
      font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
      margin: 20px;
      background: var(--bg);
      color: var(--fg);
    }}
    a {{ color: var(--link); }}
    pre {{
      background: var(--panel-bg);
      padding: 12px;
      border-radius: 8px;
      overflow-x: auto;
      white-space: pre-wrap;
    }}
    .card {{
      border: 1px solid var(--card-border);
      background: var(--card-bg);
      border-radius: 8px;
      padding: 10px;
      margin: 8px 0;
    }}
    .card-image {{
      max-width: 100%;
      height: auto;
      border: 1px solid var(--img-border);
      margin-top: 8px;
    }}
  </style>
</head>
<body>
  <h1>faetond /state</h1>
  <p><b>Current game:</b> {html.escape(current_game_id)}</p>
  <p><b>Game directory:</b> {html.escape(current_game_dir)}</p>
  <form method="post" action="/state" style="margin-bottom:16px;">
    <button type="submit" style="padding:8px 12px;">Reset Game</button>
  </form>
  <h2>Prompt</h2>
  <pre>{html.escape(last_full_prompt)}</pre>
  <h2>Players ({len(rows)})</h2>
  {cards_html}
</body>
</html>"""
        return HTMLResponse(page)

    @api.post("/state")
    async def reset_game_state():
        async with store._write_lock:
            store.ensure_dirs()
            store.rotate_game()
            ts = _now_ts()
            while (store.events_dir / ts).exists():
                ts = _now_ts()
            text = "Restarted"
            text_path = store.text_dir / f"{ts}.txt"
            text_path.write_text(text, encoding="utf-8")
            event = {
                "type": "text",
                "text": text,
                "source": "system",
                "blob": _blob_relpath(store, f"blobs/text/{ts}.txt"),
            }
            _write_event_at_path(store.events_dir / ts, event)
        updates_ready.set()
        return await get_state_page()

    @api.post("/scrub/{node}")
    async def scrub_player(node: str):
        store.ensure_dirs()
        removed = _scrub_player_png_history(store, node)
        return HTMLResponse(
            f"<html><body><p>Scrubbed node {html.escape(node)}. Removed {removed} PNG events.</p>"
            "<p><a href='/state'>Back to /state</a></p></body></html>"
        )

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
                        if payload.get("type") != "text":
                            continue
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

    async def _publish_text_event(text: str) -> str:
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
                "source": "codex",
                "blob": _blob_relpath(store, f"blobs/text/{ts}.txt"),
            }
            return _write_event_at_path(store.events_dir / ts, event)

    async def _codex_worker_loop() -> None:
        last_signature = ""
        while True:
            try:
                store.ensure_dirs()
                rows = _latest_png_rows_by_node(store)
                request_marker = _latest_user_request_marker(store)
                signature = (
                    "|".join(f"{r.get('node','')}:{r.get('ts','')}" for r in rows)
                    + f"|req:{request_marker}"
                )
                if rows and signature and signature != last_signature:
                    advice = await asyncio.to_thread(_run_codex_for_rows, rows, store)
                    if advice:
                        ts = await _publish_text_event(advice)
                        last_signature = signature
                        updates_ready.set()
                        print(f"codex ts={ts} hosts={len(rows)}", flush=True)
                await asyncio.sleep(CODEX_LOOP_INTERVAL_SECONDS)
            except asyncio.CancelledError:
                return
            except Exception as exc:
                print(f"codex worker error: {exc}", flush=True)
                await asyncio.sleep(CODEX_LOOP_INTERVAL_SECONDS)

    @api.on_event("startup")
    async def _on_startup():
        nonlocal codex_task
        codex_task = asyncio.create_task(_codex_worker_loop())

    @api.on_event("shutdown")
    async def _on_shutdown():
        nonlocal codex_task
        if codex_task:
            codex_task.cancel()
            try:
                await codex_task
            except asyncio.CancelledError:
                pass
            codex_task = None

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
