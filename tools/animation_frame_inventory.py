#!/usr/bin/env python3
"""
Inventory the PNG frames that drive each LVGL emoji animation.

Run this from the root of aligned-tools (or point `--dir` at the factory firmware SPIFFS
partition) to get a per-state summary of built-in and custom frames. Export the mapping to
JSON if you want to feed it into a companion tool for generating new animation bundles.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Dict, List

PREFIXES = [
    "speaking",
    "listening",
    "greeting",
    "standby",
    "detecting",
    "detected",
    "analyzing",
]
MAX_IMAGES = 10


def numeric_key(name: str) -> int:
    """Return the numeric suffix of a filename or a high constant if none exists."""
    match = re.search(r"(\d+)(?=\D*\.png$)", name)
    return int(match.group(1)) if match else 9999


def gather_frames(directory: Path) -> Dict[str, Dict[str, List[str]]]:
    """Scan the directory and bucket PNGs by prefix and builtin/custom variant."""
    data: Dict[str, Dict[str, List[str]]] = {
        prefix: {"builtin": [], "custom": []} for prefix in PREFIXES
    }

    for entry in directory.iterdir():
        if not entry.is_file():
            continue
        if entry.suffix.lower() != ".png":
            continue

        name = entry.name
        for prefix in PREFIXES:
            if name.startswith(f"Custom_{prefix}"):
                data[prefix]["custom"].append(name)
                break
            if name.startswith(prefix):
                data[prefix]["builtin"].append(name)
                break

    for buckets in data.values():
        for key in ("builtin", "custom"):
            buckets[key].sort(key=numeric_key)

    return data


def print_summary(summary: Dict[str, Dict[str, List[str]]], target_state: str | None) -> None:
    """Print a formatted list of PNG names for each animated state."""
    candidates = [target_state] if target_state else PREFIXES
    for state in candidates:
        if state not in summary:
            continue
        builtin = summary[state]["builtin"]
        custom = summary[state]["custom"]
        overflow = " (truncated at MAX_IMAGES)" if len(builtin) + len(custom) > MAX_IMAGES else ""
        print(f"{state:12s} → builtin {len(builtin)} frame(s), custom {len(custom)} frame(s){overflow}")
        if builtin:
            print(f"  builtin: {', '.join(builtin)}")
        if custom:
            print(f"  custom : {', '.join(custom)}")
        if not builtin and not custom:
            print("  (no PNGs found; placeholders will be used)")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="List factory-firmware animation frames for each LVGL emoji state."
    )
    default_dir = (
        Path(__file__).resolve().parents[1] / "examples" / "factory_firmware" / "spiffs"
    )
    parser.add_argument(
        "--dir",
        type=Path,
        default=default_dir,
        help="Path to the SPIFFS folder that contains the PNG frames",
    )
    parser.add_argument(
        "--state",
        choices=PREFIXES,
        help="Show only a single animation state",
    )
    parser.add_argument(
        "--json-out",
        type=Path,
        help="Write the summary as JSON for downstream tooling",
    )

    args = parser.parse_args()
    if not args.dir.exists():
        raise SystemExit(f"{args.dir} does not exist")
    summary = gather_frames(args.dir)
    print(f"MAX_IMAGES limit: {MAX_IMAGES}")
    print(f"Scanned {len(list(args.dir.glob('*.png')))} PNG(s) in {args.dir}")
    print_summary(summary, args.state)

    if args.json_out:
        with args.json_out.open("w", encoding="utf-8") as json_file:
            json.dump(summary, json_file, indent=2)
        print(f"Wrote JSON summary to {args.json_out}")


if __name__ == "__main__":
    main()
