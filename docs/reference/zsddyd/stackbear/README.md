<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Stackbear — Always-On Voice Recorder

Application playbook and engineering memory for the `stackbear` firmware built on
the FoloToy AI Passport. It records speech continuously, encodes it on-device and
streams it to a private receiver on an Aliyun ECS, where the audio is stored per
session for later transcription.

## Requirement

| Item | Decision |
| --- | --- |
| Device role | Always-on VAD recorder; only speech is encoded and uploaded |
| Network | Wi-Fi station joined to a phone hotspot, provisioned on-device |
| Transport | Private binary framing over a long-lived TCP connection to the ECS |
| Cloud (this stage) | Receive and store the audio stream only; no ASR/LLM yet |
| Later stages | AliCloud ASR -> LLM extraction -> Feishu Bitable (deferred) |
| UI | Redesigned English screens on the 240x320 display; three physical buttons |

## Architecture

```text
[Passport]  ES8311 capture -> VAD segmentation -> IMA-ADPCM -> private TCP frames
                                                                    |
                                                                    v
[ECS 18080] asyncio receiver -> data/<device>/<session>/audio.adpcm + meta.json
```

- Firmware entry: `main/main.c`; UI in `main/sb_ui.c`; audio path in
  `main/sb_recorder.c`; codec in `main/sb_adpcm.c`; framing in `main/sb_proto.c`;
  VAD in `main/sb_vad.c`; Wi-Fi/provisioning in `main/sb_net.c` and
  `main/sb_net_web.c`; uplink in `main/sb_uplink.c`; NVS config in
  `main/sb_config.c`.
- Receiver project: `cloud/`, run with `uv` on the ECS as
  `PYTHONPATH=$PWD/src .venv/bin/python -m stackbear.server --host 0.0.0.0 --port 18080`.
- Audio format: IMA-ADPCM, 4 bit, 16 kHz mono, one self-contained 60 ms block per
  frame (`[2-byte big-endian length][4-byte header + nibbles]`), roughly 7.8 KB/s.
- Segment boundaries travel with the audio: the first block of every VAD segment
  sets the `SEG_START` flag on its AUDIO frame, and the receiver records each
  segment's start offset and frame count in `meta.json`, so transcription can be
  submitted per utterance.

## Hand-won engineering findings

These were all found with the on-device console tool, in this order:

1. **Wi-Fi state race aborted the device.** `WIFI_EVENT_STA_START` triggered an
   automatic `esp_wifi_connect()`, so a later `esp_wifi_set_config()` returned
   `ESP_ERR_WIFI_STATE` and the surrounding `ESP_ERROR_CHECK` aborted. Wi-Fi
   setup now disconnects first and returns errors instead of aborting.
2. **Allocation order vs. I2S DMA.** Creating the encoder before audio
   initialization starved `i2s_alloc_dma_desc` (`ESP_ERR_NO_MEM`). Audio now
   initializes before the codec, and recording starts before Wi-Fi.
3. **Opus does not fit this part.** libopus needed about 25 KB of encoder state
   and, because `78/esp-opus` builds with `-DUSE_ALLOCA`, allocated all CELT/SILK
   scratch on the task stack (tens of KB). Task stacks of 24 KB and 32 KB both
   overflowed. Attempting to switch the component to a pseudostack did not
   resolve it.
4. **Decision: IMA-ADPCM instead of Opus.** ADPCM has a few bytes of state and no
   large scratch, costs about half the compression, and is enough for
   record-then-transcribe. This removed the 25 KB encoder and shrank the image by
   about 170 KB.
5. **The uplink wrote into dead sockets.** `send()` failures were swallowed, so
   after the receiver restarted the device never reconnected. Send failures now
   close the socket and reconnect.
6. **Task stack sizing.** The capture task needs a 32 KB static stack for
   `bsp_audio_init()` plus the encoder loop.

## Tooling added

| Tool | Purpose |
| --- | --- |
| `tools/usb_console.py` | Reads the ESP32 native USB-CDC console on Termux/Android, where there is no serial tty and minicom/pyserial cannot attach. Wraps the `termux-usb` descriptor with pyusb and streams the bulk-IN endpoint. |
| `tools/adpcm_to_wav.py` | Decodes a stored session (`audio.adpcm`, or legacy `audio.opus`) into a playable WAV, and prints duration, peak and RMS so the recording can be checked without listening. |

## Transcription provider layer

`cloud/src/stackbear/providers.py` isolates the ASR vendor behind one interface
(vendor, model, endpoint, key), so the receiver does not care who transcribes.
Configuration is environment-only and keys are never committed:

```text
STACKBEAR_ASR_PROVIDER  provider name (default: openrouter)
STACKBEAR_ASR_VENDOR    label recorded with the result
STACKBEAR_ASR_MODEL     e.g. nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free
STACKBEAR_ASR_BASE_URL  default: https://openrouter.ai/api/v1
STACKBEAR_ASR_API_KEY   bearer token
```

`python -m stackbear.transcribe <session> --per-segment` decodes the session,
splits it on the recorded VAD boundaries and transcribes each utterance;
`--dry-run` checks the setup without calling the API, and `--write` stores
`transcript.txt` / `transcript.json` beside the session.

## Build, flash and test workflow

- **Build:** GitHub Actions only (`.github/workflows/build-firmware.yml`), which
  runs `./tools/validate.sh --firmware` under ESP-IDF 5.5.3 and uploads
  `build/FoloToy-AI-Passport-full.bin` for `0x0`.
- **Flash:** `nrflash write --chip esp32c3 --offset 0x0 --verify` over the
  `termux-usb` path; the phone must approve the USB permission prompt.
- **Important:** a merged `0x0` image overwrites the NVS partition, so Wi-Fi
  credentials are lost and provisioning must be repeated after every flash.
  Flashing only the application partition at `0x10000` preserves them.
- **Boot after flash:** the ESP32-C3 USB-Serial/JTAG survives a CPU reset, so a
  flash may look like it did not restart. Power-cycling the board is the
  reliable way to run new firmware.
- **Observability:** `tools/usb_console.py`; `pkill -f stackbear.server` also
  matches the invoking shell, so use a bracketed pattern.

## Verification status

```text
Build:        PASS  (GitHub Actions, ESP-IDF 5.5.3 / esp32c3)
Host tests:   PASS  (sb_proto, sb_vad, sb_adpcm, cloud protocol + server, repo checks)
Device tests: PASS for the core path
              flash + on-device MD5, boot, display, three buttons, SoftAP setup,
              microphone capture, VAD segmentation, ADPCM encode, private-protocol
              upload, and per-session storage on the ECS
Unverified:   decoded speech quality, long-run stability and battery life,
              reconnect after a network drop, and the ASR/LLM/Feishu stage
```

## Next steps

1. Support application-partition flashing so provisioning survives updates.
2. Add a decode step and a session listing to the cloud side before ASR.
3. Long-run stability and power measurements on the device.
4. AliCloud ASR, LLM extraction, and Feishu Bitable delivery.
