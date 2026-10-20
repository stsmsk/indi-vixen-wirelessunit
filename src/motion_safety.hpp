/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#pragma once

namespace vixen::wirelessunit::motion_safety
{

inline bool stopRequiredBeforeDisconnect(bool manualMotion, bool guidePulse,
                                         bool scopeSlewing, bool scopeTracking,
                                         bool scopeParking, bool reportedActive) noexcept
{
    return manualMotion || guidePulse || scopeSlewing || scopeTracking ||
           scopeParking || reportedActive;
}

} // namespace vixen::wirelessunit::motion_safety
