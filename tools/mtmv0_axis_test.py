#!/usr/bin/env python3
# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

"""Identify the two motion axes and signs used by the MTMV0 command."""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import pathlib
import socket
import sys
import time
from typing import TextIO


@dataclasses.dataclass(frozen=True)
class MountStatus:
    raw: str
    mjd: float
    status: int
    unknown_2: str
    unknown_3: str
    mechanical_x: float
    mechanical_y: float
    azimuth: float
    altitude: float
    right_ascension: float
    declination: float
    target_type: int
    target_name: str


@dataclasses.dataclass(frozen=True)
class TrialResult:
    argument: int
    rate: float
    start: MountStatus
    end: MountStatus
    observed_direction: str


def parse_mtsta(response: str) -> MountStatus:
    line = response.strip("\r\n")
    if not line.startswith("$MTSTA="):
        raise ValueError(f"response is not MTSTA: {line!r}")
    fields = line[len("$MTSTA=") :].split(",")
    if len(fields) < 12 or fields[0] == "NG":
        raise ValueError(f"invalid MTSTA response: {line!r}")
    try:
        return MountStatus(
            raw=line,
            mjd=float(fields[0]),
            status=int(fields[1]),
            unknown_2=fields[2],
            unknown_3=fields[3],
            mechanical_x=float(fields[4]),
            mechanical_y=float(fields[5]),
            azimuth=float(fields[6]),
            altitude=float(fields[7]),
            right_ascension=float(fields[8]),
            declination=float(fields[9]),
            target_type=int(fields[10]),
            target_name=fields[11],
        )
    except ValueError as error:
        raise ValueError(f"could not parse numeric MTSTA fields: {line!r}") from error


def angular_delta(after: float, before: float) -> float:
    return (after - before + 180.0) % 360.0 - 180.0


class Reporter:
    def __init__(self, path: pathlib.Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self.file: TextIO = path.open("a", encoding="utf-8", buffering=1)

    def close(self) -> None:
        self.file.close()

    def emit(self, message: str = "") -> None:
        timestamp = dt.datetime.now().astimezone().isoformat(timespec="milliseconds")
        line = f"[{timestamp}] {message}"
        print(line, flush=True)
        print(line, file=self.file, flush=True)


class WirelessUnitClient:
    def __init__(self, host: str, port: int, timeout: float, reporter: Reporter) -> None:
        self.reporter = reporter
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.connect((host, port))
        self.socket.settimeout(timeout)

    def close(self) -> None:
        self.socket.close()

    def request(self, command: str, retries: int = 1) -> str:
        for attempt in range(1, retries + 1):
            self.reporter.emit(f"TX {command.rstrip()!r}")
            started = time.monotonic()
            self.socket.send(command.encode("ascii"))
            try:
                data = self.socket.recv(4096)
            except socket.timeout:
                self.reporter.emit(f"RX timeout (attempt {attempt}/{retries})")
                if attempt == retries:
                    raise
                continue
            elapsed_ms = (time.monotonic() - started) * 1000.0
            response = data.decode("ascii", errors="replace").strip("\r\n")
            self.reporter.emit(f"RX {response!r} ({elapsed_ms:.3f} ms)")
            return response
        raise RuntimeError("unreachable")

    def status(self, retries: int) -> MountStatus:
        return parse_mtsta(self.request("$MTSTA?\r\n", retries))

    def move(self, duration: float, argument_2: float, argument_3: float) -> None:
        command = f"$MTMV0={duration:.1f},{argument_2:.1f},{argument_3:.1f}\r\n"
        response = self.request(command)
        if response != command.rstrip("\r\n"):
            raise RuntimeError(f"MTMV0 response does not match the request: {response!r}")

    def stop(self, duration: float) -> None:
        self.move(duration, 0.0, 0.0)

    def rates(self, retries: int) -> str:
        return self.request("$MTRAT?\r\n", retries)


def status_delta(start: MountStatus, end: MountStatus) -> str:
    return (
        f"delta MechX={angular_delta(end.mechanical_x, start.mechanical_x):+.6f} deg, "
        f"MechY={angular_delta(end.mechanical_y, start.mechanical_y):+.6f} deg, "
        f"Az={angular_delta(end.azimuth, start.azimuth):+.6f} deg, "
        f"Alt={end.altitude - start.altitude:+.6f} deg, "
        f"RA={angular_delta(end.right_ascension * 15.0, start.right_ascension * 15.0) / 15.0:+.6f} h, "
        f"Dec={end.declination - start.declination:+.6f} deg"
    )


def ask_direction() -> str:
    answer = input("Enter the observed motion direction [N/S/E/W/U(unknown)]: ").strip().upper()
    return answer if answer in {"N", "S", "E", "W"} else "U"


def run_trial(
    client: WirelessUnitClient,
    reporter: Reporter,
    *,
    argument: int,
    rate: float,
    duration: float,
    settle: float,
    retries: int,
    interactive: bool,
) -> TrialResult:
    if interactive:
        input(f"Starting argument {argument}={rate:+.1f}. Verify safety and press Enter: ")

    start = client.status(retries)
    arg2, arg3 = (rate, 0.0) if argument == 2 else (0.0, rate)
    reporter.emit(f"TRIAL argument={argument}, rate={rate:+.1f}, duration={duration:.1f}s")
    client.move(duration, arg2, arg3)
    time.sleep(duration + settle)
    client.stop(duration)
    time.sleep(settle)
    end = client.status(retries)
    reporter.emit(client.rates(retries))
    reporter.emit(status_delta(start, end))

    observed = ask_direction() if interactive else "U"
    reporter.emit(f"OBSERVED direction={observed}")
    return TrialResult(argument, rate, start, end, observed)


def default_log_path() -> pathlib.Path:
    stamp = dt.datetime.now().astimezone().strftime("%Y%m%d-%H%M%S")
    return pathlib.Path(f"mtmv0-axis-test-{stamp}.log")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Identify the physical axes and directions controlled by MTMV0 arguments 2 and 3"
    )
    parser.add_argument("--host", default="10.42.0.60")
    parser.add_argument("--port", type=int, default=60023)
    parser.add_argument("--duration", type=float, default=1.0, help="duration of each motion in seconds")
    parser.add_argument("--rate", type=float, default=30.0, help="test rate; both signs are exercised")
    parser.add_argument("--settle", type=float, default=0.30)
    parser.add_argument("--timeout", type=float, default=1.0)
    parser.add_argument("--retries", type=int, default=3)
    parser.add_argument("--log", type=pathlib.Path, default=None)
    parser.add_argument("--execute", action="store_true", help="send commands to the physical mount")
    parser.add_argument("--yes", action="store_true", help="skip startup and per-trial confirmations")
    parser.add_argument(
        "--allow-initialized",
        action="store_true",
        help="test an initialized mount; normally this should not be specified",
    )
    return parser


def validate_args(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if not (1 <= args.port <= 65535):
        parser.error("--port must be between 1 and 65535")
    if args.duration <= 0 or args.duration > 3:
        parser.error("--duration must be greater than 0 and no more than 3 seconds")
    if args.rate <= 0 or args.rate > 1000:
        parser.error("--rate must be greater than 0 and no more than 1000")
    if args.settle < 0 or args.timeout <= 0 or args.retries < 1:
        parser.error("settling time, timeout, or retry count is invalid")


def print_preview(args: argparse.Namespace) -> None:
    print("Nothing was sent to the mount. With --execute, commands are sent in this order:")
    for argument, rate in ((2, args.rate), (2, -args.rate), (3, args.rate), (3, -args.rate)):
        arg2, arg3 = (rate, 0.0) if argument == 2 else (0.0, rate)
        print(f"  $MTMV0={args.duration:.1f},{arg2:.1f},{arg3:.1f}")
        print(f"  $MTMV0={args.duration:.1f},0.0,0.0  # stop")
    print("MTINI will not be sent.")


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
            "Is the Wireless Unit freshly powered on, are INDI/Ekos and the Android app disconnected, "
            "and have you stopped tracking and verified equipment and cable safety? [yes/N] "
        )
        if answer.strip().lower() != "yes":
            print("Aborted.")
            return 1

    reporter = Reporter(args.log or default_log_path())
    client = WirelessUnitClient(args.host, args.port, args.timeout, reporter)
    results: list[TrialResult] = []
    try:
        initial = client.status(args.retries)
        reporter.emit(f"INITIAL {initial.raw}")
        if initial.status in {2, 3}:
            raise RuntimeError(f"the mount is moving (MTSTA status={initial.status})")
        if initial.mjd >= 10000 and not args.allow_initialized:
            raise RuntimeError(
                f"the mount appears initialized (MJD={initial.mjd:.6f}). "
                "Restart the Wireless Unit and run this test before connecting INDI or Android"
            )

        client.stop(args.duration)
        for argument, rate in ((2, args.rate), (2, -args.rate), (3, args.rate), (3, -args.rate)):
            results.append(
                run_trial(
                    client,
                    reporter,
                    argument=argument,
                    rate=rate,
                    duration=args.duration,
                    settle=args.settle,
                    retries=args.retries,
                    interactive=not args.yes,
                )
            )

        reporter.emit("SUMMARY")
        for result in results:
            reporter.emit(
                f"argument={result.argument}, rate={result.rate:+.1f}, "
                f"observed={result.observed_direction}, {status_delta(result.start, result.end)}"
            )
        reporter.emit("MTINI was not sent. The mount remains uninitialized after this test.")
        reporter.emit(f"log file: {reporter.path.resolve()}")
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        reporter.emit(f"ERROR {error}")
        return 1
    finally:
        try:
            client.stop(args.duration)
        except (OSError, RuntimeError):
            reporter.emit("WARNING: the final stop-command response was not confirmed")
        client.close()
        reporter.close()


if __name__ == "__main__":
    raise SystemExit(main())
