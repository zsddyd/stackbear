"""Transcribe a stored session.

Decodes the session's IMA-ADPCM to WAV in memory and hands it to the configured
ASR provider. With `--per-segment` it uses the VAD boundaries recorded in
`meta.json` and submits each utterance separately, which gives one line per
utterance with its start offset instead of one blob of text.

Examples:
    PYTHONPATH=src python -m stackbear.transcribe data/<dev>/<session> --dry-run
    STACKBEAR_ASR_API_KEY=... PYTHONPATH=src python -m stackbear.transcribe \
        data/<dev>/<session> --per-segment --write
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import pathlib
import secrets
import struct
import sys
import wave

from . import adpcm, providers


@contextlib.contextmanager
def publish(wav: bytes, public_url: str):
    """Expose exactly this one WAV on the receiver's HTTP port for the duration
    of the call, then remove it.

    Only one file is ever present in the share directory: anything left from an
    earlier run is deleted first, so the endpoint can never serve two files.
    """
    from .server import SHARE_DIR

    SHARE_DIR.mkdir(parents=True, exist_ok=True)
    for stale in SHARE_DIR.iterdir():
        if stale.is_file():
            stale.unlink(missing_ok=True)
    token = secrets.token_hex(16)
    path = SHARE_DIR / f"{token}.wav"
    path.write_bytes(wav)
    try:
        yield f"{public_url.rstrip('/')}/a/{token}.wav"
    finally:
        path.unlink(missing_ok=True)


def wav_bytes(samples: list[int]) -> bytes:
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(adpcm.SAMPLE_RATE)
        wav.writeframes(struct.pack(f"<{len(samples)}h", *samples))
    return buffer.getvalue()


def segment_slices(meta: dict, total: int) -> list[tuple[int, int]]:
    """Return (start, end) sample ranges from the recorded VAD segments."""
    spans = []
    for seg in meta.get("segments", []) or []:
        start = int(seg.get("start_ms", 0)) * adpcm.SAMPLE_RATE // 1000
        frames = int(seg.get("frames", 0))
        end = min(start + frames * adpcm.FRAME_SAMPLES, total)
        if end > start:
            spans.append((start, end))
    return spans


def main() -> int:
    parser = argparse.ArgumentParser(description="Transcribe a stored session")
    parser.add_argument("path", help="session directory or audio file")
    parser.add_argument("--provider", help="override STACKBEAR_ASR_PROVIDER")
    parser.add_argument("--model", help="override STACKBEAR_ASR_MODEL")
    parser.add_argument("--language", help="optional language hint for the model")
    parser.add_argument("--per-segment", action="store_true",
                        help="transcribe each recorded VAD segment separately")
    parser.add_argument("--dry-run", action="store_true",
                        help="decode and report only; do not call the provider")
    parser.add_argument("--write", action="store_true",
                        help="write transcript.txt / transcript.json next to the session")
    args = parser.parse_args()

    session = pathlib.Path(args.path)
    src = adpcm.find_audio(session)
    samples, bad = adpcm.decode_stream(src.read_bytes())
    if not samples:
        print(f"nothing decodable in {src}", file=sys.stderr)
        return 1
    info = adpcm.stats(samples)
    print(f"{src}: {info['seconds']:.2f}s, peak {info['peak']}, rms {info['rms']}, bad {bad}",
          file=sys.stderr)

    meta = {}
    meta_path = session / "meta.json"
    if meta_path.is_file():
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            meta = {}

    if args.dry_run:
        spans = segment_slices(meta, len(samples))
        print(f"dry run: provider config -> {providers.AsrConfig.from_env().describe()}", file=sys.stderr)
        print(f"dry run: {len(spans)} segment(s) recorded" if spans else "dry run: no segment metadata",
              file=sys.stderr)
        return 0

    config = providers.AsrConfig.from_env()
    if args.provider:
        config.provider = args.provider
        config.vendor = args.provider
    if args.model:
        config.model = args.model
    provider = providers.get_provider(config)
    print(f"using {config.describe()}", file=sys.stderr)

    if config.audio_mode == "url" and not config.public_url:
        print("audio_mode=url requires STACKBEAR_ASR_PUBLIC_URL", file=sys.stderr)
        return 2

    def run(span_samples: list[int]) -> providers.Transcript:
        """Transcribe one span, publishing it over HTTP first when the provider
        is configured to hand the model a URL instead of inline audio."""
        blob = wav_bytes(span_samples)
        if config.audio_mode == "url":
            with publish(blob, config.public_url) as url:
                print(f"published {len(blob)} bytes at {url}", file=sys.stderr)
                return provider.transcribe(blob, language=args.language, audio_url=url)
        return provider.transcribe(blob, language=args.language)

    results: list[dict] = []
    try:
        spans = segment_slices(meta, len(samples)) if args.per_segment else []
        if spans:
            for index, (start, end) in enumerate(spans):
                transcript = run(samples[start:end])
                start_ms = start * 1000 // adpcm.SAMPLE_RATE
                results.append({"index": index, "start_ms": start_ms,
                                "end_ms": end * 1000 // adpcm.SAMPLE_RATE,
                                "text": transcript.text,
                                "vendor": transcript.vendor, "model": transcript.model})
                print(f"[{start_ms:>7} ms] {transcript.text}")
        else:
            if args.per_segment:
                print("no segment metadata; transcribing the whole session", file=sys.stderr)
            transcript = run(samples)
            results.append({"index": 0, "start_ms": 0,
                            "end_ms": len(samples) * 1000 // adpcm.SAMPLE_RATE,
                            "text": transcript.text,
                            "vendor": transcript.vendor, "model": transcript.model})
            print(transcript.text)
    except providers.ProviderError as exc:
        print(f"transcription failed: {exc}", file=sys.stderr)
        return 2

    if args.write:
        payload = {"provider": config.provider, "vendor": config.vendor,
                   "model": config.model, "results": results}
        (session / "transcript.json").write_text(
            json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        (session / "transcript.txt").write_text(
            "\n".join(r["text"] for r in results) + "\n", encoding="utf-8")
        print(f"wrote {session}/transcript.json and transcript.txt", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
