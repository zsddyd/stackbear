#!/usr/bin/env python3
"""把设备上传的 IMA-ADPCM 块流解码成可直接播放的 WAV。

注意:云端目前把文件存成 `audio.opus`,但方案 B 之后它的内容其实是 IMA-ADPCM
(4bit),普通播放器无法直接播放 —— 需要先用本工具转成 WAV。

块流格式(与固件 main/sb_adpcm.[ch] 一致):
    [2 字节大端长度][块 payload] ...  重复
  块 payload:
    [0..1] 初始预测值 predictor(int16, 小端)
    [2]    初始步长索引 index(0..88)
    [3]    保留 0
    [4..]  4bit 差分,每字节两个采样,低半字节在前

用法:
    python3 tools/adpcm_to_wav.py <audio.opus | 会话目录> [-o out.wav]
    python3 tools/adpcm_to_wav.py audio.opus --stats
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import wave

SAMPLE_RATE = 16000
STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
]
INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def _clamp16(v: int) -> int:
    if v > 32767:
        return 32767
    if v < -32768:
        return -32768
    return v


def decode_block(payload: bytes) -> list[int]:
    """解码一个块,返回 int16 采样列表(与固件编码端逐位一致)。"""
    if len(payload) <= 4:
        return []
    predictor = struct.unpack_from("<h", payload, 0)[0]
    index = payload[2]
    if index > 88:
        return []
    samples = (len(payload) - 4) * 2
    out: list[int] = []
    for i in range(samples):
        byte = payload[4 + i // 2]
        nibble = (byte >> 4) if (i & 1) else (byte & 0x0F)
        step = STEP_TABLE[index]
        diff = step >> 3
        if nibble & 1:
            diff += step >> 2
        if nibble & 2:
            diff += step >> 1
        if nibble & 4:
            diff += step
        if nibble & 8:
            predictor -= diff
        else:
            predictor += diff
        predictor = _clamp16(predictor)
        index += INDEX_TABLE[nibble]
        index = 0 if index < 0 else (88 if index > 88 else index)
        out.append(predictor)
    return out


def decode_stream(blob: bytes) -> tuple[list[int], int]:
    """按 [2B 大端长度][块] 解析整个文件,返回 (采样, 坏块数)。"""
    samples: list[int] = []
    bad = 0
    off = 0
    while off + 2 <= len(blob):
        (length,) = struct.unpack_from(">H", blob, off)
        off += 2
        if length == 0 or off + length > len(blob):
            bad += 1
            break
        block = blob[off:off + length]
        off += length
        if length <= 4:
            bad += 1
            continue
        samples.extend(decode_block(block))
    return samples, bad


def write_wav(path: pathlib.Path, samples: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(struct.pack(f"<{len(samples)}h", *samples))


def _resolve(path_str: str) -> pathlib.Path:
    path = pathlib.Path(path_str)
    if path.is_dir():
        for name in ("audio.adpcm", "audio.opus", "audio.pcm"):
            candidate = path / name
            if candidate.is_file():
                return candidate
        raise SystemExit(f"{path} 里没有找到音频文件")
    if not path.is_file():
        raise SystemExit(f"找不到文件: {path}")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description="把设备上传的 ADPCM 块流解码成 WAV")
    parser.add_argument("path", help="audio.opus 文件或会话目录")
    parser.add_argument("-o", "--output", help="输出 WAV 路径(默认同名 .wav)")
    parser.add_argument("--stats", action="store_true", help="只打印统计,不写文件")
    args = parser.parse_args()

    src = _resolve(args.path)
    blob = src.read_bytes()
    samples, bad = decode_stream(blob)
    if not samples:
        print(f"解码失败:{src} 里没有可用的 ADPCM 块", file=sys.stderr)
        return 1

    seconds = len(samples) / SAMPLE_RATE
    peak = max(abs(s) for s in samples)
    # 简单 RMS,用于判断"是不是真的有人声"而不是一片静音。
    rms = (sum(s * s for s in samples) / len(samples)) ** 0.5
    print(f"文件: {src}  ({len(blob)} 字节, 坏块 {bad})")
    print(f"采样: {len(samples)}  ({seconds:.2f} 秒 @ {SAMPLE_RATE} Hz 单声道)")
    print(f"峰值: {peak}   RMS: {rms:.0f}")
    if args.stats:
        return 0

    out = pathlib.Path(args.output) if args.output else src.with_suffix(".wav")
    write_wav(out, samples)
    print(f"已写出: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
