"""The session/transcript index: one entry point over the stored recordings."""

from __future__ import annotations

import json
import os
import pathlib
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from stackbear import server  # noqa: E402

FAILURES = 0


def check(cond: bool, msg: str) -> None:
    global FAILURES
    if not cond:
        print(f"FAIL: {msg}")
        FAILURES += 1


def _session(root: pathlib.Path, device: str, name: str, *, text: str = "",
             audio: int = 0, model: str = "m1") -> pathlib.Path:
    path = root / device / name
    path.mkdir(parents=True)
    if audio:
        (path / "audio.adpcm").write_bytes(b"x" * audio)
    (path / "meta.json").write_text(json.dumps(
        {"audio_ms": 12000, "seg_count": 3, "started_at": 1.0}), encoding="utf-8")
    if text:
        (path / "transcript.txt").write_text(text, encoding="utf-8")
        (path / "transcript.status.json").write_text(
            json.dumps({"ok": True, "model": model, "dropped": 0}), encoding="utf-8")
    return path


def test_index_lists_newest_first_with_state() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp) / "data"
        old = _session(root, "devA", "20260101-100000-0001", text="早上好\n", audio=500)
        time.sleep(0.02)   # ensure a distinct mtime
        new = _session(root, "devA", "20260102-100000-0002", audio=600)
        server.DATA = root

        rows = server.list_sessions()
        check(len(rows) == 2, f"two sessions, got {len(rows)}")
        check(rows[0]["session"] == new.name, "newest first")
        check(rows[1]["session"] == old.name, "older second")

        done = next(r for r in rows if r["session"] == old.name)
        check(done["transcribed"] is True, "transcribed flag")
        check(done["chars"] == 3, f"character count, got {done['chars']}")
        check(done["preview"] == "早上好", f"preview, got {done['preview']!r}")
        check(done["model"] == "m1", "model recorded")
        check(done["audio_bytes"] == 500, "audio size")
        check(done["segments"] == 3, "segment count from meta")

        pending = next(r for r in rows if r["session"] == new.name)
        check(pending["transcribed"] is False, "untranscribed flagged")
        check("chars" not in pending, "no char count before transcription")


def test_filter_by_device_and_limit() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp) / "data"
        _session(root, "devA", "s1", audio=10)
        _session(root, "devA", "s2", audio=10)
        _session(root, "devB", "s3", audio=10)
        server.DATA = root

        check(len(server.list_sessions("devA")) == 2, "device filter")
        check(len(server.list_sessions("devB")) == 1, "other device")
        check(len(server.list_sessions()) == 3, "no filter lists all")
        check(len(server.list_sessions(limit=2)) == 2, "limit honoured")
        check(server.list_sessions("nope") == [], "unknown device yields nothing")


def test_search_returns_matching_lines_only() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp) / "data"
        _session(root, "devA", "s1", text="今天开会讨论屏幕\n明天再看接口\n", audio=10)
        _session(root, "devB", "s2", text="完全无关的内容\n", audio=10)
        _session(root, "devC", "s3", audio=10)   # no transcript
        server.DATA = root

        hits = server.search_transcripts("屏幕")
        check(len(hits) == 1, f"one hit, got {len(hits)}")
        check(hits[0]["matches"] == ["今天开会讨论屏幕"], f"line match: {hits[0]['matches']}")
        check(server.search_transcripts("") == [], "empty query matches nothing")
        check(server.search_transcripts("不存在的词") == [], "no match yields nothing")
        # Search is case-insensitive for latin text.
        _session(root, "devD", "s4", text="Hello World\n", audio=10)
        check(len(server.search_transcripts("hello")) == 1, "case-insensitive search")


def test_search_scans_beyond_the_first_page() -> None:
    """Regression: an early version scanned a bounded page and therefore missed a
    match that sorted below the cutoff, reporting no hit for a word that was
    present. `limit` must bound matches, not how much is scanned."""
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp) / "data"
        # Many later sessions without the word, one early session that has it.
        _session(root, "devA", "20260101-000000-early", text="这里有屏幕\n", audio=10)
        for i in range(60):
            _session(root, "devA", f"20260201-{i:06d}-noise", text="无关内容\n", audio=10)
        server.DATA = root

        hits = server.search_transcripts("屏幕", limit=5)
        check(len(hits) == 1, f"the match must be found despite later sessions: {len(hits)}")
        check(hits[0]["session"] == "20260101-000000-early", f"right session: {hits[0]['session']}")


def test_path_safety() -> None:
    """A crafted device or session name must not escape the data directory."""
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp) / "data"
        _session(root, "devA", "s1", text="secret\n", audio=10)
        server.DATA = root
        # The handler sanitises these; the filter must not throw or match oddly.
        check(server.list_sessions("../../etc") == [], "traversal filter yields nothing")


def test_non_ascii_query_is_decoded() -> None:
    """Regression: the request line is decoded as latin1, so a URL-encoded or raw
    UTF-8 search word arrived as mojibake and never matched. The query string must
    be restored to text first."""
    import urllib.parse

    check(server._decode_query(urllib.parse.quote("屏幕")) == "屏幕", "percent-encoded UTF-8")
    check(server._decode_query("屏幕".encode("utf-8").decode("latin1")) == "屏幕",
          "raw UTF-8 bytes that arrived as latin1")
    check(server._decode_query("dev=abc&limit=5") == "dev=abc&limit=5", "ASCII untouched")
    check(server._decode_query("") == "", "empty query")

    # And end to end: a Chinese word in a transcript is found through the query path.
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp) / "data"
        _session(root, "devA", "s1", text="你你看开发这有屏幕啊\n", audio=10)
        server.DATA = root
        hits = server.search_transcripts(urllib.parse.unquote(
            urllib.parse.quote("屏幕")))
        check(len(hits) == 1, f"Chinese search finds the session, got {len(hits)}")


def main() -> int:
    test_index_lists_newest_first_with_state()
    test_filter_by_device_and_limit()
    test_search_returns_matching_lines_only()
    test_search_scans_beyond_the_first_page()
    test_path_safety()
    test_non_ascii_query_is_decoded()
    if FAILURES:
        print(f"test_sessions_index: {FAILURES} failure(s)")
        return 1
    print("test_sessions_index: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
