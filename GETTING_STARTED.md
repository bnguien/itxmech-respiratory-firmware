# First build and hardware check after cloning

This guide is for the ESP32 DevKit **stethoscope**, not the SpO2 firmware.
The first test checks Wi-Fi, NTP, MQTT TLS, subscription, and retained online
status. A valid `ASSIGN_VISIT` immediately starts recording and uploading;
**do not issue one until you are ready for a full recording test**.

## 1. Confirm prerequisites

- Python 3, a working USB data cable and a suitable ESP32 USB serial driver.
- The actual board must be a dual-core ESP32 DevKit with **4 MB flash** for
  the checked-in `esp32dev` target and `default.csv` partition table. Confirm
  its model, flash size and existing partition/data layout before flashing.
- Microphone I2S pins must be SCK 18, WS 19 and SD 34; check 3.3 V power.
- The device Wi-Fi needs DNS, UDP/123 to an NTP server and outbound TLS to
  the MQTT broker on 8883. Obtain Wi-Fi credentials, the device-specific
  MQTT password and device API key privately from the operator.
- Obtain `respicare-mqtt-ca.crt` from the broker operator through a trusted
  channel: it is intentionally **not in Git**. Check its fingerprint with the
  operator before placing it at the repository root. HTTPS roots are embedded:
  `src/WebApiCa.h` for the Web API and `src/StorageCa.h` for Storage.
  Do not add `HTTPS_CA_FILE`.
- Obtain the exact Supabase Storage hostname from the deployed backend's
  signed URL for `UPLOAD_HOST`. It is required at build even though this
  initial test does not upload. Do not paste the signed URL or its token into
  the configuration.

## 2. Install the build tool and configure a private device

On macOS/Linux (replace the clone URL and directory with your own):

```sh
git clone <FIRMWARE_REPOSITORY_URL>
cd itxmech-respiratory-firmware
python3 -m venv .venv
. .venv/bin/activate
python -m pip install platformio
cp .env.example .env
chmod 600 .env
```

Edit `.env` locally: replace every `REPLACE_ME` and `YOUR_PROJECT` value.
Do not copy passwords from another device, paste them into chat/issues, or
commit `.env`. `AUDIO_SERIAL_DEBUG` must be unset (or `0`) for production;
the Python/Serial flow is not part of this test. Check that the private
`respicare-mqtt-ca.crt` is present in this directory. Do **not** create an
HTTPS CA file: the firmware uses embedded public certificates.

Load the private settings in the same shell you will build from:

```sh
set -a
. ./.env
set +a
pio run -e esp32dev
```

`pio run` installs the pinned ESP32 Arduino platform, MQTT and ArduinoJson
dependencies. A missing CA, unset credential or placeholder value fails the
build. Generated `include/BuildConfig.h` and `.pio` firmware binaries contain
credentials: both are ignored by Git; protect your computer/build artifacts.
Never upload a locally built firmware binary to a public artifact store.

## 3. Flash **only after** checking the board and existing data

The checked-in 4 MB layout includes 1280 KiB app slots and a 1408 KiB
filesystem partition labeled `spiffs` (used by LittleFS). Uploading firmware
also writes the configured partition table. Stop if the device has a different
partition map or recordings/NVS data you need to preserve. A new, empty
LittleFS partition may need provisioning later; do not run `uploadfs` on an
active device, as it replaces the filesystem and erases recordings/history.

```sh
pio device list
pio run -e esp32dev -t upload --upload-port /dev/cu.YOUR_ESP32
pio device monitor --port /dev/cu.YOUR_ESP32 --baud 921600
```

Replace `/dev/cu.YOUR_ESP32` with the **verified** port from `pio device list`.
On Linux, a typical port is `/dev/ttyUSB0` or `/dev/ttyACM0`; ensure your user
has serial permissions. If no ESP32 port is identified, do not guess a port.

## 4. Observe the first test (no WAV upload)

Serial should show, in order:

```text
[WIFI] connected
[NTP] synchronized UTC ...
[MQTT] connecting TLS
[MQTT] TLS connected; command subscribed QoS1
[MQTT] DEVICE_ONLINE retained QoS1
```

The device waits for an NTP response **on this boot** before MQTT TLS. If it
stalls after Wi-Fi, check DNS/UDP 123. MQTT failures include numeric error
and CONNACK codes but never the password; check CA authenticity, broker ACLs,
broker hostname, port 8883 and clock. Use a separately authorized operator
MQTT client to check the retained `DEVICE_ONLINE` message on
`respicare/device/RESPICARE-STETH-001/status`. See `README.md` for the MQTT
subscription command and complete E2E acceptance checklist.

`[BOOT] audio/storage unavailable` means the device is **not ready for an
ASSIGN_VISIT** even if MQTT is online. Check confirmed filesystem hardware,
partition and provisioning without erasing existing data. Testing reception
of a *valid* `ASSIGN_VISIT` necessarily proceeds to WAV capture/upload in the
unchanged production flow; defer that command until a full E2E test is agreed.

## 5. Optional offline checks

With a C++ compiler and Python installed (no hardware or production key):

```sh
python3 tests/test_audio.py
python3 tests/test_config.py
python3 tests/test_flow.py
```

These are host tests, not proof of microphone quality, broker connectivity,
physical flash capacity or signed-upload compatibility on the actual board.

## 6. Web API TLS verification after the first recording test

If the device stops after `[API] requesting upload URL` with
`esp_crt_verify_callback(): Failed to verify certificate`, the previous
Web API connection used only the public CA bundle. This version verifies
  the fixed Vercel Web API with Mozilla-trusted self-signed GTS Root R1 instead.
  Signed Storage uses GTS Root R4 because its October 2026 chain has the same
  cross-sign issue. Rebuild and flash only after
checking current TLS certificates and the device's flash/data layout. Do not
use `setInsecure()` or put device API credentials in TLS diagnostics.

If the previous run ended in `[FLOW] failed`, its WAV and NVS checkpoint are
retained, and a reboot will **not** automatically retry an ERROR job. Before
resetting any device state, preserve the WAV and coordinate recovery of the
existing visit with the backend operator. Do not erase LittleFS/NVS, republish
the same visit, or assume reflashing will upload that recording.

## 7. One-time discard of the failed WAV (backend operator approval only)

This **permanently deletes** `/recording.wav` for the selected failed job and
clears only the `respicare` NVS `job` key; `/accepted.log` and all other NVS
keys remain intact. Use only after the backend operator has handled the old
visit/recording, confirmed the WAV is no longer needed, and approved the
specific old visit ID. Verify the board/partition and serial port first.
On Windows PowerShell, after `git pull`, with PlatformIO installed:

```powershell
$env:MAINTENANCE_VISIT_ID = 'ad2d6b03-b921-4094-b584-d249426a0279'
$env:MAINTENANCE_CONFIRM = 'DISCARD_FAILED_WAV'
pio run -e esp32dev -t clean
pio run -e esp32dev -t upload --upload-port COM3
pio device monitor --port COM3 --baud 921600
```

Replace `COM3` with the verified ESP32 port. The service firmware does **not**
connect to Wi-Fi/MQTT. Proceed only when Serial prints `[SERVICE] DONE: failed
WAV/job removed; accepted.log preserved`. If it prints `STOP`, do not assign
a visit: inspect the reason instead. Exit the monitor (Ctrl+C), then restore
production in the **same PowerShell** window:

```powershell
Remove-Item Env:MAINTENANCE_VISIT_ID
Remove-Item Env:MAINTENANCE_CONFIRM
Remove-Item Env:AUDIO_SERIAL_DEBUG -ErrorAction SilentlyContinue
Get-Content .env | ForEach-Object {
    if ($_ -match '^\s*(?:export\s+)?([A-Z][A-Z0-9_]*)=(.*)$') {
        $value = $matches[2].Trim().Trim([char[]]@([char]39, [char]34))
        [Environment]::SetEnvironmentVariable($matches[1], $value, 'Process')
    }
}
pio run -e esp32dev -t clean
pio run -e esp32dev -t upload --upload-port COM3
pio device monitor --port COM3 --baud 921600
```

Confirm `[MQTT] DEVICE_ONLINE retained QoS1` with **no** `[FLOW] restoring
checkpoint` and **no** `[SERVICE]` output. Then create an entirely **new**
in-progress backend visit and publish a new `ASSIGN_VISIT` with a new UUID
`messageId`. Never reuse the discarded visit ID (the dedup ledger retains it).

## 8. Recover a WAV after a failed Storage TLS PUT (do not re-record)

Use only when the backend operator confirms the visit is still valid and its
recording is **not uploaded** (waiting/failed). Do not use this mode if complete
may already have been accepted; coordinate with the backend first. The actual
visit ID from the failed recording is required; do **not** reuse the example
visit ID from section 7. Confirm the same ESP32/COM port, that the WAV is still
needed, and `RECORD_SECONDS` matches the original capture (20 by default).
After `git pull`, in PowerShell:

```powershell
$env:MAINTENANCE_VISIT_ID = '<ACTUAL_FAILED_VISIT_UUID>'
$env:MAINTENANCE_CONFIRM = 'RECOVER_FAILED_UPLOAD'
$env:RECORD_SECONDS = '20'
pio run -e esp32dev -t clean
pio run -e esp32dev -t upload --upload-port COM11
pio device monitor --port COM11 --baud 921600
```

The service firmware makes **no network request**. Only after
`[SERVICE] DONE: failed WAV preserved; upload checkpoint restored` should you
exit the monitor (Ctrl+C), clear both maintenance environment variables,
load `.env` as shown above, clean/build and flash production onto the **same**
board. Production resumes the old WAV with a *fresh* signed URL; do **not**
send another `ASSIGN_VISIT`. Expect `[FLOW] restoring checkpoint`, then
`[API] requesting upload URL`, `UPLOAD_STARTED`, `UPLOAD_SUCCESS` after the
complete API returns 2xx. If service prints `STOP` (including `job NOT_FOUND`),
do not erase flash or proceed with a new visit; inspect the actual device/job.
