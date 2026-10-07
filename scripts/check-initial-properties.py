#!/usr/bin/env python3
# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

"""Check the first INDI getProperties reply from a fresh driver process."""

from __future__ import annotations

import argparse
import re
import socket
import time
import xml.etree.ElementTree as ET


VECTOR_TAG = re.compile(rb"<def(?:Text|Number|Switch|Light|BLOB)Vector\b[^>]*>")


def validate_definitions(data: bytes, device: str = "Vixen Wireless Unit") -> None:
    definitions: list[tuple[str, str, int]] = []
    for match in VECTOR_TAG.finditer(data):
        try:
            tag = ET.fromstring(match.group()[:-1] + b"/>")
        except ET.ParseError as error:
            raise ValueError("invalid INDI vector definition opening tag") from error
        vector_device = tag.attrib.get("device", "")
        vector_name = tag.attrib.get("name", "")
        if not vector_device or not vector_name:
            raise ValueError("INDI sent a vector definition with an empty device or name")
        if vector_device == device:
            definitions.append((vector_name, tag.tag, match.start()))

    discovery = [item for item in definitions if item[0] == "WIRELESS_DISCOVERY"]
    if len(discovery) != 1 or discovery[0][1] != "defSwitchVector":
        raise ValueError("expected exactly one valid WIRELESS_DISCOVERY switch definition")
    endpoint = [item for item in definitions if item[0] == "WIRELESS_ENDPOINT"]
    if len(endpoint) != 1 or discovery[0][2] >= endpoint[0][2]:
        raise ValueError("Discovery must precede the Wireless Unit endpoint definition")

    closing = data.find(b"</defSwitchVector>", discovery[0][2])
    if closing < 0:
        raise ValueError("Discovery switch definition was incomplete")
    try:
        vector = ET.fromstring(data[discovery[0][2]:closing + len(b"</defSwitchVector>")])
    except ET.ParseError as error:
        raise ValueError("Discovery switch definition was malformed") from error
    if not any(item.tag == "defSwitch" and item.attrib.get("name") == "SEARCH" for item in vector):
        raise ValueError("Discovery did not contain the SEARCH switch")


def capture_first_reply(host: str, port: int, device: str, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    chunks: list[bytes] = []
    with socket.create_connection((host, port), timeout=timeout) as peer:
        peer.sendall(f'<getProperties version="1.7" device="{device}"/>\n'.encode("ascii"))
        while time.monotonic() < deadline:
            peer.settimeout(min(0.5, max(0.01, deadline - time.monotonic())))
            try:
                chunk = peer.recv(8192)
            except socket.timeout:
                if b"WIRELESS_DISCOVERY" in b"".join(chunks) and b"WIRELESS_ENDPOINT" in b"".join(chunks):
                    break
                continue
            if not chunk:
                break
            chunks.append(chunk)
            if sum(map(len, chunks)) > 1024 * 1024:
                raise ValueError("initial INDI reply exceeded 1 MiB")
    return b"".join(chunks)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7624)
    parser.add_argument("--device", default="Vixen Wireless Unit")
    parser.add_argument("--timeout", type=float, default=8.0)
    args = parser.parse_args()
    try:
        reply = capture_first_reply(args.host, args.port, args.device, args.timeout)
        validate_definitions(reply, args.device)
    except (OSError, ValueError) as error:
        parser.exit(1, f"FAIL: {error}\n")
    print("PASS: first getProperties contains one initialized Discovery before Wireless Unit endpoint")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
