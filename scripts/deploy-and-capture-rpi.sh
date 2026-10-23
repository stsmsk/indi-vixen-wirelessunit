#!/usr/bin/env bash
# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

set -euo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
driver_package="indi-vixen-wirelessunit"
driver_executable="indi_vixen_wirelessunit"
device_name="Vixen Wireless Unit"
server_port=7624
wireless_port=60023
capture_interface="any"
duration_seconds=0
build_package=true
install_package=true
capture_packets=true
enable_driver_debug=true
requested_log_dir=""

usage()
{
    cat <<'EOF'
Usage: ./scripts/deploy-and-capture-rpi.sh [options]

Build and install the current Debian package, then run indiserver while
capturing its log and Vixen Wireless Unit UDP traffic. Press Ctrl+C to stop.

Options:
  --interface NAME       tcpdump interface (default: any)
  --server-port PORT     INDI server port (default: 7624)
  --wireless-port PORT   Wireless Unit UDP port (default: 60023)
  --duration SECONDS     stop automatically; 0 waits for Ctrl+C (default: 0)
  --log-dir DIRECTORY    output directory (default: log/run-TIMESTAMP)
  --skip-build           install an existing current-version package
  --skip-install         run the already installed driver
  --no-packet-capture    do not run tcpdump
  --no-driver-debug      do not enable the INDI Debug property at startup
  -h, --help             show this help

The script never stops an existing INDI server. It exits if the selected
server port is already in use.
EOF
}

die()
{
    echo "ERROR: $*" >&2
    exit 1
}

require_command()
{
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

authorize_sudo()
{
    if sudo -n true 2>/dev/null; then
        return
    fi

    [[ -t 0 ]] || \
        die "sudo needs a password, but no interactive terminal is available"

    echo
    echo "Raspberry Pi sudo authorization is required."
    echo "Enter the password for $(id -un) on $(hostname)."
    echo "No characters are displayed while the password is being entered."
    sudo -v
}

valid_port()
{
    [[ "$1" =~ ^[0-9]+$ ]] && (( $1 >= 1 && $1 <= 65535 ))
}

while (( $# > 0 )); do
    case "$1" in
        --interface)
            (( $# >= 2 )) || die "--interface requires a value"
            capture_interface="$2"
            shift 2
            ;;
        --server-port)
            (( $# >= 2 )) || die "--server-port requires a value"
            server_port="$2"
            shift 2
            ;;
        --wireless-port)
            (( $# >= 2 )) || die "--wireless-port requires a value"
            wireless_port="$2"
            shift 2
            ;;
        --duration)
            (( $# >= 2 )) || die "--duration requires a value"
            duration_seconds="$2"
            shift 2
            ;;
        --log-dir)
            (( $# >= 2 )) || die "--log-dir requires a value"
            requested_log_dir="$2"
            shift 2
            ;;
        --skip-build)
            build_package=false
            shift
            ;;
        --skip-install)
            install_package=false
            shift
            ;;
        --no-packet-capture)
            capture_packets=false
            shift
            ;;
        --no-driver-debug)
            enable_driver_debug=false
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

valid_port "${server_port}" || die "invalid INDI server port: ${server_port}"
valid_port "${wireless_port}" || die "invalid Wireless Unit port: ${wireless_port}"
[[ "${duration_seconds}" =~ ^[0-9]+$ ]] || die "duration must be zero or a positive integer"
[[ -n "${capture_interface}" ]] || die "capture interface must not be empty"

for command in awk date dpkg-query indiserver sed ss stdbuf; do
    require_command "${command}"
done
if ${install_package} || ${capture_packets}; then
    require_command sudo
fi
if ${install_package}; then
    require_command apt
    require_command dpkg
fi
if ${capture_packets}; then
    require_command tcpdump
fi
if ${enable_driver_debug}; then
    require_command indi_setprop
fi

project_version="$({
    sed -nE 's/^project\(indi_vixen_wireless VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' \
        "${source_dir}/CMakeLists.txt"
} | head -n 1)"
[[ -n "${project_version}" ]] || die "could not read the project version from CMakeLists.txt"

timestamp="$(date +%Y%m%d-%H%M%S)"
if [[ -n "${requested_log_dir}" ]]; then
    if [[ "${requested_log_dir}" = /* ]]; then
        log_dir="${requested_log_dir}"
    else
        log_dir="${source_dir}/${requested_log_dir}"
    fi
else
    log_dir="${source_dir}/log/run-${timestamp}"
fi
mkdir -p "${log_dir}"
log_dir="$(cd "${log_dir}" && pwd)"

setup_log="${log_dir}/setup.log"
indi_log="${log_dir}/vixen-indi.log"
packet_pcap="${log_dir}/vixen-packet.pcap"
packet_log="${log_dir}/vixen-packet.log"
before_log="${log_dir}/system-before.log"
after_log="${log_dir}/system-after.log"

exec > >(tee -a "${setup_log}") 2>&1

echo "Vixen Wireless Unit deployment and capture"
echo "Project version : ${project_version}"
echo "Source directory: ${source_dir}"
echo "Log directory   : ${log_dir}"

if ss -H -ltn "sport = :${server_port}" | awk 'NF { found=1 } END { exit !found }'; then
    die "TCP port ${server_port} is already in use; stop the existing server or select another port"
fi

if ${install_package} || ${capture_packets}; then
    authorize_sudo
fi

if ${build_package}; then
    echo
    echo "Building and testing the Debian package..."
    "${source_dir}/scripts/build-deb-trixie.sh"
fi

architecture="$(dpkg --print-architecture 2>/dev/null || true)"
if ${install_package}; then
    [[ -n "${architecture}" ]] || die "could not determine the Debian architecture"
    package_pattern="${source_dir}/build/${driver_package}_${project_version}-"'*_'
    package_pattern+="${architecture}.deb"
    mapfile -t packages < <(compgen -G "${package_pattern}" | sort -V)
    (( ${#packages[@]} > 0 )) || die "current package was not found: ${package_pattern}"
    package="${packages[${#packages[@]} - 1]}"

    echo
    echo "Installing ${package}..."
    # Development builds can legitimately reuse the project version while
    # source changes are being tested. Force installation so apt never keeps
    # an older binary merely because its package version is identical.
    sudo apt install -y --reinstall "${package}"
fi

installed_status="$(dpkg-query -W -f='${Status}' "${driver_package}" 2>/dev/null || true)"
installed_version="$(dpkg-query -W -f='${Version}' "${driver_package}" 2>/dev/null || true)"
[[ "${installed_status}" == "install ok installed" ]] || die "${driver_package} is not installed"
[[ "${installed_version}" == "${project_version}-"* ]] || \
    die "installed version ${installed_version} does not match project version ${project_version}"

driver_path="$(command -v "${driver_executable}" || true)"
[[ -n "${driver_path}" && -x "${driver_path}" ]] || die "installed driver is not executable"

capture_system_state()
{
    local output_file="$1"
    {
        echo "Captured: $(date --iso-8601=seconds)"
        echo "Driver: ${driver_path}"
        echo "Package: ${driver_package} ${installed_version}"
        echo
        uname -a
        echo
        ip -br -4 address 2>/dev/null || true
        echo
        ip -4 route 2>/dev/null || true
        echo
        nstat -az 2>/dev/null || true
        if [[ "${capture_interface}" != "any" ]] && command -v iw >/dev/null 2>&1; then
            echo
            sudo iw dev "${capture_interface}" link 2>/dev/null || true
            echo
            sudo iw dev "${capture_interface}" station dump 2>/dev/null || true
        fi
    } >"${output_file}" 2>&1
}

capture_system_state "${before_log}"

indiserver_pid=""
tcpdump_pid=""
cleanup_complete=false

cleanup()
{
    local exit_status=$?
    if ${cleanup_complete}; then
        return
    fi
    cleanup_complete=true
trap - EXIT HUP INT TERM
    set +e

    echo
    echo "Stopping capture session..."
    if [[ -n "${indiserver_pid}" ]] && kill -0 "${indiserver_pid}" 2>/dev/null; then
        kill -TERM "${indiserver_pid}" 2>/dev/null
        wait "${indiserver_pid}" 2>/dev/null
    fi
    if [[ -n "${tcpdump_pid}" ]] && sudo kill -0 "${tcpdump_pid}" 2>/dev/null; then
        sudo kill -INT "${tcpdump_pid}" 2>/dev/null
        wait "${tcpdump_pid}" 2>/dev/null
    fi

    capture_system_state "${after_log}"
    if ${capture_packets} && [[ -s "${packet_pcap}" ]]; then
        sudo tcpdump -nn -tttt -r "${packet_pcap}" -A >"${packet_log}" 2>&1
        sudo chmod a+r "${packet_pcap}" 2>/dev/null
    elif ${capture_packets}; then
        echo "WARNING: no packets were captured" | tee "${packet_log}"
    fi

    echo "Logs saved in: ${log_dir}"
    echo "  INDI server : ${indi_log}"
    if ${capture_packets}; then
        echo "  UDP packets : ${packet_log}"
        echo "  Raw capture : ${packet_pcap}"
    fi
    echo "  Setup       : ${setup_log}"
    exit "${exit_status}"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT TERM

if ${capture_packets}; then
    echo
    echo "Starting packet capture on ${capture_interface}, UDP port ${wireless_port}..."
    sudo tcpdump -ni "${capture_interface}" -U -s 0 -w "${packet_pcap}" \
        "udp port ${wireless_port}" >/dev/null 2>&1 &
    tcpdump_pid=$!
    sleep 1
    sudo kill -0 "${tcpdump_pid}" 2>/dev/null || die "tcpdump exited before capture began"
fi

echo "Starting INDI server on TCP port ${server_port}..."
stdbuf -oL -eL indiserver -vv -p "${server_port}" "${driver_path}" \
    >"${indi_log}" 2>&1 &
indiserver_pid=$!

server_ready=false
for _ in {1..20}; do
    if ! kill -0 "${indiserver_pid}" 2>/dev/null; then
        break
    fi
    if ss -H -ltn "sport = :${server_port}" | awk 'NF { found=1 } END { exit !found }'; then
        server_ready=true
        break
    fi
    sleep 0.25
done
${server_ready} || die "indiserver did not start; inspect ${indi_log}"

if ${enable_driver_debug}; then
    if indi_setprop -h 127.0.0.1 -p "${server_port}" \
        "${device_name}.DEBUG.ENABLE=On" >/dev/null 2>&1; then
        echo "Driver debug logging enabled."
    else
        echo "WARNING: the driver started, but its Debug property could not be enabled."
    fi
fi

echo
echo "The test server is ready."
echo "Configure KStars/Ekos to use this Raspberry Pi and INDI port ${server_port}."
if (( duration_seconds > 0 )); then
    echo "The session will stop after ${duration_seconds} seconds."
else
    echo "Run the test, then press Ctrl+C here to stop and finalize the logs."
fi

start_epoch="$(date +%s)"
while kill -0 "${indiserver_pid}" 2>/dev/null; do
    if (( duration_seconds > 0 && $(date +%s) - start_epoch >= duration_seconds )); then
        break
    fi
    sleep 1
done

if ! kill -0 "${indiserver_pid}" 2>/dev/null; then
    wait "${indiserver_pid}" || server_status=$?
    die "indiserver exited unexpectedly with status ${server_status:-0}; inspect ${indi_log}"
fi
