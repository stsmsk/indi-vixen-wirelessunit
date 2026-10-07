/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#include "client.hpp"
#include "numeric_validation.hpp"

#include <cmath>
#include <utility>

namespace vixen::wirelessunit
{

namespace
{

constexpr auto kWifiInformationTimeout = std::chrono::milliseconds(1500);
constexpr auto kWifiScanTimeout = std::chrono::milliseconds(5000);
constexpr auto kWifiReconfigurationTimeout = std::chrono::milliseconds(30000);

} // namespace

ProtocolClient::ProtocolClient(std::unique_ptr<ITransport> transport) : transport_(std::move(transport))
{
}

bool ProtocolClient::connect(std::string &error)
{
    if (!transport_)
    {
        error = "transport is not configured";
        return false;
    }
    return transport_->connect(error);
}

void ProtocolClient::disconnect() noexcept
{
    if (transport_)
        transport_->disconnect();
    lastStatus_.reset();
}

bool ProtocolClient::isConnected() const noexcept
{
    return transport_ && transport_->isConnected();
}

Result<Response> ProtocolClient::exchange(std::string_view request, std::chrono::milliseconds timeout,
                                          bool acceptNegativeResponse)
{
    if (!transport_)
        return { std::nullopt, "transport is not configured" };

    auto wire = transport_->transact(request, timeout);
    if (!wire.success)
        return { std::nullopt, std::move(wire.error) };

    auto response = parseResponse(wire.response);
    if (!response)
        return { std::nullopt, "malformed response: expected $COMMAND=fields followed by CRLF" };
    const auto commandEnd = request.find_first_of("?=");
    if (commandEnd == std::string_view::npos || response->command != request.substr(0, commandEnd))
        return { std::nullopt, "response command does not match the request" };
    if (!response->accepted && !acceptNegativeResponse)
        return { std::nullopt, response->fields.empty() ? "device returned a negative response"
                                                        : "device returned: " + response->fields.front() };
    return { std::move(response), {} };
}

Result<void> ProtocolClient::command(std::string_view request, std::chrono::milliseconds timeout)
{
    auto response = exchange(request, timeout);
    if (!response)
        return { false, std::move(response.error) };
    return { true, {} };
}

Result<void> ProtocolClient::targetCommand(std::string_view request)
{
    auto response = exchange(request);
    if (!response)
        return { false, std::move(response.error) };
    // Firmware 2.60 explicitly rejects a target below the horizon with
    // MTTGT=BELOWHORIZON, not MTTGT=NG. Other non-NG payloads remain accepted.
    if (!response.value->fields.empty() && response.value->fields.front() == "BELOWHORIZON")
        return { false, "target is below the horizon" };
    return { true, {} };
}

Result<double> ProtocolClient::systemVersion()
{
    return query<double>(query::systemVersion, decodeSystemVersion);
}

Result<float> ProtocolClient::mountVersion()
{
    return query<float>(query::mountVersion, decodeMountVersion);
}

Result<MountIdentity> ProtocolClient::mountIdentity()
{
    return query<MountIdentity>(query::mountModel, decodeMountIdentity);
}

Result<Location> ProtocolClient::location()
{
    return query<Location>(query::location, decodeLocation);
}

Result<MountStatus> ProtocolClient::mountStatus()
{
    auto result = query<MountStatus>(query::mountStatus, decodeMountStatus);
    if (result)
        lastStatus_ = result.value;
    else
        lastStatus_.reset();
    return result;
}

Result<int> ProtocolClient::gotoSpeed()
{
    return query<int>(query::gotoSpeed, decodeGotoSpeed);
}

Result<GuideRates> ProtocolClient::guideRates()
{
    return query<GuideRates>(query::autoguideSpeed, decodeGuideRates);
}

Result<BacklashCompensation> ProtocolClient::backlash()
{
    return query<BacklashCompensation>(query::backlash, decodeBacklash);
}

Result<int> ProtocolClient::motorPower()
{
    return query<int>(query::motorPower, decodeMotorPower);
}

Result<int> ProtocolClient::mountType()
{
    return query<int>(query::mountType, decodeMountType);
}

Result<MeridianSettings> ProtocolClient::meridianSettings()
{
    return query<MeridianSettings>(query::meridian, decodeMeridianSettings);
}

Result<std::vector<AlignmentPoint>> ProtocolClient::alignmentPoints()
{
    return query<std::vector<AlignmentPoint>>(query::alignments, decodeAlignmentPoints);
}

Result<WifiStationSettings> ProtocolClient::wifiStationSettings()
{
    return query<WifiStationSettings>(query::wifiStationSettings, decodeWifiStationSettings,
                                      kWifiInformationTimeout);
}

Result<WifiStationConnection> ProtocolClient::wifiStationConnection()
{
    return query<WifiStationConnection>(query::wifiStationConnection, decodeWifiStationConnection,
                                        kWifiInformationTimeout);
}

Result<std::vector<WifiAccessPoint>> ProtocolClient::wifiAccessPoints()
{
    return query<std::vector<WifiAccessPoint>>(query::wifiAccessPoints, decodeWifiAccessPoints,
                                               kWifiScanTimeout);
}

Result<void> ProtocolClient::setWifiStationSettings(bool autoConnect, std::string_view ssid,
                                                    std::string_view password)
{
    return command(vixen::wirelessunit::setWifiStationSettings(autoConnect, ssid, password),
                   kWifiReconfigurationTimeout);
}

Result<void> ProtocolClient::setWifiStationConnected(bool connected)
{
    return command(setWifiStationConnection(connected), kWifiReconfigurationTimeout);
}

Result<std::string> ProtocolClient::wifiHotspotSsid()
{
    return query<std::string>(query::wifiHotspotSsid, decodeWifiHotspotSsid);
}

Result<std::string> ProtocolClient::wifiHotspotPassword()
{
    return query<std::string>(query::wifiHotspotPassword, decodeWifiHotspotPassword);
}

Result<int> ProtocolClient::wifiHotspotChannel()
{
    return query<int>(query::wifiHotspotChannel, decodeWifiHotspotChannel);
}

Result<void> ProtocolClient::setWifiHotspotSsidValue(std::string_view ssid)
{
    return command(setWifiHotspotSsid(ssid), std::chrono::milliseconds(1500));
}

Result<void> ProtocolClient::setWifiHotspotPasswordValue(std::string_view password)
{
    return command(setWifiHotspotPassword(password), std::chrono::milliseconds(1500));
}

Result<void> ProtocolClient::setWifiHotspotChannelValue(int channel)
{
    if (channel < 1 || channel > 11)
        return { false, "Wi-Fi access point channel must be between 1 and 11" };
    return command(setWifiHotspotChannel(channel), std::chrono::milliseconds(1500));
}

Result<void> ProtocolClient::slewRaDec(double rightAscensionHours, double declinationDegrees)
{
    return targetCommand(gotoRaDec(rightAscensionHours, declinationDegrees));
}

Result<void> ProtocolClient::slewAltAz(double azimuthDegrees, double altitudeDegrees)
{
    return targetCommand(gotoAltAz(azimuthDegrees, altitudeDegrees));
}

Result<void> ProtocolClient::home()
{
    return targetCommand(gotoHome());
}

Result<bool> ProtocolClient::initializeMount()
{
    auto response = exchange(initializeMountCommand(), std::chrono::milliseconds(500), true);
    if (!response)
        return { std::nullopt, std::move(response.error) };
    return { response.value->accepted, {} };
}

Result<void> ProtocolClient::syncRaDec(double rightAscensionHours, double declinationDegrees)
{
    // ALIGN becomes slower as the saved alignment list grows.  SXP2 captures
    // with 18-19 points show replies arriving 0.53-0.63 s after the request,
    // so the normal 500 ms command timeout leaves a stale ALIGN response for
    // the following MTSTA transaction.
    return command(alignRaDec(rightAscensionHours, declinationDegrees), std::chrono::milliseconds(1500));
}

Result<void> ProtocolClient::syncAltAz(double azimuthDegrees, double altitudeDegrees)
{
    return command(alignAltAz(azimuthDegrees, altitudeDegrees), std::chrono::milliseconds(1500));
}

Result<void> ProtocolClient::deleteAlignmentPoint(std::size_t zeroBasedIndex)
{
    if (zeroBasedIndex >= 20)
        return { false, "alignment index must be between 0 and 19" };
    return command(vixen::wirelessunit::deleteAlignmentPoint(zeroBasedIndex));
}

Result<void> ProtocolClient::abortMotion()
{
    return command(stopMount());
}

Result<void> ProtocolClient::moveBeforeInitialization(double durationSeconds, double raSiderealRate,
                                                      double decSiderealRate)
{
    if (!std::isfinite(durationSeconds) || durationSeconds <= 0 ||
        !std::isfinite(raSiderealRate) || !std::isfinite(decSiderealRate))
        return { false, "MTMV0 duration and axis rates must be finite; duration must be positive" };
    return command(vixen::wirelessunit::moveBeforeInitialization(durationSeconds, raSiderealRate, decSiderealRate));
}

Result<void> ProtocolClient::moveRaDec(double durationSeconds, double raSiderealRate, double decSiderealRate)
{
    if (!std::isfinite(durationSeconds) || durationSeconds <= 0 ||
        !std::isfinite(raSiderealRate) || !std::isfinite(decSiderealRate))
        return { false, "MTMV2 duration and axis rates must be finite; duration must be positive" };
    return command(vixen::wirelessunit::moveRaDec(durationSeconds, raSiderealRate, decSiderealRate));
}

Result<void> ProtocolClient::pulseGuide(double durationSeconds, double raSiderealRate, double decSiderealRate)
{
    return moveRaDec(durationSeconds, raSiderealRate, decSiderealRate);
}

Result<void> ProtocolClient::setTrackingRates(double raSiderealRate, double decSiderealRate)
{
    if (!std::isfinite(raSiderealRate) || !std::isfinite(decSiderealRate))
        return { false, "tracking rates must be finite" };
    return command(setMoveRate(raSiderealRate, decSiderealRate));
}

Result<void> ProtocolClient::setGotoSpeedValue(int speed)
{
    if (speed < 100 || speed > 1000)
        return { false, "GoTo speed must be between 100 and 1000" };
    return command(setGotoSpeed(speed));
}

Result<void> ProtocolClient::setGuideRates(float raSiderealRate, float decSiderealRate)
{
    if (!numeric_validation::finiteInRange(raSiderealRate, 0.1, 10.0) ||
        !numeric_validation::finiteInRange(decSiderealRate, 0.1, 10.0))
        return { false, "guide rates must be finite values from 0.1 to 10.0" };
    if (!numeric_validation::oneDecimalPlace(raSiderealRate) ||
        !numeric_validation::oneDecimalPlace(decSiderealRate))
        return { false, "guide rates must be in 0.1 increments" };

    return command(setAutoguideSpeed(raSiderealRate, decSiderealRate));
}

Result<void> ProtocolClient::setBacklashCompensation(int rightAscension, int declination)
{
    if (rightAscension < 0 || rightAscension > 99 || declination < 0 || declination > 99)
        return { false, "backlash values must be integers from 0 to 99" };
    return command(setBacklash(rightAscension, declination));
}

Result<void> ProtocolClient::setMotorPowerLevel(int power)
{
    if (power < 1 || power > 4)
        return { false, "motor power must be between 1 and 4" };
    return command(setMotorPower(power));
}

Result<void> ProtocolClient::setMountTypeValue(int type)
{
    if (type < 0 || type > 1)
        return { false, "polar alignment type must be 0 or 1" };
    return command(setMountType(type));
}

Result<void> ProtocolClient::setMeridianSettings(int forcedTrackingStopDegrees, int automaticGotoDegrees)
{
    if (!numeric_validation::validMeridianSettings(forcedTrackingStopDegrees, automaticGotoDegrees))
        return { false, "Meridian GoTo flip point magnitude must not exceed the stop point" };
    return command(vixen::wirelessunit::setMeridianSettings(forcedTrackingStopDegrees, automaticGotoDegrees));
}

Result<void> ProtocolClient::setSite(double longitudeDegrees, double latitudeDegrees, float timezoneHours)
{
    if (!std::isfinite(longitudeDegrees) || !std::isfinite(latitudeDegrees) || !std::isfinite(timezoneHours))
        return { false, "SYLOC location and timezone must be finite" };

    return command(setLocation(longitudeDegrees, latitudeDegrees, timezoneHours));
}

Result<void> ProtocolClient::setTime(std::chrono::system_clock::time_point utc)
{
    return command(setUtc(utc));
}

Result<void> ProtocolClient::startTrackingAt(double currentRightAscensionHours, double currentDeclinationDegrees)
{
    return slewRaDec(currentRightAscensionHours, currentDeclinationDegrees);
}

Result<void> ProtocolClient::stopTracking()
{
    return abortMotion();
}

const std::optional<MountStatus> &ProtocolClient::lastMountStatus() const noexcept
{
    return lastStatus_;
}

} // namespace vixen::wirelessunit
