"""stackbear 私有帧协议的 Python 侧实现。与固件 `main/sb_proto.h` 必须保持一致。

帧格式(全部大端,总开销 12 字节):
    off 0  magic[2] = 'S' 'B'
    off 2  version u8 = 1
    off 3  type    u8
    off 4  flags   u8   (bit0 = LAST)
    off 5  reserved u8
    off 6  seq     u16
    off 8  len     u16
    off 10 payload[len]
    off 10+len crc16 u16  (CRC-16/CCITT-FALSE,覆盖 header + payload)
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = b"SB"
VERSION = 1
HEADER_LEN = 10
CRC_LEN = 2
OVERHEAD = HEADER_LEN + CRC_LEN
MAX_PAYLOAD = 1400
MAX_FRAME = OVERHEAD + MAX_PAYLOAD

TYPE_HELLO = 0x01
TYPE_AUDIO = 0x02
TYPE_END = 0x03
TYPE_PING = 0x04
TYPE_ACK = 0x80

FLAG_LAST = 0x01


def crc16_ccitt(data: bytes) -> int:
    """CRC-16/CCITT-FALSE(poly 0x1021, init 0xFFFF)。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


@dataclass
class Frame:
    type: int
    flags: int
    seq: int
    payload: bytes


def encode(frame_type: int, seq: int, payload: bytes = b"", flags: int = 0) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    header = struct.pack(">2sBBBBHH", MAGIC, VERSION, frame_type, flags, 0, seq, len(payload))
    body = header + payload
    return body + struct.pack(">H", crc16_ccitt(body))


def decode(buf: bytes) -> tuple[int, Frame | None]:
    """尝试解出第一帧。

    返回 (consumed, frame):
      consumed > 0  一帧完整且有效,frame 非空
      consumed == 0 数据不足,应继续收
      consumed < 0  坏数据/未对齐,调用方应向后滑动 1 字节重找 magic
    """
    if len(buf) < HEADER_LEN:
        return 0, None
    if buf[0:2] != MAGIC or buf[2] != VERSION:
        return -1, None
    payload_len = struct.unpack_from(">H", buf, 8)[0]
    if payload_len > MAX_PAYLOAD:
        return -1, None
    total = OVERHEAD + payload_len
    if len(buf) < total:
        return 0, None
    want = struct.unpack_from(">H", buf, HEADER_LEN + payload_len)[0]
    if crc16_ccitt(bytes(buf[: HEADER_LEN + payload_len])) != want:
        return -1, None
    frame_type = buf[3]
    flags = buf[4]
    seq = struct.unpack_from(">H", buf, 6)[0]
    payload = bytes(buf[HEADER_LEN : HEADER_LEN + payload_len])
    return total, Frame(frame_type, flags, seq, payload)
