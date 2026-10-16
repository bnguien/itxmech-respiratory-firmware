# RespiCare lung-sound firmware

**New clone / first hardware test:** follow [GETTING_STARTED.md](GETTING_STARTED.md)
before building or flashing. Do not use an existing device's flash as a test
partition without confirming whether it holds data.

Production flow: Dashboard → Web Backend → MQTT Gateway → Mosquitto → ESP32
→ local WAV → Web Device API → signed Storage PUT → complete API.
The backend, not this firmware, invokes AI and updates DB/Realtime.
`record_audio.py` is unchanged and is a local Serial-debug utility only.

## Architecture and files

- `code_esp.ino`: Arduino entry point.
- `src/Firmware.cpp`: MQTT loop, command validation, task/queue coordination;
  isolated compile-time Serial mode.
- `src/Network.*`: WiFiManager and MQTT/TLS connection, reconnect, retained
  online status, QoS 1 Last Will and event delivery.
- `src/Protocol.*`: UUID/UTC validation and versioned JSON event envelopes.
- `src/AudioRecorder.*`: original I2S configuration/conversion and WavWriter.
- `src/RecordingStorage.*`: replaceable LittleFS adapter, persistent accepted
  command/visit ledger, NVS checkpoint.
- `src/RecordingApiClient.*`: HTTPS upload-url, streaming PUT and complete.
- `src/WebCaBundle.h`: embedded public Mozilla roots for signed Storage PUT.
- `src/WebApiCa.h`: Mozilla-trusted GTS Root R1 PEM for the Web Device API.
- `src/DeviceFlow.*`: recording/upload state machine and bounded retry.
- `scripts/build_config.py`, private `.env`: build-injected runtime configuration.
- `platformio.ini`: pinned Arduino ESP32 build, default 4 MB partition table.
- `tests/test_audio.py`: host test of the actual capture/conversion/WAV writer.
- `tests/test_flow.py`, `tests/test_config.py`: deterministic state/retry/recovery
  and environment/security configuration tests.

No SD card or PSRAM is assumed. A broker CA is present in the repository;
confirm its provenance and validity before flashing production hardware.

## State machine

BOOT → Wi-Fi/time connecting → MQTT connecting → IDLE → VISIT_ASSIGNED
→ RECORDING → RECORDING_COMPLETE → REQUESTING_UPLOAD_URL → UPLOADING
→ COMPLETING → IDLE. Network connection state is independent of the worker's
recording state. Serial logs never contain raw PCM in production.

The MQTT owner task remains active while a separate worker performs I2S,
filesystem and bounded synchronous HTTPS operations. Only the MQTT owner
touches the MQTT client. Events cross a 16-entry queue and are removed after
QoS 1 publication succeeds. No API request or capture runs in the callback.

Commands require version 1, ASSIGN_VISIT, UUID messageId/visitId and UTC
timestamp (`...SSZ` or `...SS.sssZ`). A durable append-only ledger deduplicates
both commands and visits, including after reboot. Busy/error assignments are
explicitly rejected in Serial; no current visit is overwritten and no new
protocol event is invented. The gateway must not assume PUBACK means business
acceptance: require VISIT_ASSIGNED before considering a visit accepted.
Do not publish retained commands. Status is retained; events are not.

One visit produces one complete WAV. Defaults match the Python utility:
20 seconds, 640,044 bytes. I2S_NUM_0, pins 18/19/34, 32-bit left channel,
DMA 8 × 256, 16 kHz and `(int16_t)(raw >> 16)` are preserved. I2S reads now
time out at 250 ms instead of blocking forever. WAV is little-endian RIFF PCM
format 1, mono, 16 kHz, 16-bit; the 44-byte header is finalized after capture.
Only fixed 256-sample buffers are used, not a recording-sized RAM allocation.

## Libraries and build prerequisites

Install Python and PlatformIO Core (`pip install platformio`, preferably in a
virtual environment). `pio run` installs the pinned dependencies:

- Espressif32 PlatformIO platform 6.10.0 (Arduino ESP32 2.0.17).
- 256dpi **MQTT** 2.5.2: supports outgoing QoS 1 (unlike PubSubClient).
- ArduinoJson 7.4.2.
- WiFi, WiFiClientSecure, HTTPClient, LittleFS, Preferences and legacy I2S
  are bundled with the pinned Arduino core.

`esp32dev` is a **build target assumption**, not confirmed hardware. Confirm
the real board, dual-core ESP32, 4 MB flash and partition layout before flash.
The worker is pinned to core 1; single-core ESP32 variants are not supported
by this configuration. No extra Python runtime is needed on a production PC.

## Secrets and CA embedding

Configure a private `.env` (never commit it). Required names: `WIFI_SSID`,
`WIFI_PASSWORD`, `DEVICE_ID`, `MQTT_HOST`, `MQTT_PORT`, `MQTT_USERNAME`,
`MQTT_PASSWORD`, `WEB_BASE_URL`, `FIRMWARE_DEVICE_API_KEY`, `UPLOAD_HOST`;
optional `RECORD_SECONDS` defaults to 20. `.env` contains no CA file paths.
The build environment must contain real values, not placeholders. Then:

```sh
set -a
. ./.env
set +a
pio run
```

The pre-build script rejects placeholder credentials and generates ignored
`include/BuildConfig.h`. No secret is hardcoded in tracked firmware sources.
Both the generated header and `.pio` binaries contain secrets: protect them,
avoid shared build caches/artifacts, and clean/rebuild when switching devices.
Protect deployed flash with the appropriate secure-boot/flash-encryption and
provisioning process; environment injection alone does not protect firmware
against physical extraction.

Confirm `respicare-mqtt-ca.crt` with the broker operator over a trusted channel.
The build script embeds it from the repository root for **MQTT only**. HTTPS
uses `src/WebCaBundle.h`, a compact bundle of 121 public Mozilla CA roots
generated from certifi's Mozilla trust store (2026.7.22) for Arduino ESP32
2.0.17 `setCACertBundle()`. Source `cacert.pem` SHA-256:
`9cc2a774b5198dcff14d9be1e66091f538975d867ce029a96bce15a55dfd730f`.
Mozilla root data is licensed under MPL 2.0. To rotate roots, verify a trusted Mozilla PEM
bundle and run `python3 scripts/generate_web_ca_bundle.py /trusted/roots.pem`;
OpenSSL is needed only to regenerate the public header, not at build/runtime.
Confirm the actual Vercel and Supabase HTTPS chains anchor in this bundle on
hardware before testing upload. Plan CA rotation before expiry.

MQTT uses its private CA; the fixed Vercel Web API uses `setCACert` with
Mozilla-trusted GTS Root R1, while signed Storage PUT uses `setCACertBundle`.
The Vercel chain in October 2026 includes a cross-signed GTS Root R1 whose
GlobalSign issuer is not in the bundled Mozilla roots; explicit GTS R1 lets
the regular mbedTLS verifier anchor the chain to the trusted GTS root.
All HTTPS connections still verify hostnames and
wait for an SNTP **response on this boot** before TLS connects. A plausible
RTC clock alone is insufficient. No insecure fallback exists. HTTPS redirects
are disabled.
`UPLOAD_HOST` is the exact allowed Storage hostname, provided by deployment;
paths/tokens come exclusively from the API. The signed URL and credentials
are never printed. The device API key is attached only to the Web API.

## Storage provisioning and flash

LittleFS is an explicit replaceable fallback for unconfirmed hardware.
Mount uses `LittleFS.begin(false)`: **no automatic format or partition change**.
At least WAV size + 32 KiB free space is required before accepting capture.
The pinned Arduino ESP32 `default.csv` for 4 MB flash reserves `spiffs`
(LittleFS-compatible partition label) at 0x290000, size 0x160000 (1,441,792
bytes), and two OTA app slots of 0x140000 (1,310,720 bytes). A production
test build with a public bundle used 1,077,533 bytes of one app slot (82.2%).
Verify the actual device flash/partition map instead of assuming they match.
Long recordings require a larger confirmed filesystem; no SD/PSRAM is implied.
The firmware rejects WAVs over 50 MiB and refuses capture if storage is short.

Before flashing, confirm the ESP32 DevKit module/revision, actual flash size,
whether its existing NVS/partition/WAV data must be preserved, microphone
wiring (SCK18, WS19, SD34), 3.3 V supply and the correct USB serial port.
No physical ESP32 has been identified on the development host. After these
checks, from the project directory with the private `.env` loaded:

```sh
pio device list
pio run -e esp32dev
pio run -e esp32dev -t upload --upload-port /dev/cu.YOUR_ESP32
pio device monitor --port /dev/cu.YOUR_ESP32 --baud 921600
```

On a **new, empty, confirmed filesystem partition only**, provision the empty
LittleFS image using `pio run -t uploadfs --upload-port /dev/cu.YOUR_ESP32`.
This replaces that partition and erases existing WAV/history: never run it
as a normal deployment/retry step on an active device. Firmware upload alone
does not create a missing filesystem. Do not erase NVS/history during updates.

## First hardware check (no WAV upload)

Serial logs should show `[WIFI] connected` → `[NTP] synchronized UTC ...`
→ `[MQTT] connecting TLS` → `[MQTT] TLS connected; command subscribed QoS1`
→ `[MQTT] DEVICE_ONLINE retained QoS1`. Broker connection errors print
numeric MQTT error/CONNACK codes, never passwords or signed URLs. The firmware
does **not** make any TLS connection before an NTP reply this boot. Check the
retained status from a separate, authorized MQTT operator client.

**Do not send a valid ASSIGN_VISIT yet:** under the unchanged production flow
it starts recording and then WAV upload immediately. The log
`[MQTT] ASSIGN_VISIT received` is ready for a later full-flow test. To test
command-subscription transport without audio, subscribe via the authorized
operator client and observe the Web/Gateway commands without publishing a
valid assignment to this device. A malformed command is rejected/logged, but
is not a test of a successful ASSIGN_VISIT.

## MQTT production test (when ready for full flow)

Use a separately authorized operator account, not broadly shared device
credentials. Device ACLs should allow subscribe only to its command topic and
publish only to its event/status topics. Use mosquitto client config files
with restricted permissions for operator credentials (avoid shell history).

```sh
mosquitto_sub -h mqtt.ndbnguyen.dpdns.org -p 8883 \
  --cafile /trusted/path/respicare-mqtt-ca.crt -q 1 -v \
  -t 'respicare/device/RESPICARE-STETH-001/event' \
  -t 'respicare/device/RESPICARE-STETH-001/status'
```

Prefer assigning through the actual Web/Gateway. For an authorized direct
test, create an in-progress test visit in Web, then publish a non-retained
command with fresh UUID messageId and that visitId:

```sh
mosquitto_pub -h mqtt.ndbnguyen.dpdns.org -p 8883 \
  --cafile /trusted/path/respicare-mqtt-ca.crt -q 1 \
  -t 'respicare/device/RESPICARE-STETH-001/command' \
  -m '{"version":1,"type":"ASSIGN_VISIT","messageId":"REPLACE_UUID","visitId":"ACTUAL_VISIT_UUID","timestamp":"2026-10-16T00:00:00Z"}'
```

Expect DEVICE_ONLINE retained, then VISIT_ASSIGNED, RECORDING_STARTED,
RECORDING_STOPPED, UPLOAD_STARTED, UPLOAD_SUCCESS. Retries can repeat
UPLOAD_STARTED; never expect UPLOAD_SUCCESS before complete returns 2xx.
Disconnect power/network to verify broker-published retained DEVICE_OFFLINE.
The Last Will timestamp is necessarily connection-setup time, not failure time.

## Full E2E acceptance

1. Configure the same device ID, broker credentials/ACL, device API key,
   HTTPS host/roots and MQTT CA across production services/device.
2. In Web create an in-progress visit; assign the stethoscope. Verify Gateway
   sends ASSIGN_VISIT through Mosquitto and device acknowledges that visit.
3. Verify one 20-second recording and finalized local WAV, then Web API
   `POST /api/device/visits/{visitId}/recording/upload-url` (X-Device-Key).
4. Verify direct binary PUT to the returned URL, `Content-Type: audio/wav`,
   without X-Device-Key. Check Storage object size and WAV properties.
5. Verify `POST /api/device/recordings/{recordingId}/complete`, JSON `{}`,
   X-Device-Key. Confirm backend validation, AI trigger, respiratory_cycles
   persisted and Web Realtime UI update. Firmware has no AI endpoint/key.
6. Only after complete 2xx: UPLOAD_SUCCESS, local WAV cleanup, then next visit.
7. Replay the same command after completion/reboot: no second recording.
   Send a new visit during recording/upload: it must not replace the active visit.
8. Test wrong API key (401), nonexistent visit (404), ended/uploaded visit
   conflict (409), large WAV (413), malformed WAV (415) on a test deployment.
   None is treated as success or retried indefinitely.
9. Test Wi-Fi loss during capture/upload, backend 5xx/timeouts, expired signed
   URL, PUT response loss, complete response loss, restart after PUT, and invalid
   CA/hostname. Verify file preservation, bounded retries and no insecure TLS.

## Retry, recovery and limitations

- API network errors/408/429/5xx use at most 4 attempts per stage with
  exponential backoff + jitter. Upload attempts obtain a fresh signed URL
  and verify the returned visit/recording IDs. Signed-token 400/401/403 is
  retried only in the Storage PUT stage; Web API 401 is terminal.
- Web API 404/409/413/415 and other non-transient 4xx are terminal. A backend
  409 is a conflict, **not** an invented successful completion.
- Backend source was reviewed: upload-url reuses a waiting/failed recording
  with upsert; complete is idempotent even after upload is marked uploaded.
  It currently awaits AI (route maxDuration 180 seconds). ESP32 HTTPClient
  uses a bounded 60-second timeout and retries **complete**, never re-records
  or re-requests upload-url after entering COMPLETING.
- Checkpoints preserve the recording ID before PUT and COMPLETING before
  complete. Reboot resumes finalized uploads/completion. Mid-capture power
  loss, exhausted retries, or storage errors enter a locked ERROR state,
  retain the file and reject new assignments. No remote retry/reset API is
  invented. Operator/service firmware must inspect/preserve the WAV/checkpoint
  before explicitly clearing a failed job; reboot alone does not retry ERROR.
- Before requesting a signed URL or uploading, the storage adapter checks the
  local WAV's size and full 44-byte PCM/mono/16 kHz/16-bit header. A missing,
  truncated or malformed WAV locks the job and retains the file; the backend
  still performs definitive validation after upload.
- Dedup ledger is append-only (74 bytes per accepted visit). No evictions:
  full/corrupt/unreadable storage fails closed. Interrupted ledger/NVS writes
  may require service rather than risking a duplicate recording. NVS saves
  are single-key checkpoints, not an atomic transaction with LittleFS.
- MQTT events are RAM-queued across network outages, **not** a durable outbox
  across power loss. Exactly-once event delivery is not claimed. Consumer
  handling must tolerate QoS 1 duplicates and use backend recording state.
- Real board microphone integrity, flash write stalls/wear, TLS heap margin,
  CA chains, broker ACLs and production E2E require physical-device tests.
  No hardware/production credentials were available during this refactor.

## Serial debug and host tests

```sh
AUDIO_SERIAL_DEBUG=1 pio run -t upload
python3 record_audio.py
python3 tests/test_audio.py
python3 tests/test_config.py
python3 tests/test_flow.py
```

Debug builds compile only entry point, AudioRecorder and the Serial branch;
networking/secrets/CA are not required. Set the Python COM port appropriately.
Close the Serial monitor before running Python. Clean/rebuild with production
environment values and AUDIO_SERIAL_DEBUG=0 before deploying a real device.
Host tests exercise actual C++ conversion/WAV code with mocked I2S/storage;
they do not replace ESP32 or production network acceptance tests.

## Validation performed

- PlatformIO debug and production builds passed for the pinned esp32dev target.
  Production compilation ran in an isolated temporary project with test-only
  credentials and a public CA bundle, leaving local secrets/artifacts untouched;
  it did not connect to any production service or flash a device.
- Audio host tests passed: original pins/I2S/DMA, signed PCM conversion,
  RIFF sizes/format, exact sample count, oversize and read/write failures.
- Six configuration tests passed, including missing secret, private key,
  insecure HTTP URL, oversize duration and secret-free debug configuration.
- State-machine host tests passed: event ordering, duplicate message/visit,
  fresh URL after expiry, terminal API errors, bounded complete retry,
  interrupted recording, capture failure and upload/complete reboot recovery.
- Physical-device recording quality, MQTT/TLS delivery and full production
  Web → AI → Realtime acceptance remain unverified.
