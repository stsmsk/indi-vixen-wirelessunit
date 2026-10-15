#!/usr/bin/env bash
# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

set -euo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
driver_name="indi-vixen-wirelessunit"
build_root="${source_dir}/build/deb_indi-3rdparty_${driver_name}"
staged_source="${build_root}/${driver_name}"

for command in cmake dh dpkg-deb fakeroot tar; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "ERROR: required command not found: ${command}" >&2
        exit 1
    fi
done

if [[ ! -r "${source_dir}/debian/${driver_name}/control" ]]; then
    echo "ERROR: package metadata not found: debian/${driver_name}" >&2
    exit 1
fi

# Follow the official indi-3rdparty packaging layout: stage the driver source
# beside a package-specific debian directory, then invoke debian/rules there.
cmake -E remove_directory "${build_root}"
cmake -E make_directory "${staged_source}"

# Generate the source list with anchored find paths. A tar exclusion such as
# './build-*' also matches scripts/build-*.sh with some tar implementations.
# Only root-level build products are excluded here, so release helper scripts
# remain in the staged source.
(
    cd "${source_dir}"
    find . \
        \( -path './.git' -o \
           -path './build' -o \
           -path './build-*' -o \
           -path './debian' -o \
           -path './log' -o \
           -path './outputs' -o \
           -path './work' -o \
           -path '*/__pycache__' \) -prune -o \
        \( -name '.DS_Store' -o \
           -name '._*' -o \
           -name '*.pyc' -o \
           -name '*.log' -o \
           -name '*.tar.gz' \) -prune -o \
        -type f -print0
) | tar -C "${source_dir}" --null -T - -cf - | tar -C "${staged_source}" -xf -

cmake -E copy_directory \
    "${source_dir}/debian/${driver_name}" \
    "${build_root}/debian"

cd "${build_root}"
fakeroot debian/rules binary

shopt -s nullglob
packages=("${source_dir}/build/${driver_name}_"*.deb)
if (( ${#packages[@]} == 0 )); then
    echo "ERROR: package build completed without producing a .deb file" >&2
    exit 1
fi

for package in "${packages[@]}"; do
    dpkg-deb --info "${package}" >/dev/null
done

echo
echo "Debian package build completed."
printf '%s\n' "${packages[@]}"

if command -v lintian >/dev/null 2>&1; then
    lintian "${packages[@]}"
else
    echo "NOTE: install lintian to run Debian policy checks."
fi
