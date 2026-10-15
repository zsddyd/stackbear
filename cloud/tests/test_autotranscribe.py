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
    """A session with several VAD segments.

    Note that segments are merged before submission (see transcribe.merge_spans),
    so a caller wanting one block per segment must pass frames_per_seg large
    enough that a single segment already exceeds MERGE_TARGET_MS.
    """
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
        # 每段 10s:超过 MERGE_TARGET_MS(8s),所以各段不会被合并,一段一个请求。
        session = _make_long_session(pathlib.Path(tmp), frames_per_seg=167, segments=2)
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
        check([r["start_ms"] for r in payload["results"]] == [0, 10020], "segment offsets")
        status = json.loads((session / "transcript.status.json").read_text(encoding="utf-8"))
        check(status["ok"] is True and status["segments"] == 2, f"status {status}")


def test_fragments_merge_instead_of_being_dropped() -> None:
    """Three 60 ms fragments must become one request, not three - and none of the
    audio may be discarded (an earlier rule dropped sub-400 ms segments)."""
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
        check(len(payload["results"]) == 1, f"one merged result, got {len(payload['results'])}")
        check(FakeProvider.calls == 1, f"exactly one provider call, got {FakeProvider.calls}")


def test_one_bad_segment_does_not_lose_the_session() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        session = _make_long_session(pathlib.Path(tmp), frames_per_seg=167, segments=3)
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


def test_empty_session_is_not_a_failure() -> None:
    """A session where VAD found nothing is normal: it must be recorded as
    'no speech', not as a transcription error, and no provider call is made."""
    with tempfile.TemporaryDirectory() as tmp:
        session = pathlib.Path(tmp) / "dev3" / "20260101-000000-0001"
        session.mkdir(parents=True)
        (session / "audio.adpcm").write_bytes(b"")      # nothing captured

        FakeProvider.calls = 0
        original = _inject_fake()
        server.AUTO_TRANSCRIBE = True
        try:
            server._transcribe_worker(session)
        finally:
            transcribe.providers.get_provider = original

        check(FakeProvider.calls == 0, "no provider call for an empty session")
        st = json.loads((session / "transcript.status.json").read_text(encoding="utf-8"))
        check(st.get("ok") is True, f"empty session is not an error: {st}")
        check(st.get("skipped") == "no speech", f"marked as no speech: {st}")


def test_corrupt_audio_is_a_failure() -> None:
    """Bytes that decode to nothing ARE a defect and must be reported as one."""
    with tempfile.TemporaryDirectory() as tmp:
        session = pathlib.Path(tmp) / "dev3" / "20260101-000000-0002"
        session.mkdir(parents=True)
        (session / "audio.adpcm").write_bytes(b"\x00\x01garbage-not-a-frame")

        original = _inject_fake()
        server.AUTO_TRANSCRIBE = True
        try:
            server._transcribe_worker(session)
        finally:
            transcribe.providers.get_provider = original
        st = json.loads((session / "transcript.status.json").read_text(encoding="utf-8"))
        check(st.get("ok") is False, f"corrupt audio is an error: {st}")
        check("corrupt" in st.get("error", ""), f"error names the cause: {st}")


def test_pending_marker_is_cleared_and_detects_orphans() -> None:
    """A killed transcription leaves a marker; the startup scan finds it and
    re-queues, and a session with a transcript is left alone."""
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp) / "data"
        server.DATA = root
        done = root / "dev" / "s-done"
        done.mkdir(parents=True)
        (done / "audio.adpcm").write_bytes(adpcm.pack_blocks(
            [adpcm.encode_block([0] * adpcm.FRAME_SAMPLES)]))
        (done / "transcript.txt").write_text("already done\n", encoding="utf-8")

        orphan = root / "dev" / "s-orphan"
        orphan.mkdir(parents=True)
        (orphan / "audio.adpcm").write_bytes(adpcm.pack_blocks(
            [adpcm.encode_block([0] * adpcm.FRAME_SAMPLES)]))

        empty = root / "dev" / "s-empty"
        empty.mkdir(parents=True)
        (empty / "audio.adpcm").write_bytes(b"")

        found = server.find_orphaned_sessions()
        names = sorted(p.name for p in found)
        check(names == ["s-orphan"], f"only the unfinished session, got {names}")

        # A successful run clears its pending marker.
        FakeProvider.calls = 0
        FakeProvider.fail_first = False
        original = _inject_fake()
        server.AUTO_TRANSCRIBE = True
        try:
            server._transcribe_worker(orphan)
        finally:
            transcribe.providers.get_provider = original
        check(not (orphan / "transcript.pending").exists(), "pending cleared after success")
        check(server.find_orphaned_sessions() == [], "nothing left to requeue")


def main() -> int:
    test_worker_writes_transcript_and_status()
    test_fragments_merge_instead_of_being_dropped()
    test_one_bad_segment_does_not_lose_the_session()
    test_worker_records_failure()
    test_auto_can_be_disabled()
    test_empty_session_is_not_a_failure()
    test_corrupt_audio_is_a_failure()
    test_pending_marker_is_cleared_and_detects_orphans()
    if FAILURES:
        print(f"test_autotranscribe: {FAILURES} failure(s)")
        return 1
    print("test_autotranscribe: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
