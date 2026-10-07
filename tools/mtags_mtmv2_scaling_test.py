#!/usr/bin/env python3
# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

"""Determine whether MTAGS internally scales MTMV2 motion speed."""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import sys
import time

from mtmv2_duration_test import (
    MountStatus,
    Reporter,
    WirelessUnitClient,
    angular_delta,
    axis_value,
)


@dataclasses.dataclass(frozen=True)
class AutoguideSpeed:
    ra: float
    dec: float


@dataclasses.dataclass(frozen=True)
class ScalingTrial:
    setting: float
    displacement_degrees: float
    restore_error_degrees: float


def parse_mtags(response: str) -> AutoguideSpeed:
    line = response.strip("\r\n")
    if not line.startswith("$MTAGS="):
        raise ValueError(f"response is not MTAGS: {line!r}")
    fields = line[len("$MTAGS=") :].split(",")
    if len(fields) != 2 or fields[0] == "NG":
        raise ValueError(f"invalid MTAGS response: {line!r}")
    try:
        return AutoguideSpeed(float(fields[0]), float(fields[1]))
    except ValueError as error:
        raise ValueError(f"could not parse numeric MTAGS fields: {line!r}") from error


def get_mtags(client: WirelessUnitClient, retries: int) -> AutoguideSpeed:
    return parse_mtags(client.request("$MTAGS?\r\n", retries=retries))


def set_mtags(client: WirelessUnitClient, value: AutoguideSpeed) -> None:
    response = client.request(f"$MTAGS={value.ra:.6f},{value.dec:.6f}\r\n", retries=1)
    applied = parse_mtags(response)
    if abs(applied.ra - value.ra) > 5e-5 or abs(applied.dec - value.dec) > 5e-5:
        raise RuntimeError(
            f"MTAGS setting does not match: requested=({value.ra},{value.dec}), "
            f"response=({applied.ra},{applied.dec})"
        )


def setting_for_axis(original: AutoguideSpeed, axis: str, setting: float) -> AutoguideSpeed:
    if axis == "ra":
        return AutoguideSpeed(setting, original.dec)
    return AutoguideSpeed(original.ra, setting)


def run_trial(
    client: WirelessUnitClient,
    reporter: Reporter,
    *,
    original_mtags: AutoguideSpeed,
    setting: float,
    duration: float,
    move_rate: float,
    axis: str,
    settle: float,
    query_retries: int,
) -> ScalingTrial:
    requested_mtags = setting_for_axis(original_mtags, axis, setting)
    set_mtags(client, requested_mtags)
    confirmed = get_mtags(client, query_retries)
    reporter.emit(f"MTAGS confirmed RA={confirmed.ra:.6f} DEC={confirmed.dec:.6f}")

    start: MountStatus = client.status(query_retries)
    if start.status != 1:
        raise RuntimeError(f"aborting test because MTSTA status={start.status}")
    start_value = axis_value(start, axis)
    reporter.emit(
        f"trial start MTAGS-{axis.upper()}={setting:.6f} MTMV2-rate={move_rate:.6f} "
        f"MechX={start.mechanical_x:.6f} MechY={start.mechanical_y:.6f}"
    )

    ra_rate, dec_rate = (move_rate, 0.0) if axis == "ra" else (0.0, move_rate)
    client.move(duration, ra_rate, dec_rate)
    time.sleep(duration + settle)
    end = client.status(query_retries)

    restore_ra, restore_dec = (-move_rate, 0.0) if axis == "ra" else (0.0, -move_rate)
    client.move(duration, restore_ra, restore_dec)
    time.sleep(duration + settle)
    restored = client.status(query_retries)

    displacement = angular_delta(axis_value(end, axis), start_value)
    restore_error = angular_delta(axis_value(restored, axis), start_value)
    reporter.emit(
        f"trial result MTAGS={setting:.6f}: displacement={displacement:+.8f} deg "
        f"({displacement * 3600.0:+.3f} arcsec), "
        f"restore error={restore_error:+.8f} deg ({restore_error * 3600.0:+.3f} arcsec)"
    )
    return ScalingTrial(setting, displacement, restore_error)


def classify_scaling(first: ScalingTrial, second: ScalingTrial) -> str:
    first_delta = abs(first.displacement_degrees)
    second_delta = abs(second.displacement_degrees)
    if first_delta < 1e-5:
        return "inconclusive: the first displacement is too small"
    measured_ratio = second_delta / first_delta
    setting_ratio = abs(second.setting / first.setting) if first.setting else float("inf")
    if 0.85 <= measured_ratio <= 1.15:
        return "MTAGS is not additionally multiplied into the MTMV2 rate"
    if 0.75 * setting_ratio <= measured_ratio <= 1.25 * setting_ratio:
        return "results are consistent with MTAGS being additionally multiplied into the MTMV2 rate"
    return "results match neither prediction; further verification is required"


def print_preview(args: argparse.Namespace) -> None:
    ra_rate, dec_rate = (args.move_rate, 0.0) if args.axis == "ra" else (0.0, args.move_rate)
    restore_ra, restore_dec = (-args.move_rate, 0.0) if args.axis == "ra" else (0.0, -args.move_rate)
    print("Nothing was sent to the mount. With --execute, the current MTAGS is saved and these steps run:")
    for setting in (args.first_setting, args.second_setting):
        print(f"  Set the MTAGS {args.axis.upper()} axis to {setting:.6f} (preserve the other axis)")
        print(f"  $MTMV2={args.duration:.2f},{ra_rate:.6f},{dec_rate:.6f}")
        print(f"  $MTMV2={args.duration:.2f},{restore_ra:.6f},{restore_dec:.6f}  # restore")
    print("  Finally restore the original MTAGS RA/DEC values")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Measure whether MTAGS additionally scales MTMV2 speed")
    parser.add_argument("--host", default="10.42.0.60")
    parser.add_argument("--port", type=int, default=60023)
    parser.add_argument("--axis", choices=("ra", "dec"), default="ra")
    parser.add_argument("--first-setting", type=float, default=1.0)
    parser.add_argument("--second-setting", type=float, default=2.0)
    parser.add_argument("--duration", type=float, default=0.10)
    parser.add_argument("--move-rate", type=float, default=10.0)
    parser.add_argument("--settle", type=float, default=0.30)
    parser.add_argument("--timeout", type=float, default=1.0)
    parser.add_argument("--query-retries", type=int, default=3)
    parser.add_argument("--log", type=pathlib.Path)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--yes", action="store_true", help="skip the interactive safety confirmation")
    return parser


def validate_args(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if not (0 < args.port <= 65535):
        parser.error("--port must be between 1 and 65535")
    if not (0.1 <= args.first_setting <= 10.0 and 0.1 <= args.second_setting <= 10.0):
        parser.error("MTAGS settings must be between 0.1 and 10.0")
    if args.first_setting == args.second_setting:
        parser.error("the two MTAGS settings must differ")
    if args.duration <= 0 or args.move_rate == 0 or args.settle < 0:
        parser.error("duration, motion rate, or settling time is invalid")
    if args.timeout <= 0 or args.query_retries < 1:
        parser.error("timeout or retry count is invalid")


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
    original_mtags: AutoguideSpeed | None = None
    exit_code = 0
    try:
        initial_status = client.status(args.query_retries)
        if initial_status.status != 1:
            raise RuntimeError(
                f"aborting because MTSTA status={initial_status.status}; stopped status 1 is required"
            )
        original_mtags = get_mtags(client, args.query_retries)
        reporter.emit(
            f"saved pre-test MTAGS: RA={original_mtags.ra:.6f} DEC={original_mtags.dec:.6f}"
        )

        first = run_trial(
            client,
            reporter,
            original_mtags=original_mtags,
            setting=args.first_setting,
            duration=args.duration,
            move_rate=args.move_rate,
            axis=args.axis,
            settle=args.settle,
            query_retries=args.query_retries,
        )
        second = run_trial(
            client,
            reporter,
            original_mtags=original_mtags,
            setting=args.second_setting,
            duration=args.duration,
            move_rate=args.move_rate,
            axis=args.axis,
            settle=args.settle,
            query_retries=args.query_retries,
        )
        first_delta = abs(first.displacement_degrees)
        second_delta = abs(second.displacement_degrees)
        ratio = second_delta / first_delta if first_delta else float("inf")
        reporter.emit(
            f"comparison: MTAGS {first.setting:.6f}->{second.setting:.6f}, "
            f"displacement ratio={ratio:.3f}, MTAGS ratio={second.setting / first.setting:.3f}"
        )
        reporter.emit(f"classification: {classify_scaling(first, second)}")
    except KeyboardInterrupt:
        reporter.emit("interrupt detected; sending the stop command")
        client.emergency_stop()
        exit_code = 130
    except (OSError, ValueError, RuntimeError) as error:
        reporter.emit(f"error: {error}")
        reporter.emit("sending the stop command for safety")
        client.emergency_stop()
        exit_code = 2
    finally:
        if original_mtags is not None:
            try:
                set_mtags(client, original_mtags)
                confirmed = get_mtags(client, args.query_retries)
                reporter.emit(
                    f"restored pre-test MTAGS values: RA={confirmed.ra:.6f} DEC={confirmed.dec:.6f}"
                )
            except (OSError, ValueError, RuntimeError) as error:
                reporter.emit(f"CRITICAL: could not restore the pre-test MTAGS values: {error}")
                exit_code = 3
        client.close()
        reporter.close()
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
