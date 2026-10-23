#!/usr/bin/env bash
# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

set -euo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
remote_host="trixie.local"
remote_base="build/codex/indi-vixen-wirelessunit-runs"
identity_file="${HOME}/.ssh/id_ed25519_codex_trixie"
requested_local_log_dir=""
sudo_mode="auto"
dry_run=false
remote_options=()

usage()
{
    cat <<'EOF'
Usage: ./scripts/remote-deploy-and-capture-rpi.sh [options] [-- PI-OPTIONS]

Copy the current source tree to a new directory on trixie.local, build and
install the Debian package there, run the INDI server with packet capture,
then copy the resulting logs back to this computer.

Options:
  --host [USER@]HOST       SSH destination (default: trixie.local)
  --remote-base DIRECTORY  new run directories are created below this path
                           relative to the remote home directory
  --identity-file FILE     SSH private key (default: dedicated trixie key when present)
  --local-log-dir DIR      retrieved-log directory (default: log/remote-TIME)
  --allow-sudo-prompt      force an interactive remote sudo password prompt
  --non-interactive        never prompt; require passwordless/cached remote sudo
  --dry-run                print the selected paths without transferring files
  -h, --help               show this help

Arguments after -- are passed to deploy-and-capture-rpi.sh on the Pi. Example:

  ./scripts/remote-deploy-and-capture-rpi.sh \
      --host masuko@trixie.local -- --interface wlan1 --duration 300

When this command is run from a terminal, it automatically allocates a remote
TTY so the Raspberry Pi sudo password can be entered. Password input is not
echoed. Non-interactive execution requires passwordless or cached remote sudo.
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

safe_relative_remote_path()
{
    [[ "$1" =~ ^[A-Za-z0-9._/-]+$ ]] &&
        [[ "$1" != /* ]] &&
        [[ "/$1/" != *"/../"* ]] &&
        [[ "/$1/" != *"/./"* ]]
}

while (( $# > 0 )); do
    case "$1" in
        --host)
            (( $# >= 2 )) || die "--host requires a value"
            remote_host="$2"
            shift 2
            ;;
        --remote-base)
            (( $# >= 2 )) || die "--remote-base requires a value"
            remote_base="$2"
            shift 2
            ;;
        --local-log-dir)
            (( $# >= 2 )) || die "--local-log-dir requires a value"
            requested_local_log_dir="$2"
            shift 2
            ;;
        --identity-file)
            (( $# >= 2 )) || die "--identity-file requires a value"
            identity_file="$2"
            shift 2
            ;;
        --allow-sudo-prompt)
            sudo_mode="prompt"
            shift
            ;;
        --non-interactive)
            sudo_mode="non-interactive"
            shift
            ;;
        --dry-run)
            dry_run=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            remote_options=("$@")
            break
            ;;
        *)
            die "unknown wrapper option: $1 (put Pi options after --)"
            ;;
    esac
done

for command in date rsync ssh; do
    require_command "${command}"
done
[[ -n "${remote_host}" ]] || die "SSH host must not be empty"
safe_relative_remote_path "${remote_base}" || \
    die "--remote-base must be a safe path relative to the remote home directory"

timestamp="$(date +%Y%m%d-%H%M%S)"
remote_run="${remote_base}/run-${timestamp}"
remote_source="${remote_run}/source"
remote_logs="${remote_run}/logs"

if [[ -n "${requested_local_log_dir}" ]]; then
    if [[ "${requested_local_log_dir}" = /* ]]; then
        local_log_dir="${requested_local_log_dir}"
    else
        local_log_dir="${source_dir}/${requested_local_log_dir}"
    fi
else
    local_log_dir="${source_dir}/log/remote-${timestamp}"
fi

echo "Vixen Wireless Unit remote deployment"
echo "SSH destination : ${remote_host}"
echo "Remote source   : ${remote_source}"
echo "Remote logs     : ${remote_logs}"
echo "Retrieved logs  : ${local_log_dir}"

if ${dry_run}; then
    echo "Dry run: no files were transferred and no remote command was run."
    exit 0
fi

if [[ "${sudo_mode}" == "auto" ]]; then
    if [[ -t 0 && -t 1 ]]; then
        sudo_mode="prompt"
    else
        sudo_mode="non-interactive"
    fi
fi

ssh_check_options=(-o BatchMode=yes -o ConnectTimeout=10)
ssh_run_options=(-o BatchMode=yes -o ConnectTimeout=10)
rsync_shell="ssh -o BatchMode=yes -o ConnectTimeout=10"
if [[ "${sudo_mode}" == "prompt" ]]; then
    ssh_run_options+=(-tt)
fi
if [[ -n "${identity_file}" && -f "${identity_file}" ]]; then
    ssh_check_options+=(-i "${identity_file}" -o IdentitiesOnly=yes)
    ssh_run_options+=(-i "${identity_file}" -o IdentitiesOnly=yes)
    printf -v quoted_identity '%q' "${identity_file}"
    rsync_shell+=" -i ${quoted_identity} -o IdentitiesOnly=yes"
elif [[ -n "${identity_file}" && "${identity_file}" != "${HOME}/.ssh/id_ed25519_codex_trixie" ]]; then
    die "SSH identity file was not found: ${identity_file}"
fi

echo
echo "Checking SSH access and remote prerequisites..."
ssh "${ssh_check_options[@]}" "${remote_host}" \
    "command -v bash >/dev/null && command -v rsync >/dev/null && mkdir -p '${remote_source}' '${remote_logs}'" || \
    die "SSH access or a remote prerequisite failed"

if [[ "${sudo_mode}" == "non-interactive" ]]; then
    ssh "${ssh_check_options[@]}" "${remote_host}" "sudo -n true" || \
        die "remote sudo needs a password, but no interactive terminal is available; run this command directly in a terminal or configure passwordless sudo"
fi

echo "Transferring the current source tree..."
rsync --archive --compress -e "${rsync_shell}" \
    --exclude='/.git/' \
    --exclude='/build/' \
    --exclude='/build-*' \
    --exclude='/log/' \
    --exclude='/outputs/' \
    --exclude='/work/' \
    --exclude='__pycache__/' \
    --exclude='.DS_Store' \
    --exclude='._*' \
    --exclude='*.pyc' \
    --exclude='*.log' \
    --exclude='*.deb' \
    --exclude='*.tar.gz' \
    "${source_dir}/" "${remote_host}:${remote_source}/"

printf -v quoted_source '%q' "${remote_source}"
remote_command="cd ${quoted_source} && ./scripts/deploy-and-capture-rpi.sh --log-dir ../logs"
for option in "${remote_options[@]}"; do
    printf -v quoted_option '%q' "${option}"
    remote_command+=" ${quoted_option}"
done

echo
echo "Starting the build, installation, INDI server, and log capture on the Pi..."
if [[ "${sudo_mode}" == "prompt" ]]; then
    echo "If sudo authorization is required, enter the Raspberry Pi password below."
    echo "The password is accepted even though no characters are displayed while typing."
fi
echo "Press Ctrl+C after the test; the logs will then be retrieved automatically."
set +e
ssh "${ssh_run_options[@]}" "${remote_host}" "bash -lc $(printf '%q' "${remote_command}")"
remote_status=$?
set -e

mkdir -p "${local_log_dir}"
echo
echo "Retrieving logs from the Pi..."
if rsync --archive --compress -e "${rsync_shell}" \
    "${remote_host}:${remote_logs}/" "${local_log_dir}/"; then
    echo "Logs retrieved to: ${local_log_dir}"
else
    echo "WARNING: log retrieval failed; remote files remain in ${remote_host}:${remote_logs}" >&2
fi

if (( remote_status != 0 && remote_status != 129 && remote_status != 130 )); then
    die "remote deployment exited with status ${remote_status}"
fi

echo "Remote run directory retained at: ${remote_host}:${remote_run}"
