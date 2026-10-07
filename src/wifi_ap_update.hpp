/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#pragma once

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vixen::wirelessunit
{

struct WifiApTextUpdate
{
    // A missing element differs from an explicitly submitted empty value.
    std::optional<std::string> ssid;
    std::optional<std::string> password;
    std::optional<int> channel;
};

struct WifiApTextParseResult
{
    std::optional<WifiApTextUpdate> value;
    std::string error;

    explicit operator bool() const noexcept { return value.has_value(); }
};

inline WifiApTextParseResult parseWifiApTextUpdate(
    const std::vector<std::pair<std::string, std::string>> &items)
{
    if (items.empty())
        return { std::nullopt, "Supply at least one Access point setting." };

    WifiApTextUpdate update;
    for (const auto &[name, text] : items)
    {
        if (name == "SSID")
        {
            if (update.ssid)
                return { std::nullopt, "Duplicate Access point SSID." };
            if (text.empty() || text.size() > 32 || text.find_first_of(",\r\n") != std::string::npos)
                return { std::nullopt,
                         "Access point SSID must be 1-32 bytes and must not contain comma or a line break." };
            update.ssid = text;
        }
        else if (name == "PASSWORD")
        {
            if (update.password)
                return { std::nullopt, "Duplicate Access point Password." };
            // An empty Password is the existing UI convention for retaining
            // the saved password. It is present, but does not cause WFPWD.
            if (!text.empty() &&
                (text.size() < 8 || text.size() > 63 || text.find_first_of(",\r\n") != std::string::npos))
                return { std::nullopt,
                         "Access point Password must be 8-63 bytes and must not contain comma or a line break." };
            update.password = text;
        }
        else if (name == "CHANNEL")
        {
            if (update.channel)
                return { std::nullopt, "Duplicate Access point Channel." };
            int channel = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), channel);
            if (text.empty() || parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size() ||
                channel < 1 || channel > 11)
                return { std::nullopt, "Access point Channel must be an integer from 1 through 11." };
            update.channel = channel;
        }
        else
        {
            return { std::nullopt, "Unknown Access point setting." };
        }
    }
    return { std::move(update), {} };
}

inline bool shouldApplyWifiApText(std::string_view submitted,
                                  std::string_view current, bool currentLoaded)
{
    return !submitted.empty() && (!currentLoaded || submitted != current);
}

inline bool shouldApplyWifiApChannel(const std::optional<int> &submitted,
                                     int current, bool currentLoaded)
{
    return submitted && (!currentLoaded || *submitted != current);
}

} // namespace vixen::wirelessunit
