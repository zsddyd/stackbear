"""The receiver must transcribe a finished session on its own, without blocking
the receive loop, and record a status file either way. A fake provider stands in
for the network, so this needs no key."""

from __future__ import annotations

import json
import os
import pathlib
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from stackbear import adpcm, providers, server, transcribe  # noqa: E402

FAILURES = 0


def check(cond: bool, msg: str) -> None:
    global FAILURES
    if not cond:
        print(f"FAIL: {msg}")
        FAILURES += 1


class FakeProvider(providers.Provider):
    """Echoes a per-call marker so segment count is observable."""

    name = "fake"
    calls = 0
    fail_first = False

    def transcribe(self, wav, *, language=None, audio_url=None):
        FakeProvider.calls += 1
        if FakeProvider.fail_first and FakeProvider.calls == 1:
            raise providers.ProviderError("upstream refused a short clip")
        return providers.Transcript(text=f"line{FakeProvider.calls}", provider="fake",
                                    vendor="fake", model="fake-1",
                                    raw={"usage": {"total_tokens": 7, "cost": 0.0}})


def _meta(segments: list[dict]) -> dict:
    return {"dev": "dev1", "codec": "adpcm", "segments": segments}


def _make_session(root: pathlib.Path) -> pathlib.Path:
    """Three speech blocks in two segments, as the firmware would write them."""
    session = root / "dev1" / "20260101-000000-abcd"
    session.mkdir(parents=True)
    blocks = [adpcm.encode_block([0] * adpcm.FRAME_SAMPLES) for _ in range(3)]
    (session / "audio.adpcm").write_bytes(adpcm.pack_blocks(blocks))
    (session / "meta.json").write_text(json.dumps(_meta(
        [{"start_ms": 0, "first_seq": 0, "frames": 2},
         {"start_ms": 120, "first_seq": 2, "frames": 1}])), encoding="utf-8")
    return session


def _make_long_session(root: pathlib.Path, frames_per_seg: int,
                       segments: int) -> pathlib.Path:
    """A session whose VAD segments are long enough to submit individually."""
    session = root / "dev2" / "20260101-000000-beef"
    session.mkdir(parents=True)
    total = frames_per_seg * segments
    blocks = [adpcm.encode_block([0] * adpcm.FRAME_SAMPLES) for _ in range(total)]
    (session / "audio.adpcm").write_bytes(adpcm.pack_blocks(blocks))
    segs = [{"start_ms": i * frames_per_seg * adpcm.FRAME_MS, "first_seq": 0,
             "frames": frames_per_seg} for i in range(segments)]
    (session / "meta.json").write_text(json.dumps(_meta(segs)), encoding="utf-8")
    return session


def _inject_fake():
    original = transcribe.providers.get_provider
    transcribe.providers.get_provider = lambda cfg=None, transport=None: FakeProvider(
        cfg or providers.AsrConfig(provider="fake", model="fake-1", api_key="x"))
    return original


def test_worker_writes_transcript_and_status() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        # Two 3.84 s segments: long enough to be submitted individually.
        session = _make_long_session(pathlib.Path(tmp), frames_per_seg=64, segments=2)
        FakeProvider.calls = 0
        FakeProvider.fail_first = False
        original = _inject_fake()
        try:
            server.AUTO_TRANSCRIBE = True
            server.AUTO_PER_SEGMENT = True
            server._transcribe_worker(session)
        finally:
            transcribe.providers.get_provider = original

        text = (session / "transcript.txt").read_text(encoding="utf-8")
        check(text == "line1\nline2\n", f"one line per segment, got {text!r}")
        payload = json.loads((session / "transcript.json").read_text(encoding="utf-8"))
        check(payload["model"] == "fake-1", "model recorded")
        check([r["start_ms"] for r in payload["results"]] == [0, 3840], "segment offsets")
        status = json.loads((session / "transcript.status.json").read_text(encoding="utf-8"))
        check(status["ok"] is True and status["segments"] == 2, f"status {status}")


def test_short_segments_are_skipped() -> None:
    """A 60 ms fragment must not be sent as its own request; the session falls
    back to one whole-file transcription instead."""
    with tempfile.TemporaryDirectory() as tmp:
        session = _make_session(pathlib.Path(tmp))  # 3 x 60 ms segments
        FakeProvider.calls = 0
        FakeProvider.fail_first = False
        original = _inject_fake()
        server.AUTO_TRANSCRIBE = True
        server.AUTO_PER_SEGMENT = True
        try:
            server._transcribe_worker(session)
        finally:
            transcribe.providers.get_provider = original
        payload = json.loads((session / "transcript.json").read_text(encoding="utf-8"))
        check(len(payload["results"]) == 1, f"one whole-file result, got {len(payload['results'])}")
        check(FakeProvider.calls == 1, f"exactly one provider call, got {FakeProvider.calls}")


def test_one_bad_segment_does_not_lose_the_session() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        session = _make_long_session(pathlib.Path(tmp), frames_per_seg=64, segments=3)
        FakeProvider.calls = 0
        FakeProvider.fail_first = True  # first segment fails, the rest must survive
        original = _inject_fake()
        server.AUTO_TRANSCRIBE = True
        server.AUTO_PER_SEGMENT = True
        try:
            server._transcribe_worker(session)
        finally:
            transcribe.providers.get_provider = original
        payload = json.loads((session / "transcript.json").read_text(encoding="utf-8"))
        check(len(payload["results"]) == 2, f"two good segments kept, got {len(payload['results'])}")
        check(len(payload["errors"]) == 1, f"one error recorded, got {payload.get('errors')}")
        status = json.loads((session / "transcript.status.json").read_text(encoding="utf-8"))
        check(status["ok"] is True and status["dropped"] == 1, f"status {status}")


def test_worker_records_failure() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        session = _make_session(pathlib.Path(tmp))

        def boom(*a, **k):
            raise providers.ProviderError("no API key")

        original = transcribe.transcribe_session
        transcribe.transcribe_session = boom
        server.AUTO_TRANSCRIBE = True
        try:
            server._transcribe_worker(session)
        finally:
            transcribe.transcribe_session = original
        status = json.loads((session / "transcript.status.json").read_text(encoding="utf-8"))
        check(status["ok"] is False and "no API key" in status["error"], f"status {status}")
        check(not (session / "transcript.txt").exists(), "no transcript on failure")


def test_auto_can_be_disabled() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        session = _make_session(pathlib.Path(tmp))
        server.AUTO_TRANSCRIBE = False
        server._schedule_transcribe(session)
        check(not (session / "transcript.status.json").exists(), "disabled -> nothing scheduled")


def main() -> int:
    test_worker_writes_transcript_and_status()
    test_short_segments_are_skipped()
    test_one_bad_segment_does_not_lose_the_session()
    test_worker_records_failure()
    test_auto_can_be_disabled()
    if FAILURES:
        print(f"test_autotranscribe: {FAILURES} failure(s)")
        return 1
    print("test_autotranscribe: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
