#!/usr/bin/env python3
# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

"""Create a reproducible first-pass summary of a Wireless Unit pcap.

This tool never transmits network traffic. It shells out to tshark and writes
tabular conversation data plus raw application payloads grouped by stream.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys


def run_tshark(capture: pathlib.Path, fields: list[str], display_filter: str = "") -> str:
    command = ["tshark", "-r", str(capture), "-T", "fields"]
    if display_filter:
        command.extend(["-Y", display_filter])
    for field in fields:
        command.extend(["-e", field])
    command.extend(["-E", "header=y", "-E", "separator=\t", "-E", "quote=d"])
    result = subprocess.run(command, check=True, text=True, capture_output=True)
    return result.stdout


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()

    if shutil.which("tshark") is None:
        parser.error("tshark was not found; install the 'tshark' package")
    if not args.capture.is_file():
        parser.error(f"capture does not exist: {args.capture}")

    args.output.mkdir(parents=True, exist_ok=True)

    packets = run_tshark(
        args.capture,
        [
            "frame.number",
            "frame.time_epoch",
            "_ws.col.Protocol",
            "ip.src",
            "tcp.srcport",
            "udp.srcport",
            "ip.dst",
            "tcp.dstport",
            "udp.dstport",
            "tcp.stream",
            "udp.stream",
            "tcp.len",
            "udp.length",
            "tcp.payload",
            "udp.payload",
        ],
        "ip",
    )
    (args.output / "packets.tsv").write_text(packets, encoding="utf-8")

    endpoints = run_tshark(
        args.capture,
        [
            "frame.number",
            "eth.src",
            "eth.dst",
            "ip.src",
            "ip.dst",
            "tcp.srcport",
            "tcp.dstport",
            "udp.srcport",
            "udp.dstport",
            "_ws.col.Protocol",
        ],
    )
    (args.output / "endpoints.tsv").write_text(endpoints, encoding="utf-8")

    manifest = {
        "source_file": args.capture.name,
        "sha256": sha256(args.capture),
        "outputs": ["packets.tsv", "endpoints.tsv"],
        "warning": "Review outputs for private network and observing data before sharing.",
    }
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Wrote analysis to {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print(error.stderr, file=sys.stderr)
        raise SystemExit(error.returncode)
