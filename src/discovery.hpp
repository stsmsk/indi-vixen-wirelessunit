/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace vixen::wirelessunit
{

struct DiscoveredUnit
{
    std::string address;
    double firmwareVersion { 0 };
};

struct DiscoveryOptions
{
    std::uint16_t port { 60023 };
    std::chrono::milliseconds timeout { 800 };

    // Empty means the global broadcast address plus the directed broadcast
    // address of each active local IPv4 network. Tests and specialist callers
    // may supply explicit IPv4 targets.
    std::vector<std::string> candidateAddresses;
};

std::vector<DiscoveredUnit> discoverWirelessUnits(const DiscoveryOptions &options, std::string &error);

} // namespace vixen::wirelessunit
