/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#include "wifi_ap_update.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
#include <utility>
#include <vector>

using vixen::wirelessunit::parseWifiApTextUpdate;
using vixen::wirelessunit::shouldApplyWifiApChannel;
using vixen::wirelessunit::shouldApplyWifiApText;

int main()
{
    const auto ssidOnly = parseWifiApTextUpdate({ { "SSID", "test-ap" } });
    assert(ssidOnly && ssidOnly.value->ssid == "test-ap");
    assert(!ssidOnly.value->password && !ssidOnly.value->channel);
    assert(shouldApplyWifiApText(*ssidOnly.value->ssid, "old-ap", true));
    assert(!shouldApplyWifiApText("", "old-password", true));
    assert(!shouldApplyWifiApChannel(ssidOnly.value->channel, 6, true));

    const auto passwordOnly = parseWifiApTextUpdate({ { "PASSWORD", "new-pass88" } });
    assert(passwordOnly && !passwordOnly.value->ssid && !passwordOnly.value->channel);
    assert(shouldApplyWifiApText(*passwordOnly.value->password, "old-pass88", true));
    assert(!shouldApplyWifiApText("", "old-ap", true));

    const auto channelOnly = parseWifiApTextUpdate({ { "CHANNEL", "11" } });
    assert(channelOnly && !channelOnly.value->ssid && !channelOnly.value->password);
    assert(shouldApplyWifiApChannel(channelOnly.value->channel, 6, true));

    const auto all = parseWifiApTextUpdate({ { "SSID", "test-ap" },
                                             { "PASSWORD", "new-pass88" },
                                             { "CHANNEL", "11" } });
    assert(all && all.value->ssid && all.value->password && all.value->channel);
    assert(shouldApplyWifiApText(*all.value->ssid, "old-ap", true));
    assert(shouldApplyWifiApText(*all.value->password, "old-pass88", true));
    assert(shouldApplyWifiApChannel(all.value->channel, 6, true));
    assert(!shouldApplyWifiApText(*all.value->ssid, "test-ap", true));
    assert(!shouldApplyWifiApChannel(all.value->channel, 11, true));

    const auto emptyPassword = parseWifiApTextUpdate({ { "PASSWORD", "" } });
    assert(emptyPassword && emptyPassword.value->password && emptyPassword.value->password->empty());
    assert(!shouldApplyWifiApText(*emptyPassword.value->password, "old-pass88", true));
    assert(!parseWifiApTextUpdate({ { "SSID", "" } }));
    assert(!parseWifiApTextUpdate({ { "CHANNEL", "" } }));
    assert(!parseWifiApTextUpdate({ { "CHANNEL", "12" } }));
    assert(!parseWifiApTextUpdate({ { "CHANNEL", "1.5" } }));
    assert(!parseWifiApTextUpdate({ { "PASSWORD", "short" } }));
    assert(!parseWifiApTextUpdate({ { "SSID", "a" }, { "SSID", "b" } }));
    assert(!parseWifiApTextUpdate({ { "UNKNOWN", "value" } }));
    assert(!parseWifiApTextUpdate({}));
    return 0;
}
