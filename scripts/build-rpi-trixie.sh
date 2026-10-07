#!/usr/bin/env bash
# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

set -euo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${source_dir}/build-rpi"
install_prefix="${CMAKE_INSTALL_PREFIX:-/usr}"

cmake -S "${source_dir}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${install_prefix}" \
    -DVIXEN_BUILD_INDI_DRIVER=ON \
    -DBUILD_TESTING=ON

cmake --build "${build_dir}" --target indi_vixen_wirelessunit --parallel
cmake --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" --output-on-failure

if [[ ! -x "${build_dir}/indi_vixen_wirelessunit" || ! -f "${build_dir}/indi_vixen_wirelessunit.xml" ]]; then
    echo "ERROR: INDI driver or its XML metadata was not generated" >&2
    exit 1
fi

echo
echo "INDI driver, XML metadata, and tests completed."
echo "Install with: sudo cmake --install ${build_dir}"
