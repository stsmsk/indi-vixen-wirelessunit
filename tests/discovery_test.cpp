/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#include "discovery.hpp"

#include <arpa/inet.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

int main()
{
    const int listener = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(listener >= 0);
    timeval socketTimeout { 2, 0 };
    assert(setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &socketTimeout, sizeof(socketTimeout)) == 0);

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);

    socklen_t addressLength = sizeof(address);
    assert(::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &addressLength) == 0);
    const auto port = ntohs(address.sin_port);

    std::thread server([listener]()
    {
        char request[64] {};
        sockaddr_in client {};
        socklen_t clientLength = sizeof(client);
        const auto count = recvfrom(listener, request, sizeof(request), 0,
                                    reinterpret_cast<sockaddr *>(&client), &clientLength);
        assert(count > 0);
        assert(std::string(request, static_cast<std::size_t>(count)) == "$SYVER?\r\n");
        const char response[] = "$SYVER=2.60\r\n";
        assert(sendto(listener, response, std::strlen(response), 0,
                      reinterpret_cast<sockaddr *>(&client), clientLength) ==
               static_cast<ssize_t>(std::strlen(response)));
        ::close(listener);
    });

    std::string error;
    const auto found = vixen::wirelessunit::discoverWirelessUnits(
        { port, std::chrono::milliseconds(300), { "127.0.0.1" } }, error);
    assert(error.empty());
    assert(found.size() == 1);
    assert(found[0].address == "127.0.0.1");
    assert(found[0].firmwareVersion == 2.60);
    server.join();

    const auto invalid = vixen::wirelessunit::discoverWirelessUnits(
        { port, std::chrono::milliseconds(100), { "not-an-address" } }, error);
    assert(invalid.empty());
    assert(!error.empty());
    return 0;
}
