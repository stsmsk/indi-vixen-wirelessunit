# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

import importlib.util
import pathlib


SCRIPT = pathlib.Path(__file__).parents[1] / "scripts" / "check-initial-properties.py"
SPEC = importlib.util.spec_from_file_location("check_initial_properties", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_accepts_initialized_discovery_before_endpoint():
    data = (
        b'<defSwitchVector device="Vixen Wireless Unit" name="WIRELESS_DISCOVERY">'
        b'<defSwitch name="SEARCH">Off</defSwitch></defSwitchVector>'
        b'<defTextVector device="Vixen Wireless Unit" name="WIRELESS_ENDPOINT">'
        b'</defTextVector>'
    )
    MODULE.validate_definitions(data)


def test_rejects_empty_initial_definition():
    data = (
        b'<defSwitchVector device="" name=""><defSwitch name="SEARCH">Off</defSwitch>'
        b'</defSwitchVector>'
        b'<defSwitchVector device="Vixen Wireless Unit" name="WIRELESS_DISCOVERY">'
        b'<defSwitch name="SEARCH">Off</defSwitch></defSwitchVector>'
        b'<defTextVector device="Vixen Wireless Unit" name="WIRELESS_ENDPOINT">'
        b'</defTextVector>'
    )
    try:
        MODULE.validate_definitions(data)
    except ValueError:
        return
    raise AssertionError("empty first vector was accepted")


def test_rejects_discovery_without_search_element():
    data = (
        b'<defSwitchVector device="Vixen Wireless Unit" name="WIRELESS_DISCOVERY">'
        b'</defSwitchVector>'
        b'<defTextVector device="Vixen Wireless Unit" name="WIRELESS_ENDPOINT">'
        b'</defTextVector>'
    )
    try:
        MODULE.validate_definitions(data)
    except ValueError:
        return
    raise AssertionError("empty Discovery vector was accepted")
