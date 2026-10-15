"""merge_spans must grow VAD fragments into context-bearing blocks without
losing or splitting audio. Pure logic; standard library only."""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from stackbear import transcribe  # noqa: E402

FAILURES = 0
RATE = 16000


def check(cond: bool, msg: str) -> None:
    global FAILURES
    if not cond:
        print(f"FAIL: {msg}")
        FAILURES += 1


def ms(value: int) -> int:
    """Milliseconds expressed in samples."""
    return value * RATE // 1000


def test_empty_and_single() -> None:
    check(transcribe.merge_spans([]) == [], "no spans -> no spans")
    check(transcribe.merge_spans([(0, ms(500))]) == [(0, ms(500))],
          "a lone span is returned as is")


def test_short_fragments_are_merged() -> None:
    """The real case: speech arrives as many sub-second VAD fragments."""
    frag = ms(600)
    spans = [(i * frag, (i + 1) * frag) for i in range(20)]
    merged = transcribe.merge_spans(spans)
    check(len(merged) < len(spans), f"must merge, got {len(merged)} blocks")
    for start, end in merged[:-1]:
        length = (end - start) * 1000 // RATE
        check(length >= transcribe.MERGE_TARGET_MS,
              f"every non-final block reaches the target, got {length} ms")
    check(merged[0][0] == spans[0][0], "first sample preserved")
    check(merged[-1][1] == spans[-1][1], "last sample preserved")


def test_no_gaps_or_overlaps_introduced() -> None:
    frag = ms(300)
    spans = [(i * frag, (i + 1) * frag) for i in range(40)]
    merged = transcribe.merge_spans(spans)
    for i in range(1, len(merged)):
        check(merged[i][0] == merged[i - 1][1],
              f"contiguous input must stay contiguous at block {i}")


def test_max_block_size_is_respected() -> None:
    frag = ms(1000)
    spans = [(i * frag, (i + 1) * frag) for i in range(60)]
    merged = transcribe.merge_spans(spans)
    for start, end in merged:
        length = (end - start) * 1000 // RATE
        check(length <= transcribe.MERGE_MAX_MS,
              f"block must not exceed the maximum, got {length} ms")


def test_oversized_segment_is_kept_whole() -> None:
    """A single utterance longer than the maximum is not cut in half."""
    long_span = (0, ms(45000))
    merged = transcribe.merge_spans([long_span, (ms(45000), ms(45600))])
    check(merged[0] == long_span, f"the long segment stays whole, got {merged[0]}")


def test_short_tail_is_folded_in() -> None:
    """A trailing fragment must not be left as its own short request."""
    frag = ms(1000)
    spans = [(i * frag, (i + 1) * frag) for i in range(8)]
    spans.append((8 * frag, 8 * frag + ms(200)))
    merged = transcribe.merge_spans(spans)
    tail_len = (merged[-1][1] - merged[-1][0]) * 1000 // RATE
    check(tail_len >= transcribe.MERGE_TARGET_MS,
          f"tail folded in, last block is {tail_len} ms")


def test_stored_range_is_preserved() -> None:
    """Merged blocks must stay inside the stored range (no invented audio)."""
    spans = [(0, ms(1000)), (ms(1000), ms(2000)), (ms(2000), ms(3000))]
    merged = transcribe.merge_spans(spans)
    check(merged[0][0] == 0, "starts at the first sample")
    check(merged[-1][1] == ms(3000), "ends at the last sample")


def main() -> int:
    test_empty_and_single()
    test_short_fragments_are_merged()
    test_no_gaps_or_overlaps_introduced()
    test_max_block_size_is_respected()
    test_oversized_segment_is_kept_whole()
    test_short_tail_is_folded_in()
    test_stored_range_is_preserved()
    if FAILURES:
        print(f"test_merge_spans: {FAILURES} failure(s)")
        return 1
    print("test_merge_spans: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
