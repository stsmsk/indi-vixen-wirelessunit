#!/usr/bin/env python3
# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

"""Exercise the built INDI driver through indiserver in Simulation only."""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import signal
import socket
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape


DEVICE = "Vixen Wireless Unit"
VECTOR = re.compile(
    rb"<(?P<tag>(?:def|set)(?:Switch|Number|Text)Vector)\b[^>]*>.*?</(?P=tag)>",
    re.DOTALL,
)
DEL_PROPERTY = re.compile(rb"<delProperty\b[^>]*/>")


def find_vectors(data: bytes, tag: str, name: str) -> list[ET.Element]:
    found = []
    for match in VECTOR.finditer(data):
        if match.group("tag").decode("ascii") != tag:
            continue
        vector = ET.fromstring(match.group())
        if vector.attrib.get("device") == DEVICE and vector.attrib.get("name") == name:
            found.append(vector)
    return found


def element_value(vector: ET.Element, name: str) -> str | None:
    for item in vector:
        if item.attrib.get("name") == name:
            return (item.text or "").strip()
    return None


def find_del_property(data: bytes, name: str) -> ET.Element | None:
    for match in DEL_PROPERTY.finditer(data):
        item = ET.fromstring(match.group())
        if item.attrib.get("device") == DEVICE and item.attrib.get("name") == name:
            return item
    return None


class IndiPeer:
    def __init__(self, peer: socket.socket) -> None:
        self.peer = peer
        self.buffer = bytearray()

    def send(self, xml: str) -> int:
        start = len(self.buffer)
        self.peer.sendall(xml.encode("utf-8") + b"\n")
        return start

    def wait_for(self, predicate, start: int = 0, timeout: float = 8.0) -> ET.Element:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate(bytes(self.buffer[start:]))
            if value is not None:
                return value
            self.peer.settimeout(max(0.01, min(0.5, deadline - time.monotonic())))
            try:
                chunk = self.peer.recv(8192)
            except socket.timeout:
                continue
            if not chunk:
                raise RuntimeError("indiserver closed the XML connection")
            self.buffer.extend(chunk)
            if len(self.buffer) > 2 * 1024 * 1024:
                raise RuntimeError("INDI XML response exceeded 2 MiB")
        raise TimeoutError("INDI XML response was not received in time")

    def wait_vector(self, tag: str, name: str, start: int = 0, predicate=None) -> ET.Element:
        def matching(data: bytes):
            for vector in find_vectors(data, tag, name):
                if predicate is None or predicate(vector):
                    return vector
            return None

        try:
            return self.wait_for(matching, start)
        except TimeoutError as error:
            observed = find_vectors(bytes(self.buffer[start:]), tag, name)
            states = [vector.attrib.get("state", "<missing>") for vector in observed]
            raise TimeoutError(
                f"INDI XML {name} response did not match; observed {tag} states: {states}"
            ) from error


def switch_xml(name: str, values: dict[str, str]) -> str:
    children = "".join(
        f'<oneSwitch name="{escape(key)}">{escape(value)}</oneSwitch>'
        for key, value in values.items()
    )
    return (
        f'<newSwitchVector device="{DEVICE}" name="{name}">'
        f"{children}</newSwitchVector>"
    )


def number_xml(name: str, values: dict[str, str]) -> str:
    children = "".join(
        f'<oneNumber name="{escape(key)}">{escape(value)}</oneNumber>'
        for key, value in values.items()
    )
    return (
        f'<newNumberVector device="{DEVICE}" name="{name}">'
        f"{children}</newNumberVector>"
    )


def free_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def connect_server(port: int, process: subprocess.Popen) -> socket.socket:
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"indiserver exited with status {process.returncode}")
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=0.5)
        except OSError:
            time.sleep(0.05)
    raise TimeoutError("indiserver did not open its test port")


def exercise(peer: IndiPeer, expected_version: str) -> None:
    peer.send(f'<getProperties version="1.7" device="{DEVICE}"/>')
    discovery = peer.wait_vector("defSwitchVector", "WIRELESS_DISCOVERY")
    endpoint = peer.wait_vector("defTextVector", "WIRELESS_ENDPOINT")
    assert element_value(discovery, "SEARCH") == "Off"
    discovery_start = re.search(rb"<defSwitchVector\b[^>]*WIRELESS_DISCOVERY", peer.buffer)
    endpoint_start = re.search(rb"<defTextVector\b[^>]*WIRELESS_ENDPOINT", peer.buffer)
    assert discovery_start and endpoint_start and discovery_start.start() < endpoint_start.start()
    assert endpoint.attrib.get("device") == DEVICE
    peer.wait_vector("defSwitchVector", "SIMULATION")
    peer.wait_vector("defSwitchVector", "WIRELESS_SETTINGS_SOURCE")
    peer.wait_vector("defSwitchVector", "CONNECTION")
    driver_build = peer.wait_vector("defTextVector", "DRIVER_BUILD")
    assert element_value(driver_build, "DRIVER_VERSION") == expected_version

    start = peer.send(switch_xml("SIMULATION", {"ENABLE": "On", "DISABLE": "Off"}))
    peer.wait_vector(
        "setSwitchVector", "SIMULATION", start,
        lambda vector: element_value(vector, "ENABLE") == "On",
    )
    start = peer.send(switch_xml("WIRELESS_SETTINGS_SOURCE", {"USE_DEVICE_SETTINGS": "Off"}))
    peer.wait_vector(
        "setSwitchVector", "WIRELESS_SETTINGS_SOURCE", start,
        lambda vector: element_value(vector, "USE_DEVICE_SETTINGS") == "Off",
    )
    # Offline Set accepts a future mount setting but must not claim that the
    # Wireless Unit has applied it. Local policy/slew-rate switches differ.
    offline_settings = [
        ("setSwitchVector", "WIRELESS_GOTO_SPEED",
         switch_xml("WIRELESS_GOTO_SPEED", {"GOTO_1000": "On"})),
        ("setNumberVector", "WIRELESS_GUIDE_RATE",
         number_xml("WIRELESS_GUIDE_RATE", {"GUIDE_RATE_WE": "1.0"})),
        ("setNumberVector", "WIRELESS_BACKLASH",
         number_xml("WIRELESS_BACKLASH", {"BACKLASH_RA": "0"})),
        ("setSwitchVector", "WIRELESS_MOTOR_POWER",
         switch_xml("WIRELESS_MOTOR_POWER", {"POWER_3": "On"})),
        ("setSwitchVector", "WIRELESS_MOUNT_TYPE",
         switch_xml("WIRELESS_MOUNT_TYPE", {"NOT_POLAR_ALIGNED": "On"})),
        ("setSwitchVector", "WIRELESS_MERIDIAN_DIRECTION",
         switch_xml("WIRELESS_MERIDIAN_DIRECTION", {"EAST": "On"})),
        ("setNumberVector", "WIRELESS_MERIDIAN",
         number_xml("WIRELESS_MERIDIAN", {"STOP_POINT": "3", "GOTO_FLIP_POINT": "0"})),
    ]
    for tag, name, request in offline_settings:
        start = peer.send(request)
        peer.wait_vector(
            tag, name, start,
            lambda vector: vector.attrib.get("state") == "Idle" and
                           "Will be applied when connecting." in vector.attrib.get("message", ""),
        )
    start = peer.send(switch_xml("CONNECTION", {"CONNECT": "On", "DISCONNECT": "Off"}))
    peer.wait_vector(
        "setSwitchVector", "CONNECTION", start,
        lambda vector: vector.attrib.get("state") == "Ok" and
                       element_value(vector, "CONNECT") == "On",
    )
    peer.wait_vector("defSwitchVector", "WIRELESS_SET_HOME_POSITION", start)
    for tag, name, _ in offline_settings:
        peer.wait_vector(
            tag, name, start,
            lambda vector: vector.attrib.get("state") == "Ok",
        )

    # Deleting without a selected row must explain the rejection in both the
    # control-panel property and the ordinary INDI message log.
    start = peer.send(switch_xml("WIRELESS_ALIGNMENT_DELETE", {"DELETE_SELECTED": "On"}))
    peer.wait_vector(
        "setSwitchVector", "WIRELESS_ALIGNMENT_DELETE", start,
        lambda vector: vector.attrib.get("state") == "Alert" and
                       "Select an alignment point before deleting it." in
                       vector.attrib.get("message", ""),
    )
    peer.wait_for(
        lambda data: b"[ERROR] Select an alignment point before deleting it." in data,
        start,
    )
    start = peer.send(switch_xml("WIRELESS_ALIGNMENT_DELETE", {"DELETE_ALL": "On"}))
    peer.wait_vector(
        "setSwitchVector", "WIRELESS_ALIGNMENT_DELETE", start,
        lambda vector: vector.attrib.get("state") == "Idle" and
                       "No alignment points to delete." in vector.attrib.get("message", ""),
    )

    # A valid local setting should be accepted. The fractional backlash
    # request must then be rejected without replacing the confirmed UI value.
    start = peer.send(number_xml("WIRELESS_GUIDE_RATE", {"GUIDE_RATE_WE": "2.0"}))
    peer.wait_vector(
        "setNumberVector", "WIRELESS_GUIDE_RATE", start,
        lambda vector: vector.attrib.get("state") == "Ok" and
                       element_value(vector, "GUIDE_RATE_WE") is not None and
                       abs(float(element_value(vector, "GUIDE_RATE_WE")) - 2.0) < 1e-6,
    )
    start = peer.send(number_xml("WIRELESS_GUIDE_RATE", {"GUIDE_RATE_WE": "1.25"}))
    peer.wait_vector(
        "setNumberVector", "WIRELESS_GUIDE_RATE", start,
        lambda vector: vector.attrib.get("state") == "Alert" and
                       "0.1 increments" in vector.attrib.get("message", "") and
                       element_value(vector, "GUIDE_RATE_WE") is not None and
                       abs(float(element_value(vector, "GUIDE_RATE_WE")) - 2.0) < 1e-6,
    )
    peer.wait_for(lambda data: b"[ERROR] Invalid Guide rate" in data, start)
    start = peer.send(number_xml("WIRELESS_BACKLASH", {"BACKLASH_RA": "1.5"}))
    peer.wait_vector(
        "setNumberVector", "WIRELESS_BACKLASH", start,
        lambda vector: vector.attrib.get("state") == "Alert" and
                       "integers from 0 to 99" in vector.attrib.get("message", "") and
                       element_value(vector, "BACKLASH_RA") == "0",
    )
    peer.wait_for(lambda data: b"[ERROR] Invalid Backlash compensation" in data, start)

    # Invalid Meridian behavior must be both Alert and a visible ERROR message;
    # otherwise KStars may show only a red indicator without explaining why.
    start = peer.send(number_xml(
        "WIRELESS_MERIDIAN", {"STOP_POINT": "3", "GOTO_FLIP_POINT": "4"}
    ))
    peer.wait_vector(
        "setNumberVector", "WIRELESS_MERIDIAN", start,
        lambda vector: vector.attrib.get("state") == "Alert" and
                       "GoTo flip point must not exceed Stop point" in
                       vector.attrib.get("message", ""),
    )
    peer.wait_for(
        lambda data: b"[ERROR] Invalid Meridian behavior" in data,
        start,
    )

    # HOME setup is safe in Simulation and must change the INDI property
    # topology from pre-initialization controls to initialized controls.
    start = peer.send(switch_xml("WIRELESS_SET_HOME_POSITION", {"SET": "On"}))
    peer.wait_vector(
        "setSwitchVector", "WIRELESS_SET_HOME_POSITION", start,
        lambda vector: vector.attrib.get("state") == "Ok",
    )
    peer.wait_for(lambda data: find_del_property(data, "WIRELESS_SET_HOME_POSITION"), start)
    # Disconnecting during Park must not carry SCOPE_PARKING into the next
    # connection. A rejection immediately before that disconnect must also
    # not reappear as a stale red indicator when controls are redefined.
    start = peer.send(switch_xml("TELESCOPE_PARK", {"PARK": "On", "UNPARK": "Off"}))
    peer.wait_vector(
        "setSwitchVector", "TELESCOPE_PARK", start,
        lambda vector: vector.attrib.get("state") == "Busy" and
                       element_value(vector, "PARK") == "On",
    )
    start = peer.send(switch_xml("WIRELESS_ALIGNMENT_DELETE", {"DELETE_SELECTED": "On"}))
    peer.wait_vector(
        "setSwitchVector", "WIRELESS_ALIGNMENT_DELETE", start,
        lambda vector: vector.attrib.get("state") == "Alert",
    )
    start = peer.send(switch_xml("CONNECTION", {"CONNECT": "Off", "DISCONNECT": "On"}))
    peer.wait_vector(
        "setSwitchVector", "CONNECTION", start,
        lambda vector: vector.attrib.get("state") == "Idle" and
                       element_value(vector, "DISCONNECT") == "On",
    )
    for tag, name, _ in offline_settings:
        peer.wait_vector(
            tag, name, start,
            lambda vector: vector.attrib.get("state") == "Idle",
        )
    start = peer.send(switch_xml("CONNECTION", {"CONNECT": "On", "DISCONNECT": "Off"}))
    peer.wait_vector(
        "setSwitchVector", "CONNECTION", start,
        lambda vector: vector.attrib.get("state") == "Ok" and
                       element_value(vector, "CONNECT") == "On",
    )
    peer.wait_vector(
        "defSwitchVector", "WIRELESS_ALIGNMENT_DELETE", start,
        lambda vector: vector.attrib.get("state") == "Idle",
    )
    peer.wait_vector(
        "defSwitchVector", "TELESCOPE_PARK", start,
        lambda vector: vector.attrib.get("state") != "Busy" and
                       element_value(vector, "UNPARK") == "On",
    )
    peer.wait_vector(
        "defSwitchVector", "TELESCOPE_TRACK_STATE", start,
        lambda vector: vector.attrib.get("state") == "Idle" and
                       element_value(vector, "TRACK_OFF") == "On",
    )
    peer.wait_vector(
        "defNumberVector", "EQUATORIAL_EOD_COORD", start,
        lambda vector: vector.attrib.get("state") != "Busy",
    )
    start = peer.send(switch_xml("CONNECTION", {"CONNECT": "Off", "DISCONNECT": "On"}))
    peer.wait_vector(
        "setSwitchVector", "CONNECTION", start,
        lambda vector: vector.attrib.get("state") == "Idle" and
                       element_value(vector, "DISCONNECT") == "On",
    )
    print("PASS: driver XML Simulation settings, alignment deletion, reconnect, HOME, and disconnect")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    parser.add_argument("--indiserver", type=pathlib.Path, required=True)
    parser.add_argument("--expected-version", required=True)
    args = parser.parse_args()
    if not args.driver.is_file() or not os.access(args.driver, os.X_OK):
        parser.error("built INDI driver executable is missing")
    if not args.indiserver.is_file() or not os.access(args.indiserver, os.X_OK):
        parser.error("indiserver executable is missing")

    port = free_loopback_port()
    with tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as log:
        process = subprocess.Popen(
            [str(args.indiserver), "-p", str(port), str(args.driver)],
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
        )
        try:
            with connect_server(port, process) as socket_peer:
                exercise(IndiPeer(socket_peer), args.expected_version)
        except Exception:
            log.seek(0)
            print("indiserver output:\n" + log.read())
            raise
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=3)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
