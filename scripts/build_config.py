"""Generate a private firmware configuration from the build environment."""
import json
import os
from pathlib import Path
import re

Import("env")
root = Path(env.subst("$PROJECT_DIR"))


def required(name):
    value = os.environ.get(name, "")
    if not value or "REPLACE" in value or "YOUR_" in value or "<" in value:
        raise RuntimeError(f"Missing or placeholder environment variable: {name}")
    return value


def certificate(path):
    value = path.read_text()
    if "-----BEGIN CERTIFICATE-----" not in value or "PRIVATE KEY" in value:
        raise RuntimeError(f"{path.name} must contain public CA certificates only")
    return value


maintenance = os.environ.get("MAINTENANCE_VISIT_ID", "")
debug = os.environ.get("AUDIO_SERIAL_DEBUG", "0") == "1"
if maintenance:
    if os.environ.get("MAINTENANCE_CONFIRM") != "DISCARD_FAILED_WAV":
        raise RuntimeError("MAINTENANCE_CONFIRM=DISCARD_FAILED_WAV is required")
    if not re.fullmatch(r"[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}", maintenance):
        raise RuntimeError("MAINTENANCE_VISIT_ID must be a lowercase UUID")
    env.Replace(SRC_FILTER=["+<code_esp.ino.cpp>", "+<src/Maintenance.cpp>"])
elif debug:
    env.Replace(SRC_FILTER=["+<code_esp.ino.cpp>", "+<src/Firmware.cpp>",
                            "+<src/AudioRecorder.cpp>"])
values = {"AUDIO_SERIAL_DEBUG": int(debug)}
if maintenance:
    values["MAINTENANCE_VISIT_ID"] = maintenance
elif not debug:
    for key in ("WIFI_SSID", "WIFI_PASSWORD", "DEVICE_ID", "MQTT_HOST",
                "MQTT_USERNAME", "MQTT_PASSWORD", "WEB_BASE_URL",
                "FIRMWARE_DEVICE_API_KEY", "UPLOAD_HOST"):
        values[key] = required(key)
    if not values["WEB_BASE_URL"].startswith("https://"):
        raise RuntimeError("WEB_BASE_URL must use HTTPS")
    values["WEB_BASE_URL"] = values["WEB_BASE_URL"].rstrip("/")
    values["MQTT_PORT"] = int(os.environ.get("MQTT_PORT", "8883"))
    if not 1 <= values["MQTT_PORT"] <= 65535:
        raise RuntimeError("MQTT_PORT must be a valid TCP port")
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", values["DEVICE_ID"]):
        raise RuntimeError("DEVICE_ID must be a short topic-safe identifier")
    if not re.fullmatch(r"[A-Za-z0-9.-]+", values["UPLOAD_HOST"]):
        raise RuntimeError("UPLOAD_HOST must be an exact DNS hostname")
    values["RECORD_SECONDS"] = int(os.environ.get("RECORD_SECONDS", "20"))
    if not 1 <= values["RECORD_SECONDS"] <= 1638:
        raise RuntimeError("RECORD_SECONDS must fit the 50 MiB WAV limit")
    values["MQTT_CA"] = certificate(root / "respicare-mqtt-ca.crt")
target = root / "include" / "BuildConfig.h"
target.parent.mkdir(exist_ok=True)
target.write_text("// Generated from environment. Never commit this file.\n#pragma once\n" +
                  "\n".join(f"#define {k} {json.dumps(v)}" for k, v in values.items()) + "\n")
env.Append(CPPPATH=[str(target.parent)])
