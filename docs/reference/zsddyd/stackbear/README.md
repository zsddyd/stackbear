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
                                     |
                                     +-> auto transcription (per VAD segment)
```

- Firmware entry: `main/main.c`; UI in `main/sb_ui.c`; audio path in
  `main/sb_recorder.c`; codec in `main/sb_adpcm.c`; framing in `main/sb_proto.c`;
  VAD in `main/sb_vad.c`; Wi-Fi/provisioning in `main/sb_net.c` and
  `main/sb_net_web.c`; uplink in `main/sb_uplink.c`; NVS config in
  `main/sb_config.c`; offline queue in `main/sb_cache.c`; OTA in `main/sb_ota.c`;
  page hierarchy in `main/sb_nav.c`; build identity in `main/sb_version.c`.
- Receiver project: `cloud/`, run with `uv` on the ECS as
  `PYTHONPATH=$PWD/src .venv/bin/python -m stackbear.server --host 0.0.0.0 --port 18080`
  (or `./run_server.sh`, which logs straight to a file).
- Audio format: IMA-ADPCM, 4 bit, 16 kHz mono, one self-contained 60 ms block per
  frame (`[2-byte big-endian length][4-byte header + nibbles]`), roughly 7.8 KB/s.
- Segment boundaries travel with the audio: the first block of every VAD segment
  sets the `SEG_START` flag on its AUDIO frame, and the receiver records each
  segment's start offset and frame count in `meta.json`, so transcription can be
  submitted per utterance.

### Screen hierarchy

```text
HOME            aggregate state card (fills at startup) + SEG/SENT/DROP
                 + two selectable rows: Stop recording (red) / Settings
 └ SETTINGS     group menu (this used to be called STATUS)
     ├ RECORDER  Recording / Segments / Sent / Dropped
     ├ NETWORK   Wi-Fi / SSID / Device IP / Server IP / Uplink
     ├ STORAGE   Cache / Size / Used / Pending / Dropped
     ├ DEVICE    Device ID / Firmware / Slot / Commit / Branch / Built / update
     └ HELP      the three long-press gestures (Hold UP / DOWN / OK)
```

Every screen shows the battery percentage in its top-right corner. It used to be
on HOME only, so entering a sub-page hid it; the readout is now created and
registered by the shared screen builder, and the fuel gauge is read in one place.

Every screen is now select-and-confirm: UP/DOWN move the highlight, OK runs the
highlighted row. That is the same gesture on HOME (two rows), SETTINGS and the
sub-pages, so only two gestures remain that the screen cannot teach by itself —
and those are what HELP lists. A "tap" is one press-and-release; there is no
double-tap gesture, because the button component merges taps closer than its
short-press window (180 ms here) into one multi-click sequence, which would make
the meaning of "twice" depend on how fast the user pressed:

| Page | UP tap | DOWN tap | OK tap | UP long | DOWN long | OK long |
| --- | --- | --- | --- | --- | --- | --- |
| HOME | cursor up | cursor down | run the selected row: start/stop recording, or open SETTINGS | start/stop recording (shortcut) | — | enter provisioning |
| SETTINGS | cursor up | cursor down | open sub-page | — | back to HOME | enter provisioning |
| RECORDER / NETWORK | cursor up | cursor down | — (read-only) | — | back to SETTINGS | enter provisioning |
| STORAGE | cursor up | cursor down | toggle the selected setting (only `Cache` and `Size`) | — | back to SETTINGS | enter provisioning |
| DEVICE | cursor up | cursor down | run the selected action (only the `Check for update` row) | — | back to SETTINGS | enter provisioning |
| HELP | — | — | — | — | back to SETTINGS | enter provisioning |
| SETUP (provisioning) | — | — | — | — | cancel and return | cancel and return |

Taps always move one row each, however fast they are repeated. On SETTINGS the
rows that respond to OK show their name in the accent colour and the rest are
read-only. HOME marks only the destructive row: `Stop recording` is red (it ends
the session and closes the file) and `Settings` is accent; the first row is
labelled `Start recording` while paused, so it always names what pressing it
does. HOME starts with the highlight on that row. Long-press UP is the shortcut
for the same start/stop action and works regardless of where the highlight sits;
recording is still not bound on any list page, so browsing cannot stop a session.
The provisioning page is a modal. Short UP/DOWN/OK stay inert there — the page
says so on screen (`hold DOWN or OK to exit`) — but long-press DOWN leaves it,
like it goes back everywhere else; leaving by long-press OK still works, since
that is the switch that entered the page. The aggregate state (which fuses credential, link, pause and uplink)
lives only on HOME so no sub-page repeats it; Wi-Fi and Uplink are separate rows
because a healthy Wi-Fi link does not imply uploads work.

### Scrolling lists and the key guide

Lists are drawn from a *virtual window*: a page declares how many items it has,
but only the rows that fit on screen are created, and scrolling re-points those
slots at a different slice of the item list. The object count is therefore fixed
by the screen height and does not grow with the number of items, so adding
information to RECORDER / NETWORK / STORAGE / DEVICE later cannot inflate LVGL
memory — it just becomes scrollable. A slim scroll bar appears on the right only
when a page actually overflows. Every shipped page has at most seven rows, which
is exactly one screen, so nothing visible changed except the new HELP row.

An earlier HELP was grouped by page and ran to 32 lines, which is why the
scrolling existed. Once every screen became select-and-confirm, that copy was
mostly restating what the highlight already shows, so HELP is now three rows in
the same Key/value layout the other pages use, and needs no scroll:

| Key | Value |
| --- | --- |
| Hold UP | Start / stop |
| Hold DOWN | Back a level |
| Hold OK | Set up Wi-Fi |

It has no cursor (there is nothing to run) and no summary on the SETTINGS row —
the name `Help` is already the whole explanation. The values are sized to the
narrowest budget that layout allows: the widest key is `Hold DOWN` (88 px), which
leaves 98 px for the right-aligned value, and the three values measure 80 / 85 /
90 px, so none of them is clipped. The virtual-window machinery stays in place
for the lists (it is what keeps memory flat as pages grow); `main/sb_scroll.h` is
pure and host-tested (`tests/test_sb_scroll.c`) across the boundaries that are
easy to get wrong: top and bottom overrun, fewer items than rows, and empty
lists.

DEVICE keeps its values inside their own column. `Slot` shows `A` or `B` rather
than the internal `ota_0`/`ota_1` (the letters are the same two slots the OTA
switch names), and `Branch` shows a prefix ending in `...` unless the cursor is
on that row — selecting it starts a marquee that walks the whole name through
the column, wrapping around with a gap so every character is reachable. The
prefix length is computed from the Branch label's own width (`main/sb_text.c`,
`sb_text_fit_ellipsis`), so a truncated value can never creep over the key
beside it; long OTA failure reasons are clipped the same way. The `Check for
update` row needs no `press OK` hint: its name is already drawn in the link
colour.

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
7. **Logging must never break a connection.** The receiver had been started as
   `python -m stackbear.server | tee log`. When the tee reader died, stdout had
   no reader, so every `print()` raised `BrokenPipeError` - an `OSError` - which
   the connection handler's `except (OSError, ...)` swallowed, closing the device
   link right after it connected. The device then reconnected every couple of
   seconds and the UI showed UPLOAD ERR. All diagnostics now go through a
   best-effort `log()` that ignores write errors, and `run_server.sh` logs to a
   file instead of a pipe.
8. **A reboot stopped using the saved credentials.** `sb_net_ensure_connected()`
   only looked at the in-memory `s_ssid`, and nothing populated it on boot after
   the reconnect rework, so every restart (including an OTA reboot) behaved as
   "not provisioned". Fixed by `sb_net_load_saved()`, called once after
   `sb_net_init()`. Provisioning also failed outright for a while because
   `apply_sta_config()` refused whenever provisioning was active - the guard was
   meant for the background reconnect timer only.
9. **Offline queue items were sized to the protocol maximum.** Each uplink queue
   slot held 1400 bytes although an ADPCM frame is 484, so 24 slots cost 32.9 KB
   of permanent RAM. Combined with the upload socket, that exhausted the heap
   during an OTA download (`wifi:alloc eb ... fail`, then a load-access fault).
   Items now hold one audio frame (7.6 KB total) with static assertions tying the
   size to the ADPCM block and the cache slot, the OTA download buffer is a 2 KB
   static buffer, and the uplink task is stopped before downloading so its
   buffers return to the Wi-Fi stack.
10. **A JSON helper that never matched.** The OTA manifest parse required a quote
    immediately after the key, but the manifest has `"version": "..."` with a
    colon between, so every lookup failed and "Check for update" always reported
    `check failed` before downloading anything. Now the parser skips the
    separator, refuses truncated values (a clipped sha256 would silently break
    verification) and requires a closing quote; it lives in `main/sb_json.[ch]`
    with a regression test.

## Tooling added

| Tool | Purpose |
| --- | --- |
| `tools/usb_console.py` | Reads the ESP32 native USB-CDC console on Termux/Android, where there is no serial tty and minicom/pyserial cannot attach. Wraps the `termux-usb` descriptor with pyusb and streams the bulk-IN endpoint. |
| `tools/adpcm_to_wav.py` | Decodes a stored session (`audio.adpcm`, or legacy `audio.opus`) into a playable WAV, and prints duration, peak and RMS so the recording can be checked without listening. |
| `tools/make_ota_manifest.py` | Writes the OTA manifest (`version`, `sha256`, `size`, `commit`, `branch`, `url`) using the same version string the firmware builds, so a device recognises "already current". |

Host-testable logic is deliberately split out of the LVGL/ESP-IDF code and
covered by `tests/test_sb_*.c`: framing (`sb_proto`), VAD (`sb_vad`), codec
(`sb_adpcm`), offline-slot codec (`sb_cache_slot`), manifest parsing
(`sb_json`), page hierarchy (`sb_nav`), the list scroll window (`sb_scroll`) and
width-aware text clipping / marquee (`sb_text`).

## UI design notes (taste house style)

The layout follows the "no ornament" rules rather than assembling cards: one
ground colour, one ink, one accent; selection is shown by a one-step-lighter
background instead of borders or shadows; the main figure is set larger rather
than surrounded by decoration; explanatory copy is written out in full instead
of being replaced by icons. Status rows carry their value on the right so the
page reads without extra navigation.

## Transcription provider layer

`cloud/src/stackbear/providers.py` isolates the ASR vendor behind one interface
(vendor, model, endpoint, key), so the receiver does not care who transcribes.
Configuration is environment-only and keys are never committed:

```text
STACKBEAR_ASR_PROVIDER  provider name (default: openrouter)
STACKBEAR_ASR_VENDOR    label recorded with the result
STACKBEAR_ASR_MODEL     default: qwen/qwen3.8-omni-flash
STACKBEAR_ASR_BASE_URL  default: https://openrouter.ai/api/v1
STACKBEAR_ASR_API_KEY   bearer token
```

Three providers are registered. Presets supply endpoint, vendor and a default
audio-capable model, so switching is one variable:

| Provider | Endpoint | Key variable | Default model |
| --- | --- | --- | --- |
| `openrouter` | `https://openrouter.ai/api/v1` | `STACKBEAR_ASR_API_KEY` | `qwen/qwen3.8-omni-flash` |
| `opencode-go` | `https://opencode.ai/inference/go/v1` | `OPENCODE_API_KEY` | `mimo-v2.6-pro` |
| `local` | none (in-process) | none | `sense-voice-small-int8` |

### Local recognition (the free option that works)

Every **free** audio model on OpenRouter turned out unusable: two
(`thinkingmachines/inkling*:free`) answer HTTP 403 "only available on agentic
harnesses", and `nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free` returns
"(No spoken words detected)" for our 16 kHz mono WAV in every block tried. The
paid models that work cost per hour, so the `local` provider removes both the
cost and the network and keeps the audio on the machine.

It is sherpa-onnx with SenseVoice-Small int8, installed with `uv pip install
sherpa-onnx` (a wheel, no compilation) with the model fetched from
`hf-mirror.com` (`huggingface.co` times out from the mainland host; the mirror
resolves). Set:

```text
STACKBEAR_ASR_PROVIDER=local
STACKBEAR_ASR_MODEL_DIR=<repo>/models/sense-voice   # model.int8.onnx + tokens.txt
STACKBEAR_ASR_THREADS=2
STACKBEAR_ASR_LANGUAGE=zh
```

Measured on the ECS (2 vCPU Xeon with AVX-512, 1.6 GB RAM): the model loads in
1.6 s, runs at roughly 8x real time on two threads, and an 85 s recording
transcribes in about 9 s at ~250-400 MB RSS. Two findings from measuring it:

- **Chunk length matters more than the model.** A speech recogniser is trained
  on short clips: the same 85 s recording gave 158 characters as eleven ~8 s
  blocks and a single character as one 85 s block, with 15 s and 25 s blocks in
  between (134 and 79 characters). `STACKBEAR_ASR_CHUNK_MS` therefore defaults
  to 8 s for the local provider.
- **Pin the language.** With auto-detection the two quiet opening blocks came
  back as English ("B.", "After.") inside an otherwise Chinese conversation;
  `STACKBEAR_ASR_LANGUAGE=zh` removes that.

### Domain vocabulary (the part a prompt cannot do)

The obvious idea - give the recogniser a better prompt - does not apply here.
SenseVoice is an end-to-end model with no prompt interface, and its hotword
support (sherpa-onnx `hr_dict_dir`/`hr_lexicon`/`hr_rule_fsts`) is a homophone
replacer built from a *general* pinyin table: fetched and measured on a real
recording, it changed nothing, because that table cannot contain one team's
proper nouns.

What works is a glossary applied after recognition (`cloud/glossary.txt`,
`cloud/src/stackbear/glossary.py`), which is engine-independent and therefore
also corrects the cloud providers:

```text
# one rule per line, longest wrong-form first
<misheard> => <correct>
<variant>  => <correct>
LE         => C798-ASE
```

A replacement is never rescanned (so rules cannot chain unexpectedly), and an
all-ASCII wrong-form gets ASCII word boundaries - `LE => C798-ASE` rewrites
a Chinese sentence containing the bare code but leaves `LEVEL` and `file-led`
untouched. Measured effect on the 39 s recording that exposed it: a colleague
heard as three different names collapsed into one, and the model code came out
correctly.

The OpenCode Go subscription reaches audio-capable models per the models.dev
registry: `mimo-v2.6-flash`, `mimo-v2.6-pro`, `mimo-v2.5`,
`gemini-4-argon-contributor`, `muse-spark-1.2/1.3-contributor`. Note the vendor
documents it as being for coding-agent traffic, so treat it as an experiment
rather than a production ASR path.

Two client details are required for OpenCode Go and are easy to miss:

- Cloudflare rejects generic SDK user agents with **error 1010**; the request
  must send a real client name (`STACKBEAR_ASR_USER_AGENT`) and the
  `x-opencode-session` header the vendor asks for.
- Its MiMo models are reasoning models: `content` comes back null and the
  transcript arrives as `reasoning_content` (mimo-v2.6) or `reasoning`
  (mimo-v2.5), so the parser falls back through those fields.

Measured on the same 12.5 s recording: `mimo-v2.6-flash` and `mimo-v2.5` both
transcribe correctly (and better than OpenRouter's route for the same model,
which returned an empty transcript); `gemini-4-argon-contributor` refuses until
the workspace enables endpoints that train on request data.

Model notes measured on a mainland-China host with one 12.5 s device recording
(16 kHz mono WAV sent as an `input_audio` part): `mistralai/voxtral-small-24b-2507`
and `qwen/qwen3.8-omni-flash` both return a correct transcript;
`nvidia/nemotron-3-nano-omni-30b-a3b-reasoning:free` is listed as audio-capable but
drops the audio and asks for a file; `google/*` and `openai/*` audio models return
HTTP 403 "not available in your region"; `xiaomi/mimo-v2.6-flash` returns an empty
transcript. Changing models needs only this environment variable.

`python -m stackbear.transcribe <session> --per-segment` decodes the session,
splits it on the recorded VAD boundaries and transcribes each utterance;
`--dry-run` checks the setup without calling the API, and `--write` stores
`transcript.txt` / `transcript.json` beside the session.

VAD produces many short fragments (85 s of speech arrived as 62 segments, some
60 ms long), and speech models answer fragments that small with stock fillers or
with a description of the sound rather than a transcript - the missing
ingredient is context. Consecutive fragments are
therefore merged into blocks of at least 8 s (never more than 30 s) before
submitting; an utterance longer than the maximum is kept whole rather than cut,
and a short trailing block is folded into the previous one. On the 85 s
recording this turned 62 requests into 8 with identical total coverage, and the
output changed from fragments to a readable conversation.

The receiver can also transcribe by itself: when a session ends (an `END` frame,
a reconnect or a timeout), it decodes and transcribes in a worker thread so the
receive loop never stalls, then writes `transcript.txt`, `transcript.json` and
`transcript.status.json` into the session directory. `STACKBEAR_ASR_AUTO=0`
turns it off; `STACKBEAR_ASR_PER_SEGMENT` (default on) chooses per-utterance or
whole-session transcription. Segments shorter than 400 ms are skipped rather
than submitted, a single failed segment no longer discards the rest of the
session, and `STACKBEAR_ASR_RETRIES` (default 3) retries a request because some
gateways route one model across a pool where only part of the workers accept
audio.

### Querying the recordings

Transcriptions live beside their audio
(`data/<device>/<session>/{audio.adpcm, meta.json, transcript.txt, transcript.json}`),
and `/transcripts` is the single entry point over them:

| Request | Result |
| --- | --- |
| `GET /transcripts?limit=20` | Index of sessions, newest first: device, time, duration, segment count, whether a transcript exists, character count and a one-line preview |
| `GET /transcripts?dev=<id>` | The same, filtered to one device |
| `GET /transcripts?q=<word>` | Content search; each hit carries the matching lines |
| `GET /transcript/<dev>/<session>` | One session's full text |

The index reports transcription state per session, so finished work is
distinguishable from pending, and the search reads the transcripts on disk
rather than an index that could drift away from them. A non-ASCII search word
must be URL-encoded: the receiver restores the query string to UTF-8 before
matching, because decoding the request line as latin1 (correct for headers)
otherwise leaves a Chinese word as mojibake that never matches.

## Build, flash and test workflow

- **Build:** GitHub Actions only (`.github/workflows/build-firmware.yml`), which
  runs `./tools/validate.sh --firmware` under ESP-IDF 5.5.3 and uploads two
  packages: `build/FoloToy-AI-Passport-full.bin` (the merged `0x0` image) and
  `build/FoloToy-AI-Passport-ota.bin` plus `build/ota-latest.json` (the OTA
  package). With the ECS secrets configured, the workflow then publishes the
  image and the manifest to the receiver; without them it prints a skip and the
  files must be copied by hand.
- **Flash:** `nrflash write --chip esp32c3 --offset 0x0 --verify` over the
  `termux-usb` path; the phone must approve the USB permission prompt.
- **Partition layout:** two OTA slots (`ota_0`/`ota_1`, 2.94 MB each), `fota`
  (otadata, renamed by project convention), a ~2 MB `audioq` ring for the offline
  queue. The spare OTA slot doubles as the download target, so no separate
  staging partition is needed. The application does not fit a single-slot layout
  any more; changing `factory` to `ota_0/ota_1` required one line flash, which is
  why it was the last time USB was needed.
- **Flashing data:** a merged `0x0` image overwrites the NVS partition, so Wi-Fi
  credentials are lost and provisioning must be repeated **after a line flash**.
  An OTA update does **not** touch NVS, so credentials survive it.
- **Boot after flash:** the ESP32-C3 USB-Serial/JTAG survives a CPU reset, so a
  flash may look like it did not restart. Power-cycling the board is the
  reliable way to run new firmware.
- **Observability:** `tools/usb_console.py`; `pkill -f stackbear.server` also
  matches the invoking shell, so use a bracketed pattern.

### OTA

The device asks the receiver for `/firmware/latest.json`, compares versions,
downloads the image into the spare slot over plain HTTP, verifies the sha256
from the manifest, sets the boot partition and restarts. The image is an app
image only (no bootloader or partition table). Rollback is enabled
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) and `app_main` confirms the running
image, so a firmware that crashes before confirming is rolled back to the
previous slot. Before downloading, the OTA task ends the current recording
session and stops the uplink so its buffers return to the Wi-Fi stack.

HTTPS is not used: the receiver currently has only a public IP and port 18080,
so integrity rests on the sha256 from the manifest. `/firmware/` serves plain
names only, so a request cannot escape the directory.

## Verification status

```text
Build:        PASS  (GitHub Actions, ESP-IDF 5.5.3 / esp32c3)
Host tests:   PASS  (sb_proto, sb_vad, sb_adpcm, sb_cache_slot, sb_json, sb_nav,
                     sb_scroll (list window), sb_text (clip / marquee),
                     cloud protocol/adpcm/providers/server/autotranscribe/firmware,
                     repo checks)
Device tests: PASS for the core path
              flash + on-device MD5, boot, display, three buttons, SoftAP setup,
              microphone capture, VAD segmentation, ADPCM encode, private-protocol
              upload, per-session storage on the ECS, automatic transcription,
              and one real OTA update (check -> download -> sha256 -> slot switch
              -> reboot into the new version)
Unverified:   decoded speech quality, long-run stability and battery life,
              reconnect after a network drop, the offline queue writing to flash
              on real hardware, and the LLM/Feishu stage
```

## Next steps

1. Configure the `ECS_HOST` / `ECS_USER` / `ECS_PASSWORD` repository secrets so
   the workflow publishes the OTA package automatically.
2. Long-run stability and power measurements on the device, and an offline-queue
   write/upload test with the network down.
3. Rotate the API keys used during development (they appeared in chat; only the
   environment variables change).
4. LLM extraction from the transcript, then Feishu Bitable delivery.
