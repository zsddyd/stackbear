"""stackbear 私有协议 Python 侧单元测试。纯标准库,可直接 `python test_protocol.py`。"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from stackbear import protocol  # noqa: E402

FAILURES = 0


def check(cond: bool, msg: str) -> None:
    global FAILURES
    if not cond:
        print(f"FAIL: {msg}")
        FAILURES += 1


def test_roundtrip() -> None:
    payload = bytes([1, 2, 3, 0x7F, 0x80, 0xFF])
    raw = protocol.encode(protocol.TYPE_AUDIO, 0x1234, payload, protocol.FLAG_LAST)
    consumed, frame = protocol.decode(raw)
    check(consumed == len(raw), "consumed == len")
    check(frame is not None and frame.type == protocol.TYPE_AUDIO, "type")
    check(frame.flags == protocol.FLAG_LAST, "flags")
    check(frame.seq == 0x1234, "seq")
    check(frame.payload == payload, "payload")


def test_truncated() -> None:
    raw = protocol.encode(protocol.TYPE_AUDIO, 1, bytes(20))
    check(protocol.decode(raw[:-1]) == (0, None), "truncated -> 0")
    check(protocol.decode(raw[:3]) == (0, None), "short header -> 0")


def test_corruption() -> None:
    raw = bytearray(protocol.encode(protocol.TYPE_AUDIO, 9, bytes(range(16))))
    raw[protocol.HEADER_LEN] ^= 0x5A
    check(protocol.decode(bytes(raw)) == (-1, None), "crc mismatch -> -1")


def test_resync() -> None:
    noise = bytes([0x00, 0x11, 0x53, 0x00, 0xAB])
    frame = protocol.encode(protocol.TYPE_HELLO, 3, b"x")
    buf = noise + frame
    offset = 0
    decoded = 0
    while offset < len(buf):
        consumed, parsed = protocol.decode(buf[offset:])
        if consumed > 0:
            decoded = consumed
            break
        if consumed < 0:
            offset += 1
            continue
        break
    check(decoded == len(frame), "found frame after noise")
    check(offset == len(noise), "offset == len(noise)")


def test_encode_rejects_oversize() -> None:
    try:
        protocol.encode(protocol.TYPE_AUDIO, 0, bytes(protocol.MAX_PAYLOAD + 1))
    except ValueError:
        check(True, "oversize rejected")
    else:
        check(False, "oversize should raise")


def main() -> int:
    test_roundtrip()
    test_truncated()
    test_corruption()
    test_resync()
    test_encode_rejects_oversize()
    if FAILURES:
        print(f"test_protocol: {FAILURES} failure(s)")
        return 1
    print("test_protocol: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
