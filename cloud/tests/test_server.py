"""stackbear 服务端端到端测试:在进程内起 TCP 服务,模拟设备走完整协议,校验落盘。

纯标准库(`asyncio`),可直接 `python test_server.py`。
"""

from __future__ import annotations

import asyncio
import json
import os
import pathlib
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from stackbear import protocol, server  # noqa: E402

FAILURES = 0


def check(cond: bool, msg: str) -> None:
    global FAILURES
    if not cond:
        print(f"FAIL: {msg}")
        FAILURES += 1


async def _client(host: str, port: int) -> None:
    reader, writer = await asyncio.open_connection(host, port)
    hello = json.dumps(
        {"dev": "testdev01", "codec": "opus", "sample_rate": 16000,
         "channels": 1, "frame_ms": 60, "boot_ms": 1234}
    ).encode()
    writer.write(protocol.encode(protocol.TYPE_HELLO, 0, hello))
    for seq in range(1, 6):
        frame = bytes([seq] * 20)  # 假装 20 字节的 opus 包
        writer.write(protocol.encode(protocol.TYPE_AUDIO, seq, frame))
    writer.write(protocol.encode(protocol.TYPE_END, 6, b'{"frames":5}', protocol.FLAG_LAST))
    await writer.drain()

    # 期待收到 5 个 AUDIO ACK + 1 个 END ACK
    acks = 0
    buf = bytearray()
    while acks < 6:
        chunk = await asyncio.wait_for(reader.read(1024), timeout=5)
        if not chunk:
            break
        buf.extend(chunk)
        progressed = True
        while progressed and buf:
            progressed = False
            consumed, frame = protocol.decode(bytes(buf))
            if consumed > 0:
                del buf[:consumed]
                progressed = True
                if frame and frame.type == protocol.TYPE_ACK:
                    acks += 1
            elif consumed < 0:
                del buf[:1]
                progressed = True
    writer.close()
    await writer.wait_closed()
    check(acks == 6, f"expected 6 acks, got {acks}")


async def _run() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        server.DATA = pathlib.Path(tmp) / "data"
        srv = await asyncio.start_server(server._handler, host="127.0.0.1", port=0)
        port = srv.sockets[0].getsockname()[1]
        async with srv:
            await _client("127.0.0.1", port)

        dev_dir = server.DATA / "testdev01"
        sessions = [p for p in dev_dir.iterdir() if p.is_dir()]
        check(len(sessions) == 1, f"exactly one session dir, got {len(sessions)}")
        if sessions:
            sdir = sessions[0]
            meta = json.loads((sdir / "meta.json").read_text(encoding="utf-8"))
            check(meta["dev"] == "testdev01", "meta dev")
            check(meta["frames"] == 5, f"meta frames == 5, got {meta['frames']}")
            check(meta["audio_ms"] == 300, f"meta audio_ms == 300, got {meta['audio_ms']}")
            check(meta["first_seq"] == 1 and meta["last_seq"] == 5, "seq range")
            blob = (sdir / "audio.adpcm").read_bytes()
            # 每个包 2 字节长度前缀 + 20 字节负载
            check(len(blob) == 5 * 22, f"audio.adpcm length, got {len(blob)}")
            check(blob[:2] == b"\x00\x14", "first length prefix == 20")


def main() -> int:
    asyncio.run(_run())
    if FAILURES:
        print(f"test_server: {FAILURES} failure(s)")
        return 1
    print("test_server: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
