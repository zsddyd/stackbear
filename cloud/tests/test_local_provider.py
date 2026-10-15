"""The local (sherpa-onnx) provider: wiring, chunking hint and error paths.

No real model and no network: a stub recognizer stands in, so this runs anywhere.
The actual accuracy of SenseVoice is a device/host measurement, not a unit test.
"""

from __future__ import annotations

import os
import sys
import types

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from stackbear import providers, transcribe  # noqa: E402

FAILURES = 0


def check(cond: bool, msg: str) -> None:
    global FAILURES
    if not cond:
        print(f"FAIL: {msg}")
        FAILURES += 1


class _StubStream:
    def __init__(self) -> None:
        self.result = types.SimpleNamespace(text="")

    def accept_waveform(self, rate, pcm) -> None:
        self.rate = rate
        self.samples = len(pcm)


class _StubRecognizer:
    def __init__(self) -> None:
        self.decoded = 0

    def create_stream(self):
        return _StubStream()

    def decode_stream(self, stream) -> None:
        self.decoded += 1
        stream.result.text = f"decoded {stream.samples} samples"


def test_local_provider_is_selected_and_reports_identity() -> None:
    cfg = providers.AsrConfig.from_env({
        "STACKBEAR_ASR_PROVIDER": "local",
        "STACKBEAR_ASR_MODEL_DIR": "/nonexistent",
    })
    check(cfg.vendor == "local", f"vendor defaults to local: {cfg.vendor}")
    check("sense-voice" in cfg.model, f"model label: {cfg.model}")
    # A local provider must not demand an API key.
    check("api_key=missing" in cfg.describe(), "no key required")


def test_chunk_hint_defaults_to_short_blocks() -> None:
    """The local recogniser degrades on long input, so its chunk hint is short."""
    cfg = providers.AsrConfig.from_env({"STACKBEAR_ASR_PROVIDER": "local"})
    check(cfg.chunk_ms == 8000, f"chunk_ms default {cfg.chunk_ms}")


def test_missing_model_dir_is_a_clear_error() -> None:
    cfg = providers.AsrConfig.from_env({"STACKBEAR_ASR_PROVIDER": "local"})
    cfg.model_dir = ""
    provider = providers.get_provider(cfg)
    try:
        provider.transcribe(transcribe.wav_bytes([0] * 160))
    except providers.ProviderError as exc:
        check("MODEL_DIR" in str(exc), f"error names the setting: {exc}")
    else:
        check(False, "no model dir must raise")


def test_missing_files_are_reported() -> None:
    cfg = providers.AsrConfig.from_env({"STACKBEAR_ASR_PROVIDER": "local"})
    cfg.model_dir = "/definitely/not/here"
    try:
        providers.get_provider(cfg).transcribe(transcribe.wav_bytes([0] * 160))
    except providers.ProviderError as exc:
        check("missing model file" in str(exc), f"error names the file: {exc}")
    else:
        check(False, "missing model files must raise")


def test_wav_roundtrip_and_downmix() -> None:
    from stackbear.providers_local import _wav_to_pcm

    mono = transcribe.wav_bytes([100, -100, 200, -200])
    pcm, rate = _wav_to_pcm(mono)
    check(list(pcm) == [100, -100, 200, -200], f"mono passthrough: {list(pcm)}")
    check(rate == 16000, f"rate {rate}")

    # A stereo WAV is downmixed rather than desynchronising the recognizer.
    import io
    import struct
    import wave

    buf = io.BytesIO()
    with wave.open(buf, "wb") as handle:
        handle.setnchannels(2)
        handle.setsampwidth(2)
        handle.setframerate(16000)
        handle.writeframes(struct.pack("<4h", 100, 300, -100, -300))
    pcm, _ = _wav_to_pcm(buf.getvalue())
    check(list(pcm) == [200, -200], f"downmix average: {list(pcm)}")


def main() -> int:
    test_local_provider_is_selected_and_reports_identity()
    test_chunk_hint_defaults_to_short_blocks()
    test_missing_model_dir_is_a_clear_error()
    test_missing_files_are_reported()
    test_wav_roundtrip_and_downmix()
    if FAILURES:
        print(f"test_local_provider: {FAILURES} failure(s)")
        return 1
    print("test_local_provider: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
