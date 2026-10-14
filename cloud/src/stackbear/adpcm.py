"""IMA-ADPCM decoded/encoded on the cloud side.

The device stores one self-contained 60 ms block per frame, each prefixed with a
2-byte big-endian length:

    [len u16 BE][4-byte header + 4-bit nibbles] ... repeated

Block layout: predictor (int16 LE), step index (u8), reserved (u8), then the
nibbles (low nibble first). This module is the reference decoder used before any
ASR call; `tools/adpcm_to_wav.py` carries the same algorithm for ad-hoc local
preview when the cloud package is not present.

Decoding is deliberately integer-only and mirrors `main/sb_adpcm.c`
step-for-step, so a decoded WAV matches the encoder's reconstruction exactly.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import wave

SAMPLE_RATE = 16000
FRAME_MS = 60
FRAME_SAMPLES = 960
HEADER_LEN = 4
BLOCK_BYTES = HEADER_LEN + FRAME_SAMPLES // 2  # 484

STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
)
INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)


def _clamp16(value: int) -> int:
    if value > 32767:
        return 32767
    if value < -32768:
        return -32768
    return value


def decode_block(payload: bytes) -> list[int]:
    """Decode one block into int16 samples."""
    if len(payload) <= HEADER_LEN:
        return []
    predictor = struct.unpack_from("<h", payload, 0)[0]
    index = payload[2]
    if index > 88:
        return []
    samples: list[int] = []
    for i in range((len(payload) - HEADER_LEN) * 2):
        byte = payload[HEADER_LEN + i // 2]
        nibble = (byte >> 4) if (i & 1) else (byte & 0x0F)
        step = STEP_TABLE[index]
        diff = step >> 3
        if nibble & 1:
            diff += step >> 2
        if nibble & 2:
            diff += step >> 1
        if nibble & 4:
            diff += step
        predictor += -diff if (nibble & 8) else diff
        predictor = _clamp16(predictor)
        index += INDEX_TABLE[nibble]
        index = 0 if index < 0 else (88 if index > 88 else index)
        samples.append(predictor)
    return samples


def decode_stream(blob: bytes) -> tuple[list[int], int]:
    """Decode a stored session. Returns (samples, malformed_block_count)."""
    samples: list[int] = []
    bad = 0
    offset = 0
    while offset + 2 <= len(blob):
        (length,) = struct.unpack_from(">H", blob, offset)
        offset += 2
        if length == 0 or offset + length > len(blob):
            bad += 1
            break
        block = blob[offset:offset + length]
        offset += length
        if length <= HEADER_LEN:
            bad += 1
            continue
        samples.extend(decode_block(block))
    return samples, bad


def encode_block(pcm: list[int], index: int = 0) -> bytes:
    """Encode one block. Present so tests can round-trip and so the cloud can
    re-encode if a later stage needs the same container."""
    if not pcm or len(pcm) > FRAME_SAMPLES or len(pcm) % 2:
        raise ValueError("pcm must hold an even sample count up to one frame")
    predictor = pcm[0]
    out = bytearray(HEADER_LEN)
    struct.pack_into("<h", out, 0, predictor)
    out[2] = index
    for i, sample in enumerate(pcm):
        step = STEP_TABLE[index]
        diff = sample - predictor
        nibble = 0
        if diff < 0:
            nibble = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            nibble |= 4
            diff -= step
            delta += step
        if diff >= (step >> 1):
            nibble |= 2
            diff -= step >> 1
            delta += step >> 1
        if diff >= (step >> 2):
            nibble |= 1
            delta += step >> 2
        predictor = _clamp16(predictor + (-delta if (nibble & 8) else delta))
        index += INDEX_TABLE[nibble]
        index = 0 if index < 0 else (88 if index > 88 else index)
        if (i & 1) == 0:
            out.append(nibble)
        else:
            out[-1] |= nibble << 4
    return bytes(out)


def pack_blocks(blocks: list[bytes]) -> bytes:
    """Wrap encoded blocks in the stored [len][block] container."""
    return b"".join(struct.pack(">H", len(b)) + b for b in blocks)


def write_wav(path: pathlib.Path, samples: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(struct.pack(f"<{len(samples)}h", *samples))


def stats(samples: list[int]) -> dict:
    if not samples:
        return {"samples": 0, "seconds": 0.0, "peak": 0, "rms": 0.0}
    peak = max(abs(s) for s in samples)
    rms = (sum(s * s for s in samples) / len(samples)) ** 0.5
    return {
        "samples": len(samples),
        "seconds": len(samples) / SAMPLE_RATE,
        "peak": peak,
        "rms": round(rms, 1),
    }


def find_audio(path: pathlib.Path) -> pathlib.Path:
    if path.is_dir():
        for name in ("audio.adpcm", "audio.opus", "audio.pcm"):
            candidate = path / name
            if candidate.is_file():
                return candidate
        raise FileNotFoundError(f"no audio file in {path}")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description="Decode a stored session to WAV")
    parser.add_argument("path", help="audio.adpcm file or session directory")
    parser.add_argument("-o", "--output", help="output WAV (default: alongside input)")
    parser.add_argument("--stats", action="store_true", help="print stats only")
    args = parser.parse_args()

    src = find_audio(pathlib.Path(args.path))
    samples, bad = decode_stream(src.read_bytes())
    if not samples:
        print(f"nothing decodable in {src}", file=sys.stderr)
        return 1
    info = stats(samples)
    print(f"{src}: {info['seconds']:.2f}s, peak {info['peak']}, rms {info['rms']}, bad {bad}")
    if args.stats:
        return 0
    out = pathlib.Path(args.output) if args.output else src.with_suffix(".wav")
    write_wav(out, samples)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
