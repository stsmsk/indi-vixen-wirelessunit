/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#include "protocol.hpp"

#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <type_traits>

namespace vixen::wirelessunit
{
namespace
{

std::vector<std::string> split(std::string_view input, char separator)
{
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= input.size())
    {
        const auto end = input.find(separator, start);
        if (end == std::string_view::npos)
        {
            result.emplace_back(input.substr(start));
            break;
        }
        result.emplace_back(input.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

template <typename T>
std::optional<T> parseNumber(std::string_view text)
{
    std::istringstream stream(std::string { text });
    stream.imbue(std::locale::classic());
    T value {};
    stream >> value;
    if (!stream || !stream.eof())
        return std::nullopt;
    if constexpr (std::is_floating_point_v<T>)
    {
        if (!std::isfinite(value))
            return std::nullopt;
    }
    return value;
}

template <typename... Values>
std::string command(std::string_view name, Values &&...values)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << '$' << name << '=';
    (stream << ... << std::forward<Values>(values));
    stream << "\r\n";
    return stream.str();
}

struct Fixed
{
    double value;
    int precision;
};

std::ostream &operator<<(std::ostream &stream, const Fixed &fixed)
{
    stream << std::fixed << std::setprecision(fixed.precision) << fixed.value;
    return stream;
}

bool validAcceptedResponse(const Response &response, std::string_view request, std::size_t minimumFields)
{
    const auto commandEnd = request.find_first_of("?=");
    return commandEnd != std::string_view::npos && response.accepted &&
           response.command == request.substr(0, commandEnd) && response.fields.size() >= minimumFields;
}

std::string removeSurroundingQuotes(std::string_view value)
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
    return std::string(value);
}

} // namespace

std::optional<Response> parseResponse(std::string_view wireData)
{
    if (wireData.size() < 2 || wireData.substr(wireData.size() - 2) != "\r\n")
        return std::nullopt;

    wireData.remove_suffix(2);
    if (wireData.empty() || wireData.front() != '$')
        return std::nullopt;

    const auto equals = wireData.find('=');
    if (equals == std::string_view::npos || equals == 0)
        return std::nullopt;

    Response response;
    response.command.assign(wireData.substr(0, equals));
    response.fields = split(wireData.substr(equals + 1), ',');
    response.accepted = !response.fields.empty() && response.fields.front() != "NG";
    return response;
}

std::optional<MountStatus> decodeMountStatus(const Response &response)
{
    if (!validAcceptedResponse(response, query::mountStatus, 11))
        return std::nullopt;

    const auto mjd = parseNumber<double>(response.fields[0]);
    const auto status = parseNumber<int>(response.fields[1]);
    const auto mechanicalX = parseNumber<double>(response.fields[4]);
    const auto mechanicalY = parseNumber<double>(response.fields[5]);
    const auto azimuth = parseNumber<double>(response.fields[6]);
    const auto altitude = parseNumber<double>(response.fields[7]);
    const auto ra = parseNumber<double>(response.fields[8]);
    const auto dec = parseNumber<double>(response.fields[9]);
    const auto targetType = parseNumber<int>(response.fields[10]);
    if (!mjd || !status || !mechanicalX || !mechanicalY || !azimuth || !altitude || !ra || !dec || !targetType)
        return std::nullopt;

    const bool knownStatus = *status >= 1 && *status <= 3;
    const bool knownTargetType = *targetType >= 1 && *targetType <= 3;
    const bool validCoordinates = *mjd >= 0.0 &&
                                  *azimuth >= 0.0 && *azimuth <= 360.0 &&
                                  *altitude >= -90.0 && *altitude <= 90.0 &&
                                  *ra >= 0.0 && *ra <= 24.0 &&
                                  *dec >= -90.0 && *dec <= 90.0;
    if (!knownStatus || !knownTargetType || !validCoordinates)
        return std::nullopt;

    return MountStatus {
        *mjd, *status, *mechanicalX, *mechanicalY, *azimuth, *altitude, *ra, *dec, *targetType
    };
}

std::optional<Location> decodeLocation(const Response &response)
{
    if (!validAcceptedResponse(response, query::location, 3))
        return std::nullopt;
    const auto longitude = parseNumber<double>(response.fields[0]);
    const auto latitude = parseNumber<double>(response.fields[1]);
    const auto timezone = parseNumber<float>(response.fields[2]);
    if (!longitude || !latitude || !timezone)
        return std::nullopt;
    return Location { *longitude, *latitude, *timezone };
}

std::optional<MountIdentity> decodeMountIdentity(const Response &response)
{
    if (!validAcceptedResponse(response, query::mountModel, 2))
        return std::nullopt;
    const auto model = parseNumber<int>(response.fields[0]);
    const auto maximumSpeed = parseNumber<int>(response.fields[1]);
    if (!model || !maximumSpeed)
        return std::nullopt;
    int maximumSecondarySpeed = *maximumSpeed;
    if (response.fields.size() >= 3)
    {
        const auto parsed = parseNumber<int>(response.fields[2]);
        if (!parsed)
            return std::nullopt;
        maximumSecondarySpeed = *parsed;
    }
    return MountIdentity { *model, *maximumSpeed, maximumSecondarySpeed };
}

std::optional<double> decodeSystemVersion(const Response &response)
{
    if (!validAcceptedResponse(response, query::systemVersion, 1))
        return std::nullopt;
    return parseNumber<double>(response.fields[0]);
}

std::optional<float> decodeMountVersion(const Response &response)
{
    if (!validAcceptedResponse(response, query::mountVersion, 1))
        return std::nullopt;
    return parseNumber<float>(response.fields[0]);
}

std::optional<int> decodeGotoSpeed(const Response &response)
{
    if (!validAcceptedResponse(response, query::gotoSpeed, 1))
        return std::nullopt;
    return parseNumber<int>(response.fields[0]);
}

std::optional<GuideRates> decodeGuideRates(const Response &response)
{
    if (!validAcceptedResponse(response, query::autoguideSpeed, 2))
        return std::nullopt;
    const auto ra = parseNumber<float>(response.fields[0]);
    const auto dec = parseNumber<float>(response.fields[1]);
    if (!ra || !dec)
        return std::nullopt;
    return GuideRates { *ra, *dec };
}

std::optional<BacklashCompensation> decodeBacklash(const Response &response)
{
    if (!validAcceptedResponse(response, query::backlash, 2))
        return std::nullopt;
    const auto ra = parseNumber<int>(response.fields[0]);
    const auto dec = parseNumber<int>(response.fields[1]);
    if (!ra || !dec)
        return std::nullopt;
    return BacklashCompensation { *ra, *dec };
}

std::optional<int> decodeMotorPower(const Response &response)
{
    if (!validAcceptedResponse(response, query::motorPower, 1))
        return std::nullopt;
    return parseNumber<int>(response.fields[0]);
}

std::optional<int> decodeMountType(const Response &response)
{
    if (!validAcceptedResponse(response, query::mountType, 1))
        return std::nullopt;
    return parseNumber<int>(response.fields[0]);
}

std::optional<MeridianSettings> decodeMeridianSettings(const Response &response)
{
    if (!validAcceptedResponse(response, query::meridian, 2))
        return std::nullopt;
    const auto forcedTrackingStop = parseNumber<int>(response.fields[0]);
    const auto automaticGoto = parseNumber<int>(response.fields[1]);
    if (!forcedTrackingStop || !automaticGoto)
        return std::nullopt;
    return MeridianSettings { *forcedTrackingStop, *automaticGoto };
}

std::optional<std::vector<AlignmentPoint>> decodeAlignmentPoints(const Response &response)
{
    if (!validAcceptedResponse(response, query::alignments, 1))
        return std::nullopt;

    const auto count = parseNumber<int>(response.fields[0]);
    if (!count || *count < 0 || response.fields.size() != 1 + static_cast<std::size_t>(*count) * 3)
        return std::nullopt;

    std::vector<AlignmentPoint> points;
    points.reserve(static_cast<std::size_t>(*count));
    for (int index = 0; index < *count; ++index)
    {
        const std::size_t offset = 1 + static_cast<std::size_t>(index) * 3;
        const auto state = parseNumber<int>(response.fields[offset]);
        const auto mjd = parseNumber<double>(response.fields[offset + 1]);
        if (!state || !mjd)
            return std::nullopt;
        points.push_back(AlignmentPoint { *state, *mjd, response.fields[offset + 2] });
    }
    return points;
}

std::optional<WifiStationSettings> decodeWifiStationSettings(const Response &response)
{
    // The fourth field is the stored password. Deliberately validate its
    // presence but never return it to callers, so credentials cannot reach
    // the INDI property layer or diagnostic logs.
    if (!validAcceptedResponse(response, query::wifiStationSettings, 4) || response.fields.size() != 4)
        return std::nullopt;
    const auto autoConnect = parseNumber<int>(response.fields[0]);
    const auto stationModeEnabled = parseNumber<int>(response.fields[1]);
    if (!autoConnect || !stationModeEnabled || (*autoConnect != 0 && *autoConnect != 1) ||
        (*stationModeEnabled != 0 && *stationModeEnabled != 1))
        return std::nullopt;
    return WifiStationSettings { *autoConnect == 1, *stationModeEnabled == 1,
                                 removeSurroundingQuotes(response.fields[2]) };
}

std::optional<WifiStationConnection> decodeWifiStationConnection(const Response &response)
{
    if (!validAcceptedResponse(response, query::wifiStationConnection, 2) || response.fields.size() != 2)
        return std::nullopt;
    const auto connected = parseNumber<int>(response.fields[0]);
    if (!connected || (*connected != 0 && *connected != 1))
        return std::nullopt;
    return WifiStationConnection { *connected == 1, removeSurroundingQuotes(response.fields[1]) };
}

std::optional<std::vector<WifiAccessPoint>> decodeWifiAccessPoints(const Response &response)
{
    if (!validAcceptedResponse(response, query::wifiAccessPoints, 1))
        return std::nullopt;
    const auto count = parseNumber<int>(response.fields[0]);
    if (!count || *count < 0 || *count > 1000 ||
        response.fields.size() != 1 + static_cast<std::size_t>(*count) * 3)
        return std::nullopt;

    std::vector<WifiAccessPoint> accessPoints;
    accessPoints.reserve(static_cast<std::size_t>(*count));
    for (int index = 0; index < *count; ++index)
    {
        const auto offset = 1 + static_cast<std::size_t>(index) * 3;
        const auto type = parseNumber<int>(response.fields[offset]);
        const auto signal = parseNumber<int>(response.fields[offset + 2]);
        if (!type || !signal)
            return std::nullopt;
        accessPoints.push_back(WifiAccessPoint {
            *type, removeSurroundingQuotes(response.fields[offset + 1]), *signal
        });
    }
    return accessPoints;
}

std::optional<std::string> decodeWifiHotspotSsid(const Response &response)
{
    if (!validAcceptedResponse(response, query::wifiHotspotSsid, 1) || response.fields.size() != 1)
        return std::nullopt;
    return removeSurroundingQuotes(response.fields[0]);
}

std::optional<std::string> decodeWifiHotspotPassword(const Response &response)
{
    if (!validAcceptedResponse(response, query::wifiHotspotPassword, 1) || response.fields.size() != 1)
        return std::nullopt;
    return removeSurroundingQuotes(response.fields[0]);
}

std::optional<int> decodeWifiHotspotChannel(const Response &response)
{
    if (!validAcceptedResponse(response, query::wifiHotspotChannel, 1) || response.fields.size() != 1)
        return std::nullopt;
    const auto channel = parseNumber<int>(response.fields[0]);
    if (!channel || *channel < 1 || *channel > 11)
        return std::nullopt;
    return channel;
}

std::string gotoRaDec(double rightAscensionHours, double declinationDegrees)
{
    return command(query::gotoTargetName, 3, ',', Fixed { rightAscensionHours, 6 }, ',',
                   Fixed { declinationDegrees, 6 }, ",INDI");
}

std::string gotoAltAz(double azimuthDegrees, double altitudeDegrees)
{
    return command(query::gotoTargetName, 2, ',', Fixed { azimuthDegrees, 6 }, ',', Fixed { altitudeDegrees, 6 },
                   ",INDI");
}

std::string gotoHome()
{
    return std::string(query::gotoHome);
}

std::string initializeMountCommand()
{
    return std::string(query::initializeMount);
}

std::string alignmentLabelRaDec(double rightAscensionHours, double declinationDegrees)
{
    // The Wireless Unit stores at most 15 label characters.  Four decimals
    // of RA and three of Dec retain useful pointing precision while fitting
    // exactly: R20.7061+45.379.
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << 'R' << std::fixed << std::setfill('0')
           << std::setw(7) << std::setprecision(4) << rightAscensionHours
           << (std::signbit(declinationDegrees) ? '-' : '+')
           << std::setw(6) << std::setprecision(3) << std::abs(declinationDegrees);
    return stream.str();
}

std::string alignmentLabelAltAz(double azimuthDegrees, double altitudeDegrees)
{
    // Keep the coordinate-system prefix and both angles within the Wireless
    // Unit's 15-character alignment-label limit: A270.000+00.000.
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << 'A' << std::fixed << std::setfill('0')
           << std::setw(7) << std::setprecision(3) << azimuthDegrees
           << (std::signbit(altitudeDegrees) ? '-' : '+')
           << std::setw(6) << std::setprecision(3) << std::abs(altitudeDegrees);
    return stream.str();
}

std::string alignRaDec(double rightAscensionHours, double declinationDegrees)
{
    return command(query::alignmentName, 3, ',', Fixed { rightAscensionHours, 6 }, ',', Fixed { declinationDegrees, 6 },
                   ',', alignmentLabelRaDec(rightAscensionHours, declinationDegrees));
}

std::string alignAltAz(double azimuthDegrees, double altitudeDegrees)
{
    return command(query::alignmentName, 2, ',', Fixed { azimuthDegrees, 6 }, ',', Fixed { altitudeDegrees, 6 },
                   ',', alignmentLabelAltAz(azimuthDegrees, altitudeDegrees));
}

std::string deleteAlignmentPoint(std::size_t zeroBasedIndex)
{
    return command(query::deleteAlignmentName, zeroBasedIndex);
}

std::string moveBeforeInitialization(double durationSeconds, double raSiderealRate, double decSiderealRate)
{
    return command(query::moveBeforeInitializationName, Fixed { durationSeconds, 1 }, ',', Fixed { raSiderealRate, 4 }, ',',
                   Fixed { decSiderealRate, 4 });
}

std::string moveRaDec(double durationSeconds, double raSiderealRate, double decSiderealRate)
{
    return command(query::moveRaDecName, Fixed { durationSeconds, 2 }, ',', Fixed { raSiderealRate, 6 }, ',',
                   Fixed { decSiderealRate, 6 });
}

std::string stopMount()
{
    return std::string(query::stopMount);
}

std::string setMoveRate(double xSiderealRate, double ySiderealRate)
{
    return command(query::moveRateName, Fixed { xSiderealRate, 6 }, ',', Fixed { ySiderealRate, 6 });
}

std::string setGotoSpeed(int speed)
{
    return command(query::gotoSpeedName, speed);
}

std::string setAutoguideSpeed(float raSiderealRate, float decSiderealRate)
{
    return command(query::autoguideSpeedName, Fixed { raSiderealRate, 6 }, ',', Fixed { decSiderealRate, 6 });
}

std::string setBacklash(int rightAscension, int declination)
{
    return command(query::backlashName, rightAscension, ',', declination);
}

std::string setMotorPower(int power)
{
    return command(query::motorPowerName, power);
}

std::string setMountType(int type)
{
    return command(query::mountTypeName, type);
}

std::string setMeridianSettings(int forcedTrackingStopDegrees, int automaticGotoDegrees)
{
    return command(query::meridianName, forcedTrackingStopDegrees, ',', automaticGotoDegrees);
}

std::string setWifiStationSettings(bool autoConnect, std::string_view ssid, std::string_view password)
{
    // Field 2 enables station mode. Real-device captures show that it remains
    // 1 when boot auto-connect is Off; 0 is the factory-reset/unconfigured state.
    return command(query::wifiStationSettingsName, autoConnect ? 1 : 0, ",1,", ssid, ',', password);
}

std::string setWifiStationConnection(bool connected)
{
    return command(query::wifiStationConnectionName, connected ? 1 : 0);
}

std::string setWifiHotspotSsid(std::string_view ssid)
{
    return command(query::wifiHotspotSsidName, ssid);
}

std::string setWifiHotspotPassword(std::string_view password)
{
    return command(query::wifiHotspotPasswordName, password);
}

std::string setWifiHotspotChannel(int channel)
{
    return command(query::wifiHotspotChannelName, channel);
}

std::string setLocation(double longitudeDegrees, double latitudeDegrees, float timezoneHours)
{
    return command(query::locationName, Fixed { longitudeDegrees, 6 }, ',', Fixed { latitudeDegrees, 6 }, ',',
                   Fixed { timezoneHours, 6 });
}

std::string setUtc(std::chrono::system_clock::time_point utc)
{
    const auto wholeSeconds = std::chrono::time_point_cast<std::chrono::seconds>(utc);
    const auto fractional = std::chrono::duration<double>(utc - wholeSeconds).count();
    const std::time_t raw = std::chrono::system_clock::to_time_t(wholeSeconds);
    std::tm value {};
#if defined(_WIN32)
    gmtime_s(&value, &raw);
#else
    gmtime_r(&raw, &value);
#endif
    const double seconds = static_cast<double>(value.tm_sec) + fractional;
    return command(query::utcName, value.tm_year + 1900, ',', value.tm_mon + 1, ',', value.tm_mday, ',', value.tm_hour, ',',
                   value.tm_min, ',', Fixed { seconds, 6 });
}

std::string destinationPierSide(double rightAscensionHours, double declinationDegrees)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << query::destinationPierSidePrefix << Fixed { rightAscensionHours, 6 } << ','
           << Fixed { declinationDegrees, 6 } << "\r\n";
    return stream.str();
}

} // namespace vixen::wirelessunit
