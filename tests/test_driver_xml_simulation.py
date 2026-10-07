# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

"""Offline checks for the Pi-only indiserver XML integration harness."""

import driver_xml_simulation_test as harness
from unittest.mock import patch


def test_find_vectors_handles_attribute_quote_styles():
    data = (
        b'<defSwitchVector device="Vixen Wireless Unit" name="SIMULATION">'
        b'<defSwitch name="ENABLE">Off</defSwitch></defSwitchVector>\n'
        b"<setSwitchVector device='Vixen Wireless Unit' name='SIMULATION' state='Ok'>"
        b"<oneSwitch name='ENABLE'>On</oneSwitch></setSwitchVector>"
    )
    defined = harness.find_vectors(data, "defSwitchVector", "SIMULATION")
    updated = harness.find_vectors(data, "setSwitchVector", "SIMULATION")
    assert len(defined) == len(updated) == 1
    assert harness.element_value(defined[0], "ENABLE") == "Off"
    assert harness.element_value(updated[0], "ENABLE") == "On"


def test_find_vectors_ignores_partial_and_other_device():
    data = (
        b'<setNumberVector device="Other" name="WIRELESS_BACKLASH">'
        b'<oneNumber name="BACKLASH_RA">2</oneNumber></setNumberVector>'
        b'<setNumberVector device="Vixen Wireless Unit" name="WIRELESS_BACKLASH">'
        b'<oneNumber name="BACKLASH_RA">0'
    )
    assert harness.find_vectors(data, "setNumberVector", "WIRELESS_BACKLASH") == []


def test_find_del_property_accepts_single_quotes():
    data = b"<delProperty device='Vixen Wireless Unit' name='WIRELESS_SET_HOME_POSITION'/>"
    item = harness.find_del_property(data, "WIRELESS_SET_HOME_POSITION")
    assert item is not None
    assert item.attrib["name"] == "WIRELESS_SET_HOME_POSITION"


def test_new_vector_payloads():
    switch = harness.switch_xml("SIMULATION", {"ENABLE": "On"})
    number = harness.number_xml("WIRELESS_GUIDE_RATE", {"GUIDE_RATE_WE": "2.0"})
    assert '<newSwitchVector device="Vixen Wireless Unit" name="SIMULATION">' in switch
    assert '<oneSwitch name="ENABLE">On</oneSwitch>' in switch
    assert '<newNumberVector device="Vixen Wireless Unit" name="WIRELESS_GUIDE_RATE">' in number
    assert '<oneNumber name="GUIDE_RATE_WE">2.0</oneNumber>' in number


def test_wait_vector_timeout_reports_observed_state():
    peer = harness.IndiPeer(None)
    peer.buffer.extend(
        b'<setSwitchVector device="Vixen Wireless Unit" name="CONNECTION" state="Idle">'
        b'<oneSwitch name="DISCONNECT">On</oneSwitch></setSwitchVector>'
    )
    with patch.object(peer, "wait_for", side_effect=TimeoutError("timed out")):
        try:
            peer.wait_vector("setSwitchVector", "CONNECTION", predicate=lambda vector: False)
        except TimeoutError as error:
            assert "observed setSwitchVector states: ['Idle']" in str(error)
        else:
            assert False, "expected a diagnostic timeout"
