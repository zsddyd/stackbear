"""ASR provider layer.

The receiver must not care which vendor transcribes the audio, so transcription
goes through one small abstraction: a provider is chosen by name, configured by
vendor/model/endpoint/key, and turns WAV bytes into text.

Configuration is read from the environment only. Keys are never written to the
repository, logs or session metadata:

    STACKBEAR_ASR_PROVIDER   provider name (default: openrouter)
    STACKBEAR_ASR_VENDOR     vendor label recorded with the result
    STACKBEAR_ASR_MODEL      model id, e.g.
                             nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free
    STACKBEAR_ASR_BASE_URL   API base (default: https://openrouter.ai/api/v1)
    STACKBEAR_ASR_API_KEY    bearer token for the endpoint
    STACKBEAR_ASR_TIMEOUT    request timeout in seconds (default: 120)
    STACKBEAR_ASR_PROMPT     override the transcription instruction
    STACKBEAR_ASR_AUDIO_MODE inline (default) or url; "url" hands the model a
                             link to fetch instead of inline base64
    STACKBEAR_ASR_PUBLIC_URL base URL the model can reach, e.g.
                             http://8.137.56.181:18080
    STACKBEAR_ASR_AUDIO_PART which content part to use for URL mode:
                             input_audio_url (default) | file_url | file_base64
    STACKBEAR_ASR_USER_AGENT client identity sent to the endpoint (default
                             stackbear/0.1); OpenCode Go requires a real client
                             name because Cloudflare rejects generic SDK agents

Adding a vendor means adding one class below and registering it in PROVIDERS;
nothing else in the receiver changes.

Two providers are registered:
  openrouter  chat completions with an `input_audio` part (default)
  opencode-go OpenCode's Go subscription, which speaks an OpenAI-compatible
              chat API at https://opencode.ai/inference/go/v1 and offers
              audio-capable models (MiMo omni, Gemini argon, Muse Spark)
"""

from __future__ import annotations

import base64
import json
import os
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Callable

DEFAULT_PROVIDER = "openrouter"
PRESETS = {
    # name: (base_url, vendor label, default model, key env var)
    "openrouter": ("https://openrouter.ai/api/v1", "openrouter", "qwen/qwen3.8-omni-flash",
                   "STACKBEAR_ASR_API_KEY"),
    # OpenCode Go: subscription access to, among others, audio-capable models.
    # mimo-v2.6-flash is the fast/cheap omni option, best fit for transcription.
    "opencode-go": ("https://opencode.ai/inference/go/v1", "opencode-go", "mimo-v2.6-flash",
                    "OPENCODE_API_KEY"),
}
DEFAULT_OPENROUTER_URL, DEFAULT_VENDOR, DEFAULT_MODEL, _ = PRESETS[DEFAULT_PROVIDER]
# A model that is actually reachable and audio-capable from a mainland-China host
# was chosen as the default after measuring several candidates: Mistral Voxtral
# and Qwen omni both transcribe our 16 kHz mono WAV; the Nemotron omni free model
# drops the audio, and the Google/OpenAI audio models are geo-blocked there.
DEFAULT_PROMPT = (
    "Transcribe the speech in this audio. Reply with the plain transcript only, "
    "without commentary, timestamps or speaker labels."
)


class ProviderError(RuntimeError):
    """A provider could not produce a transcript."""


@dataclass
class AsrConfig:
    provider: str = DEFAULT_PROVIDER
    vendor: str = "openrouter"
    model: str = DEFAULT_MODEL
    base_url: str = DEFAULT_OPENROUTER_URL
    api_key: str = ""
    timeout: float = 120.0
    prompt: str = DEFAULT_PROMPT
    audio_mode: str = "inline"
    public_url: str = ""
    audio_part: str = "input_audio_url"
    user_agent: str = "stackbear/0.1"
    session: str = ""
    extra: dict = field(default_factory=dict)

    @classmethod
    def from_env(cls, env: dict | None = None) -> "AsrConfig":
        source = os.environ if env is None else env
        cfg = cls(
            provider=source.get("STACKBEAR_ASR_PROVIDER", DEFAULT_PROVIDER),
            vendor="",
            model="",
            base_url="",
            api_key="",
            timeout=float(source.get("STACKBEAR_ASR_TIMEOUT", "120")),
            prompt=source.get("STACKBEAR_ASR_PROMPT", DEFAULT_PROMPT),
            audio_mode=source.get("STACKBEAR_ASR_AUDIO_MODE", "inline").lower(),
            public_url=source.get("STACKBEAR_ASR_PUBLIC_URL", "").rstrip("/"),
            audio_part=source.get("STACKBEAR_ASR_AUDIO_PART", "input_audio_url").lower(),
            user_agent=source.get("STACKBEAR_ASR_USER_AGENT", "stackbear/0.1"),
            # OpenCode asks clients to send a stable per-conversation session id.
            session=source.get("STACKBEAR_ASR_SESSION", "stackbear-transcribe"),
        )
        # Explicit STACKBEAR_ASR_* values win; the rest come from the preset.
        cfg.vendor = source.get("STACKBEAR_ASR_VENDOR", "")
        cfg.model = source.get("STACKBEAR_ASR_MODEL", "")
        cfg.base_url = source.get("STACKBEAR_ASR_BASE_URL", "")
        cfg.api_key = source.get("STACKBEAR_ASR_API_KEY", "")
        cfg.apply_preset(source)
        return cfg

    def apply_preset(self, env: dict) -> None:
        """Fill unset fields from the provider's preset, so switching provider is
        one environment variable and the key variable name follows the vendor."""
        preset = PRESETS.get(self.provider)
        base_url, vendor, model, key_env = preset if preset else ("", self.provider, "", "STACKBEAR_ASR_API_KEY")
        self.vendor = self.vendor or env.get("STACKBEAR_ASR_VENDOR") or vendor
        self.model = self.model or env.get("STACKBEAR_ASR_MODEL") or model
        self.base_url = self.base_url or env.get("STACKBEAR_ASR_BASE_URL") or base_url
        self.api_key = self.api_key or env.get("STACKBEAR_ASR_API_KEY") or env.get(key_env, "")

    def describe(self) -> str:
        """Human-readable identity for logs. Never includes the key."""
        key_state = "set" if self.api_key else "missing"
        return (f"provider={self.provider} vendor={self.vendor} model={self.model or '(unset)'} "
                f"base_url={self.base_url} api_key={key_state} "
                f"audio_mode={self.audio_mode} part={self.audio_part}")


@dataclass
class Transcript:
    text: str
    provider: str
    vendor: str
    model: str
    raw: dict = field(default_factory=dict)


class Provider:
    """Base class: every provider turns WAV bytes into a Transcript."""

    name = "base"

    def __init__(self, config: AsrConfig, transport: Callable | None = None) -> None:
        self.config = config
        # transport(url, data, headers, timeout) -> parsed JSON dict.
        # Injected by tests so the HTTP layer is not hit; None uses urllib.
        self._transport = transport or _http_post_json

    def transcribe(self, wav: bytes, *, language: str | None = None,
                   audio_url: str | None = None) -> Transcript:  # pragma: no cover
        raise NotImplementedError


def _http_post_json(url: str, data: bytes, headers: dict, timeout: float) -> dict:
    request = urllib.request.Request(url, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:  # surface the body, it carries the reason
        body = exc.read().decode("utf-8", "replace")[:500]
        raise ProviderError(f"HTTP {exc.code} from {url}: {body}") from exc
    except urllib.error.URLError as exc:
        raise ProviderError(f"cannot reach {url}: {exc.reason}") from exc
    except json.JSONDecodeError as exc:
        raise ProviderError(f"{url} returned non-JSON: {exc}") from exc


class OpenAiCompatibleProvider(Provider):
    """A chat-completions endpoint with an audio content part.

    The wire shape is the OpenAI-compatible `input_audio` part: base64 data plus
    a format tag. OpenRouter and OpenCode Go both speak this shape, so a vendor
    usually needs only a different base_url/model/key (see PRESETS).
    """

    name = "openai-compatible"

    def _audio_part(self, wav: bytes, audio_url: str | None) -> dict:
        """Build the audio content part.

        Some models only see the audio when they can fetch a URL themselves, so
        URL mode is selectable; the two URL shapes are kept switchable because
        gateways disagree on which one they accept.
        """
        cfg = self.config
        if cfg.audio_mode == "url":
            if not audio_url:
                raise ProviderError(
                    "audio_mode=url requires a reachable URL: set STACKBEAR_ASR_PUBLIC_URL "
                    "and let the receiver publish the file"
                )
            if cfg.audio_part == "file_url":
                return {"type": "file", "file": {"filename": "audio.wav", "file_data": audio_url}}
            return {"type": "input_audio",
                    "input_audio": {"url": audio_url, "format": "wav"}}
        encoded = base64.b64encode(wav).decode("ascii")
        if cfg.audio_part == "file_base64":
            return {"type": "file",
                    "file": {"filename": "audio.wav",
                             "file_data": f"data:audio/wav;base64,{encoded}"}}
        return {"type": "input_audio",
                "input_audio": {"data": encoded, "format": "wav"}}

    def transcribe(self, wav: bytes, *, language: str | None = None,
                   audio_url: str | None = None) -> Transcript:
        cfg = self.config
        if not cfg.api_key:
            raise ProviderError(
                "no API key: set STACKBEAR_ASR_API_KEY (the key is never stored in the repo)"
            )
        if not cfg.model:
            raise ProviderError("no model: set STACKBEAR_ASR_MODEL")

        instruction = cfg.prompt
        if language:
            instruction += f" The audio is in {language}."
        audio = self._audio_part(wav, audio_url)
        payload = {
            "model": cfg.model,
            # Ask the gateway to return real usage accounting (token counts and,
            # where supported, the actual cost of this call) so spend can be
            # measured instead of estimated.
            "usage": {"include": True},
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": instruction},
                        audio,
                    ],
                }
            ],
        }
        headers = {
            "Authorization": f"Bearer {cfg.api_key}",
            "Content-Type": "application/json",
            # Identify as a real client: OpenCode Go sits behind Cloudflare, which
            # returns error 1010 for generic SDK user agents such as urllib's.
            "User-Agent": cfg.user_agent,
            "x-opencode-session": cfg.session,
            # OpenRouter attributes free-tier traffic with these; harmless elsewhere.
            "HTTP-Referer": cfg.extra.get("referer", "https://github.com/zsddyd/stackbear"),
            "X-Title": cfg.extra.get("title", "stackbear"),
        }
        url = cfg.base_url.rstrip("/") + "/chat/completions"
        data = json.dumps(payload).encode("utf-8")
        reply = self._transport(url, data, headers, cfg.timeout)
        return self._parse(reply)

    def _parse(self, reply: dict) -> Transcript:
        if "error" in reply:
            message = reply["error"]
            if isinstance(message, dict):
                message = message.get("message", message)
            raise ProviderError(f"provider error: {message}")
        try:
            choices = reply["choices"]
            message = choices[0]["message"]
            content = message.get("content")
            # Reasoning models leave `content` null and put the answer elsewhere;
            # the field name differs per gateway/model (observed: reasoning_content
            # on mimo-v2.6, reasoning on mimo-v2.5, text on some others).
            if not content:
                for field_name in ("reasoning_content", "reasoning", "text"):
                    candidate = message.get(field_name)
                    if isinstance(candidate, str) and candidate.strip():
                        content = candidate
                        break
        except (KeyError, IndexError, TypeError) as exc:
            raise ProviderError(f"unexpected response shape: {str(reply)[:300]}") from exc
        if isinstance(content, list):  # some gateways return content parts
            content = "".join(part.get("text", "") for part in content if isinstance(part, dict))
        text = (content or "").strip()
        if not text:
            raise ProviderError(f"empty transcript in response: {str(reply)[:300]}")
        return Transcript(text=text, provider=self.name, vendor=self.config.vendor,
                          model=self.config.model, raw=reply)


PROVIDERS: dict[str, type[Provider]] = {
    "openrouter": OpenAiCompatibleProvider,
    "opencode-go": OpenAiCompatibleProvider,
}


def get_provider(config: AsrConfig | None = None, transport: Callable | None = None) -> Provider:
    cfg = config or AsrConfig.from_env()
    try:
        factory = PROVIDERS[cfg.provider]
    except KeyError:
        known = ", ".join(sorted(PROVIDERS))
        raise ProviderError(f"unknown provider {cfg.provider!r}; known: {known}") from None
    return factory(cfg, transport=transport)
