"""Exercise the PlatformIO config hook without toolchains or real secrets."""
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/build_config.py"
BUNDLE = SCRIPT.parents[1] / "src/WebCaBundle.h"
WEB_API_CA = SCRIPT.parents[1] / "src/WebApiCa.h"


class FakeEnvironment:
    def __init__(self, root):
        self.root = root
        self.filter = None

    def subst(self, value):
        return str(self.root)

    def Replace(self, **values):
        self.filter = values["SRC_FILTER"]

    def Append(self, **values):
        pass


class ConfigTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(
            prefix="respicare-config-", dir=os.environ.get("RESPICARE_TEST_TMPDIR"))
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.env = FakeEnvironment(self.root)
        cert = self.root / "respicare-mqtt-ca.crt"
        cert.write_text("-----BEGIN CERTIFICATE-----\nTEST_ONLY\n-----END CERTIFICATE-----")
        self.values = {key: "test-only" for key in (
            "WIFI_SSID", "WIFI_PASSWORD", "DEVICE_ID", "MQTT_HOST",
            "MQTT_USERNAME", "MQTT_PASSWORD", "FIRMWARE_DEVICE_API_KEY")}
        self.values.update(WEB_BASE_URL="https://backend.example",
                           UPLOAD_HOST="storage.example")

    def run_hook(self, values):
        with patch.dict(os.environ, values, clear=True):
            runpy.run_path(str(SCRIPT), init_globals={
                "env": self.env, "Import": lambda _: None})

    def test_production_generates_private_header(self):
        self.run_hook(self.values)
        header = (self.root / "include/BuildConfig.h").read_text()
        self.assertIn('#define MQTT_PASSWORD "test-only"', header)
        self.assertIn('#define RECORD_SECONDS 20', header)
        self.assertIn('#define AUDIO_SERIAL_DEBUG 0', header)
        self.assertNotIn("HTTPS_CA", header)

    def test_missing_secret_fails_before_writing(self):
        del self.values["MQTT_PASSWORD"]
        with self.assertRaisesRegex(RuntimeError, "MQTT_PASSWORD"):
            self.run_hook(self.values)
        self.assertFalse((self.root / "include/BuildConfig.h").exists())

    def test_placeholder_wifi_fails_before_writing(self):
        self.values["WIFI_SSID"] = "REPLACE_ME"
        with self.assertRaisesRegex(RuntimeError, "WIFI_SSID"):
            self.run_hook(self.values)
        self.assertFalse((self.root / "include/BuildConfig.h").exists())

    def test_insecure_base_url_rejected(self):
        self.values["WEB_BASE_URL"] = "http://backend.example"
        with self.assertRaisesRegex(RuntimeError, "HTTPS"):
            self.run_hook(self.values)

    def test_private_key_rejected(self):
        (self.root / "respicare-mqtt-ca.crt").write_text("-----BEGIN PRIVATE KEY-----")
        with self.assertRaisesRegex(RuntimeError, "public CA"):
            self.run_hook(self.values)

    def test_oversize_recording_rejected(self):
        self.values["RECORD_SECONDS"] = "1639"
        with self.assertRaisesRegex(RuntimeError, "50 MiB"):
            self.run_hook(self.values)

    def test_debug_requires_no_secrets(self):
        self.run_hook({"AUDIO_SERIAL_DEBUG": "1"})
        self.assertEqual((self.root / "include/BuildConfig.h").read_text().count("#define"), 1)
        self.assertNotIn("+<src/Network.cpp>", self.env.filter)

    def test_web_ca_bundle_is_sorted_and_complete(self):
        blob = bytes(int(value, 16) for value in re.findall(
            r"0x([0-9a-f]{2})", BUNDLE.read_text()))
        count = int.from_bytes(blob[:2], "big")
        self.assertGreater(count, 100)
        offset = 2
        previous = b""
        for _ in range(count):
            name_size = int.from_bytes(blob[offset:offset + 2], "big")
            key_size = int.from_bytes(blob[offset + 2:offset + 4], "big")
            name = blob[offset + 4:offset + 4 + name_size]
            self.assertGreaterEqual(name, previous)
            self.assertEqual(name[0], 0x30)  # DER X.509 subject SEQUENCE.
            self.assertEqual(blob[offset + 4 + name_size], 0x30)  # DER public key.
            previous = name
            offset += 4 + name_size + key_size
        self.assertEqual(offset, len(blob))

    def test_web_api_root_is_verified_public_gts_root(self):
        pem = WEB_API_CA.read_text().split('R"CERT(', 1)[1].split(')CERT"', 1)[0]
        result = subprocess.run(
            ["openssl", "x509", "-noout", "-subject", "-issuer", "-fingerprint", "-sha256"],
            input=pem, text=True, capture_output=True, check=True).stdout
        self.assertIn("CN=GTS Root R1", result)
        self.assertIn("D9:47:43:2A:BD:E7:B7:FA:90:FC:2E:6B:59:10:1B:12:", result)
        self.assertEqual(result.splitlines()[0].replace("subject=", ""),
                         result.splitlines()[1].replace("issuer=", ""))


if __name__ == "__main__":
    unittest.main()
