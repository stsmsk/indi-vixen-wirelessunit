/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#include "discovery.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <map>
#include <net/if.h>
#include <poll.h>
#include <set>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

namespace vixen::wirelessunit
{
namespace
{

struct ScanTarget
{
    std::uint32_t addressNetworkOrder { 0 };
};

std::string systemError(std::string_view operation)
{
    return std::string(operation) + ": " + std::strerror(errno);
}

bool parseVersionResponse(std::string_view response, double &version)
{
    constexpr std::string_view prefix = "$SYVER=";
    if (response.size() < prefix.size() + 3 || response.substr(0, prefix.size()) != prefix ||
        response.substr(response.size() - 2) != "\r\n")
        return false;

    const std::string value(response.substr(prefix.size(), response.size() - prefix.size() - 2));
    char *end = nullptr;
    errno = 0;
    const double parsed = std::strtod(value.c_str(), &end);
    if (errno != 0 || !end || *end != '\0' || parsed <= 0)
        return false;
    version = parsed;
    return true;
}

void addTarget(std::vector<ScanTarget> &targets, std::set<std::uint32_t> &seen, std::uint32_t address)
{
    if (seen.insert(address).second)
        targets.push_back({ address });
}

std::vector<ScanTarget> explicitTargets(const std::vector<std::string> &addresses, std::string &error)
{
    std::vector<ScanTarget> targets;
    std::set<std::uint32_t> seen;
    for (const auto &text : addresses)
    {
        in_addr address {};
        if (inet_pton(AF_INET, text.c_str(), &address) != 1)
        {
            error = "invalid IPv4 discovery target: " + text;
            return {};
        }
        addTarget(targets, seen, address.s_addr);
    }
    return targets;
}

std::vector<ScanTarget> localTargets(std::string &error)
{
    ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0)
    {
        error = systemError("getifaddrs");
        return {};
    }

    std::vector<ScanTarget> targets;
    std::set<std::uint32_t> seen;
    addTarget(targets, seen, htonl(INADDR_BROADCAST));

    for (auto *interface = interfaces; interface != nullptr; interface = interface->ifa_next)
    {
        if (!interface->ifa_addr || !interface->ifa_broadaddr || interface->ifa_addr->sa_family != AF_INET ||
            interface->ifa_broadaddr->sa_family != AF_INET || !(interface->ifa_flags & IFF_UP) ||
            !(interface->ifa_flags & IFF_BROADCAST) || (interface->ifa_flags & IFF_LOOPBACK))
            continue;

        const auto *broadcast = reinterpret_cast<const sockaddr_in *>(interface->ifa_broadaddr);
        addTarget(targets, seen, broadcast->sin_addr.s_addr);
    }
    freeifaddrs(interfaces);

    if (targets.size() == 1)
    {
        error = "no active non-loopback IPv4 interface";
        return {};
    }
    return targets;
}

} // namespace

std::vector<DiscoveredUnit> discoverWirelessUnits(const DiscoveryOptions &options, std::string &error)
{
    error.clear();
    if (options.port == 0 || options.timeout.count() <= 0)
    {
        error = "invalid discovery port or timeout";
        return {};
    }

    auto targets = options.candidateAddresses.empty() ? localTargets(error)
                                                       : explicitTargets(options.candidateAddresses, error);
    if (targets.empty())
        return {};

    const int descriptor = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (descriptor < 0)
    {
        error = systemError("socket");
        return {};
    }

    int enabled = 1;
    if (setsockopt(descriptor, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) != 0)
    {
        error = systemError("setsockopt(SO_BROADCAST)");
        ::close(descriptor);
        return {};
    }

    // 0.0.0.0 means all local IPv4 interfaces. The kernel assigns one
    // ephemeral source port, and replies from every interface return to this
    // same socket.
    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(descriptor, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) != 0)
    {
        error = systemError("bind(0.0.0.0)");
        ::close(descriptor);
        return {};
    }

    const int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags >= 0)
        fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);

    std::size_t sentCount = 0;
    for (const auto &target : targets)
    {
        sockaddr_in peer {};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(options.port);
        peer.sin_addr.s_addr = target.addressNetworkOrder;
        const auto sent = sendto(descriptor, query::systemVersion.data(), query::systemVersion.size(), 0,
                                 reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));
        if (sent == static_cast<ssize_t>(query::systemVersion.size()))
            ++sentCount;
    }
    if (sentCount == 0)
    {
        error = "could not send a discovery datagram on any local IPv4 network";
        ::close(descriptor);
        return {};
    }

    std::map<std::uint32_t, DiscoveredUnit> found;
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        pollfd pollDescriptor { descriptor, POLLIN, 0 };
        const int pollResult = poll(&pollDescriptor, 1, static_cast<int>(std::max<std::int64_t>(1, remaining.count())));
        if (pollResult == 0)
            break;
        if (pollResult < 0)
        {
            if (errno == EINTR)
                continue;
            error = systemError("poll");
            break;
        }

        std::array<char, 512> buffer {};
        sockaddr_in sender {};
        socklen_t senderLength = sizeof(sender);
        const auto received = recvfrom(descriptor, buffer.data(), buffer.size(), 0,
                                       reinterpret_cast<sockaddr *>(&sender), &senderLength);
        if (received < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            error = systemError("recvfrom");
            break;
        }
        if (sender.sin_family != AF_INET || ntohs(sender.sin_port) != options.port)
            continue;

        double version = 0;
        if (!parseVersionResponse(std::string_view(buffer.data(), static_cast<std::size_t>(received)), version))
            continue;

        char address[INET_ADDRSTRLEN] {};
        if (!inet_ntop(AF_INET, &sender.sin_addr, address, sizeof(address)))
            continue;
        found.emplace(ntohl(sender.sin_addr.s_addr), DiscoveredUnit { address, version });
    }
    ::close(descriptor);

    std::vector<DiscoveredUnit> result;
    result.reserve(found.size());
    for (auto &[numericAddress, unit] : found)
    {
        (void)numericAddress;
        result.push_back(std::move(unit));
    }
    return result;
}

} // namespace vixen::wirelessunit
