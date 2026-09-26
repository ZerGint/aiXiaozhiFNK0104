#!/usr/bin/env python3
"""Controlled version transitions for FINAL/main/experimental worktrees."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

from firmware_version import (
    FirmwareVersion,
    format_version,
    load_version,
    next_beta,
    next_final,
    next_stable,
)

ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = ROOT / "firmware_version.json"
CHANNELS = {
    "new_ota": "ota",
    "feat-fnk0104s-aec-upstream": "aec",
}
BETA_CHANNELS = {"ota", "aec", "ui", "radio", "audio", "memory", "anime"}


def branch_name() -> str:
    return subprocess.check_output(
        ["git", "branch", "--show-current"], cwd=ROOT, text=True
    ).strip()


def require_branch(branch: str, allowed: set[str]) -> None:
    if branch not in allowed:
        names = ", ".join(sorted(allowed))
        raise SystemExit(f"release action is not allowed on branch {branch!r}; expected {names}")


def write_version(version: FirmwareVersion) -> None:
    VERSION_FILE.write_text(json.dumps(version.as_dict(), indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--show", action="store_true")
    parser.add_argument("--next-stable", action="store_true")
    parser.add_argument("--next-final", action="store_true")
    parser.add_argument("--next-beta", action="store_true")
    parser.add_argument("--channel")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    actions = sum(bool(value) for value in (args.show, args.next_stable, args.next_final, args.next_beta))
    if actions != 1:
        parser.error("choose exactly one of --show, --next-stable, --next-final, --next-beta")

    version = load_version(VERSION_FILE)
    branch = branch_name()
    if args.show:
        channel = "stable" if version.is_stable else version.channel
        print(f"version={format_version(version)}")
        print(f"branch={branch}")
        print(f"channel={channel}")
        print(f"git_sha={subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()}")
        return 0

    if args.next_stable:
        require_branch(branch, {"main"})
        target = next_stable(version)
    elif args.next_final:
        require_branch(branch, {"FINAL"})
        target = next_final(version)
    else:
        require_branch(branch, set(CHANNELS))
        expected_channel = CHANNELS[branch]
        if args.channel not in BETA_CHANNELS:
            raise SystemExit("--next-beta requires explicit --channel from ota/aec/ui/radio/audio/memory/anime")
        if args.channel != expected_channel:
            raise SystemExit(
                f"branch {branch!r} maps to beta channel {expected_channel!r}; "
                f"got {args.channel!r}"
            )
        target = next_beta(version, args.channel)

    print(f"{format_version(version)} -> {format_version(target)}")
    if not args.dry_run:
        write_version(target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
