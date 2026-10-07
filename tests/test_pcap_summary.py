# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

import importlib.util
import pathlib


MODULE_PATH = pathlib.Path(__file__).parents[1] / "tools" / "pcap_summary.py"
SPEC = importlib.util.spec_from_file_location("pcap_summary", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_sha256(tmp_path):
    sample = tmp_path / "sample.bin"
    sample.write_bytes(b"Vixen Wireless Unit\n")
    assert MODULE.sha256(sample) == (
        "04575f1c4f755f8487db66bcda4827fd3d40fd850d8a815758388881de79ae93"
    )
