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
        # Frames 1-3 form the first VAD segment, 4-5 a second one.
        flags = protocol.FLAG_SEG_START if seq in (1, 4) else 0
        writer.write(protocol.encode(protocol.TYPE_AUDIO, seq, frame, flags))
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
        # This test covers receive/storage/segmentation, not ASR: do not spawn a
        # real transcription for the synthetic frames.
        server.AUTO_TRANSCRIBE = False
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
            check(meta["seg_count"] == 2, f"seg_count == 2, got {meta['seg_count']}")
            segs = meta["segments"]
            check(segs[0]["start_ms"] == 0 and segs[0]["frames"] == 3,
                  f"segment 0 {segs[0]}")
            check(segs[1]["start_ms"] == 180 and segs[1]["frames"] == 2,
                  f"segment 1 {segs[1]}")
            blob = (sdir / "audio.adpcm").read_bytes()
            # 每个包 2 字节长度前缀 + 20 字节负载
            check(len(blob) == 5 * 22, f"audio.adpcm length, got {len(blob)}")
            check(blob[:2] == b"\x00\x14", "first length prefix == 20")


def main() -> int:
    asyncio.run(_run())
    _share_tests()
    _log_tests()
    if FAILURES:
        print(f"test_server: {FAILURES} failure(s)")
        return 1
    print("test_server: PASS")
    return 0


def _log_tests() -> None:
    """Logging must never raise, even with a dead stdout pipe.

    Regression: the service is often piped into `tee`, and when that reader
    died, print() raised BrokenPipeError. Because BrokenPipeError is an
    OSError, the connection handlers' `except OSError` swallowed it and closed
    the device connection immediately after it connected - the device then
    reconnected every couple of seconds and the UI showed UPLOAD ERR.
    """
    import io
    import contextlib

    class DeadPipe(io.TextIOBase):
        def write(self, _s: str) -> int:
            raise BrokenPipeError(32, "Broken pipe")

        def flush(self) -> None:
            raise BrokenPipeError(32, "Broken pipe")

    with contextlib.redirect_stdout(DeadPipe()):
        try:
            server.log("[hello] should not raise")
        except BaseException as exc:  # noqa: BLE001 - the point of the test
            check(False, f"log() must not raise, got {type(exc).__name__}: {exc}")
        else:
            check(True, "log() survived a dead stdout")

    # And it still works normally.
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        server.log("hello")
    check(buf.getvalue() == "hello\n", f"log wrote {buf.getvalue()!r}")


def _share_tests() -> None:
    """The transcription share endpoint must expose exactly one file."""
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        server.SHARE_DIR = pathlib.Path(tmp) / ".share"
        server.SHARE_DIR.mkdir(parents=True)

        # Nothing shared yet -> 404, even for a well-formed token.
        body, status, _ = server._serve_share("/a/" + "a" * 32)
        check(status.startswith("404"), f"empty share -> 404, got {status}")

        # A malformed token is rejected outright.
        _, status, _ = server._serve_share("/a/../etc/passwd")
        check(status.startswith("400"), f"bad token -> 400, got {status}")

        token = "b" * 32
        (server.SHARE_DIR / f"{token}.wav").write_bytes(b"RIFFdata")
        body, status, ctype = server._serve_share(f"/a/{token}.wav")
        check(status.startswith("200") and body == b"RIFFdata", "shares the file")
        check(ctype == "audio/wav", f"content type {ctype}")

        # A second file must make the endpoint refuse rather than guess.
        (server.SHARE_DIR / ("c" * 32 + ".wav")).write_bytes(b"RIFFother")
        _, status, _ = server._serve_share(f"/a/{token}.wav")
        check(status.startswith("404"), f"two shared files -> 404, got {status}")


if __name__ == "__main__":
    raise SystemExit(main())
