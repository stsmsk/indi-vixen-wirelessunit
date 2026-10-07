/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#pragma once

#include <cmath>
#include <optional>

namespace vixen::wirelessunit::numeric_validation
{

inline bool finiteInRange(double value, double minimum, double maximum)
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

inline bool oneDecimalPlace(double value)
{
    return std::isfinite(value) && std::abs(value * 10.0 - std::round(value * 10.0)) <= 1e-5;
}

inline std::optional<int> integerInRange(double value, int minimum, int maximum)
{
    if (!finiteInRange(value, minimum, maximum) || std::trunc(value) != value)
        return std::nullopt;
    return static_cast<int>(value);
}

inline bool finiteUpdate(const double *values, int count)
{
    if (!values || count <= 0)
        return false;
    for (int index = 0; index < count; ++index)
        if (!std::isfinite(values[index]))
            return false;
    return true;
}

inline bool validMeridianSettings(int stopPointDegrees, int signedGotoFlipPointDegrees)
{
    return stopPointDegrees >= 0 && stopPointDegrees <= 90 &&
           signedGotoFlipPointDegrees >= -90 && signedGotoFlipPointDegrees <= 90 &&
           std::abs(signedGotoFlipPointDegrees) <= stopPointDegrees;
}

} // namespace vixen::wirelessunit::numeric_validation
