# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

import importlib.util
import pathlib
import sys


TOOLS = pathlib.Path(__file__).parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
MODULE_PATH = TOOLS / "mtags_mtmv2_scaling_test.py"
SPEC = importlib.util.spec_from_file_location("mtags_mtmv2_scaling_test", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def trial(setting, displacement):
    return MODULE.ScalingTrial(setting, displacement, 0.0)


def test_parse_mtags():
    value = MODULE.parse_mtags("$MTAGS=2.0,3.1\r\n")
    assert value.ra == 2.0
    assert value.dec == 3.1


def test_setting_for_axis_preserves_other_axis():
    original = MODULE.AutoguideSpeed(1.2, 3.4)
    assert MODULE.setting_for_axis(original, "ra", 2.0) == MODULE.AutoguideSpeed(2.0, 3.4)
    assert MODULE.setting_for_axis(original, "dec", 2.0) == MODULE.AutoguideSpeed(1.2, 2.0)


def test_classifies_no_additional_scaling():
    assert "not additionally multiplied" in MODULE.classify_scaling(trial(1.0, 0.0041), trial(2.0, 0.0042))


def test_classifies_additional_scaling():
    assert "additionally multiplied" in MODULE.classify_scaling(trial(1.0, 0.0041), trial(2.0, 0.0082))
