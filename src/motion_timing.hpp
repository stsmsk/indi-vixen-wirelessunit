/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#pragma once

#include <cstdint>

namespace vixen::wirelessunit::motion_timing
{

// Keep the ASCOM-compatible MTSTA cadence. Telescope::TimerHit schedules its
// next callback after ReadScopeStatus returns, so two normal 500 ms UDP
// transactions can extend the interval between motion refreshes to ~2 s.
inline constexpr std::uint32_t statusPollingMilliseconds = 1000;
inline constexpr std::uint32_t normalTransactionTimeoutMilliseconds = 500;
inline constexpr double continuousMotionDurationSeconds = 3.0;

static_assert(continuousMotionDurationSeconds * 1000.0 >
              statusPollingMilliseconds + 2 * normalTransactionTimeoutMilliseconds);

} // namespace vixen::wirelessunit::motion_timing
