"""Local (offline) speech recognition with sherpa-onnx + SenseVoice.

Why this exists: every free audio model on OpenRouter turned out unusable (two
are gated behind an agentic harness and return HTTP 403; the third answers "no
spoken words detected" for our 16 kHz mono WAV), and the paid models that work
cost money per hour. A local recognizer removes both the cost and the network,
and keeps the audio on the machine.

Measured on the ECS (2 vCPU Xeon with AVX-512, 1.6 GB RAM): SenseVoice-Small
int8 loads in ~1.6 s and transcribes at about 8x real time using two threads,
in roughly 250-400 MB of RSS.

The one trap worth remembering: a speech recogniser is trained on short clips
and degrades badly when handed a long file. The same 85 s recording produced
158 characters as ~8 s blocks and a single word ("不。") as one 85 s block, so
`chunk_ms` defaults to 8 s. chunking happens in transcribe.merge_spans via the
provider's chunk hint, not here.
"""

from __future__ import annotations

import array
import pathlib
import wave

from .providers import AsrConfig, Provider, ProviderError, Transcript

# 模型文件名固定为 sherpa-onnx 的 SenseVoice 导出命名;tokens 与模型必须配套。
MODEL_FILE = "model.int8.onnx"
TOKENS_FILE = "tokens.txt"


class SenseVoiceProvider(Provider):
    """Runs sherpa-onnx in-process. Heavy state is created lazily and reused, so
    the cost is paid once per receiver process rather than per request."""

    name = "local"

    _recognizer = None

    def _load(self):
        if SenseVoiceProvider._recognizer is not None:
            return SenseVoiceProvider._recognizer
        cfg = self.config
        if not cfg.model_dir:
            raise ProviderError("local provider needs STACKBEAR_ASR_MODEL_DIR")
        base = pathlib.Path(cfg.model_dir)
        model = base / MODEL_FILE
        tokens = base / TOKENS_FILE
        for path in (model, tokens):
            if not path.is_file():
                raise ProviderError(f"missing model file: {path}")
        try:
            import sherpa_onnx
        except ImportError as exc:
            raise ProviderError(
                "sherpa-onnx is not installed in this environment "
                "(pip install sherpa-onnx)") from exc
        try:
            SenseVoiceProvider._recognizer = sherpa_onnx.OfflineRecognizer.from_sense_voice(
                model=str(model), tokens=str(tokens),
                num_threads=max(1, cfg.threads), use_itn=True, debug=False,
                language=cfg.language or "")
        except Exception as exc:  # noqa: BLE001 - surfaced as a provider error
            raise ProviderError(f"cannot load the local model: {exc}") from exc
        return SenseVoiceProvider._recognizer

    def transcribe(self, wav: bytes, *, language: str | None = None,
                   audio_url: str | None = None) -> Transcript:
        recognizer = self._load()
        pcm, rate = _wav_to_pcm(wav)
        stream = recognizer.create_stream()
        stream.accept_waveform(rate, pcm)
        recognizer.decode_stream(stream)
        text = (stream.result.text or "").strip()
        if not text:
            # A block with no words is normal (breath, noise); report it as an
            # empty transcript so the caller keeps the other blocks.
            return Transcript(text="", provider=self.name,
                              vendor=self.config.vendor or "local",
                              model=self.config.model or "sense-voice")
        return Transcript(text=text, provider=self.name,
                          vendor=self.config.vendor or "local",
                          model=self.config.model or "sense-voice")


def _wav_to_pcm(wav: bytes) -> tuple[array.array, int]:
    """Decode the WAV we built in transcribe.wav_bytes back to samples."""
    import io

    try:
        with wave.open(io.BytesIO(wav), "rb") as handle:
            if handle.getsampwidth() != 2:
                raise ProviderError("local provider expects 16-bit PCM")
            channels = handle.getnchannels()
            rate = handle.getframerate()
            pcm = array.array("h", handle.readframes(handle.getnframes()))
    except (wave.Error, EOFError) as exc:
        raise ProviderError(f"cannot read the WAV: {exc}") from exc
    if channels > 1:
        # Downmix so a stereo input cannot desynchronise the recognizer.
        pcm = array.array("h", (sum(pcm[i:i + channels]) // channels
                                for i in range(0, len(pcm), channels)))
    return pcm, rate
