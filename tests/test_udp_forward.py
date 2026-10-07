# Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
# Licensed under the GNU General Public License, version 3 or later.
# See COPYING for the full license text.

import importlib.util
import pathlib
import sys


MODULE_PATH = pathlib.Path(__file__).parents[1] / "tools" / "udp_forward.py"
SPEC = importlib.util.spec_from_file_location("udp_forward", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def test_parse_and_format_mtsta():
    line = (
        "$MTSTA=61273.268050,1,0,16,83.609683,-3.059667,263.819078,"
        "3.464340,8.165369,-3.059667,3,ASCOM\r\n"
    )
    status = MODULE.parse_mtsta(line)
    assert status is not None
    assert status.status == 1
    assert status.target_type == 3
    assert status.target_label == "ASCOM"
    rendered = MODULE.format_mtsta(status)
    assert "Az=263:49:08.681" in rendered
    assert "Alt=+003:27:51.624" in rendered
    assert "RA=08:09:55.328" in rendered
    assert "Dec=-003:03:34.801" in rendered


def test_non_mtsta_and_invalid_mtsta_are_ignored():
    assert MODULE.parse_mtsta("$SYVER=2.60\r\n") is None
    assert MODULE.parse_mtsta("$MTSTA=NG\r\n") is None


def test_negative_zero_keeps_its_sign():
    assert MODULE.format_dms(-0.0).startswith("-")


class FakeSocket:
    def __init__(self):
        self.target = None
        self.sent = []
        self.closed = False

    def connect(self, target):
        self.target = target

    def send(self, data):
        self.sent.append(data)

    def close(self):
        self.closed = True


class FakeSocketFactory:
    def __init__(self):
        self.sockets = []

    def __call__(self, *args):
        assert args == (MODULE.socket.AF_INET, MODULE.socket.SOCK_DGRAM)
        instance = FakeSocket()
        self.sockets.append(instance)
        return instance


def make_router(*, max_retired=64, response_timeout=2.0, retire_grace=5.0):
    factory = FakeSocketFactory()
    router = MODULE.RequestRouter(
        ("10.42.0.60", 60023), response_timeout, retire_grace, max_retired, factory
    )
    return router, factory


def test_command_family_and_invalid_requests():
    assert MODULE.command_family(b"$MTSTA?\r\n") == b"MTSTA"
    assert MODULE.command_family(b"$MTCPI?3,12.0,-5.0\r\n") == b"MTCPI"
    assert MODULE.command_family(b"$MTMV2=3.00,0,0\r\n") == b"MTMV2"
    assert MODULE.command_family(b'$WFPWD="foo?bar"\r\n') == b"WFPWD"
    assert MODULE.command_family(b"hello\r\n") is None
    router, factory = make_router()
    assert router.begin(b"hello\r\n", ("10.42.0.191", 1000), 0.0)
    assert not factory.sockets
    assert router.owner_host is None


def test_lost_reply_and_new_client_port_do_not_misroute():
    router, factory = make_router()
    first = ("10.42.0.191", 1000)
    second = ("10.42.0.191", 2000)
    assert router.begin(b"$MTSTA?\r\n", first, 0.0) is None
    old_socket = router.pending.upstream
    assert router.begin(b"$MTSTA?\r\n", second, 0.6) is None
    new_socket = router.pending.upstream
    assert old_socket is not new_socket
    assert factory.sockets == [old_socket, new_socket]
    assert router.receive(old_socket, b"$MTSTA=old\r\n", 0.7) is None
    assert router.receive(new_socket, b"$MTSTA=new\r\n", 0.8) == second
    assert router.receive(new_socket, b"$MTSTA=duplicate\r\n", 0.9) is None
    router.expire(6.0)
    assert old_socket.closed and new_socket.closed


def test_response_family_mismatch_waits_for_matching_reply():
    router, _ = make_router()
    peer = ("10.42.0.191", 1000)
    assert router.begin(b"$MTSTA?\r\n", peer, 0.0) is None
    upstream = router.pending.upstream
    assert router.receive(upstream, b"$SYVER=2.60\r\n", 0.1) is None
    assert router.pending is not None
    assert router.receive(upstream, b"$MTSTA=state\r\n", 0.2) == peer
    router.close()


def test_timeout_quarantines_old_port_and_bounded_retired_sockets():
    router, factory = make_router(max_retired=2, response_timeout=0.5, retire_grace=5.0)
    peer = ("10.42.0.191", 1000)
    assert router.begin(b"$SYVER?\r\n", peer, 0.0) is None
    first_socket = router.pending.upstream
    router.expire(0.6)
    assert router.pending is None
    assert not first_socket.closed
    assert router.begin(b"$MTSTA?\r\n", peer, 0.7) is None
    second_socket = router.pending.upstream
    assert router.begin(b"$MTSTA?\r\n", peer, 0.8)
    assert len(factory.sockets) == 2
    assert router.pending.upstream is second_socket
    assert router.receive(first_socket, b"$SYVER=2.60\r\n", 0.9) is None
    assert router.receive(second_socket, b"$MTSTA=state\r\n", 1.0) == peer
    assert router.begin(b"$MTSTA?\r\n", peer, 1.1)
    router.expire(6.1)
    assert first_socket.closed and second_socket.closed
    assert router.begin(b"$MTSTA?\r\n", peer, 6.2) is None
    router.close()


def test_other_client_host_is_rejected_without_forwarding():
    router, factory = make_router()
    first = ("10.42.0.191", 1000)
    other = ("10.42.0.192", 2000)
    assert router.begin(b"$MTSTA?\r\n", first, 0.0) is None
    assert router.begin(b"$MTSTA?\r\n", other, 0.1)
    assert len(factory.sockets) == 1
    assert router.receive(factory.sockets[0], b"$MTSTA=state\r\n", 0.2) == first
    router.close()


def test_failed_new_upstream_send_keeps_previous_request():
    router, factory = make_router()
    peer = ("10.42.0.191", 1000)
    assert router.begin(b"$MTSTA?\r\n", peer, 0.0) is None
    first_socket = router.pending.upstream
    original_factory = router.socket_factory

    def failing_factory(*args):
        instance = original_factory(*args)

        def fail_send(_data):
            raise OSError("simulated send failure")

        instance.send = fail_send
        return instance

    router.socket_factory = failing_factory
    try:
        router.begin(b"$SYVER?\r\n", peer, 0.1)
    except OSError:
        pass
    else:
        raise AssertionError("failed upstream send was accepted")
    assert factory.sockets[-1].closed
    assert router.pending.upstream is first_socket
    assert router.receive(first_socket, b"$MTSTA=state\r\n", 0.2) == peer
    router.close()
