#!/usr/bin/env python3
# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

"""Forward Vixen Wireless Unit UDP packets and decode MTSTA responses.

The proxy listens for one INDI client host, forwards datagrams unchanged to the
Wireless Unit, and sends each matching response back to the requesting client.
One request is outstanding at a time; a new request retires the previous
upstream UDP socket so late replies cannot be routed to a new client port. MTSTA
coordinates are printed in human-readable sexagesimal form in addition to the
raw wire packet.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import datetime as dt
import math
import pathlib
import selectors
import socket
import sys
import time
from typing import TextIO


MJD_EPOCH = dt.datetime(1858, 11, 17, tzinfo=dt.timezone.utc)


@dataclasses.dataclass(frozen=True)
class MountStatus:
    mjd: float
    status: int
    field2: str
    field3: str
    mechanical_x: float
    mechanical_y: float
    azimuth_degrees: float
    altitude_degrees: float
    right_ascension_hours: float
    declination_degrees: float
    target_type: int
    target_label: str


def parse_mtsta(line: str) -> MountStatus | None:
    """Parse one complete MTSTA response, returning None for other packets."""
    line = line.strip("\r\n")
    if not line.startswith("$MTSTA="):
        return None

    fields = line[len("$MTSTA=") :].split(",")
    if len(fields) < 11 or fields[0] == "NG":
        return None

    try:
        return MountStatus(
            mjd=float(fields[0]),
            status=int(fields[1]),
            field2=fields[2],
            field3=fields[3],
            mechanical_x=float(fields[4]),
            mechanical_y=float(fields[5]),
            azimuth_degrees=float(fields[6]),
            altitude_degrees=float(fields[7]),
            right_ascension_hours=float(fields[8]),
            declination_degrees=float(fields[9]),
            target_type=int(fields[10]),
            target_label=fields[11] if len(fields) >= 12 else "",
        )
    except ValueError:
        return None


def format_hms(hours: float, precision: int = 3) -> str:
    total_seconds = round((hours % 24.0) * 3600.0, precision)
    if total_seconds >= 86400.0:
        total_seconds = 0.0
    hh = int(total_seconds // 3600)
    total_seconds -= hh * 3600
    mm = int(total_seconds // 60)
    ss = total_seconds - mm * 60
    width = 3 + precision if precision else 2
    return f"{hh:02d}:{mm:02d}:{ss:0{width}.{precision}f}"


def format_dms(degrees: float, *, signed: bool = True, precision: int = 3) -> str:
    negative = math.copysign(1.0, degrees) < 0
    total_seconds = round(abs(degrees) * 3600.0, precision)
    dd = int(total_seconds // 3600)
    total_seconds -= dd * 3600
    mm = int(total_seconds // 60)
    ss = total_seconds - mm * 60
    sign = ("-" if negative else "+") if signed else ""
    width = 3 + precision if precision else 2
    return f"{sign}{dd:03d}:{mm:02d}:{ss:0{width}.{precision}f}"


def format_mtsta(status: MountStatus) -> str:
    utc = MJD_EPOCH + dt.timedelta(days=status.mjd)
    state_name = {
        1: "IDLE/TRACKING",
        2: "SLEWING",
        3: "STOPPING",
    }.get(status.status, "UNKNOWN")
    return (
        f"MTSTA UTC={utc.isoformat(timespec='milliseconds')} "
        f"state={status.status}({state_name}) "
        f"Az={format_dms(status.azimuth_degrees, signed=False)} "
        f"Alt={format_dms(status.altitude_degrees)} "
        f"RA={format_hms(status.right_ascension_hours)} "
        f"Dec={format_dms(status.declination_degrees)} "
        f"MechX={status.mechanical_x:.6f} MechY={status.mechanical_y:.6f} "
        f"target={status.target_type} label={status.target_label or '-'} "
        f"field2={status.field2} field3={status.field3}"
    )


class Reporter:
    def __init__(self, log_path: pathlib.Path | None, show_raw: bool) -> None:
        self.show_raw = show_raw
        self.log_file: TextIO | None = None
        if log_path is not None:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            self.log_file = log_path.open("a", encoding="utf-8", buffering=1)

    def close(self) -> None:
        if self.log_file is not None:
            self.log_file.close()

    def emit(self, message: str) -> None:
        timestamp = dt.datetime.now().astimezone().isoformat(timespec="milliseconds")
        rendered = f"[{timestamp}] {message}"
        print(rendered, flush=True)
        if self.log_file is not None:
            print(rendered, file=self.log_file, flush=True)

    def packet(self, direction: str, data: bytes, peer: tuple[str, int]) -> None:
        text = data.decode("ascii", errors="backslashreplace")
        if self.show_raw:
            self.emit(f"{direction} {peer[0]}:{peer[1]} {text.rstrip()!r}")
        for line in text.splitlines():
            status = parse_mtsta(line)
            if status is not None:
                self.emit(format_mtsta(status))


def command_family(packet: bytes) -> bytes | None:
    """Extract the command family from the first Vixen protocol line."""
    line = packet.split(b"\r\n", 1)[0]
    if not line.startswith(b"$"):
        return None
    separators = [position for position in (line.find(b"?"), line.find(b"="))
                  if position >= 0]
    separator = min(separators) if separators else -1
    if separator <= 1:
        return None
    family = line[1:separator]
    if not all(byte in b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" for byte in family):
        return None
    return family


@dataclasses.dataclass
class PendingRequest:
    client: tuple[str, int]
    family: bytes
    upstream: socket.socket
    deadline: float


class RequestRouter:
    """Route one active request; quarantine old UDP ports against late replies."""

    def __init__(
        self,
        target: tuple[str, int],
        response_timeout: float,
        retire_grace: float,
        max_retired: int,
        socket_factory=socket.socket,
    ) -> None:
        self.target = target
        self.response_timeout = response_timeout
        self.retire_grace = retire_grace
        self.max_retired = max_retired
        self.socket_factory = socket_factory
        self.owner_host: str | None = None
        self.pending: PendingRequest | None = None
        self.retired: collections.deque[tuple[socket.socket, float]] = collections.deque()

    def _retire_pending(self, now: float) -> None:
        if self.pending is not None:
            self.retired.append((self.pending.upstream, now + self.retire_grace))
            self.pending = None

    def expire(self, now: float) -> None:
        if self.pending is not None and now >= self.pending.deadline:
            self._retire_pending(now)
        while self.retired and now >= self.retired[0][1]:
            upstream, _ = self.retired.popleft()
            upstream.close()

    def begin(self, data: bytes, client: tuple[str, int], now: float) -> str | None:
        self.expire(now)
        family = command_family(data)
        if family is None:
            return "request is not a Vixen command"
        if self.owner_host is not None and client[0] != self.owner_host:
            return f"another client host owns this proxy ({self.owner_host})"
        # Reserve a quarantine slot even for the new active socket. Never
        # recycle a port early merely to accommodate a burst of requests.
        if len(self.retired) + (self.pending is not None) >= self.max_retired:
            return "retired upstream socket limit reached; wait for expiry"

        upstream = self.socket_factory(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            upstream.connect(self.target)
            upstream.send(data)
        except OSError:
            upstream.close()
            raise

        if self.pending is not None:
            self._retire_pending(now)
        if self.owner_host is None:
            self.owner_host = client[0]
        self.pending = PendingRequest(client, family, upstream, now + self.response_timeout)
        return None

    def receive(
        self, upstream: socket.socket, data: bytes, now: float
    ) -> tuple[str, int] | None:
        self.expire(now)
        if self.pending is None or upstream is not self.pending.upstream:
            return None
        if command_family(data) != self.pending.family:
            return None
        client = self.pending.client
        self._retire_pending(now)
        return client

    def select_timeout(self, now: float) -> float:
        deadlines = []
        if self.pending is not None:
            deadlines.append(self.pending.deadline)
        if self.retired:
            deadlines.append(self.retired[0][1])
        return max(0.0, min(deadlines) - now) if deadlines else 1.0

    def close(self) -> None:
        if self.pending is not None:
            self.pending.upstream.close()
            self.pending = None
        while self.retired:
            upstream, _ = self.retired.popleft()
            upstream.close()


def run_proxy(args: argparse.Namespace) -> int:
    if args.response_timeout <= 0 or args.retire_grace <= 0 or args.max_retired < 1:
        raise ValueError("timeouts and max-retired must be positive")
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client_socket.bind((args.listen_host, args.listen_port))
    selector = selectors.DefaultSelector()
    selector.register(client_socket, selectors.EVENT_READ, "client")
    router = RequestRouter(
        (args.target_host, args.target_port),
        args.response_timeout,
        args.retire_grace,
        args.max_retired,
    )
    registered_upstream: socket.socket | None = None
    reporter = Reporter(args.log_file, args.raw)

    reporter.emit(
        f"listening on {args.listen_host}:{args.listen_port}; "
        f"forwarding to {args.target_host}:{args.target_port}; "
        "single client host, one outstanding request"
    )
    try:
        while True:
            now = time.monotonic()
            # Unregister an already retired socket before expiry can close it.
            if registered_upstream is not None and (
                router.pending is None or registered_upstream is not router.pending.upstream
            ):
                selector.unregister(registered_upstream)
                registered_upstream = None
            router.expire(now)
            desired_upstream = router.pending.upstream if router.pending else None
            if desired_upstream is not registered_upstream:
                if registered_upstream is not None:
                    selector.unregister(registered_upstream)
                if desired_upstream is not None:
                    selector.register(desired_upstream, selectors.EVENT_READ, "upstream")
                registered_upstream = desired_upstream

            for key, _ in selector.select(router.select_timeout(now)):
                if key.data == "client":
                    data, client = client_socket.recvfrom(65535)
                    reporter.packet("TX", data, client)
                    previous = router.pending
                    rejected = router.begin(data, client, time.monotonic())
                    if rejected:
                        reporter.emit(f"dropped request from {client[0]}:{client[1]}: {rejected}")
                    elif previous is not None:
                        reporter.emit(
                            f"retired unanswered {previous.family.decode('ascii')} "
                            f"request from {previous.client[0]}:{previous.client[1]}"
                        )
                else:
                    upstream = key.fileobj
                    if router.pending is None or upstream is not router.pending.upstream:
                        reporter.emit("dropped event for a retired upstream socket")
                        continue
                    data = upstream.recv(65535)
                    client = router.receive(upstream, data, time.monotonic())
                    reporter.packet(
                        "RX" if client is not None else "RX-DROP",
                        data,
                        (args.target_host, args.target_port),
                    )
                    if client is not None:
                        client_socket.sendto(data, client)
                    else:
                        reporter.emit("dropped unmatched, expired, or duplicate upstream response")
    except KeyboardInterrupt:
        reporter.emit("stopped")
        return 0
    finally:
        reporter.close()
        selector.close()
        router.close()
        client_socket.close()


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Forward Wireless Unit UDP traffic and decode MTSTA coordinates."
    )
    parser.add_argument("--listen-host", default="127.0.0.1")
    parser.add_argument("--listen-port", type=int, default=60024)
    parser.add_argument("--target-host", default="10.42.0.60")
    parser.add_argument("--target-port", type=int, default=60023)
    parser.add_argument(
        "--response-timeout", type=float, default=30.0,
        help="seconds to keep an unanswered request (default: 30)",
    )
    parser.add_argument(
        "--retire-grace", type=float, default=30.0,
        help="seconds to quarantine each retired upstream UDP port (default: 30)",
    )
    parser.add_argument(
        "--max-retired", type=int, default=64,
        help="maximum quarantined UDP ports before rejecting new requests (default: 64)",
    )
    parser.add_argument("--log-file", type=pathlib.Path)
    parser.add_argument(
        "--no-raw", dest="raw", action="store_false", help="hide raw TX/RX packets"
    )
    parser.set_defaults(raw=True)
    return parser


def main() -> int:
    return run_proxy(build_argument_parser().parse_args())


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"udp_forward: {error}", file=sys.stderr)
        raise SystemExit(1)
