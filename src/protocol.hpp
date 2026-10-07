/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#pragma once

#include <cstddef>
#include <optional>
#include <cstdint>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace vixen::wirelessunit
{

inline constexpr std::string_view kTcpDefaultHost = "0.0.0.0";
inline constexpr unsigned short kTcpDefaultPort = 60023;
inline constexpr std::string_view kUdpDefaultHost = "0.0.0.0";
inline constexpr unsigned short kUdpDefaultPort = 60023;

namespace query
{
inline constexpr std::string_view mountStatus = "$MTSTA?\r\n";
inline constexpr std::string_view location = "$SYLOC?\r\n";
inline constexpr std::string_view systemVersion = "$SYVER?\r\n";
inline constexpr std::string_view mountModel = "$MTMDL?\r\n";
inline constexpr std::string_view mountVersion = "$MTVER?\r\n";
inline constexpr std::string_view gotoSpeed = "$MTGTS?\r\n";
inline constexpr std::string_view autoguideSpeed = "$MTAGS?\r\n";
inline constexpr std::string_view backlash = "$MTBLC?\r\n";
inline constexpr std::string_view mountType = "$MTTYP?\r\n";
inline constexpr std::string_view motorPower = "$MTPWR?\r\n";
inline constexpr std::string_view meridian = "$MTCOM?\r\n";
inline constexpr std::string_view moveRate = "$MTRAT?\r\n";
inline constexpr std::string_view alignments = "$ALLST?\r\n";
inline constexpr std::string_view wifiStationSettings = "$WFSTS?\r\n";
inline constexpr std::string_view wifiStationConnection = "$WFSTC?\r\n";
inline constexpr std::string_view wifiAccessPoints = "$WFAPL?\r\n";
inline constexpr std::string_view wifiHotspotSsid = "$WFSID?\r\n";
inline constexpr std::string_view wifiHotspotPassword = "$WFPWD?\r\n";
inline constexpr std::string_view wifiHotspotChannel = "$WFCHN?\r\n";

// Command names used by requests with run-time parameters. Keeping these next
// to the fixed queries makes protocol.cpp a formatter rather than a second,
// implicit command catalogue.
inline constexpr std::string_view gotoTargetName = "MTTGT";
inline constexpr std::string_view alignmentName = "ALIGN";
inline constexpr std::string_view deleteAlignmentName = "ALDEL";
inline constexpr std::string_view moveBeforeInitializationName = "MTMV0";
inline constexpr std::string_view moveRaDecName = "MTMV2";
inline constexpr std::string_view moveRateName = "MTRAT";
inline constexpr std::string_view gotoSpeedName = "MTGTS";
inline constexpr std::string_view autoguideSpeedName = "MTAGS";
inline constexpr std::string_view backlashName = "MTBLC";
inline constexpr std::string_view motorPowerName = "MTPWR";
inline constexpr std::string_view mountTypeName = "MTTYP";
inline constexpr std::string_view meridianName = "MTCOM";
inline constexpr std::string_view wifiStationSettingsName = "WFSTS";
inline constexpr std::string_view wifiStationConnectionName = "WFSTC";
inline constexpr std::string_view wifiHotspotSsidName = "WFSID";
inline constexpr std::string_view wifiHotspotPasswordName = "WFPWD";
inline constexpr std::string_view wifiHotspotChannelName = "WFCHN";
inline constexpr std::string_view locationName = "SYLOC";
inline constexpr std::string_view utcName = "SYUTM";

// Complete fixed-form commands and the one parameterized query prefix.
inline constexpr std::string_view gotoHome = "$MTTGT=1,270,0,HOME\r\n";
inline constexpr std::string_view initializeMount = "$MTINI=1\r\n";
inline constexpr std::string_view stopMount = "$MTSTP=1\r\n";
inline constexpr std::string_view wifiStationConnect = "$WFSTC=1\r\n";
inline constexpr std::string_view wifiStationDisconnect = "$WFSTC=0\r\n";
inline constexpr std::string_view destinationPierSidePrefix = "$MTCPI?3,";
} // namespace query

struct Response
{
    std::string command;
    std::vector<std::string> fields;
    bool accepted { false };
};

// Parses one complete response line. No coordinate or enum semantics are
// applied here; those require validation against real Wireless Unit responses.
std::optional<Response> parseResponse(std::string_view wireData);

struct MountStatus
{
    double modifiedJulianDate { 0 };
    int status { 0 };
    double mechanicalX { 0 };
    double mechanicalY { 0 };
    double azimuthDegrees { 0 };
    double altitudeDegrees { 0 };
    double rightAscensionHours { 0 };
    double declinationDegrees { 0 };
    int targetObjectType { 0 };
};

struct Location
{
    double longitudeDegrees { 0 };
    double latitudeDegrees { 0 };
    float timezoneHours { 0 };
};

struct MountIdentity
{
    int model { 0 };
    int maximumGotoSpeed { 0 };
    // Firmware 2.60 emits a third MTMDL field. It appears to be the second
    // axis maximum. Older two-field responses use field 1 for both axes.
    int maximumSecondarySpeed { 0 };
};

struct GuideRates
{
    float rightAscensionSidereal { 0 };
    float declinationSidereal { 0 };
};

struct BacklashCompensation
{
    int rightAscension { 0 };
    int declination { 0 };
};

struct MeridianSettings
{
    int forcedTrackingStopDegrees { 0 };
    int automaticGotoDegrees { 0 };
};

struct AlignmentPoint
{
    int state { 0 };
    double modifiedJulianDate { 0 };
    std::string label;
};

struct WifiStationSettings
{
    bool autoConnect { false };
    bool stationModeEnabled { false };
    std::string ssid;
};

struct WifiStationConnection
{
    bool connected { false };
    std::string ipAddress;
};

struct WifiAccessPoint
{
    int type { 0 };
    std::string ssid;
    int signalDbm { 0 };
};

std::optional<MountStatus> decodeMountStatus(const Response &response);
std::optional<Location> decodeLocation(const Response &response);
std::optional<MountIdentity> decodeMountIdentity(const Response &response);
std::optional<double> decodeSystemVersion(const Response &response);
std::optional<float> decodeMountVersion(const Response &response);
std::optional<int> decodeGotoSpeed(const Response &response);
std::optional<GuideRates> decodeGuideRates(const Response &response);
std::optional<BacklashCompensation> decodeBacklash(const Response &response);
std::optional<int> decodeMotorPower(const Response &response);
std::optional<int> decodeMountType(const Response &response);
std::optional<MeridianSettings> decodeMeridianSettings(const Response &response);
std::optional<std::vector<AlignmentPoint>> decodeAlignmentPoints(const Response &response);
std::optional<WifiStationSettings> decodeWifiStationSettings(const Response &response);
std::optional<WifiStationConnection> decodeWifiStationConnection(const Response &response);
std::optional<std::vector<WifiAccessPoint>> decodeWifiAccessPoints(const Response &response);
std::optional<std::string> decodeWifiHotspotSsid(const Response &response);
std::optional<std::string> decodeWifiHotspotPassword(const Response &response);
std::optional<int> decodeWifiHotspotChannel(const Response &response);

std::string gotoRaDec(double rightAscensionHours, double declinationDegrees);
std::string gotoAltAz(double azimuthDegrees, double altitudeDegrees);
std::string gotoHome();
std::string initializeMountCommand();
std::string alignmentLabelRaDec(double rightAscensionHours, double declinationDegrees);
std::string alignmentLabelAltAz(double azimuthDegrees, double altitudeDegrees);
std::string alignRaDec(double rightAscensionHours, double declinationDegrees);
std::string alignAltAz(double azimuthDegrees, double altitudeDegrees);
std::string deleteAlignmentPoint(std::size_t zeroBasedIndex);
std::string moveBeforeInitialization(double durationSeconds, double raSiderealRate, double decSiderealRate);
std::string moveRaDec(double durationSeconds, double raSiderealRate, double decSiderealRate);
std::string stopMount();
std::string setMoveRate(double xSiderealRate, double ySiderealRate);
std::string setGotoSpeed(int speed);
std::string setAutoguideSpeed(float raSiderealRate, float decSiderealRate);
std::string setBacklash(int rightAscension, int declination);
std::string setMotorPower(int power);
std::string setMountType(int type);
std::string setMeridianSettings(int forcedTrackingStopDegrees, int automaticGotoDegrees);
std::string setWifiStationSettings(bool autoConnect, std::string_view ssid, std::string_view password);
std::string setWifiStationConnection(bool connected);
std::string setWifiHotspotSsid(std::string_view ssid);
std::string setWifiHotspotPassword(std::string_view password);
std::string setWifiHotspotChannel(int channel);
std::string setLocation(double longitudeDegrees, double latitudeDegrees, float timezoneHours);
std::string setUtc(std::chrono::system_clock::time_point utc);
std::string destinationPierSide(double rightAscensionHours, double declinationDegrees);

constexpr double siderealMultiplierToDegreesPerSecond(double multiplier)
{
    return multiplier * 0.0041780555555555555;
}

constexpr double degreesPerSecondToSiderealMultiplier(double degreesPerSecond)
{
    return degreesPerSecond / 0.0041780555555555555;
}

constexpr bool mountStatusIsInitialized(const MountStatus &status)
{
    // An uninitialized SXP2 reports an MJD near the 1858-11-17 epoch
    // (0.000442 and 0.001110 observed). A live initialized unit reports the
    // current MJD, approximately 61,000 in 2026.
    return status.modifiedJulianDate >= 10000.0;
}

} // namespace vixen::wirelessunit
