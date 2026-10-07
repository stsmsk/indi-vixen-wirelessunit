/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#include "transport.hpp"

#include <arpa/inet.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

namespace
{

std::string responseOfSize(std::size_t size)
{
    assert(size >= 2);
    std::string response(size - 2, 'A');
    response += "\r\n";
    return response;
}

vixen::wirelessunit::TransportResult transactUdp(const std::string &response)
{
    const int listener = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(listener >= 0);

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t addressLength = sizeof(address);
    assert(::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &addressLength) == 0);
    const auto port = ntohs(address.sin_port);

    std::thread server([listener, response]()
    {
        char request[64] {};
        sockaddr_in clientAddress {};
        socklen_t clientAddressLength = sizeof(clientAddress);
        const auto count = ::recvfrom(listener, request, sizeof(request), 0,
                                      reinterpret_cast<sockaddr *>(&clientAddress), &clientAddressLength);
        assert(count > 0);
        assert(std::string(request, static_cast<std::size_t>(count)) == "$TEST?\r\n");
        assert(::sendto(listener, response.data(), response.size(), 0,
                        reinterpret_cast<sockaddr *>(&clientAddress), clientAddressLength) ==
               static_cast<ssize_t>(response.size()));
        ::close(listener);
    });

    vixen::wirelessunit::SocketTransport transport({
        "127.0.0.1", port, vixen::wirelessunit::TransportKind::udp
    });
    std::string error;
    assert(transport.connect(error));
    auto result = transport.transact("$TEST?\r\n");
    server.join();
    return result;
}

void testUdpTimeoutAndReconnect()
{
    const int listener = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(listener >= 0);
    timeval socketTimeout { 2, 0 };
    assert(::setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &socketTimeout,
                        sizeof(socketTimeout)) == 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t addressLength = sizeof(address);
    assert(::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &addressLength) == 0);

    std::thread server([listener]()
    {
        char request[64] {};
        sockaddr_in clientAddress {};
        socklen_t clientLength = sizeof(clientAddress);
        auto count = ::recvfrom(listener, request, sizeof(request), 0,
                                reinterpret_cast<sockaddr *>(&clientAddress), &clientLength);
        assert(count > 0);
        assert(std::string(request, static_cast<std::size_t>(count)) == "$TEST?\r\n");
        // First response is deliberately lost. The second transaction must
        // create a fresh socket after its normal receive timeout.
        clientLength = sizeof(clientAddress);
        count = ::recvfrom(listener, request, sizeof(request), 0,
                           reinterpret_cast<sockaddr *>(&clientAddress), &clientLength);
        assert(count > 0);
        assert(std::string(request, static_cast<std::size_t>(count)) == "$SYVER?\r\n");
        constexpr char response[] = "$SYVER=2.60\r\n";
        assert(::sendto(listener, response, sizeof(response) - 1, 0,
                        reinterpret_cast<sockaddr *>(&clientAddress), clientLength) ==
               static_cast<ssize_t>(sizeof(response) - 1));
        ::close(listener);
    });

    vixen::wirelessunit::SocketTransport transport({
        "127.0.0.1", ntohs(address.sin_port), vixen::wirelessunit::TransportKind::udp
    });
    std::string error;
    assert(transport.connect(error));
    const auto first = transport.transact("$TEST?\r\n", std::chrono::milliseconds(100));
    assert(!first.success);
    assert(first.error == "receive: timeout");
    assert(!transport.isConnected());
    const auto second = transport.transact("$SYVER?\r\n", std::chrono::milliseconds(500));
    assert(second.success);
    assert(second.response == "$SYVER=2.60\r\n");
    server.join();
}

} // namespace

int main()
{
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    timeval socketTimeout { 2, 0 };
    assert(setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &socketTimeout, sizeof(socketTimeout)) == 0);

    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    assert(::listen(listener, 1) == 0);

    socklen_t addressLength = sizeof(address);
    assert(::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &addressLength) == 0);
    const auto port = ntohs(address.sin_port);

    std::thread server([listener]()
    {
        const int client = ::accept(listener, nullptr, nullptr);
        assert(client >= 0);
        char request[64] {};
        const auto count = ::recv(client, request, sizeof(request), 0);
        assert(count > 0);
        assert(std::string(request, static_cast<std::size_t>(count)) == "$SYVER?\r\n");

        const char first[] = "$SYVER=2.";
        const char second[] = "600000\r\n";
        assert(::send(client, first, std::strlen(first), 0) == static_cast<ssize_t>(std::strlen(first)));
        std::this_thread::yield();
        assert(::send(client, second, std::strlen(second), 0) == static_cast<ssize_t>(std::strlen(second)));
        ::close(client);
        ::close(listener);
    });

    vixen::wirelessunit::SocketTransport transport({
        "127.0.0.1", port, vixen::wirelessunit::TransportKind::tcp
    });
    std::string error;
    assert(transport.connect(error));
    const auto result = transport.transact("$SYVER?\r\n");
    assert(result.success);
    assert(result.response == "$SYVER=2.600000\r\n");
    server.join();

    for (const std::size_t size : { 1023U, 1024U, 1025U, 1323U, 4096U })
    {
        const auto response = responseOfSize(size);
        const auto udpResult = transactUdp(response);
        assert(udpResult.success);
        assert(udpResult.response == response);
    }

    const auto oversized = transactUdp(responseOfSize(4097));
    assert(!oversized.success);
    assert(oversized.error == "receive: UDP datagram exceeds 4096 bytes");

    const auto missingTerminator = transactUdp("$TEST=missing-terminator");
    assert(!missingTerminator.success);
    assert(missingTerminator.error == "receive: UDP datagram did not end with CRLF");

    const auto multipleLines = transactUdp("$TEST=first\r\n$TEST=second\r\n");
    assert(!multipleLines.success);
    assert(multipleLines.error == "receive: UDP datagram contains multiple response lines");

    const auto trailingData = transactUdp("$TEST=first\r\ntrailing-data\r\n");
    assert(!trailingData.success);
    assert(trailingData.error == "receive: UDP datagram contains multiple response lines");
    testUdpTimeoutAndReconnect();
    return 0;
}
