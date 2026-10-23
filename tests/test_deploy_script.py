# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

"""Safety and reproducibility checks for the Pi deployment workflow."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "deploy-and-capture-rpi.sh"
REMOTE_SCRIPT = ROOT / "scripts" / "remote-deploy-and-capture-rpi.sh"


def script_text():
    return SCRIPT.read_text(encoding="utf-8")


def test_deployment_script_builds_installs_and_captures_logs():
    text = script_text()
    assert '"${source_dir}/scripts/build-deb-trixie.sh"' in text
    assert 'sudo apt install -y --reinstall "${package}"' in text
    assert 'installed_status="$(dpkg-query -W -f=\'${Status}\'' in text
    assert 'installed_version="$(dpkg-query -W -f=\'${Version}\'' in text
    assert '"install ok installed"' in text
    assert 'indiserver -vv -p "${server_port}"' in text
    assert '"udp port ${wireless_port}"' in text
    assert 'vixen-indi.log' in text
    assert 'vixen-packet.pcap' in text
    assert 'vixen-packet.log' in text
    assert 'system-before.log' in text
    assert 'system-after.log' in text


def test_deployment_script_does_not_stop_an_existing_server():
    text = script_text()
    assert "TCP port ${server_port} is already in use" in text
    assert "pkill" not in text
    assert "killall" not in text
    assert 'kill -TERM "${indiserver_pid}"' in text
    assert 'sudo kill -INT "${tcpdump_pid}"' in text


def test_deployment_script_documents_interactive_and_bounded_runs():
    text = script_text()
    assert "Press Ctrl+C to stop" in text
    assert "--duration SECONDS" in text
    assert "--skip-build" in text
    assert "--skip-install" in text
    assert "--no-packet-capture" in text
    assert 'authorize_sudo' in text
    assert '[[ -t 0 ]]' in text
    assert "No characters are displayed while the password is being entered." in text
    assert "if ${install_package} || ${capture_packets}; then" in text


def test_remote_workflow_transfers_runs_and_retrieves_without_deleting():
    text = REMOTE_SCRIPT.read_text(encoding="utf-8")
    assert 'remote_host="trixie.local"' in text
    assert 'id_ed25519_codex_trixie' in text
    assert "--identity-file" in text
    assert 'rsync --archive --compress' in text
    assert '"${source_dir}/" "${remote_host}:${remote_source}/"' in text
    assert "./scripts/deploy-and-capture-rpi.sh --log-dir ../logs" in text
    assert '"${remote_host}:${remote_logs}/" "${local_log_dir}/"' in text
    assert "sudo -n true" in text
    assert 'if [[ -t 0 && -t 1 ]]' in text
    assert 'ssh_run_options+=(-tt)' in text
    assert "--non-interactive" in text
    assert "The password is accepted even though no characters are displayed while typing." in text
    assert "--delete" not in text
    assert "rm -" not in text
    assert "pkill" not in text
    assert "killall" not in text
