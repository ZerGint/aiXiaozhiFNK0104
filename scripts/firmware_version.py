#!/usr/bin/env python3
"""Structured firmware version parsing, formatting, and ordering."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Any


VERSION_RE = re.compile(
    r"^(?P<major>\d+)\.(?P<stable>\d+)\.(?P<final>\d+)"
    r"(?:_beta_(?P<channel>[a-z][a-z0-9]*)_(?P<beta>\d+))?$"
)


class Comparison(str, Enum):
    LESS = "LESS"
    EQUAL = "EQUAL"
    GREATER = "GREATER"
    INCOMPARABLE = "INCOMPARABLE"


@dataclass(frozen=True)
class FirmwareVersion:
    major: int
    stable: int
    final: int
    channel: str = "stable"
    beta: int = 0

    @property
    def is_stable(self) -> bool:
        return self.channel == "stable"

    def as_dict(self) -> dict[str, Any]:
        return {
            "major": self.major,
            "stable": self.stable,
            "final": self.final,
            "channel": self.channel,
            "beta": self.beta,
        }


def _validate(version: FirmwareVersion) -> FirmwareVersion:
    if min(version.major, version.stable, version.final, version.beta) < 0:
        raise ValueError("version numbers must be non-negative")
    if version.channel == "stable":
        if version.beta != 0:
            raise ValueError("stable versions must have beta=0")
    elif not re.fullmatch(r"[a-z][a-z0-9]*", version.channel):
        raise ValueError(f"invalid beta channel: {version.channel!r}")
    elif version.beta < 1:
        raise ValueError("beta versions must have beta >= 1")
    return version


def load_version(path: str | Path = "firmware_version.json") -> FirmwareVersion:
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    required = ("major", "stable", "final", "channel", "beta")
    missing = [key for key in required if key not in data]
    if missing:
        raise ValueError(f"missing version fields: {', '.join(missing)}")
    return _validate(FirmwareVersion(
        major=int(data["major"]),
        stable=int(data["stable"]),
        final=int(data["final"]),
        channel=str(data["channel"]),
        beta=int(data["beta"]),
    ))


def parse_version(value: str) -> FirmwareVersion:
    match = VERSION_RE.fullmatch(value)
    if not match:
        raise ValueError(f"invalid firmware version: {value!r}")
    channel = match.group("channel") or "stable"
    beta = int(match.group("beta") or 0)
    return _validate(FirmwareVersion(
        int(match.group("major")), int(match.group("stable")),
        int(match.group("final")), channel, beta,
    ))


def format_version(version: FirmwareVersion) -> str:
    base = f"{version.major}.{version.stable}.{version.final}"
    return base if version.is_stable else f"{base}_beta_{version.channel}_{version.beta}"


def compare_versions(left: str | FirmwareVersion, right: str | FirmwareVersion) -> Comparison:
    lhs = parse_version(left) if isinstance(left, str) else left
    rhs = parse_version(right) if isinstance(right, str) else right
    lhs_base = (lhs.major, lhs.stable, lhs.final)
    rhs_base = (rhs.major, rhs.stable, rhs.final)
    if lhs_base != rhs_base:
        return Comparison.LESS if lhs_base < rhs_base else Comparison.GREATER
    if lhs.channel == rhs.channel:
        if lhs.beta == rhs.beta:
            return Comparison.EQUAL
        return Comparison.LESS if lhs.beta < rhs.beta else Comparison.GREATER
    if lhs.is_stable and not rhs.is_stable:
        return Comparison.GREATER
    if rhs.is_stable and not lhs.is_stable:
        return Comparison.LESS
    return Comparison.INCOMPARABLE


def next_stable(version: FirmwareVersion) -> FirmwareVersion:
    return FirmwareVersion(version.major, version.stable + 1, 0)


def next_final(version: FirmwareVersion) -> FirmwareVersion:
    return FirmwareVersion(version.major, version.stable, version.final + 1)


def next_beta(version: FirmwareVersion, channel: str) -> FirmwareVersion:
    beta = version.beta + 1 if version.channel == channel else 1
    return FirmwareVersion(version.major, version.stable, version.final, channel, beta)
