/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#pragma once

#include "protocol.hpp"
#include "transport.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace vixen::wirelessunit
{

template <typename T>
struct Result
{
    std::optional<T> value;
    std::string error;

    explicit operator bool() const noexcept
    {
        return value.has_value();
    }
};

template <>
struct Result<void>
{
    bool success { false };
    std::string error;

    explicit operator bool() const noexcept
    {
        return success;
    }
};

class ProtocolClient
{
    public:
        explicit ProtocolClient(std::unique_ptr<ITransport> transport);

        bool connect(std::string &error);
        void disconnect() noexcept;
        bool isConnected() const noexcept;

        Result<double> systemVersion();
        Result<float> mountVersion();
        Result<MountIdentity> mountIdentity();
        Result<Location> location();
        Result<MountStatus> mountStatus();
        Result<int> gotoSpeed();
        Result<GuideRates> guideRates();
        Result<BacklashCompensation> backlash();
        Result<int> motorPower();
        Result<int> mountType();
        Result<MeridianSettings> meridianSettings();
        Result<std::vector<AlignmentPoint>> alignmentPoints();
        Result<WifiStationSettings> wifiStationSettings();
        Result<WifiStationConnection> wifiStationConnection();
        Result<std::vector<WifiAccessPoint>> wifiAccessPoints();
        Result<void> setWifiStationSettings(bool autoConnect, std::string_view ssid,
                                            std::string_view password);
        Result<void> setWifiStationConnected(bool connected);
        Result<std::string> wifiHotspotSsid();
        Result<std::string> wifiHotspotPassword();
        Result<int> wifiHotspotChannel();
        Result<void> setWifiHotspotSsidValue(std::string_view ssid);
        Result<void> setWifiHotspotPasswordValue(std::string_view password);
        Result<void> setWifiHotspotChannelValue(int channel);

        Result<void> slewRaDec(double rightAscensionHours, double declinationDegrees);
        Result<void> slewAltAz(double azimuthDegrees, double altitudeDegrees);
        Result<void> home();
        // true: initialized now; false: Wireless Unit returned NG because this
        // power session was already initialized.
        Result<bool> initializeMount();
        Result<void> syncRaDec(double rightAscensionHours, double declinationDegrees);
        Result<void> syncAltAz(double azimuthDegrees, double altitudeDegrees);
        Result<void> deleteAlignmentPoint(std::size_t zeroBasedIndex);
        Result<void> abortMotion();
        Result<void> stopBeforeInitialization();
        Result<void> moveBeforeInitialization(double durationSeconds, double raSiderealRate,
                                              double decSiderealRate);
        Result<void> moveRaDec(double durationSeconds, double raSiderealRate, double decSiderealRate);
        Result<void> pulseGuide(double durationSeconds, double raSiderealRate, double decSiderealRate);
        Result<void> setTrackingRates(double raSiderealRate, double decSiderealRate);
        Result<void> setGotoSpeedValue(int speed);
        Result<void> setGuideRates(float raSiderealRate, float decSiderealRate);
        Result<void> setBacklashCompensation(int rightAscension, int declination);
        Result<void> setMotorPowerLevel(int power);
        Result<void> setMountTypeValue(int type);
        Result<void> setMeridianSettings(int forcedTrackingStopDegrees, int automaticGotoDegrees);
        Result<void> setSite(double longitudeDegrees, double latitudeDegrees, float timezoneHours);
        Result<void> setTime(std::chrono::system_clock::time_point utc);
        Result<void> startTrackingAt(double currentRightAscensionHours, double currentDeclinationDegrees);
        Result<void> stopTracking();

        const std::optional<MountStatus> &lastMountStatus() const noexcept;

    private:
        Result<Response> exchange(std::string_view request,
                                  std::chrono::milliseconds timeout = std::chrono::milliseconds(500),
                                  bool acceptNegativeResponse = false);
        Result<void> command(std::string_view request,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds(500));
        Result<void> retryIdempotentStop(std::string_view request);
        Result<void> targetCommand(std::string_view request);

        template <typename T, typename Decoder>
        Result<T> query(std::string_view request, Decoder decoder,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(500))
        {
            auto response = exchange(request, timeout);
            if (!response)
                return { std::nullopt, std::move(response.error) };
            auto decoded = decoder(*response.value);
            if (!decoded)
                return { std::nullopt, "response fields did not match the expected schema" };
            return { std::move(decoded), {} };
        }

        std::unique_ptr<ITransport> transport_;
        std::optional<MountStatus> lastStatus_;
};

} // namespace vixen::wirelessunit
