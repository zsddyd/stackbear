"""stackbear 云端接收服务。

一个 asyncio TCP 服务,监听一个端口(默认 18080),同时承担两件事:

1. **设备私有协议**(裸 TCP):接收 AI Passport 上传的 Opus 音频流,按"会话"落盘。
2. **极简 HTTP 状态接口**:同一端口上顺便响应 `GET /`、`GET /health`、`POST /upload`
   (兼容早期心跳自测),方便用浏览器/curl 查看状态。靠嗅探首字节区分两种流量。

落盘布局::

    data/<dev>/<YYYYmmdd-HHMMSS>/
        audio.adpcm  每块前加 2 字节大端长度前缀的 IMA-ADPCM(4bit)块流
                     (用 tools/adpcm_to_wav.py 可解码成 WAV 试听)
        meta.json    设备 id、编码、采样率、帧长、帧数、音频时长、起止时间等

之所以用"长度前缀帧日志"而不是直接字节拼接:后续接 ASR 时需要逐包边界,
长度前缀删掉即可还原真实包长,也方便用脚本转成 Ogg/Opus。
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import pathlib
import re
import struct
import time
import uuid
from urllib.parse import unquote_plus

from . import __version__, protocol

ROOT = pathlib.Path(__file__).resolve().parents[2]
DATA = pathlib.Path(os.environ.get("STACKBEAR_DATA", ROOT / "data"))
# 转写时临时对外暴露的音频放在这里:同一时刻只允许存在一个文件,避免把整个
# data/ 目录变成可下载的。
SHARE_DIR = DATA / ".share"
SHARE_TOKEN_RE = re.compile(r"[0-9a-f]{16,64}")
# OTA images and the manifest the device polls. Populated by the build/deploy
# step; empty by default so a device that asks early just gets a 404.
FIRMWARE_DIR = pathlib.Path(os.environ.get("STACKBEAR_FIRMWARE",
                                           ROOT / "firmware"))
QUOTA_BYTES = int(os.environ.get("STACKBEAR_QUOTA_BYTES", 4 * 1024 * 1024 * 1024))
IDLE_TIMEOUT_S = float(os.environ.get("STACKBEAR_IDLE_TIMEOUT", 120.0))
# Transcribe automatically when a session ends. On by default; set
# STACKBEAR_ASR_AUTO=0 to receive audio only.
AUTO_TRANSCRIBE = os.environ.get("STACKBEAR_ASR_AUTO", "1") not in ("0", "false", "no", "")
# Transcripts are produced per recorded VAD segment when the firmware supplied
# boundaries, which yields one line per utterance with its start offset.
AUTO_PER_SEGMENT = os.environ.get("STACKBEAR_ASR_PER_SEGMENT", "1") not in ("0", "false", "no", "")

_SAFE = re.compile(r"[^0-9A-Za-z_.-]")


def _safe_component(value: str, default: str) -> str:
    cleaned = _SAFE.sub("", value or "")
    return cleaned[:64] or default


def _decode_query(query: str) -> str:
    """Restore a query string to text.

    The request line is decoded as latin1 (correct for HTTP headers), so any
    byte above 0x7F - e.g. the UTF-8 bytes of a Chinese search word - arrives as
    a latin1 character. Re-encode to bytes, decode as UTF-8, then apply
    percent-decoding. Without this a search for a non-ASCII word never matches.
    """
    if not query:
        return query
    try:
        raw = query.encode("latin1", "replace")
        return unquote_plus(raw.decode("utf-8", "replace"))
    except (UnicodeDecodeError, UnicodeEncodeError):
        return query


def log(message: str) -> None:
    """Print a diagnostic line, but never let logging break the service.

    This service is often started as `python -m stackbear.server | tee log`.
    If that pipe's reader dies, every print raises BrokenPipeError - and since
    BrokenPipeError is an OSError, it used to be swallowed by the connection
    handlers' broad `except OSError`, closing the device's connection right
    after it connected. The device then reconnected every couple of seconds and
    the UI showed UPLOAD ERR. Logging must stay best-effort.
    """
    try:
        print(message, flush=True)
    except (OSError, ValueError):
        pass


def _dir_bytes(path: pathlib.Path) -> int:
    total = 0
    for f in path.rglob("*"):
        try:
            if f.is_file():
                total += f.stat().st_size
        except OSError:
            continue
    return total


class Session:
    """一条设备连接对应一个录音会话。"""

    def __init__(self, dev: str) -> None:
        self.dev = dev
        self.started = time.time()
        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(self.started))
        # 同一秒内重连也要能区分,加短随机后缀。
        self.dir = DATA / dev / f"{stamp}-{uuid.uuid4().hex[:4]}"
        self.dir.mkdir(parents=True, exist_ok=True)
        # 注意:固件端已经用 IMA-ADPCM 取代 Opus,文件名必须叫 adpcm,
        # 否则会被误当成 Opus 拿去播放(普通播放器打不开)。
        self.audio_path = self.dir / "audio.adpcm"
        self.meta: dict = {
            "dev": dev,
            "codec": "adpcm",
            "container": "len-prefixed-blocks",
            "sample_rate": 16000,
            "channels": 1,
            "frame_ms": 60,
            "started_at": self.started,
            "frames": 0,
            "audio_ms": 0,
            "bytes": 0,
            "ended_at": None,
            "first_seq": None,
            "last_seq": None,
            "gaps": 0,
            # 每个 VAD 语音段一条:{start_ms, first_seq, frames}。设备在段落第一块上
            # 置 SB_FLAG_SEG_START,云端据此切分,供后续按段提交 ASR。
            "segments": [],
            "seg_count": 0,
        }
        self._fh = self.audio_path.open("wb")
        self._last_seq: int | None = None
        self._cur_seg: dict | None = None

    def set_hello(self, hello: dict) -> None:
        for key in ("codec", "sample_rate", "channels", "frame_ms"):
            if key in hello:
                self.meta[key] = hello[key]
        for key in ("boot_ms", "rec_err", "opus_err", "heap"):
            if hello.get(key) is not None:
                self.meta[key] = hello[key]

    def add_audio(self, seq: int, payload: bytes, flags: int = 0) -> None:
        if self._last_seq is not None and seq != (self._last_seq + 1) & 0xFFFF:
            self.meta["gaps"] += 1
        if self.meta["first_seq"] is None:
            self.meta["first_seq"] = seq
        if flags & protocol.FLAG_SEG_START:
            self._cur_seg = {"start_ms": self.meta["audio_ms"], "first_seq": seq, "frames": 0}
            self.meta["segments"].append(self._cur_seg)
            self.meta["seg_count"] = len(self.meta["segments"])
        elif self._cur_seg is None:
            # 容错:第一块没带标记(旧固件或丢包)时,从它开始自建一段。
            self._cur_seg = {"start_ms": self.meta["audio_ms"], "first_seq": seq, "frames": 0}
            self.meta["segments"].append(self._cur_seg)
            self.meta["seg_count"] = len(self.meta["segments"])
        self._cur_seg["frames"] += 1
        self._last_seq = seq
        self.meta["last_seq"] = seq
        self._fh.write(struct.pack(">H", len(payload)) + payload)
        self.meta["frames"] += 1
        self.meta["bytes"] += len(payload)
        self.meta["audio_ms"] += int(self.meta.get("frame_ms", 60))

    def finish(self) -> pathlib.Path:
        try:
            self._fh.flush()
            self._fh.close()
        except OSError:
            pass
        self.meta["ended_at"] = time.time()
        (self.dir / "meta.json").write_text(
            json.dumps(self.meta, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        return self.dir


def _http_response(body: bytes, status: str = "200 OK", ctype: str = "application/json") -> bytes:
    head = (
        f"HTTP/1.1 {status}\r\n"
        f"Content-Type: {ctype}\r\n"
        f"Content-Length: {len(body)}\r\n"
        "Connection: close\r\n"
        "\r\n"
    ).encode("ascii")
    return head + body


def _serve_share(path: str) -> tuple[bytes, str, str]:
    """Serve exactly the one currently shared audio file.

    Refuses when the share directory does not hold precisely the requested file,
    so a stale or renamed file can never be fetched and a second file can never
    be exposed by accident.
    """
    # The public URL carries a .wav suffix (friendlier to fetchers); the file on
    # disk is <token>.wav, so strip it before validating the token.
    name = path[len("/a/"):]
    token = name[:-4] if name.endswith(".wav") else name
    if not SHARE_TOKEN_RE.fullmatch(token or ""):
        return b'{"error":"bad token"}', "400 Bad Request", "application/json"
    wanted = SHARE_DIR / f"{token}.wav"
    try:
        entries = [p for p in SHARE_DIR.iterdir() if p.is_file()] if SHARE_DIR.is_dir() else []
    except OSError:
        entries = []
    if len(entries) != 1 or entries[0] != wanted or not wanted.is_file():
        return b'{"error":"not found"}', "404 Not Found", "application/json"
    try:
        return wanted.read_bytes(), "200 OK", "audio/wav"
    except OSError:
        return b'{"error":"unreadable"}', "500 Internal Server Error", "application/json"


def _serve_firmware(path: str) -> tuple[bytes, str, str]:
    """Serve the OTA manifest or an image. Only plain names are accepted, so a
    request can never escape the firmware directory."""
    name = path[len("/firmware/"):]
    if name == "latest.json":
        try:
            body = (FIRMWARE_DIR / "latest.json").read_bytes()
        except OSError:
            return b'{"error":"no firmware published"}', "404 Not Found", "application/json"
        return body, "200 OK", "application/json"
    if name.startswith("app-") and name.endswith(".bin") and \
            pathlib.PurePosixPath(name).name == name:
        try:
            body = (FIRMWARE_DIR / name).read_bytes()
        except OSError:
            return b'{"error":"not found"}', "404 Not Found", "application/json"
        return body, "200 OK", "application/octet-stream"
    return b'{"error":"not found"}', "404 Not Found", "application/json"


async def _handle_http(initial: bytes, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    """处理状态查询 / 兼容旧心跳上传。请求很小,只读一次。"""
    try:
        head = initial
        while b"\r\n\r\n" not in head and len(head) < 8192:
            chunk = await asyncio.wait_for(reader.read(2048), timeout=5)
            if not chunk:
                break
            head += chunk
        request_line = head.split(b"\r\n", 1)[0].decode("latin1", "replace")
        parts = request_line.split()
        method = parts[0] if parts else ""
        target = parts[1] if len(parts) > 1 else "/"
        # 查询值里的非 ASCII(搜索用的中文词)在线上是百分号编码的 UTF-8 字节。
        # 整行按 latin1 解码是 HTTP 头的正确做法,但那样查询值会变成乱码字符,
        # 直接拿去找子串永远匹配不上 —— 必须先按 UTF-8 还原、再做百分号解码。
        path, _, query = target.partition("?")
        query = _decode_query(query)

        if method in ("GET", "HEAD") and path in ("/", "/health"):
            payload = {
                "service": "stackbear",
                "version": __version__,
                "protocol": "SB binary over TCP",
                "data_dir": str(DATA),
                "data_bytes": _dir_bytes(DATA),
                "quota_bytes": QUOTA_BYTES,
            }
            if path == "/health":
                payload = {"ok": True, "ts": time.time()}
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            writer.write(_http_response(body))
        elif method in ("GET", "HEAD") and path.startswith("/a/"):
            # Expose exactly the one audio file registered for transcription, so a
            # model that needs to fetch the audio itself can be handed a URL.
            body, status, ctype = _serve_share(path)
            writer.write(_http_response(body, status, ctype))
        elif method in ("GET", "HEAD") and path.startswith("/firmware/"):
            # OTA: the manifest and the image the device installs.
            body, status, ctype = _serve_firmware(path)
            writer.write(_http_response(body, status, ctype))
        elif method in ("GET", "HEAD") and path == "/transcripts":
            # 统一入口:列出所有会话及其转写状态,新的在前。
            #   /transcripts?dev=<设备>&limit=<n>&q=<关键词>
            # q 存在时按内容搜索(返回命中的行),否则返回索引摘要。
            params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
            dev = params.get("dev", "")
            try:
                limit = int(params.get("limit", "0"))
            except ValueError:
                limit = 0
            needle = params.get("q", "")
            if needle:
                rows = search_transcripts(needle, limit or 50)
            else:
                rows = list_sessions(dev, limit)
            payload = {"count": len(rows), "sessions": rows}
            writer.write(_http_response(
                json.dumps(payload, ensure_ascii=False).encode("utf-8")))
        elif method in ("GET", "HEAD") and path.startswith("/transcript/"):
            # 单个会话的全文:/transcript/<设备>/<会话>
            parts = [p for p in path.split("/") if p]
            if len(parts) == 3:
                dev = _safe_component(parts[1], "")
                sess = _safe_component(parts[2], "")
                session = DATA / dev / sess
                text_path = session / "transcript.txt"
                if dev and sess and text_path.is_file():
                    writer.write(_http_response(text_path.read_bytes(),
                                                "200 OK", "text/plain; charset=utf-8"))
                else:
                    writer.write(_http_response(b'{"error":"not found"}', "404 Not Found"))
            else:
                writer.write(_http_response(b'{"error":"bad path"}', "400 Bad Request"))
        elif method == "POST" and path == "/upload":
            # 兼容早期 M1 心跳自测:POST /upload?dev=&seq=
            params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
            dev = _safe_component(params.get("dev", ""), "unknown")
            seq = _safe_component(params.get("seq", "0"), "0")
            body = head.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in head else b""
            d = DATA / dev
            d.mkdir(parents=True, exist_ok=True)
            (d / f"{seq}.bin").write_bytes(body)
            log(f"[legacy-upload] dev={dev} seq={seq} bytes={len(body)}")
            writer.write(_http_response(json.dumps({"ok": True, "bytes": len(body)}).encode()))
        else:
            writer.write(_http_response(b'{"error":"not found"}', "404 Not Found"))
        await writer.drain()
    except (asyncio.TimeoutError, ConnectionError, OSError):
        pass
    finally:
        writer.close()


async def _handle_device(initial: bytes, reader: asyncio.StreamReader, writer: asyncio.StreamWriter,
                         peer: str) -> None:
    """设备私有协议主循环。"""
    buf = bytearray(initial)
    session: Session | None = None
    try:
        while True:
            # 先把缓冲区里的完整帧全部消费掉。
            progressed = True
            while progressed:
                progressed = False
                if not buf:
                    break
                consumed, frame = protocol.decode(bytes(buf))
                if consumed > 0 and frame is not None:
                    del buf[:consumed]
                    progressed = True
                    if frame.type == protocol.TYPE_HELLO:
                        hello = _parse_hello(frame.payload)
                        dev = _safe_component(str(hello.get("dev", "")), "unknown")
                        # 同一连接上重发 HELLO:设备端重启了会话。先把上一段收尾,
                        # 否则旧文件句柄一直开着,还会留下永远不写 meta 的空会话目录。
                        if session is not None:
                            prev = session.finish()
                            log(f"[resync] dev={session.dev} 上一会话未正常 END,已收尾 {prev}")
                            _schedule_transcribe(prev)
                        session = Session(dev)
                        session.set_hello(hello)
                        log(f"[hello] peer={peer} dev={dev} {hello}")
                    elif frame.type == protocol.TYPE_AUDIO:
                        if session is None:
                            session = Session("unknown")  # 容错:未见 HELLO 也收
                        session.add_audio(frame.seq, frame.payload, frame.flags)
                        writer.write(protocol.encode(protocol.TYPE_ACK, frame.seq))
                    elif frame.type == protocol.TYPE_END:
                        if session is not None:
                            d = session.finish()
                            log(f"[end] dev={session.dev} dir={d} "
                                f"frames={session.meta['frames']} "
                                f"audio_ms={session.meta['audio_ms']}")
                            _schedule_transcribe(d)
                            session = None
                        writer.write(protocol.encode(protocol.TYPE_ACK, frame.seq))
                    elif frame.type == protocol.TYPE_PING:
                        writer.write(protocol.encode(protocol.TYPE_ACK, frame.seq))
                    await writer.drain()
                elif consumed < 0:
                    # 未对齐/坏数据:滑一字节重找 magic。
                    del buf[:1]
                    progressed = True
                else:
                    break  # 数据不足

            if len(buf) > protocol.MAX_FRAME * 4:
                log(f"[warn] peer={peer} buffer={len(buf)} 超限,断开")
                break

            chunk = await asyncio.wait_for(reader.read(2048), timeout=IDLE_TIMEOUT_S)
            if not chunk:
                break
            buf.extend(chunk)
    except (asyncio.TimeoutError, ConnectionError, OSError) as exc:
        log(f"[close] peer={peer} {type(exc).__name__}")
    finally:
        if session is not None:
            d = session.finish()
            log(f"[finalize] dev={session.dev} dir={d} frames={session.meta['frames']} "
                f"(连接未正常 END)")
            _schedule_transcribe(d)
        writer.close()
        try:
            await writer.wait_closed()
        except (ConnectionError, OSError):
            pass


def _parse_hello(payload: bytes) -> dict:
    try:
        data = json.loads(payload.decode("utf-8"))
        return data if isinstance(data, dict) else {}
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
        return {}


def _transcribe_worker(session_dir: pathlib.Path) -> None:
    """Decode and transcribe a finished session, then write the result.

    Runs in a worker thread: a provider call is slow (seconds), and the receive
    loop must not stall for it. Failures are logged and recorded in a status file
    so a missing transcript can be told apart from "not attempted".
    """
    from . import transcribe as transcribe_mod

    status = session_dir / "transcript.status.json"
    # Pending marker: written before the slow call so that a session interrupted by
    # a process restart is recognisable afterwards. Without it a kill mid-call
    # left neither a transcript nor a status file, and the session looked as if it
    # had never been attempted (an 85 s recording was lost this way).
    pending = session_dir / "transcript.pending"

    def clear_pending() -> None:
        try:
            pending.unlink(missing_ok=True)
        except OSError:
            pass

    def note(payload: dict) -> None:
        try:
            status.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                              encoding="utf-8")
        except OSError:
            # The directory may be gone (rotated/cleaned); the transcript attempt
            # still matters for logs, so never let bookkeeping mask the result.
            pass

    try:
        pending.write_text(json.dumps({"started_at": time.time(), "pid": os.getpid()}),
                           encoding="utf-8")
    except OSError:
        pending = None   # 标记写不了不影响转写本身

    try:
        payload = transcribe_mod.transcribe_session(
            session_dir, per_segment=AUTO_PER_SEGMENT)
        transcribe_mod.write_result(session_dir, payload)
        lines = payload["results"]
        note({"ok": True, "provider": payload["provider"], "model": payload["model"],
              "segments": len(lines), "chars": sum(len(r["text"]) for r in lines),
              "dropped": len(payload.get("errors", [])),
              "usage": [r.get("usage") for r in lines]})
        log(f"[asr] {session_dir} ok model={payload['model']} segments={len(lines)}")
    except transcribe_mod.NoSpeech as exc:
        # Not a failure: a quiet session. Record it distinctly so the operator can
        # tell "nothing was said" from "transcription broke".
        note({"ok": True, "skipped": "no speech", "detail": str(exc)})
        log(f"[asr] {session_dir} no speech")
    except transcribe_mod.AudioCorrupt as exc:
        note({"ok": False, "error": f"audio corrupt: {exc}"})
        log(f"[asr] {session_dir} audio corrupt: {exc}")
    except Exception as exc:  # noqa: BLE001 - reported, never fatal to the service
        note({"ok": False, "error": str(exc)})
        log(f"[asr] {session_dir} failed: {exc}")
    finally:
        clear_pending()


def _schedule_transcribe(session_dir: pathlib.Path) -> None:
    """Queue a session for transcription without blocking the receive loop."""
    if not AUTO_TRANSCRIBE:
        return
    try:
        import asyncio

        asyncio.get_running_loop().run_in_executor(None, _transcribe_worker, session_dir)
    except RuntimeError:
        _transcribe_worker(session_dir)


def find_orphaned_sessions(limit: int = 0) -> list[pathlib.Path]:
    """Sessions that have audio but no transcript, and no pending marker.

    Two cases: a transcript interrupted by a process restart (the marker survives
    the kill), and sessions recorded before automatic transcription existed. Both
    are worth finishing. A session already holding transcript.txt is skipped so a
    restart does not redo finished work, and one with a pending marker is skipped
    because another worker is on it.
    """
    found: list[pathlib.Path] = []
    if not DATA.is_dir():
        return found
    for audio in DATA.glob("*/*/audio.adpcm"):
        session = audio.parent
        if audio.stat().st_size == 0:
            continue                      # VAD captured nothing; nothing to do
        if (session / "transcript.txt").is_file():
            continue                      # already done
        if (session / "transcript.pending").is_file():
            continue                      # a worker holds it
        found.append(session)
    found.sort(key=lambda p: p.stat().st_mtime)
    return found[:limit] if limit > 0 else found


def list_sessions(device: str = "", limit: int = 0) -> list[dict]:
    """Enumerate sessions with their transcription state, newest first.

    This is the single entry point into the recordings: instead of walking
    `data/` by hand, a caller asks for the index and gets one row per session
    with the device, time, duration, transcript availability and a text preview.
    """
    rows: list[dict] = []
    if not DATA.is_dir():
        return rows
    pattern = f"{_safe_component(device, '*')}/*" if device else "*/*"
    for session in DATA.glob(pattern):
        if not session.is_dir():
            continue
        audio = session / "audio.adpcm"
        row: dict = {
            "device": session.parent.name,
            "session": session.name,
            "path": str(session),
        }
        try:
            row["mtime"] = session.stat().st_mtime
        except OSError:
            continue
        meta_path = session / "meta.json"
        if meta_path.is_file():
            try:
                meta = json.loads(meta_path.read_text(encoding="utf-8"))
                row["audio_ms"] = meta.get("audio_ms", 0)
                row["segments"] = meta.get("seg_count", 0)
                row["started_at"] = meta.get("started_at")
            except (OSError, json.JSONDecodeError):
                pass
        if audio.is_file():
            row["audio_bytes"] = audio.stat().st_size
        text_path = session / "transcript.txt"
        status_path = session / "transcript.status.json"
        if text_path.is_file():
            row["transcribed"] = True
            try:
                text = text_path.read_text(encoding="utf-8")
            except OSError:
                text = ""
            row["chars"] = len(text.strip())
            row["preview"] = text.strip().splitlines()[0][:80] if text.strip() else ""
        else:
            row["transcribed"] = False
        if status_path.is_file():
            try:
                status = json.loads(status_path.read_text(encoding="utf-8"))
                row["model"] = status.get("model")
                row["skipped"] = status.get("skipped")
                row["dropped"] = status.get("dropped", 0)
            except (OSError, json.JSONDecodeError):
                pass
        rows.append(row)
    rows.sort(key=lambda r: r.get("mtime", 0), reverse=True)
    return rows[:limit] if limit > 0 else rows


def search_transcripts(query: str, limit: int = 50) -> list[dict]:
    """Find sessions whose transcript contains `query` (case-insensitive).

    Scans the transcripts that exist rather than building an index that could
    drift out of sync with the files. `limit` bounds the number of *matches*, not
    how much is scanned - bounding the scan instead would silently miss a match
    that happens to sort later (an early version returned no hit for a word that
    was present, because the matching session was below the scan cutoff).
    """
    needle = query.strip().lower()
    if not needle:
        return []
    hits: list[dict] = []
    if not DATA.is_dir():
        return hits
    # 只看真正有转写结果的会话:直接扫文件,比先构造全量索引更省。
    for text_path in DATA.glob("*/*/transcript.txt"):
        session = text_path.parent
        try:
            text = text_path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if needle in text.lower():
            lines = [ln for ln in text.splitlines() if needle in ln.lower()]
            hits.append({
                "device": session.parent.name,
                "session": session.name,
                "path": str(session),
                "transcribed": True,
                "matches": lines[:5],
            })
            if len(hits) >= limit:
                break
    hits.sort(key=lambda r: r["session"], reverse=True)
    return hits


def requeue_orphaned_sessions(limit: int = 50) -> int:
    """Re-queue unfinished sessions (bounded). Returns how many were queued."""
    queued = 0
    for session in find_orphaned_sessions(limit):
        log(f"[asr] requeue {session} (unfinished)")
        _schedule_transcribe(session)
        queued += 1
    return queued


async def _handler(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    peer = "?"
    try:
        sock = writer.get_extra_info("peername")
        if sock:
            peer = f"{sock[0]}:{sock[1]}"
    except (IndexError, TypeError):
        pass
    try:
        initial = await asyncio.wait_for(reader.read(2048), timeout=IDLE_TIMEOUT_S)
    except (asyncio.TimeoutError, ConnectionError, OSError):
        writer.close()
        return
    if not initial:
        writer.close()
        return
    if initial[:4] in (b"GET ", b"POST", b"HEAD", b"OPTI"):
        await _handle_http(initial, reader, writer)
    else:
        await _handle_device(initial, reader, writer, peer)


async def serve(host: str, port: int) -> None:
    DATA.mkdir(parents=True, exist_ok=True)
    server = await asyncio.start_server(_handler, host=host, port=port)
    addrs = ", ".join(sorted({str(s.getsockname()) for s in server.sockets}))
    log(f"stackbear v{__version__} listening on {addrs} (data={DATA})")
    # 启动时补跑没转写完的会话(进程重启时被杀在途、或早于自动转写存在的旧会话)。
    # 放在 listen 之后:补跑是后台线程,不影响接收新连接。
    try:
        queued = requeue_orphaned_sessions()
        if queued:
            log(f"[asr] {queued} unfinished session(s) queued")
    except Exception as exc:  # noqa: BLE001 - 补跑失败不能挡住服务
        log(f"[asr] requeue scan failed: {exc}")
    async with server:
        await server.serve_forever()


def main() -> None:
    parser = argparse.ArgumentParser(description="stackbear 云端接收服务")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--requeue-only", action="store_true",
                        help="只补跑没转写完的会话然后退出(用于手动回填)")
    args = parser.parse_args()
    if args.requeue_only:
        # 同步跑完再退出,便于用一条命令回填历史会话并直接看到结果。
        pending = find_orphaned_sessions()
        log(f"{len(pending)} unfinished session(s)")
        for session in pending:
            _transcribe_worker(session)
        return
    try:
        asyncio.run(serve(args.host, args.port))
    except KeyboardInterrupt:
        log("bye")


if __name__ == "__main__":
    main()
