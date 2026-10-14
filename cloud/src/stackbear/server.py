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

from . import __version__, protocol

ROOT = pathlib.Path(__file__).resolve().parents[2]
DATA = pathlib.Path(os.environ.get("STACKBEAR_DATA", ROOT / "data"))
QUOTA_BYTES = int(os.environ.get("STACKBEAR_QUOTA_BYTES", 4 * 1024 * 1024 * 1024))
IDLE_TIMEOUT_S = float(os.environ.get("STACKBEAR_IDLE_TIMEOUT", 120.0))

_SAFE = re.compile(r"[^0-9A-Za-z_.-]")


def _safe_component(value: str, default: str) -> str:
    cleaned = _SAFE.sub("", value or "")
    return cleaned[:64] or default


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
        }
        self._fh = self.audio_path.open("wb")
        self._last_seq: int | None = None

    def set_hello(self, hello: dict) -> None:
        for key in ("codec", "sample_rate", "channels", "frame_ms"):
            if key in hello:
                self.meta[key] = hello[key]
        for key in ("boot_ms", "rec_err", "opus_err", "heap"):
            if hello.get(key) is not None:
                self.meta[key] = hello[key]

    def add_audio(self, seq: int, payload: bytes) -> None:
        if self._last_seq is not None and seq != (self._last_seq + 1) & 0xFFFF:
            self.meta["gaps"] += 1
        if self.meta["first_seq"] is None:
            self.meta["first_seq"] = seq
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
        path, _, query = target.partition("?")

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
        elif method == "POST" and path == "/upload":
            # 兼容早期 M1 心跳自测:POST /upload?dev=&seq=
            params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
            dev = _safe_component(params.get("dev", ""), "unknown")
            seq = _safe_component(params.get("seq", "0"), "0")
            body = head.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in head else b""
            d = DATA / dev
            d.mkdir(parents=True, exist_ok=True)
            (d / f"{seq}.bin").write_bytes(body)
            print(f"[legacy-upload] dev={dev} seq={seq} bytes={len(body)}", flush=True)
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
                            print(f"[resync] dev={session.dev} 上一会话未正常 END,已收尾 {prev}",
                                  flush=True)
                        session = Session(dev)
                        session.set_hello(hello)
                        print(f"[hello] peer={peer} dev={dev} {hello}", flush=True)
                    elif frame.type == protocol.TYPE_AUDIO:
                        if session is None:
                            session = Session("unknown")  # 容错:未见 HELLO 也收
                        session.add_audio(frame.seq, frame.payload)
                        writer.write(protocol.encode(protocol.TYPE_ACK, frame.seq))
                    elif frame.type == protocol.TYPE_END:
                        if session is not None:
                            d = session.finish()
                            print(f"[end] dev={session.dev} dir={d} "
                                  f"frames={session.meta['frames']} "
                                  f"audio_ms={session.meta['audio_ms']}", flush=True)
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
                print(f"[warn] peer={peer} buffer={len(buf)} 超限,断开", flush=True)
                break

            chunk = await asyncio.wait_for(reader.read(2048), timeout=IDLE_TIMEOUT_S)
            if not chunk:
                break
            buf.extend(chunk)
    except (asyncio.TimeoutError, ConnectionError, OSError) as exc:
        print(f"[close] peer={peer} {type(exc).__name__}", flush=True)
    finally:
        if session is not None:
            d = session.finish()
            print(f"[finalize] dev={session.dev} dir={d} frames={session.meta['frames']} "
                  f"(连接未正常 END)", flush=True)
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
    print(f"stackbear v{__version__} listening on {addrs} (data={DATA})", flush=True)
    async with server:
        await server.serve_forever()


def main() -> None:
    parser = argparse.ArgumentParser(description="stackbear 云端接收服务")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=18080)
    args = parser.parse_args()
    try:
        asyncio.run(serve(args.host, args.port))
    except KeyboardInterrupt:
        print("bye", flush=True)


if __name__ == "__main__":
    main()
