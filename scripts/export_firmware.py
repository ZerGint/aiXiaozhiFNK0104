#!/usr/bin/env python3
"""Validate and export the current worktree's application binary."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path

from firmware_version import format_version, load_version

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "xiaozhi.bin"
OUTPUT = ROOT / "release" / "latest"
OTA_SLOT = 0x3F0000


def app_descriptor(data: bytes) -> dict[str, str]:
    if len(data) < 0x18 or data[0] != 0xE9:
        raise SystemExit("build/xiaozhi.bin is not an ESP image")
    segment_count = data[1]
    offset = 0x18
    first = None
    for index in range(segment_count):
        if offset + 8 > len(data):
            raise SystemExit("truncated ESP image segment header")
        size = int.from_bytes(data[offset + 4:offset + 8], "little")
        offset += 8
        segment = data[offset:offset + size]
        if len(segment) != size:
            raise SystemExit("truncated ESP image segment")
        if index == 0:
            first = segment
        offset += size
    if first is None or len(first) < 0xB0:
        raise SystemExit("ESP app descriptor is missing")
    return {
        "version": first[0x10:0x30].split(b"\0", 1)[0].decode("utf-8"),
        "idf_version": first[0x70:0x90].split(b"\0", 1)[0].decode("utf-8"),
        "compile_time": first[0x60:0x70].split(b"\0", 1)[0].decode("utf-8") + "T" + first[0x50:0x60].split(b"\0", 1)[0].decode("utf-8"),
        "elf_sha256": first[0x90:0xB0].hex(),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    if not BIN.is_file():
        raise SystemExit("missing build/xiaozhi.bin; run idf.py build first")
    data = BIN.read_bytes()
    desc = app_descriptor(data)
    expected = format_version(load_version(ROOT / "firmware_version.json"))
    if desc["version"] != expected:
        raise SystemExit(f"binary version {desc['version']!r} != source version {expected!r}")
    if data[0xC] != 0x09:
        raise SystemExit(f"binary chip id 0x{data[0xC]:02x} is not ESP32-S3")
    if len(data) > OTA_SLOT:
        raise SystemExit(f"application size {len(data)} exceeds OTA slot {OTA_SLOT}")
    branch = subprocess.check_output(["git", "branch", "--show-current"], cwd=ROOT, text=True).strip()
    if branch != "FINAL":
        raise SystemExit(f"release export requires FINAL, got {branch!r}")
    sha256 = hashlib.sha256(data).hexdigest()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    output_bin = OUTPUT / "fnk0104s-firmware.bin"
    shutil.copyfile(BIN, output_bin)
    info = {
        "version": desc["version"],
        "channel": "stable" if "_beta_" not in desc["version"] else desc["version"].split("_beta_", 1)[1].rsplit("_", 1)[0],
        "branch": branch,
        "git_sha": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "size": len(data),
        "sha256": sha256,
        "chip": "esp32s3",
        "board": "freenove-fnk0104s",
        "build_time": desc["compile_time"],
        "idf_version": desc["idf_version"],
    }
    (OUTPUT / "firmware-info.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(info, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
