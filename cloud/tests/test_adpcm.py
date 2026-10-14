"""Unit tests for the cloud-side IMA-ADPCM codec. Standard library only."""

from __future__ import annotations

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from stackbear import adpcm  # noqa: E402

FAILURES = 0


def check(cond: bool, msg: str) -> None:
    global FAILURES
    if not cond:
        print(f"FAIL: {msg}")
        FAILURES += 1


def _triangle(n: int, amp: int, period: int) -> list[int]:
    out = []
    for i in range(n):
        phase = i % period
        half = period // 2
        tri = phase if phase < half else period - phase
        out.append((tri * 4 * amp) // period - amp)
    return out


def test_block_shape() -> None:
    pcm = _triangle(adpcm.FRAME_SAMPLES, 8000, 36)
    block = adpcm.encode_block(pcm)
    check(len(block) == adpcm.BLOCK_BYTES, f"block size {len(block)}")
    predictor = struct.unpack_from("<h", block, 0)[0]
    check(predictor == pcm[0], "header stores first sample")
    check(block[2] == 0 and block[3] == 0, "initial index/reserved are zero")


def test_roundtrip_is_bit_exact() -> None:
    """The encoder and decoder must reconstruct identical samples, otherwise a
    stored session would not match what the firmware produced."""
    pcm = _triangle(adpcm.FRAME_SAMPLES, 8000, 36)
    block = adpcm.encode_block(pcm)
    got = adpcm.decode_block(block)
    check(len(got) == adpcm.FRAME_SAMPLES, "sample count")
    # Decode, then re-encode the decoded samples: the second block must be
    # byte-identical, proving the state machine is stable.
    again = adpcm.encode_block(got)
    check(again == block, "re-encode of decoded samples is identical")


def test_steady_state_error_is_small() -> None:
    """A block always restarts at step index 0, so the first few samples lag on a
    steep attack; what matters for ASR is the steady-state error."""
    pcm = _triangle(adpcm.FRAME_SAMPLES, 8000, 36)
    block = adpcm.encode_block(pcm)
    got = adpcm.decode_block(block)
    half = adpcm.FRAME_SAMPLES // 2
    tail_err = max(abs(a - b) for a, b in zip(pcm[half:], got[half:]))
    check(tail_err < 1500, f"steady-state max error {tail_err}")


def test_silence_is_exact() -> None:
    pcm = [0] * adpcm.FRAME_SAMPLES
    got = adpcm.decode_block(adpcm.encode_block(pcm))
    check(all(v == 0 for v in got), "silence round-trips exactly")


def test_container_roundtrip_and_damage() -> None:
    blocks = [adpcm.encode_block(_triangle(adpcm.FRAME_SAMPLES, 4000, 40)) for _ in range(3)]
    blob = adpcm.pack_blocks(blocks)
    samples, bad = adpcm.decode_stream(blob)
    check(len(samples) == 3 * adpcm.FRAME_SAMPLES, "three blocks decode")
    check(bad == 0, "no malformed blocks")

    # A truncated tail must be reported, not silently accepted.
    _, bad2 = adpcm.decode_stream(blob[:-10])
    check(bad2 == 1, "truncated stream reports a bad block")


def test_rejects_bad_input() -> None:
    check(adpcm.decode_block(b"\x00\x01") == [], "short block decodes to nothing")
    check(adpcm.decode_block(b"\x00\x00\xff\x00" + b"\x00" * 4) == [], "index 0xff rejected")
    try:
        adpcm.encode_block([1, 2, 3])
    except ValueError:
        check(True, "odd sample count rejected")
    else:
        check(False, "odd sample count must raise")


def main() -> int:
    test_block_shape()
    test_roundtrip_is_bit_exact()
    test_steady_state_error_is_small()
    test_silence_is_exact()
    test_container_roundtrip_and_damage()
    test_rejects_bad_input()
    if FAILURES:
        print(f"test_adpcm: {FAILURES} failure(s)")
        return 1
    print("test_adpcm: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
