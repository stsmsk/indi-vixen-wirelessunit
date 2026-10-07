#!/usr/bin/env python3
# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

"""Measure the real-unit interpretation of the MTMV2 duration field."""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import pathlib
import socket
import sys
import time
from typing import TextIO


SIDEREAL_DEGREES_PER_SECOND = 0.0041780555555555555


@dataclasses.dataclass(frozen=True)
class MountStatus:
    raw: str
    status: int
    mechanical_x: float
    mechanical_y: float


@dataclasses.dataclass(frozen=True)
class TrialResult:
    duration: float
    start: MountStatus
    end: MountStatus
    restored: MountStatus
    displacement_degrees: float
    restore_error_degrees: float


def parse_mtsta(response: str) -> MountStatus:
    line = response.strip("\r\n")
    if not line.startswith("$MTSTA="):
        raise ValueError(f"response is not MTSTA: {line!r}")
    fields = line[len("$MTSTA=") :].split(",")
    if len(fields) < 11 or fields[0] == "NG":
        raise ValueError(f"invalid MTSTA response: {line!r}")
    try:
        return MountStatus(
            raw=line,
            status=int(fields[1]),
            mechanical_x=float(fields[4]),
            mechanical_y=float(fields[5]),
        )
    except ValueError as error:
        raise ValueError(f"could not parse numeric MTSTA fields: {line!r}") from error


def angular_delta(after: float, before: float) -> float:
    """Return the shortest signed angular difference in degrees."""
    return (after - before + 180.0) % 360.0 - 180.0


def classify_duration_unit(short: TrialResult, long: TrialResult) -> str:
    short_delta = abs(short.displacement_degrees)
    long_delta = abs(long.displacement_degrees)
    if short_delta < 1e-5:
        return "inconclusive: the short-duration displacement is too small"
    ratio = long_delta / short_delta
    expected_ratio = long.duration / short.duration
    if 0.5 * expected_ratio <= ratio <= 1.5 * expected_ratio:
        return "consistent with seconds"
    return "inconsistent with the seconds hypothesis; further verification is required"


class Reporter:
    def __init__(self, path: pathlib.Path | None) -> None:
        self.file: TextIO | None = None
        if path is not None:
            path.parent.mkdir(parents=True, exist_ok=True)
            self.file = path.open("a", encoding="utf-8", buffering=1)

    def close(self) -> None:
        if self.file is not None:
            self.file.close()

    def emit(self, message: str) -> None:
        timestamp = dt.datetime.now().astimezone().isoformat(timespec="milliseconds")
        line = f"[{timestamp}] {message}"
        print(line, flush=True)
        if self.file is not None:
            print(line, file=self.file, flush=True)


class WirelessUnitClient:
    def __init__(self, host: str, port: int, timeout: float, reporter: Reporter) -> None:
        self.peer = (host, port)
        self.timeout = timeout
        self.reporter = reporter
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.connect(self.peer)
        self.socket.settimeout(timeout)

    def close(self) -> None:
        self.socket.close()

    def request(self, command: str, *, retries: int = 1) -> str:
        payload = command.encode("ascii")
        for attempt in range(1, retries + 1):
            self.reporter.emit(f"TX {command.rstrip()!r}")
            started = time.monotonic()
            self.socket.send(payload)
            try:
                data = self.socket.recv(4096)
            except socket.timeout:
                self.reporter.emit(f"RX timeout ({self.timeout:.3f} s, attempt {attempt}/{retries})")
                if attempt == retries:
                    raise
                continue
            elapsed_ms = (time.monotonic() - started) * 1000.0
            response = data.decode("ascii", errors="replace").strip("\r\n")
            self.reporter.emit(f"RX {response!r} ({elapsed_ms:.3f} ms)")
            return response
        raise RuntimeError("unreachable")

    def status(self, retries: int) -> MountStatus:
        return parse_mtsta(self.request("$MTSTA?\r\n", retries=retries))

    def move(self, duration: float, ra_rate: float, dec_rate: float) -> None:
        command = f"$MTMV2={duration:.2f},{ra_rate:.6f},{dec_rate:.6f}\r\n"
        response = self.request(command, retries=1)
        if response != command.rstrip("\r\n"):
            raise RuntimeError(f"MTMV2 response does not match the request: {response}")

    def emergency_stop(self) -> None:
        try:
            response = self.request("$MTSTP=1\r\n", retries=1)
            if response != "$MTSTP=1":
                raise RuntimeError(f"invalid MTSTP response: {response}")
        except (OSError, RuntimeError):
            self.reporter.emit("WARNING: the emergency-stop response was not confirmed")


def axis_value(status: MountStatus, axis: str) -> float:
    return status.mechanical_x if axis == "ra" else status.mechanical_y


def run_trial(
    client: WirelessUnitClient,
    reporter: Reporter,
    *,
    duration: float,
    rate: float,
    axis: str,
    settle: float,
    query_retries: int,
) -> TrialResult:
    start = client.status(query_retries)
    reporter.emit(
        f"trial start duration={duration:.2f} rate={rate:.6f} axis={axis.upper()} "
        f"MechX={start.mechanical_x:.6f} MechY={start.mechanical_y:.6f}"
    )
    ra_rate, dec_rate = (rate, 0.0) if axis == "ra" else (0.0, rate)
    client.move(duration, ra_rate, dec_rate)
    time.sleep(duration + settle)
    end = client.status(query_retries)

    # Return by issuing the same pulse with the opposite sign. This is not a
    # precision positioning operation; backlash can leave a small residual.
    restore_ra, restore_dec = (-rate, 0.0) if axis == "ra" else (0.0, -rate)
    client.move(duration, restore_ra, restore_dec)
    time.sleep(duration + settle)
    restored = client.status(query_retries)

    start_value = axis_value(start, axis)
    displacement = angular_delta(axis_value(end, axis), start_value)
    restore_error = angular_delta(axis_value(restored, axis), start_value)
    reporter.emit(
        f"trial result duration={duration:.2f}: displacement={displacement:+.8f} deg "
        f"({displacement * 3600.0:+.3f} arcsec), "
        f"restore error={restore_error:+.8f} deg ({restore_error * 3600.0:+.3f} arcsec)"
    )
    return TrialResult(duration, start, end, restored, displacement, restore_error)


def print_preview(args: argparse.Namespace) -> None:
    axis_rates = (args.rate, 0.0) if args.axis == "ra" else (0.0, args.rate)
    restore_rates = (-args.rate, 0.0) if args.axis == "ra" else (0.0, -args.rate)
    print("Nothing was sent to the mount. With --execute, the following test runs:")
    for duration in (args.short_duration, args.long_duration):
        print(f"  $MTMV2={duration:.2f},{axis_rates[0]:.6f},{axis_rates[1]:.6f}")
        print(f"  $MTMV2={duration:.2f},{restore_rates[0]:.6f},{restore_rates[1]:.6f}  # restore")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Measure the physical time unit of MTMV2 argument 1")
    parser.add_argument("--host", default="10.42.0.60", help="Wireless Unit IP address")
    parser.add_argument("--port", type=int, default=60023)
    parser.add_argument("--axis", choices=("ra", "dec"), default="ra")
    parser.add_argument("--rate", type=float, default=10.0, help="sidereal-rate multiplier")
    parser.add_argument("--short-duration", type=float, default=0.10)
    parser.add_argument("--long-duration", type=float, default=1.00)
    parser.add_argument("--settle", type=float, default=0.30, help="settling time after motion in seconds")
    parser.add_argument("--timeout", type=float, default=1.0)
    parser.add_argument("--query-retries", type=int, default=3)
    parser.add_argument("--log", type=pathlib.Path)
    parser.add_argument("--execute", action="store_true", help="send motion commands to the physical mount")
    parser.add_argument("--yes", action="store_true", help="skip the interactive safety confirmation")
    return parser


def validate_args(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if not (0 < args.port <= 65535):
        parser.error("--port must be between 1 and 65535")
    if args.rate == 0:
        parser.error("--rate must be nonzero")
    if args.short_duration <= 0 or args.long_duration <= args.short_duration:
        parser.error("require 0 < --short-duration < --long-duration")
    if args.settle < 0 or args.timeout <= 0 or args.query_retries < 1:
        parser.error("settling time, timeout, or retry count is invalid")


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    validate_args(parser, args)
    if not args.execute:
        print_preview(args)
        return 0

    if not args.yes:
        if not sys.stdin.isatty():
            parser.error("--yes is also required in a non-interactive environment")
        answer = input(
            "Have you disconnected INDI/Ekos, stopped tracking, and verified equipment and cable safety? [yes/N] "
        )
        if answer.strip().lower() != "yes":
            print("Aborted.")
            return 1

    reporter = Reporter(args.log)
    client = WirelessUnitClient(args.host, args.port, args.timeout, reporter)
    try:
        initial = client.status(args.query_retries)
        reporter.emit(
            f"connection check MTSTA status={initial.status} "
            f"MechX={initial.mechanical_x:.6f} MechY={initial.mechanical_y:.6f}"
        )
        if initial.status != 1:
            raise RuntimeError(f"aborting because MTSTA status={initial.status}; stopped status 1 is required")

        short = run_trial(
            client,
            reporter,
            duration=args.short_duration,
            rate=args.rate,
            axis=args.axis,
            settle=args.settle,
            query_retries=args.query_retries,
        )
        long = run_trial(
            client,
            reporter,
            duration=args.long_duration,
            rate=args.rate,
            axis=args.axis,
            settle=args.settle,
            query_retries=args.query_retries,
        )
        short_delta = abs(short.displacement_degrees)
        long_delta = abs(long.displacement_degrees)
        ratio = long_delta / short_delta if short_delta else float("inf")
        expected_ratio = args.long_duration / args.short_duration
        expected_short = abs(args.rate) * SIDEREAL_DEGREES_PER_SECOND * args.short_duration
        expected_long = abs(args.rate) * SIDEREAL_DEGREES_PER_SECOND * args.long_duration
        reporter.emit(
            f"comparison: measured ratio={ratio:.3f}, expected ratio for seconds={expected_ratio:.3f}; "
            f"theoretical displacement short={expected_short * 3600.0:.3f} arcsec, "
            f"long={expected_long * 3600.0:.3f} arcsec"
        )
        reporter.emit(f"classification: {classify_duration_unit(short, long)}")
        return 0
    except KeyboardInterrupt:
        reporter.emit("interrupt detected; sending the stop command")
        client.emergency_stop()
        return 130
    except (OSError, ValueError, RuntimeError) as error:
        reporter.emit(f"error: {error}")
        reporter.emit("sending the stop command for safety")
        client.emergency_stop()
        return 2
    finally:
        client.close()
        reporter.close()


if __name__ == "__main__":
    raise SystemExit(main())
