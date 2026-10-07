# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

import importlib.util
import pathlib
import sys


MODULE_PATH = pathlib.Path(__file__).parents[1] / "tools" / "mtmv2_duration_test.py"
SPEC = importlib.util.spec_from_file_location("mtmv2_duration_test", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def status(x):
    return MODULE.MountStatus("test", 1, x, 0.0)


def result(duration, displacement):
    return MODULE.TrialResult(duration, status(0), status(displacement), status(0), displacement, 0)


def test_parse_mtsta_extracts_mechanical_coordinates():
    parsed = MODULE.parse_mtsta(
        "$MTSTA=61280.271006,1,0,286,107.968117,153.615083,270.005810,"
        "-0.003988,8.269868,0.002233,2,--\r\n"
    )
    assert parsed.status == 1
    assert parsed.mechanical_x == 107.968117
    assert parsed.mechanical_y == 153.615083


def test_angular_delta_handles_wraparound():
    assert abs(MODULE.angular_delta(0.01, 359.99) - 0.02) < 1e-10


def test_classification_accepts_duration_proportional_motion():
    assert MODULE.classify_duration_unit(result(0.1, 0.004), result(1.0, 0.04)) == "consistent with seconds"


def test_classification_rejects_non_proportional_motion():
    assert "inconsistent" in MODULE.classify_duration_unit(result(0.1, 0.004), result(1.0, 0.005))


def test_move_rejects_same_command_with_different_echoed_value():
    client = MODULE.WirelessUnitClient.__new__(MODULE.WirelessUnitClient)
    client.request = lambda command, retries=1: "$MTMV2=0.10,2.000000,0.000000"
    try:
        client.move(0.1, 1.0, 0.0)
    except RuntimeError:
        return
    raise AssertionError("a stale MTMV2 acknowledgement was accepted")
