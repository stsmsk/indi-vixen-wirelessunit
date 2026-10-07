#!/usr/bin/env python3
# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

"""Run the repository's small Python tests without an optional pytest install.

CTest invokes this for each test_*.py file. Unknown fixture signatures and an
empty file are errors, so a green CTest cannot silently omit Python tests.
"""

from __future__ import annotations

import argparse
import importlib.util
import inspect
import pathlib
import sys
import tempfile
import traceback


def run_file(path: pathlib.Path) -> int:
    if not path.is_file():
        raise ValueError(f"Python test file does not exist: {path}")
    module_name = f"vixen_test_{path.stem}"
    specification = importlib.util.spec_from_file_location(module_name, path)
    if specification is None or specification.loader is None:
        raise ValueError(f"Cannot import Python tests from: {path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[module_name] = module
    specification.loader.exec_module(module)

    tests = [
        (name, value)
        for name, value in vars(module).items()
        if name.startswith("test_") and inspect.isfunction(value)
    ]
    if not tests:
        raise ValueError(f"No test_ functions found in: {path}")

    failures = 0
    for name, function in sorted(tests):
        try:
            parameters = list(inspect.signature(function).parameters)
            if not parameters:
                function()
            elif parameters == ["tmp_path"]:
                with tempfile.TemporaryDirectory(prefix="vixen-python-test-") as temp:
                    function(pathlib.Path(temp))
            else:
                raise ValueError(f"unsupported test fixture parameters: {parameters}")
        except Exception:
            failures += 1
            print(f"FAIL {path.name}::{name}", file=sys.stderr)
            traceback.print_exc()
        else:
            print(f"PASS {path.name}::{name}")

    print(f"{path.name}: {len(tests) - failures}/{len(tests)} passed")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test_file", type=pathlib.Path)
    args = parser.parse_args()
    try:
        return run_file(args.test_file.resolve())
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
