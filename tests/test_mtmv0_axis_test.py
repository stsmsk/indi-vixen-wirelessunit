# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

import importlib.util
import pathlib
import sys


MODULE_PATH = pathlib.Path(__file__).parents[1] / "tools" / "mtmv0_axis_test.py"
SPEC = importlib.util.spec_from_file_location("mtmv0_axis_test", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def test_parse_uninitialized_mtsta():
    parsed = MODULE.parse_mtsta(
        "$MTSTA=0.000442,1,0,-1,90.000000,0.000000,270.000000,0.000000,"
        "7.061458,-0.000000,2,--\r\n"
    )
    assert parsed.mjd == 0.000442
    assert parsed.unknown_3 == "-1"
    assert parsed.mechanical_x == 90.0
    assert parsed.target_name == "--"


def test_status_delta_reports_each_axis():
    start = MODULE.parse_mtsta(
        "$MTSTA=0.000442,1,0,-1,90.000000,0.000000,270.000000,0.000000,"
        "7.061458,0.000000,2,--\r\n"
    )
    end = MODULE.parse_mtsta(
        "$MTSTA=0.000450,1,0,-1,90.416567,0.000000,270.251009,-0.332450,"
        "7.034884,0.000000,2,--\r\n"
    )
    summary = MODULE.status_delta(start, end)
    assert "MechX=+0.416567 deg" in summary
    assert "MechY=+0.000000 deg" in summary


def test_move_rejects_same_command_with_different_echoed_value():
    client = MODULE.WirelessUnitClient.__new__(MODULE.WirelessUnitClient)
    client.request = lambda command, retries=1: "$MTMV0=1.0,99.0,-30.0"
    try:
        client.move(1.0, 100.0, -30.0)
    except RuntimeError:
        return
    raise AssertionError("a stale MTMV0 acknowledgement was accepted")
