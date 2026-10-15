"""Tests for the ASR provider layer. The HTTP call is injected, so nothing
touches the network and no API key is needed."""

from __future__ import annotations

import base64
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from stackbear import providers, transcribe  # noqa: E402

FAILURES = 0


def check(cond: bool, msg: str) -> None:
    global FAILURES
    if not cond:
        print(f"FAIL: {msg}")
        FAILURES += 1


MODEL = "nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free"


def _config(**overrides) -> providers.AsrConfig:
    env = {
        "STACKBEAR_ASR_PROVIDER": "openrouter",
        "STACKBEAR_ASR_MODEL": MODEL,
        "STACKBEAR_ASR_API_KEY": "test-key-not-a-secret",
        "STACKBEAR_ASR_BASE_URL": "https://openrouter.ai/api/v1",
    }
    env.update(overrides)
    return providers.AsrConfig.from_env(env)


def test_config_from_env() -> None:
    cfg = _config()
    check(cfg.provider == "openrouter", "provider name")
    check(cfg.model == MODEL, "model name")
    check(cfg.vendor == "openrouter", "vendor label defaults to provider")
    check("test-key" not in cfg.describe(), "describe() must not leak the key")
    check("set" in cfg.describe(), "describe() reports key presence")


def test_request_shape() -> None:
    seen: dict = {}

    def transport(url, data, headers, timeout):
        seen["url"] = url
        seen["headers"] = headers
        seen["payload"] = json.loads(data.decode())
        seen["timeout"] = timeout
        return {"choices": [{"message": {"content": "  你好，这是一段测试语音。 "}}]}

    provider = providers.get_provider(_config(), transport=transport)
    result = provider.transcribe(b"RIFFfake-wav-bytes")

    check(seen["url"] == "https://openrouter.ai/api/v1/chat/completions", f"url {seen['url']}")
    check(seen["headers"]["Authorization"].startswith("Bearer "), "bearer auth header")
    check(seen["payload"]["model"] == MODEL, "model echoed in payload")
    parts = seen["payload"]["messages"][0]["content"]
    audio = [p for p in parts if p.get("type") == "input_audio"][0]["input_audio"]
    check(audio["format"] == "wav", "audio sent as wav")
    check(base64.b64decode(audio["data"]) == b"RIFFfake-wav-bytes", "audio is base64 of the wav")
    check(result.text == "你好，这是一段测试语音。", f"text trimmed: {result.text!r}")
    check(result.vendor == "openrouter" and result.model == MODEL, "result identity")


def test_language_hint_is_added() -> None:
    seen: dict = {}

    def transport(url, data, headers, timeout):
        seen["payload"] = json.loads(data.decode())
        return {"choices": [{"message": {"content": "ok"}}]}

    providers.get_provider(_config(), transport=transport).transcribe(b"x", language="Chinese")
    text = seen["payload"]["messages"][0]["content"][0]["text"]
    check("Chinese" in text, f"language hint not in prompt: {text}")


def test_preset_and_override_behaviour() -> None:
    """Presets fill the blanks (so switching provider is one variable), explicit
    STACKBEAR_ASR_* values still win, and a missing key is reported clearly."""
    # opencode-go preset supplies endpoint, vendor and an audio-capable model,
    # and reads its own key variable.
    cfg = providers.AsrConfig.from_env({
        "STACKBEAR_ASR_PROVIDER": "opencode-go",
        "OPENCODE_API_KEY": "test-key-not-a-secret",
    })
    check(cfg.base_url == "https://opencode.ai/inference/go/v1", f"go base_url {cfg.base_url}")
    check(cfg.model == "mimo-v2.6-flash", f"go default model {cfg.model}")
    check(cfg.api_key == "test-key-not-a-secret", "go key from OPENCODE_API_KEY")
    check(cfg.vendor == "opencode-go", f"go vendor {cfg.vendor}")
    check("test-key" not in cfg.describe(), "describe() must not leak the key")

    # Explicit values override the preset.
    cfg2 = providers.AsrConfig.from_env({
        "STACKBEAR_ASR_PROVIDER": "opencode-go",
        "STACKBEAR_ASR_MODEL": "mimo-v2.6-pro",
        "OPENCODE_API_KEY": "k",
    })
    check(cfg2.model == "mimo-v2.6-pro", f"override model {cfg2.model}")

    # Missing key must be reported, not silently attempted.
    for provider in ("openrouter", "opencode-go"):
        try:
            providers.get_provider(providers.AsrConfig.from_env(
                {"STACKBEAR_ASR_PROVIDER": provider}), transport=lambda *a: {}).transcribe(b"x")
        except providers.ProviderError as exc:
            check("API key" in str(exc), f"{provider}: error should mention API key: {exc}")
        else:
            check(False, f"{provider}: missing key must raise")


def test_provider_error_and_bad_shape() -> None:
    def failing(url, data, headers, timeout):
        return {"error": {"message": "rate limited", "code": 429}}

    try:
        providers.get_provider(_config(), transport=failing).transcribe(b"x")
    except providers.ProviderError as exc:
        check("rate limited" in str(exc), f"provider error surfaced: {exc}")
    else:
        check(False, "error payload must raise")

    try:
        providers.get_provider(_config(), transport=lambda *a: {"unexpected": 1}).transcribe(b"x")
    except providers.ProviderError as exc:
        check("unexpected response shape" in str(exc), f"shape error: {exc}")
    else:
        check(False, "bad shape must raise")


def test_unknown_provider() -> None:
    try:
        providers.get_provider(_config(STACKBEAR_ASR_PROVIDER="nope"))
    except providers.ProviderError as exc:
        check("unknown provider" in str(exc), f"unknown provider message: {exc}")
    else:
        check(False, "unknown provider must raise")


def test_segment_slices_use_meta() -> None:
    meta = {"segments": [{"start_ms": 0, "frames": 3}, {"start_ms": 180, "frames": 2}]}
    spans = transcribe.segment_slices(meta, 10_000)
    check(spans == [(0, 3 * 960), (180 * 16, 180 * 16 + 2 * 960)], f"spans {spans}")
    check(transcribe.segment_slices({}, 100) == [], "no metadata -> no spans")


def test_wav_bytes_header() -> None:
    blob = transcribe.wav_bytes([0] * 100)
    check(blob[:4] == b"RIFF" and blob[8:12] == b"WAVE", "wav container")
    check(len(blob) == 44 + 200, f"wav size {len(blob)}")


def test_reasoning_field_fallbacks() -> None:
    """Reasoning models leave content null; the answer may arrive as
    reasoning_content, reasoning or text depending on the gateway."""
    provider = providers.OpenAiCompatibleProvider(
        providers.AsrConfig(provider="x", model="m", api_key="k"))
    cases = (
        ({"content": "plain"}, "plain"),
        ({"content": None, "reasoning_content": "rc"}, "rc"),
        ({"content": None, "reasoning": "r"}, "r"),
        ({"content": None, "text": "tt"}, "tt"),
        ({"content": [{"type": "text", "text": "a"}, {"text": "b"}]}, "ab"),
        ({"content": "c", "reasoning": "r"}, "c"),  # content wins when present
    )
    for message, expected in cases:
        got = provider._parse({"choices": [{"message": message}]}).text
        check(got == expected, f"{message} -> {got!r}, want {expected!r}")


def main() -> int:
    test_config_from_env()
    test_request_shape()
    test_language_hint_is_added()
    test_preset_and_override_behaviour()
    test_provider_error_and_bad_shape()
    test_unknown_provider()
    test_segment_slices_use_meta()
    test_wav_bytes_header()
    test_reasoning_field_fallbacks()
    if FAILURES:
        print(f"test_providers: {FAILURES} failure(s)")
        return 1
    print("test_providers: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
