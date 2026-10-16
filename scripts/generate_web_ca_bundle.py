"""Refresh the public Arduino ESP32 certificate bundle from a trusted PEM CA store.

Run manually when rotating public roots; this is not a firmware-build dependency.
Usage: python3 scripts/generate_web_ca_bundle.py /trusted/mozilla-roots.pem
"""

import re
import subprocess
import sys
from pathlib import Path


def openssl(*args, data):
    return subprocess.run(["openssl", *args], input=data, stdout=subprocess.PIPE,
                          check=True).stdout


def tlv(data, offset):
    tag = data[offset]
    first = data[offset + 1]
    if first < 128:
        length, start = first, offset + 2
    else:
        width = first & 0x7F
        if width == 0 or width > 4:
            raise ValueError("Invalid DER length")
        length = int.from_bytes(data[offset + 2:offset + 2 + width], "big")
        start = offset + 2 + width
    end = start + length
    if end > len(data):
        raise ValueError("Truncated DER certificate")
    return tag, start, end


def subject(der):
    _, outer, _ = tlv(der, 0)
    _, tbs, _ = tlv(der, outer)
    offset = tbs
    if der[offset] == 0xA0:  # Explicit X.509 version.
        offset = tlv(der, offset)[2]
    for _ in range(4):  # serial, signature, issuer, validity.
        offset = tlv(der, offset)[2]
    tag, _, end = tlv(der, offset)
    if tag != 0x30:
        raise ValueError("Invalid DER subject")
    return der[offset:end]


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    roots = Path(sys.argv[1]).read_bytes()
    certificates = re.findall(
        rb"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", roots, re.S)
    if not certificates:
        raise ValueError("No public CA certificates found")
    entries = []
    for pem in certificates:
        der = openssl("x509", "-inform", "PEM", "-outform", "DER", data=pem)
        name = subject(der)
        pub = openssl("pkey", "-pubin", "-outform", "DER",
                      data=openssl("x509", "-pubkey", "-noout", data=pem))
        if len(name) > 65535 or len(pub) > 65535:
            raise ValueError("CA entry exceeds ESP32 bundle format")
        entries.append((name, pub))
    entries.sort(key=lambda entry: entry[0])
    blob = bytearray(len(entries).to_bytes(2, "big"))
    for name, pub in entries:
        blob += len(name).to_bytes(2, "big") + len(pub).to_bytes(2, "big")
        blob += name + pub
    target = Path(__file__).resolve().parents[1] / "src/WebCaBundle.h"
    lines = ["// Public Mozilla CA roots, packed for Arduino ESP32 2.0.17.",
             "// Regenerate only from a verified CA store using scripts/generate_web_ca_bundle.py.",
             "#pragma once", "#include <Arduino.h>",
             "static const uint8_t kWebCaBundle[] PROGMEM = {"]
    for offset in range(0, len(blob), 256):
        lines.append("  " + ",".join(f"0x{byte:02x}" for byte in blob[offset:offset + 256]) + ",")
    lines.append("};")
    if len(lines) >= 300:
        raise ValueError("Bundle exceeds the repository file-length limit")
    target.write_text("\n".join(lines) + "\n")
    print(f"Embedded {len(entries)} public roots ({len(blob)} bytes) in {target.name}")


if __name__ == "__main__":
    main()
