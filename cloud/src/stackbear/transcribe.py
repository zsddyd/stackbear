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

# Segments shorter than this are not worth a separate provider call: models tend
# to answer a 60 ms clip with tool calls or nothing at all, and one bad segment
# used to abort the whole session. Shorter spans are skipped; if nothing is left,
# the session is transcribed as a whole.
MIN_SEGMENT_MS = 400


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


def transcribe_session(session: pathlib.Path, *, config: providers.AsrConfig | None = None,
                       per_segment: bool = False, language: str | None = None,
                       transport=None, provider=None) -> dict:
    """Transcribe a stored session and return the result payload.

    Shared by the CLI and the receiver's end-of-session hook so both paths use
    the same decoding, segmentation and provider code. Raises
    providers.ProviderError on a provider failure and FileNotFoundError when the
    session has no audio.
    """
    src = adpcm.find_audio(session)
    samples, _ = adpcm.decode_stream(src.read_bytes())
    if not samples:
        raise FileNotFoundError(f"nothing decodable in {src}")
    cfg = config or providers.AsrConfig.from_env()
    # `provider` is injectable so the receiver and tests can supply an
    # alternative engine without going through the environment.
    provider = provider or providers.get_provider(cfg, transport=transport)

    meta = {}
    meta_path = session / "meta.json"
    if meta_path.is_file():
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            meta = {}

    def run(span: list[int]) -> providers.Transcript:
        blob = wav_bytes(span)
        if cfg.audio_mode == "url":
            with publish(blob, cfg.public_url) as url:
                return provider.transcribe(blob, language=language, audio_url=url)
        return provider.transcribe(blob, language=language)

    results: list[dict] = []
    errors: list[dict] = []
    spans = []
    if per_segment:
        min_samples = MIN_SEGMENT_MS * adpcm.SAMPLE_RATE // 1000
        spans = [(s, e) for (s, e) in segment_slices(meta, len(samples))
                 if e - s >= min_samples]
    if spans:
        for index, (start, end) in enumerate(spans):
            try:
                transcript = run(samples[start:end])
            except providers.ProviderError as exc:
                # One unusable utterance must not lose the rest of the session.
                errors.append({"index": index, "start_ms": start * 1000 // adpcm.SAMPLE_RATE,
                               "error": str(exc)})
                continue
            results.append({"index": index,
                            "start_ms": start * 1000 // adpcm.SAMPLE_RATE,
                            "end_ms": end * 1000 // adpcm.SAMPLE_RATE,
                            "text": transcript.text,
                            "vendor": transcript.vendor, "model": transcript.model,
                            "usage": usage_of(transcript)})
        if not results:
            # Every segment failed: retry the session as one piece before giving up.
            errors.clear()
            spans = []
    if not spans:
        transcript = run(samples)
        results.append({"index": 0, "start_ms": 0,
                        "end_ms": len(samples) * 1000 // adpcm.SAMPLE_RATE,
                        "text": transcript.text,
                        "vendor": transcript.vendor, "model": transcript.model,
                        "usage": usage_of(transcript)})
    # Record the model the provider actually reported, not just the requested
    # one: a gateway may route to a different variant.
    used_model = next((r["model"] for r in results if r.get("model")), cfg.model)
    used_vendor = next((r["vendor"] for r in results if r.get("vendor")), cfg.vendor)
    return {"provider": cfg.provider, "vendor": used_vendor, "model": used_model,
            "results": results, "errors": errors}


def usage_of(transcript: providers.Transcript) -> dict:
    """Token accounting as reported by the gateway, for cost tracking."""
    usage = (transcript.raw or {}).get("usage") or {}
    return {k: usage.get(k) for k in
            ("prompt_tokens", "completion_tokens", "total_tokens",
             "cost", "cost_details") if k in usage}


def write_result(session: pathlib.Path, payload: dict) -> None:
    (session / "transcript.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    (session / "transcript.txt").write_text(
        "\n".join(r["text"] for r in payload["results"]) + "\n", encoding="utf-8")


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
    parser.add_argument("--usage", action="store_true",
                        help="print the gateway's token accounting (for cost tracking)")
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

    if args.dry_run:
        meta = {}
        meta_path = session / "meta.json"
        if meta_path.is_file():
            try:
                meta = json.loads(meta_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                meta = {}
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
    print(f"using {config.describe()}", file=sys.stderr)

    if config.audio_mode == "url" and not config.public_url:
        print("audio_mode=url requires STACKBEAR_ASR_PUBLIC_URL", file=sys.stderr)
        return 2
    try:
        payload = transcribe_session(session, config=config, per_segment=args.per_segment,
                                     language=args.language)
    except providers.ProviderError as exc:
        print(f"transcription failed: {exc}", file=sys.stderr)
        return 2

    for row in payload["results"]:
        if args.usage and row.get("usage"):
            print(f"usage: {row['usage']}", file=sys.stderr)
        print(f"[{row['start_ms']:>7} ms] {row['text']}" if len(payload["results"]) > 1
              else row["text"])
    if args.write:
        write_result(session, payload)
        print(f"wrote {session}/transcript.json and transcript.txt", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
