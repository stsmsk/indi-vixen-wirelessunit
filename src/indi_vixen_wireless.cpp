/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#include "indi_vixen_wireless.hpp"

#include "discovery.hpp"
#include "indicom.h"
#include "motion_safety.hpp"
#include "motion_timing.hpp"
#include "numeric_validation.hpp"
#include "simulator_transport.hpp"
#include "wifi_ap_update.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

namespace
{

constexpr double kMinimumWirelessFirmware = 1.39;
constexpr std::uint32_t kMountStatusPollingMilliseconds =
    vixen::wirelessunit::motion_timing::statusPollingMilliseconds;
constexpr int kAutomaticDiscoveryRetryMilliseconds = 5000;
constexpr unsigned kAutomaticDiscoveryInitialReportedAttempts = 4;
constexpr int kAutomaticDiscoverySteadyReportMilliseconds = 30000;
static_assert(kAutomaticDiscoverySteadyReportMilliseconds % kAutomaticDiscoveryRetryMilliseconds == 0);
constexpr unsigned kAutomaticDiscoverySteadyReportAttempts =
    kAutomaticDiscoverySteadyReportMilliseconds / kAutomaticDiscoveryRetryMilliseconds;
constexpr int kDefaultSlewRateIndex = 7; // 100x in the ascending rate table.
constexpr const char *kWifiTab = "Wi-Fi Station";
constexpr const char *kHotspotTab = "Wi-Fi Access Point";

bool validWifiField(const std::string &value, std::size_t maximumBytes, bool allowEmpty)
{
    return (allowEmpty || !value.empty()) && value.size() <= maximumBytes &&
           value.find_first_of(",\r\n") == std::string::npos;
}

std::unique_ptr<VixenWirelessUnit> driver = std::make_unique<VixenWirelessUnit>();

std::string mountModelName(int model)
{
    switch (model)
    {
        case 1: return "AXD / AXD2";
        case 2: return "SXP";
        case 5: return "SXD2";
        case 6: return "AP";
        case 7: return "SX2";
        case 10: return "AXJ";
        case 11: return "SXP2";
        default: return "Unknown (" + std::to_string(model) + ')';
    }
}

} // namespace

VixenWirelessUnit::VixenWirelessUnit() : INDI::GuiderInterface(this)
{
    // INDI's standard driver version is a major/minor pair. The complete
    // package version is exposed separately as DRIVER_BUILD below.
    setVersion(0, 1);
    setTelescopeConnection(CONNECTION_NONE);
    SetTelescopeCapability(
        TELESCOPE_CAN_GOTO |
        TELESCOPE_CAN_SYNC |
        TELESCOPE_CAN_PARK |
        TELESCOPE_CAN_ABORT |
        TELESCOPE_HAS_TIME |
        TELESCOPE_HAS_LOCATION |
        TELESCOPE_CAN_CONTROL_TRACK,
        10);
}

VixenWirelessUnit::~VixenWirelessUnit()
{
    cancelAutomaticDiscoveryConnect(nullptr, false);
}

const char *VixenWirelessUnit::getDefaultName()
{
    return "Vixen Wireless Unit";
}

bool VixenWirelessUnit::initProperties()
{
    INDI::Telescope::initProperties();
    INDI::GuiderInterface::initProperties(MOTION_TAB);
    setDriverInterface(getDriverInterface() | GUIDER_INTERFACE);

    // Keep the standard INDI motion properties so KStars/Ekos can control
    // them normally, but place the four direction buttons on Main Control.
    MovementNSSP.fill(getDeviceName(), "TELESCOPE_MOTION_NS", "Motion N/S", MAIN_CONTROL_TAB,
                      IP_RW, ISR_ATMOST1, 60, IPS_IDLE);
    MovementWESP.fill(getDeviceName(), "TELESCOPE_MOTION_WE", "Motion W/E", MAIN_CONTROL_TAB,
                      IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    IUFillSwitch(&ConnectionModeS[0], "AUTO_DISCOVERY", "Automatic discovery", ISS_ON);
    IUFillSwitch(&ConnectionModeS[1], "MANUAL_IP", "Manual IP", ISS_OFF);
    IUFillSwitchVector(&ConnectionModeSP, ConnectionModeS, 2, getDeviceName(),
                       "WIRELESS_CONNECTION_MODE", "Connection mode", CONNECTION_TAB,
                       IP_RW, ISR_1OFMANY, 60, IPS_IDLE);

    IUFillText(&EndpointT[0], "HOST", "Host", vixen::wirelessunit::kUdpDefaultHost.data());
    IUFillText(&EndpointT[1], "PORT", "Port", "60023");
    IUFillTextVector(&EndpointTP, EndpointT, 2, getDeviceName(), "WIRELESS_ENDPOINT", "Wireless Unit",
                     CONNECTION_TAB, IP_WO, 60, IPS_IDLE);

    // Keep the transport model internally so TCP can be restored if future
    // firmware requires it, but Wireless Unit communication is currently UDP
    // only and the selector is intentionally not exposed in the UI.
    IUFillSwitch(&TransportS[0], "TCP", "TCP", ISS_OFF);
    IUFillSwitch(&TransportS[1], "UDP", "UDP", ISS_ON);
    IUFillSwitchVector(&TransportSP, TransportS, 2, getDeviceName(), "WIRELESS_TRANSPORT", "Transport",
                       CONNECTION_TAB, IP_RW, ISR_1OFMANY, 60, IPS_IDLE);

    IUFillSwitch(&DiscoveryS[0], "SEARCH", "Search", ISS_OFF);
    IUFillSwitchVector(&DiscoverySP, DiscoveryS, 1, getDeviceName(), "WIRELESS_DISCOVERY", "Device discovery",
                       CONNECTION_TAB, IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    IUFillSwitch(&UseDeviceSettingsS[0], "USE_DEVICE_SETTINGS", "Use device settings", ISS_ON);
    IUFillSwitchVector(&UseDeviceSettingsSP, UseDeviceSettingsS, 1, getDeviceName(), "WIRELESS_SETTINGS_SOURCE",
                       "Mount settings", MOTION_TAB, IP_RW, ISR_NOFMANY, 60, IPS_IDLE);

    IUFillSwitch(&GotoSpeedS[0], "GOTO_100", "100x", ISS_OFF);
    IUFillSwitch(&GotoSpeedS[1], "GOTO_200", "200x", ISS_OFF);
    IUFillSwitch(&GotoSpeedS[2], "GOTO_400", "400x", ISS_OFF);
    IUFillSwitch(&GotoSpeedS[3], "GOTO_700", "700x", ISS_OFF);
    IUFillSwitch(&GotoSpeedS[4], "GOTO_1000", "1000x", ISS_ON);
    IUFillSwitchVector(&GotoSpeedSP, GotoSpeedS, 5, getDeviceName(), "WIRELESS_GOTO_SPEED", "GoTo speed",
                       MOTION_TAB, IP_RW, ISR_1OFMANY, 60, IPS_IDLE);

    IUFillNumber(&HomePositionN[0], "HOME_AZ", "Azimuth", "%.1f", 0, 360, 0, 270);
    IUFillNumber(&HomePositionN[1], "HOME_ALT", "Altitude", "%.1f", -90, 90, 0, 0);
    IUFillNumberVector(&HomePositionNP, HomePositionN, 2, getDeviceName(), "WIRELESS_HOME_POSITION", "Home position",
                       MAIN_CONTROL_TAB, IP_RO, 60, IPS_OK);

    IUFillSwitch(&SetHomePositionS[0], "SET", "Set", ISS_OFF);
    IUFillSwitchVector(&SetHomePositionSP, SetHomePositionS, 1, getDeviceName(), "WIRELESS_SET_HOME_POSITION",
                       "Home set", MAIN_CONTROL_TAB, IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    IUFillSwitch(&AssumeHomeS[0], "ON_CONNECT", "On connect", ISS_OFF);
    IUFillSwitchVector(&AssumeHomeSP, AssumeHomeS, 1, getDeviceName(), "WIRELESS_ASSUME_HOME",
                       "Assume HOME", MAIN_CONTROL_TAB, IP_RW, ISR_NOFMANY, 60, IPS_IDLE);

    IUFillText(&FirmwareT[0], "WIRELESS_VERSION", "Wireless firmware", "Unknown");
    IUFillText(&FirmwareT[1], "MOUNT_MODEL", "Mount model", "Unknown");
    IUFillText(&FirmwareT[2], "MOUNT_MAX_GOTO", "Maximum GoTo", "Unknown");
    IUFillTextVector(&FirmwareTP, FirmwareT, 3, getDeviceName(), "WIRELESS_INFO", "Device info",
                     INFO_TAB, IP_RO, 60, IPS_IDLE);
    IUFillText(&DriverBuildT[0], "DRIVER_VERSION", "Driver version", VIXEN_DRIVER_VERSION);
    IUFillTextVector(&DriverBuildTP, DriverBuildT, 1, getDeviceName(), "DRIVER_BUILD", "Driver build",
                     INFO_TAB, IP_RO, 60, IPS_OK);

    IUFillNumber(&GuideRateN[0], "GUIDE_RATE_WE", "W/E rate", "%.2f", 0.1, 10, 0.1, 1.0);
    IUFillNumber(&GuideRateN[1], "GUIDE_RATE_NS", "N/S rate", "%.2f", 0.1, 10, 0.1, 1.0);
    IUFillNumberVector(&GuideRateNP, GuideRateN, 2, getDeviceName(), "WIRELESS_GUIDE_RATE", "Guide rate",
                       MOTION_TAB, IP_RW, 60, IPS_IDLE);

    IUFillNumber(&BacklashN[0], "BACKLASH_RA", "RA", "%.0f", 0, 99, 1, 0);
    IUFillNumber(&BacklashN[1], "BACKLASH_DEC", "DEC", "%.0f", 0, 99, 1, 0);
    IUFillNumberVector(&BacklashNP, BacklashN, 2, getDeviceName(), "WIRELESS_BACKLASH", "Backlash compensation",
                       MOTION_TAB, IP_RW, 60, IPS_IDLE);

    IUFillSwitch(&MotorPowerS[0], "POWER_1", "1", ISS_OFF);
    IUFillSwitch(&MotorPowerS[1], "POWER_2", "2", ISS_OFF);
    IUFillSwitch(&MotorPowerS[2], "POWER_3", "3", ISS_ON);
    IUFillSwitch(&MotorPowerS[3], "POWER_4", "4", ISS_OFF);
    IUFillSwitchVector(&MotorPowerSP, MotorPowerS, 4, getDeviceName(), "WIRELESS_MOTOR_POWER", "Motor power",
                       MOTION_TAB, IP_RW, ISR_1OFMANY, 60, IPS_IDLE);

    IUFillSwitch(&MountTypeS[0], "POLAR_ALIGNED", "Polar aligned", ISS_OFF);
    IUFillSwitch(&MountTypeS[1], "NOT_POLAR_ALIGNED", "Not polar aligned", ISS_ON);
    IUFillSwitchVector(&MountTypeSP, MountTypeS, 2, getDeviceName(), "WIRELESS_MOUNT_TYPE", "Polar alignment",
                       MOTION_TAB, IP_RW, ISR_1OFMANY, 60, IPS_IDLE);

    IUFillSwitch(&MeridianDirectionS[0], "EAST", "East", ISS_ON);
    IUFillSwitch(&MeridianDirectionS[1], "WEST", "West", ISS_OFF);
    IUFillSwitchVector(&MeridianDirectionSP, MeridianDirectionS, 2, getDeviceName(),
                       "WIRELESS_MERIDIAN_DIRECTION", "GoTo flip dir", MOTION_TAB, IP_RW,
                       ISR_1OFMANY, 60, IPS_IDLE);

    IUFillNumber(&MeridianN[0], "STOP_POINT", "Stop point", "%.0f", 0, 90, 1, 3);
    IUFillNumber(&MeridianN[1], "GOTO_FLIP_POINT", "GoTo flip point", "%.0f", 0, 90, 1, 0);
    IUFillNumberVector(&MeridianNP, MeridianN, 2, getDeviceName(), "WIRELESS_MERIDIAN", "Meridian behavior",
                       MOTION_TAB, IP_RW, 60, IPS_IDLE);

    IUFillText(&AlignmentT[0], "COUNT", "Count", "0");
    for (std::size_t index = 0; index < kMaximumDisplayedAlignmentPoints; ++index)
    {
        char name[24] {};
        char label[24] {};
        std::snprintf(name, sizeof(name), "POINT_%02zu", index + 1);
        std::snprintf(label, sizeof(label), "Point %zu", index + 1);
        IUFillText(&AlignmentT[index + 1], name, label, "--");
    }
    IUFillTextVector(&AlignmentTP, AlignmentT, kMaximumDisplayedAlignmentPoints + 1, getDeviceName(),
                     "WIRELESS_ALIGNMENT_LIST", "Alignment points", "Alignment", IP_RO, 60, IPS_IDLE);

    IUFillSwitch(&AlignmentRefreshS[0], "REFRESH", "Refresh", ISS_OFF);
    IUFillSwitchVector(&AlignmentRefreshSP, AlignmentRefreshS, 1, getDeviceName(), "WIRELESS_ALIGNMENT_REFRESH",
                       "Alignment list", "Alignment", IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    for (std::size_t index = 0; index < kMaximumDisplayedAlignmentPoints; ++index)
    {
        char name[24] {};
        char label[24] {};
        std::snprintf(name, sizeof(name), "POINT_%02zu", index + 1);
        std::snprintf(label, sizeof(label), "Point %zu", index + 1);
        IUFillSwitch(&AlignmentSelectionS[index], name, label, ISS_OFF);
    }
    IUFillSwitchVector(&AlignmentSelectionSP, AlignmentSelectionS, kMaximumDisplayedAlignmentPoints,
                       getDeviceName(), "WIRELESS_ALIGNMENT_SELECTION", "Selected point", "Alignment",
                       IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    IUFillSwitch(&AlignmentDeleteS[0], "DELETE_SELECTED", "Delete selected", ISS_OFF);
    IUFillSwitch(&AlignmentDeleteS[1], "DELETE_ALL", "Delete all", ISS_OFF);
    IUFillSwitchVector(&AlignmentDeleteSP, AlignmentDeleteS, 2, getDeviceName(), "WIRELESS_ALIGNMENT_DELETE",
                       "Delete alignment", "Alignment", IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    IUFillText(&WifiInformationT[0], "STATUS", "Station status", "Not read");
    IUFillText(&WifiInformationT[1], "IP_ADDRESS", "Station IP", "--");
    IUFillText(&WifiInformationT[2], "AUTO_CONNECT", "Auto-connect", "--");
    IUFillText(&WifiInformationT[3], "SSID", "Configured SSID", "--");
    IUFillTextVector(&WifiInformationTP, WifiInformationT, 4, getDeviceName(), "WIRELESS_WIFI_INFO",
                     "Station info", kWifiTab, IP_RO, 60, IPS_IDLE);

    IUFillSwitch(&WifiRefreshS[0], "REFRESH", "Refresh", ISS_OFF);
    IUFillSwitchVector(&WifiRefreshSP, WifiRefreshS, 1, getDeviceName(), "WIRELESS_WIFI_REFRESH",
                       "Station info", kWifiTab, IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    IUFillText(&WifiSsidT[0], "SSID", "SSID", "");
    IUFillText(&WifiSsidT[1], "PASSWORD", "Password", "");
    IUFillTextVector(&WifiSsidTP, WifiSsidT, 2, getDeviceName(), "WIRELESS_WIFI_STATION_SETTINGS",
                     "Station settings", kWifiTab, IP_WO, 60, IPS_IDLE);

    IUFillSwitch(&WifiAutoConnectS[0], "ON", "On", ISS_ON);
    IUFillSwitch(&WifiAutoConnectS[1], "OFF", "Off", ISS_OFF);
    IUFillSwitchVector(&WifiAutoConnectSP, WifiAutoConnectS, 2, getDeviceName(),
                       "WIRELESS_WIFI_AUTO_CONNECT", "Auto-connect", kWifiTab,
                       IP_RW, ISR_1OFMANY, 60, IPS_IDLE);

    IUFillSwitch(&WifiStationControlS[0], "CONNECT", "Connect", ISS_OFF);
    IUFillSwitch(&WifiStationControlS[1], "DISCONNECT", "Disconnect", ISS_OFF);
    IUFillSwitchVector(&WifiStationControlSP, WifiStationControlS, 2, getDeviceName(),
                       "WIRELESS_WIFI_STATION_CONTROL", "Station link", kWifiTab,
                       IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    IUFillText(&WifiAccessPointsT[0], "COUNT", "Count", "Not scanned");
    for (std::size_t index = 0; index < kMaximumDisplayedWifiAccessPoints; ++index)
    {
        char name[24] {};
        char label[24] {};
        std::snprintf(name, sizeof(name), "ACCESS_POINT_%02zu", index + 1);
        std::snprintf(label, sizeof(label), "Network %zu", index + 1);
        IUFillText(&WifiAccessPointsT[index + 1], name, label, "--");
    }
    IUFillTextVector(&WifiAccessPointsTP, WifiAccessPointsT, kMaximumDisplayedWifiAccessPoints + 1,
                     getDeviceName(), "WIRELESS_WIFI_AP_LIST", "Scan result", kWifiTab,
                     IP_RO, 60, IPS_IDLE);
    // SSIDs are presented by the selectable switch vector below. Keep this
    // property as a stable numeric result count.
    WifiAccessPointsTP.ntp = 1;

    for (std::size_t index = 0; index < kMaximumDisplayedWifiAccessPoints; ++index)
    {
        char name[32] {};
        std::snprintf(name, sizeof(name), "ACCESS_POINT_%02zu", index + 1);
        IUFillSwitch(&WifiAccessPointSelectionS[index], name, "--", ISS_OFF);
    }
    IUFillSwitchVector(&WifiAccessPointSelectionSP, WifiAccessPointSelectionS,
                       kMaximumDisplayedWifiAccessPoints, getDeviceName(),
                       "WIRELESS_WIFI_AP_SELECTION", "Networks", kWifiTab,
                       IP_RW, ISR_1OFMANY, 60, IPS_IDLE);
    WifiAccessPointSelectionSP.nsp = 0;

    IUFillSwitch(&WifiScanS[0], "SCAN", "Scan", ISS_OFF);
    IUFillSwitchVector(&WifiScanSP, WifiScanS, 1, getDeviceName(), "WIRELESS_WIFI_SCAN",
                       "Search Networks", kWifiTab, IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    IUFillText(&HotspotInformationT[0], "CURRENT_SSID", "Current SSID", "Not read");
    IUFillText(&HotspotInformationT[1], "CURRENT_PASSWORD", "Current password", "Not read");
    IUFillText(&HotspotInformationT[2], "CURRENT_CHANNEL", "Current channel", "Not read");
    IUFillTextVector(&HotspotInformationTP, HotspotInformationT, 3, getDeviceName(),
                     "WIRELESS_HOTSPOT_INFO", "AP info", kHotspotTab,
                     IP_RO, 60, IPS_IDLE);

    IUFillSwitch(&HotspotRefreshS[0], "REFRESH", "Refresh", ISS_OFF);
    IUFillSwitchVector(&HotspotRefreshSP, HotspotRefreshS, 1, getDeviceName(),
                       "WIRELESS_HOTSPOT_REFRESH", "AP info", kHotspotTab,
                       IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    IUFillText(&HotspotSettingsT[0], "SSID", "SSID", "");
    IUFillText(&HotspotSettingsT[1], "PASSWORD", "Password", "");
    IUFillText(&HotspotSettingsT[2], "CHANNEL", "Channel (1-11)", "1");
    IUFillTextVector(&HotspotSettingsTP, HotspotSettingsT, 3, getDeviceName(),
                     "WIRELESS_HOTSPOT_SETTINGS", "AP settings", kHotspotTab,
                     IP_WO, 60, IPS_IDLE);

    static constexpr std::array<const char *, 10> slewRateLabels {
        "0.5x", "1x", "2x", "4x", "10x", "20x", "40x", "100x", "200x", "400x"
    };
    // These names are persisted in INDI configuration. Keep each identifier
    // explicit and aligned with the multiplier shown by its label.
    static constexpr std::array<const char *, 10> slewRateNames {
        "SLEW_0_5X", "SLEW_1X", "SLEW_2X", "SLEW_4X", "SLEW_10X",
        "SLEW_20X", "SLEW_40X", "SLEW_100X", "SLEW_200X", "SLEW_400X"
    };
    for (std::size_t index = 0; index < slewRateLabels.size(); ++index)
    {
        SlewRateSP[index].setName(slewRateNames[index]);
        SlewRateSP[index].setLabel(slewRateLabels[index]);
    }
    SlewRateSP.setLabel("Slew rate");
    SlewRateSP.reset();
    SlewRateSP[kDefaultSlewRateIndex].setState(ISS_ON);

    addDebugControl();
    addSimulationControl();
    // Match the official ASCOM driver's MTSTA polling interval exactly.
    setDefaultPollingPeriod(kMountStatusPollingMilliseconds);
    setPollingPeriodRange(kMountStatusPollingMilliseconds, kMountStatusPollingMilliseconds);
    TrackState = SCOPE_IDLE;
    return true;
}

void VixenWirelessUnit::ISGetProperties(const char *dev)
{
    // DefaultDevice initializes our custom vectors during the first base
    // ISGetProperties call. Never advertise DiscoverySP before that call:
    // on a fresh driver process it would have an empty name/device and nsp=0.
    INDI::Telescope::ISGetProperties(dev);
    defineProperty(&AssumeHomeSP);
    // Advertise discovery before this driver's endpoint property. The
    // standard Telescope connection vectors have already been advertised.
    defineProperty(&DiscoverySP);
    defineProperty(&ConnectionModeSP);
    defineProperty(&EndpointTP);
    defineProperty(&DriverBuildTP);
    // The endpoint is required before Connect, whereas Ekos normally requests
    // a complete CONFIG_LOAD only after a connection succeeds. Load this one
    // property once when it is first exposed so a saved Host/Port can actually
    // be used to establish the next connection.
    if (!connectionModeConfigLoaded_)
    {
        loadConfig(true, ConnectionModeSP.name);
        connectionModeConfigLoaded_ = true;
    }
    if (!endpointConfigLoaded_)
    {
        loadConfig(true, EndpointTP.name);
        endpointConfigLoaded_ = true;
    }
    if (!assumeHomeConfigLoaded_)
    {
        loadConfig(true, AssumeHomeSP.name);
        assumeHomeConfigLoaded_ = true;
    }
    // Define all driver-specific Motion Control properties last. INDI has no
    // explicit tab-order field, so this is a best-effort request to KStars to
    // place Motion Control toward the right while keeping Mount settings first
    // among this driver's Motion Control properties.
    defineProperty(&UseDeviceSettingsSP);
    // SlewRateSP is an INDI standard property already advertised by
    // Telescope::ISGetProperties(). Do not delete/re-advertise it here: some
    // clients remove it permanently after the delete notification.
    defineProperty(&GotoSpeedSP);
    defineProperty(&GuideRateNP);
    defineProperty(&BacklashNP);
    defineProperty(&MotorPowerSP);
    defineProperty(&MountTypeSP);
    // KStars presents these two custom properties in reverse notification
    // order, so send direction first to display behavior first.
    defineProperty(&MeridianDirectionSP);
    defineProperty(&MeridianNP);

    // Re-emit dynamic HOME properties when a client requests the complete
    // property list after the device is already connected.
    if (isConnected())
    {
        defineProperty(&HomePositionNP);
        homePositionDefined_ = true;
        if (initializationState_ == MountInitializationState::uninitialized)
        {
            defineProperty(&SetHomePositionSP);
            homeSetDefined_ = true;
        }
        defineProperty(&AlignmentRefreshSP);
        defineProperty(&AlignmentTP);
        if (!alignmentPoints_.empty())
        {
            AlignmentSelectionSP.nsp = static_cast<int>(
                std::min(alignmentPoints_.size(), kMaximumDisplayedAlignmentPoints));
            defineProperty(&AlignmentSelectionSP);
            alignmentSelectionDefined_ = true;
        }
        defineProperty(&AlignmentDeleteSP);
        alignmentPropertiesDefined_ = true;
        if (!alignmentListLoaded_)
            readAlignmentPoints();

        defineProperty(&WifiInformationTP);
        defineProperty(&WifiRefreshSP);
        defineProperty(&WifiSsidTP);
        defineProperty(&WifiAutoConnectSP);
        defineProperty(&WifiStationControlSP);
        defineProperty(&WifiAccessPointsTP);
        if (!wifiAccessPointSsids_.empty())
        {
            WifiAccessPointSelectionSP.nsp = static_cast<int>(wifiAccessPointSsids_.size());
            defineProperty(&WifiAccessPointSelectionSP);
            wifiAccessPointSelectionDefined_ = true;
        }
        defineProperty(&WifiScanSP);
        wifiPropertiesDefined_ = true;
        if (!wifiInformationLoaded_)
            readWifiStationInformation();

        defineProperty(&HotspotInformationTP);
        defineProperty(&HotspotRefreshSP);
        defineProperty(&HotspotSettingsTP);
        hotspotPropertiesDefined_ = true;
        if (!hotspotInformationLoaded_)
            readHotspotInformation();
    }
}

bool VixenWirelessUnit::updateProperties()
{
    INDI::Telescope::updateProperties();
    INDI::GuiderInterface::updateProperties();

    // INDI::Telescope defines Tracking near the start of Main Control but
    // Parking after the site/time properties. Re-emit just these two standard
    // properties once per connection so clients place Parking immediately
    // after Tracking. Their values and behavior are left unchanged.
    if (isConnected() && !mainControlOrderApplied_)
    {
        deleteProperty(TrackStateSP);
        deleteProperty(ParkSP);
        defineProperty(TrackStateSP);
        defineProperty(ParkSP);
        mainControlOrderApplied_ = true;
    }
    else if (!isConnected())
    {
        mainControlOrderApplied_ = false;
    }

    updateHomeProperties();

    if (isConnected())
    {
        defineProperty(&FirmwareTP);
    }
    else
    {
        deleteProperty(FirmwareTP.name);
    }
    updateAlignmentProperties();
    updateWifiProperties();
    updateHotspotProperties();
    return true;
}

void VixenWirelessUnit::updateAlignmentProperties()
{
    if (isConnected())
    {
        if (!alignmentPropertiesDefined_)
        {
            IUResetSwitch(&AlignmentDeleteSP);
            AlignmentDeleteSP.s = IPS_IDLE;
            defineProperty(&AlignmentRefreshSP);
            defineProperty(&AlignmentTP);
            defineProperty(&AlignmentDeleteSP);
            alignmentPropertiesDefined_ = true;
        }
        if (!alignmentListLoaded_)
            readAlignmentPoints();
        return;
    }

    if (alignmentPropertiesDefined_)
    {
        deleteProperty(AlignmentRefreshSP.name);
        deleteProperty(AlignmentTP.name);
        if (alignmentSelectionDefined_)
            deleteProperty(AlignmentSelectionSP.name);
        deleteProperty(AlignmentDeleteSP.name);
        alignmentPropertiesDefined_ = false;
        alignmentSelectionDefined_ = false;
    }
    alignmentListLoaded_ = false;
    alignmentPoints_.clear();
    IUResetSwitch(&AlignmentDeleteSP);
    AlignmentDeleteSP.s = IPS_IDLE;
}

void VixenWirelessUnit::updateWifiProperties()
{
    if (isConnected())
    {
        if (!wifiPropertiesDefined_)
        {
            defineProperty(&WifiInformationTP);
            defineProperty(&WifiRefreshSP);
            defineProperty(&WifiSsidTP);
            defineProperty(&WifiAutoConnectSP);
            defineProperty(&WifiStationControlSP);
            defineProperty(&WifiAccessPointsTP);
            if (!wifiAccessPointSsids_.empty())
            {
                WifiAccessPointSelectionSP.nsp = static_cast<int>(wifiAccessPointSsids_.size());
                defineProperty(&WifiAccessPointSelectionSP);
                wifiAccessPointSelectionDefined_ = true;
            }
            defineProperty(&WifiScanSP);
            wifiPropertiesDefined_ = true;
        }
        // Match the ASCOM setup sequence: read station configuration and
        // connection state automatically, but scan access points only when
        // the operator explicitly requests it.
        if (!wifiInformationLoaded_ && wifiOperationAllowed("Automatic Wi-Fi station refresh"))
            readWifiStationInformation();
        return;
    }

    if (wifiPropertiesDefined_)
    {
        deleteProperty(WifiInformationTP.name);
        deleteProperty(WifiRefreshSP.name);
        deleteProperty(WifiSsidTP.name);
        deleteProperty(WifiAutoConnectSP.name);
        deleteProperty(WifiStationControlSP.name);
        deleteProperty(WifiAccessPointsTP.name);
        if (wifiAccessPointSelectionDefined_)
            deleteProperty(WifiAccessPointSelectionSP.name);
        deleteProperty(WifiScanSP.name);
        wifiPropertiesDefined_ = false;
        wifiAccessPointSelectionDefined_ = false;
    }
    wifiInformationLoaded_ = false;
    pendingWifiSsid_.clear();
    clearPendingWifiPassword();
    IUSaveText(&WifiInformationT[0], "Not read");
    IUSaveText(&WifiInformationT[1], "--");
    IUSaveText(&WifiInformationT[2], "--");
    IUSaveText(&WifiInformationT[3], "--");
    WifiInformationTP.s = IPS_IDLE;
    IUSaveText(&WifiAccessPointsT[0], "Not scanned");
    for (std::size_t index = 0; index < kMaximumDisplayedWifiAccessPoints; ++index)
        IUSaveText(&WifiAccessPointsT[index + 1], "--");
    WifiAccessPointsTP.ntp = 1;
    WifiAccessPointsTP.s = IPS_IDLE;
    wifiAccessPointSsids_.clear();
    WifiAccessPointSelectionSP.nsp = 0;
    IUResetSwitch(&WifiAccessPointSelectionSP);
}

bool VixenWirelessUnit::readWifiStationInformation()
{
    if (!client_ || !wifiOperationAllowed("Wi-Fi station refresh"))
        return false;

    bool success = true;
    const auto settings = client_->wifiStationSettings();
    if (settings)
    {
        IUSaveText(&WifiInformationT[2], settings.value->autoConnect ? "On" : "Off");
        IUSaveText(&WifiInformationT[3], settings.value->ssid.empty() ? "--" : settings.value->ssid.c_str());
        IUSaveText(&WifiSsidT[0], settings.value->ssid.c_str());
        IUResetSwitch(&WifiAutoConnectSP);
        WifiAutoConnectS[settings.value->autoConnect ? 0 : 1].s = ISS_ON;
        WifiSsidTP.s = IPS_OK;
        WifiAutoConnectSP.s = IPS_OK;
        if (wifiPropertiesDefined_)
        {
            IDSetText(&WifiSsidTP, nullptr);
            IDSetSwitch(&WifiAutoConnectSP, nullptr);
        }
    }
    else
    {
        IUSaveText(&WifiInformationT[2], "Unknown");
        IUSaveText(&WifiInformationT[3], "Unknown");
        LOGF_WARN("WFSTS station-information read failed: %s", settings.error.c_str());
        success = false;
    }

    const auto connection = client_->wifiStationConnection();
    if (connection)
    {
        IUSaveText(&WifiInformationT[0], connection.value->connected ? "Connected" : "Disconnected");
        IUSaveText(&WifiInformationT[1], connection.value->ipAddress.empty() ? "--"
                                                                            : connection.value->ipAddress.c_str());
    }
    else
    {
        IUSaveText(&WifiInformationT[0], "Unknown");
        IUSaveText(&WifiInformationT[1], "Unknown");
        LOGF_WARN("WFSTC station-connection read failed: %s", connection.error.c_str());
        success = false;
    }

    WifiInformationTP.s = success ? IPS_OK : IPS_ALERT;
    wifiInformationLoaded_ = success;
    if (wifiPropertiesDefined_)
        IDSetText(&WifiInformationTP, "%s",
                  success ? "Wi-Fi station information refreshed."
                          : "One or more Wi-Fi station values could not be read.");
    return success;
}

void VixenWirelessUnit::clearPendingWifiPassword()
{
    std::fill(pendingWifiPassword_.begin(), pendingWifiPassword_.end(), '\0');
    pendingWifiPassword_.clear();
    IUSaveText(&WifiSsidT[1], "");
}

bool VixenWirelessUnit::applyWifiStationSettings(bool refreshAfterApply)
{
    if (!client_ || !wifiOperationAllowed("Wi-Fi station settings"))
        return false;

    const std::string ssid = !pendingWifiSsid_.empty()
                                 ? pendingWifiSsid_
                                 : (WifiSsidT[0].text ? WifiSsidT[0].text : "");
    if (!validWifiField(ssid, 32, false))
    {
        LOG_ERROR("Station SSID must be 1-32 bytes and must not contain comma or a line break.");
        clearPendingWifiPassword();
        return false;
    }
    if (!validWifiField(pendingWifiPassword_, 64, true))
    {
        LOG_ERROR("Station password must be at most 64 bytes and must not contain comma or a line break.");
        clearPendingWifiPassword();
        return false;
    }

    const bool autoConnect = WifiAutoConnectS[0].s == ISS_ON;
    const auto result = client_->setWifiStationSettings(autoConnect, ssid, pendingWifiPassword_);
    clearPendingWifiPassword();
    if (!result)
    {
        LOGF_ERROR("WFSTS station-setting update failed: %s", result.error.c_str());
        return false;
    }
    pendingWifiSsid_.clear();
    return !refreshAfterApply || readWifiStationInformation();
}

bool VixenWirelessUnit::scanWifiAccessPoints()
{
    if (!client_ || !wifiOperationAllowed("Wi-Fi network scan"))
        return false;

    const auto result = client_->wifiAccessPoints();
    if (!result)
    {
        WifiAccessPointsTP.s = IPS_ALERT;
        if (wifiPropertiesDefined_)
            IDSetText(&WifiAccessPointsTP, "Could not scan Wi-Fi access points.");
        LOGF_WARN("WFAPL access-point scan failed: %s", result.error.c_str());
        return false;
    }

    std::vector<const vixen::wirelessunit::WifiAccessPoint *> visible;
    visible.reserve(result.value->size());
    for (const auto &accessPoint : *result.value)
    {
        // Match the official ASCOM UI: hidden networks are counted by the
        // device but an empty SSID is not offered in the visible list.
        if (!accessPoint.ssid.empty())
            visible.push_back(&accessPoint);
    }

    // Count is the number of named networks actually displayed. Keep this a
    // plain number even when the device also reports one or more empty SSIDs.
    const auto displayed = std::min(visible.size(), kMaximumDisplayedWifiAccessPoints);
    char count[32] {};
    std::snprintf(count, sizeof(count), "%zu", displayed);
    IUSaveText(&WifiAccessPointsT[0], count);

    wifiAccessPointSsids_.clear();
    wifiAccessPointSsids_.reserve(displayed);
    IUResetSwitch(&WifiAccessPointSelectionSP);
    for (std::size_t index = 0; index < displayed; ++index)
    {
        wifiAccessPointSsids_.push_back(visible[index]->ssid);
        std::snprintf(WifiAccessPointSelectionS[index].label,
                      sizeof(WifiAccessPointSelectionS[index].label), "%s", visible[index]->ssid.c_str());
    }

    WifiAccessPointsTP.s = IPS_OK;
    if (wifiPropertiesDefined_)
    {
        IDSetText(&WifiAccessPointsTP, nullptr);
        if (wifiAccessPointSelectionDefined_)
            deleteProperty(WifiAccessPointSelectionSP.name);
        wifiAccessPointSelectionDefined_ = false;
        WifiAccessPointSelectionSP.nsp = static_cast<int>(displayed);
        WifiAccessPointSelectionSP.s = IPS_IDLE;
        if (displayed > 0)
        {
            defineProperty(&WifiAccessPointSelectionSP);
            wifiAccessPointSelectionDefined_ = true;
        }
        // The selectable SSID vector must be redefined whenever its dynamic
        // labels change. Re-advertise the already defined search control
        // afterwards so KStars keeps it below the SSID list.
        deleteProperty(WifiScanSP.name);
        defineProperty(&WifiScanSP);
    }
    return true;
}

void VixenWirelessUnit::updateHotspotProperties()
{
    if (isConnected())
    {
        if (!hotspotPropertiesDefined_)
        {
            defineProperty(&HotspotInformationTP);
            defineProperty(&HotspotRefreshSP);
            defineProperty(&HotspotSettingsTP);
            hotspotPropertiesDefined_ = true;
        }
        if (!hotspotInformationLoaded_ && wifiOperationAllowed("Automatic access-point refresh"))
            readHotspotInformation();
        return;
    }

    if (hotspotPropertiesDefined_)
    {
        deleteProperty(HotspotInformationTP.name);
        deleteProperty(HotspotRefreshSP.name);
        deleteProperty(HotspotSettingsTP.name);
        hotspotPropertiesDefined_ = false;
    }
    hotspotInformationLoaded_ = false;
    pendingHotspotSsid_.clear();
    clearPendingHotspotPassword();
    pendingHotspotChannel_.reset();
    currentHotspotPassword_.clear();
    currentHotspotChannel_ = 1;
    IUSaveText(&HotspotInformationT[0], "Not read");
    IUSaveText(&HotspotInformationT[1], "Not read");
    IUSaveText(&HotspotInformationT[2], "Not read");
    IUSaveText(&HotspotSettingsT[0], "");
    IUSaveText(&HotspotSettingsT[1], "");
    IUSaveText(&HotspotSettingsT[2], "1");
    HotspotInformationTP.s = IPS_IDLE;
    HotspotSettingsTP.s = IPS_IDLE;
}

bool VixenWirelessUnit::readHotspotInformation()
{
    if (!client_ || !wifiOperationAllowed("Access-point refresh"))
        return false;

    const auto ssidResult = client_->wifiHotspotSsid();
    if (!ssidResult)
    {
        HotspotInformationTP.s = IPS_ALERT;
        hotspotInformationLoaded_ = false;
        LOGF_WARN("WFSID access-point information read failed: %s", ssidResult.error.c_str());
        if (hotspotPropertiesDefined_)
            IDSetText(&HotspotInformationTP, "Could not read the Wireless Unit access point settings.");
        return false;
    }
    const auto passwordResult = client_->wifiHotspotPassword();
    if (!passwordResult)
    {
        HotspotInformationTP.s = IPS_ALERT;
        hotspotInformationLoaded_ = false;
        LOGF_WARN("WFPWD access-point information read failed: %s", passwordResult.error.c_str());
        if (hotspotPropertiesDefined_)
            IDSetText(&HotspotInformationTP, "Could not read the Wireless Unit access point settings.");
        return false;
    }
    const auto channelResult = client_->wifiHotspotChannel();
    if (!channelResult)
    {
        HotspotInformationTP.s = IPS_ALERT;
        hotspotInformationLoaded_ = false;
        LOGF_WARN("WFCHN access-point information read failed: %s", channelResult.error.c_str());
        if (hotspotPropertiesDefined_)
            IDSetText(&HotspotInformationTP, "Could not read the Wireless Unit access point settings.");
        return false;
    }

    currentHotspotPassword_ = *passwordResult.value;
    currentHotspotChannel_ = *channelResult.value;
    const std::string channelText = std::to_string(currentHotspotChannel_);
    IUSaveText(&HotspotInformationT[0], ssidResult.value->c_str());
    IUSaveText(&HotspotInformationT[1], currentHotspotPassword_.c_str());
    IUSaveText(&HotspotInformationT[2], channelText.c_str());
    IUSaveText(&HotspotSettingsT[0], ssidResult.value->c_str());
    IUSaveText(&HotspotSettingsT[1], currentHotspotPassword_.c_str());
    IUSaveText(&HotspotSettingsT[2], channelText.c_str());
    HotspotSettingsTP.s = IPS_OK;
    HotspotInformationTP.s = IPS_OK;
    hotspotInformationLoaded_ = true;
    if (hotspotPropertiesDefined_)
    {
        IDSetText(&HotspotInformationTP, "Access point SSID, password, and channel refreshed.");
        IDSetText(&HotspotSettingsTP, nullptr);
    }
    return true;
}

void VixenWirelessUnit::clearPendingHotspotPassword(bool clearUi)
{
    std::fill(pendingHotspotPassword_.begin(), pendingHotspotPassword_.end(), '\0');
    pendingHotspotPassword_.clear();
    if (clearUi)
        IUSaveText(&HotspotSettingsT[1], "");
}

bool VixenWirelessUnit::applyHotspotSettings()
{
    if (!client_ || !wifiOperationAllowed("Access-point settings"))
        return false;

    if (!pendingHotspotSsid_.empty() && !validWifiField(pendingHotspotSsid_, 32, false))
    {
        LOG_ERROR("Access point SSID must be 1-32 bytes and must not contain comma or a line break.");
        clearPendingHotspotPassword();
        return false;
    }
    if (!pendingHotspotPassword_.empty() &&
        (!validWifiField(pendingHotspotPassword_, 63, false) || pendingHotspotPassword_.size() < 8))
    {
        LOG_ERROR("Access point password must be 8-63 bytes and must not contain comma or a line break.");
        clearPendingHotspotPassword();
        return false;
    }
    if (pendingHotspotChannel_ && (*pendingHotspotChannel_ < 1 || *pendingHotspotChannel_ > 11))
    {
        LOG_ERROR("Access point Wi-Fi channel must be an integer from 1 through 11.");
        clearPendingHotspotPassword();
        return false;
    }

    const std::string currentSsid = HotspotInformationT[0].text ? HotspotInformationT[0].text : "";
    if (vixen::wirelessunit::shouldApplyWifiApText(pendingHotspotSsid_, currentSsid,
                                              hotspotInformationLoaded_))
    {
        const auto ssidResult = client_->setWifiHotspotSsidValue(pendingHotspotSsid_);
        if (!ssidResult)
        {
            LOGF_ERROR("WFSID access-point setting update failed: %s", ssidResult.error.c_str());
            pendingHotspotSsid_.clear();
            pendingHotspotChannel_.reset();
            clearPendingHotspotPassword(!pendingHotspotPassword_.empty());
            return false;
        }
    }
    pendingHotspotSsid_.clear();

    if (!pendingHotspotPassword_.empty())
    {
        const bool changed = vixen::wirelessunit::shouldApplyWifiApText(
            pendingHotspotPassword_, currentHotspotPassword_, hotspotInformationLoaded_);
        const auto passwordResult = changed
                                        ? client_->setWifiHotspotPasswordValue(pendingHotspotPassword_)
                                        : vixen::wirelessunit::Result<void> { true, {} };
        clearPendingHotspotPassword();
        if (!passwordResult)
        {
            LOGF_ERROR("WFPWD access-point password update failed: %s", passwordResult.error.c_str());
            // WFSID has already succeeded, so reflect that partial update in the UI.
            readHotspotInformation();
            return false;
        }
    }
    else
    {
        clearPendingHotspotPassword(false);
    }

    if (pendingHotspotChannel_)
    {
        const bool changed = vixen::wirelessunit::shouldApplyWifiApChannel(
            pendingHotspotChannel_, currentHotspotChannel_, hotspotInformationLoaded_);
        const auto channelResult = changed
                                       ? client_->setWifiHotspotChannelValue(*pendingHotspotChannel_)
                                       : vixen::wirelessunit::Result<void> { true, {} };
        pendingHotspotChannel_.reset();
        if (!channelResult)
        {
            LOGF_ERROR("WFCHN access-point channel update failed: %s", channelResult.error.c_str());
            readHotspotInformation();
            return false;
        }
    }

    return readHotspotInformation();
}

bool VixenWirelessUnit::readAlignmentPoints()
{
    if (!client_)
        return false;

    const auto result = client_->alignmentPoints();
    if (!result)
    {
        AlignmentTP.s = IPS_ALERT;
        if (alignmentPropertiesDefined_)
            IDSetText(&AlignmentTP, "Could not read alignment points from the Wireless Unit.");
        LOGF_WARN("ALLST alignment-list read failed: %s", result.error.c_str());
        alignmentListLoaded_ = false;
        return false;
    }

    char count[32] {};
    if (result.value->size() > kMaximumDisplayedAlignmentPoints)
        std::snprintf(count, sizeof(count), "%zu (showing %zu)", result.value->size(),
                      kMaximumDisplayedAlignmentPoints);
    else
        std::snprintf(count, sizeof(count), "%zu", result.value->size());
    IUSaveText(&AlignmentT[0], count);

    for (std::size_t index = 0; index < kMaximumDisplayedAlignmentPoints; ++index)
    {
        if (index < result.value->size())
        {
            const auto &point = (*result.value)[index];
            char description[128] {};
            std::snprintf(description, sizeof(description), "state=%d  MJD=%.5f  %s", point.state,
                          point.modifiedJulianDate, point.label.c_str());
            IUSaveText(&AlignmentT[index + 1], description);
        }
        else
            IUSaveText(&AlignmentT[index + 1], "--");
    }

    AlignmentTP.s = IPS_OK;
    alignmentListLoaded_ = true;
    alignmentPoints_ = *result.value;
    if (alignmentPropertiesDefined_)
    {
        // KStars does not reliably repaint newly populated rows in a fixed
        // read-only text vector. Advertise only the rows that exist and
        // rebuild the property so a larger or smaller ALLST result is visible
        // immediately after Sync or Refresh.
        deleteProperty(AlignmentTP.name);
        AlignmentTP.ntp = static_cast<int>(
            std::min(result.value->size(), kMaximumDisplayedAlignmentPoints) + 1);
        defineProperty(&AlignmentTP);

        // A selection is deliberately cleared whenever ALLST is refreshed.
        // This prevents an old row number from deleting a different point
        // after Sync or another client changes the device list.
        if (alignmentSelectionDefined_)
        {
            deleteProperty(AlignmentSelectionSP.name);
            alignmentSelectionDefined_ = false;
        }
        IUResetSwitch(&AlignmentSelectionSP);
        AlignmentSelectionSP.s = IPS_IDLE;
        AlignmentSelectionSP.nsp = static_cast<int>(
            std::min(result.value->size(), kMaximumDisplayedAlignmentPoints));
        if (AlignmentSelectionSP.nsp > 0)
        {
            defineProperty(&AlignmentSelectionSP);
            alignmentSelectionDefined_ = true;
        }
    }
    return true;
}

bool VixenWirelessUnit::alignmentPointMatches(
    std::size_t index, const std::vector<vixen::wirelessunit::AlignmentPoint> &current) const
{
    if (index >= alignmentPoints_.size() || index >= current.size())
        return false;
    const auto &expected = alignmentPoints_[index];
    const auto &actual = current[index];
    return expected.state == actual.state &&
           std::abs(expected.modifiedJulianDate - actual.modifiedJulianDate) < 1e-8 &&
           expected.label == actual.label;
}

void VixenWirelessUnit::updateHomeProperties()
{
    if (isConnected())
    {
        if (!homePositionDefined_)
        {
            defineProperty(&HomePositionNP);
            homePositionDefined_ = true;
        }
        if (initializationState_ == MountInitializationState::uninitialized && !homeSetDefined_)
        {
            SetHomePositionSP.s = IPS_IDLE;
            IUResetSwitch(&SetHomePositionSP);
            defineProperty(&SetHomePositionSP);
            homeSetDefined_ = true;
        }
        else if (initializationState_ != MountInitializationState::uninitialized && homeSetDefined_)
        {
            deleteProperty(SetHomePositionSP.name);
            homeSetDefined_ = false;
        }
        return;
    }

    if (homeSetDefined_)
    {
        deleteProperty(SetHomePositionSP.name);
        homeSetDefined_ = false;
    }
    if (homePositionDefined_)
    {
        deleteProperty(HomePositionNP.name);
        homePositionDefined_ = false;
    }
}

void VixenWirelessUnit::updateInitializationState(const vixen::wirelessunit::MountStatus &status)
{
    // Only the first MTSTA read, before INDI applies TIME_UTC, can be used to
    // infer the power-session initialization state from MJD. SYUTM changes an
    // uninitialized unit's MJD to the current date even though MTINI has not
    // been accepted, so do not reclassify the mount on later status polls.
    const bool observedUninitialized = !vixen::wirelessunit::mountStatusIsInitialized(status);
    if (initializationState_ != MountInitializationState::unknown && !observedUninitialized)
    {
        updateHomeProperties();
        return;
    }

    // A near-zero MJD remains useful after reconnect: it proves that the same
    // unit was power-cycled and must override a previously latched state.
    const auto next = observedUninitialized ? MountInitializationState::uninitialized
                                            : MountInitializationState::initialized;
    if (next == initializationState_)
    {
        updateHomeProperties();
        return;
    }
    initializationState_ = next;
    if (next == MountInitializationState::initialized)
        LOGF_INFO("Wireless Unit is initialized (MJD %.6f); normal mount controls are enabled.",
                  status.modifiedJulianDate);
    else
        LOGF_WARN("Wireless Unit is not initialized (MJD %.6f); use the direction buttons and Home set.",
                  status.modifiedJulianDate);
    updateHomeProperties();
}

bool VixenWirelessUnit::requireInitialized(const char *operation)
{
    if (initializationState_ == MountInitializationState::initialized)
        return true;
    LOGF_ERROR("%s is unavailable until HOME initialization is complete.", operation);
    return false;
}

bool VixenWirelessUnit::initializeHome(bool automatic)
{
    auto publish = [this, automatic](IPState state, const char *message)
    {
        auto *property = automatic ? &AssumeHomeSP : &SetHomePositionSP;
        property->s = state;
        IDSetSwitch(property, "%s", message);
    };

    if (!client_)
    {
        publish(IPS_ALERT, "Connect to the Wireless Unit before setting HOME.");
        return false;
    }
    if (!automatic && !isConnected())
    {
        publish(IPS_ALERT, "Connect to the Wireless Unit before setting HOME.");
        return false;
    }
    if (initializationState_ != MountInitializationState::uninitialized)
    {
        publish(IPS_OK, "HOME is already initialized; MTINI was not sent.");
        return true;
    }
    if (isParked() || TrackState == SCOPE_PARKED)
    {
        publish(IPS_ALERT, "Unpark before setting HOME.");
        return false;
    }

    const bool statusMotion = status_ && (status_->status == 2 || status_->status == 3);
    const bool scopeMotion = TrackState == SCOPE_SLEWING || TrackState == SCOPE_PARKING;
    if (manualMotionActive() || statusMotion || scopeMotion)
    {
        publish(IPS_ALERT, "Stop all telescope motion before setting HOME.");
        return false;
    }
    if (TrackState == SCOPE_TRACKING)
    {
        publish(IPS_ALERT, "Turn tracking off before setting HOME.");
        return false;
    }

    publish(IPS_BUSY, "Initializing the current position as HOME (Az 270 / Alt 0)...");

    // Match the Android STAR BOOK Wireless startup sequence: stop both
    // pre-initialization axes, apply the current site and UTC, then consume
    // the once-per-power-session MTINI command.
    const auto stopped = client_->stopBeforeInitialization();
    if (!stopped)
    {
        publish(IPS_ALERT, "Could not stop pre-initialization motion.");
        LOGF_ERROR("HOME MTMV0 stop failed: %s", stopped.error.c_str());
        return false;
    }

    double longitude = LocationNP[LOCATION_LONGITUDE].getValue();
    if (longitude > 180)
        longitude -= 360;
    const auto site = client_->setSite(longitude, LocationNP[LOCATION_LATITUDE].getValue(), timezoneHours_);
    if (!site)
    {
        publish(IPS_ALERT, "Could not apply the site before HOME initialization.");
        LOGF_ERROR("HOME SYLOC apply failed: %s", site.error.c_str());
        return false;
    }

    const auto time = client_->setTime(std::chrono::system_clock::now());
    if (!time)
    {
        publish(IPS_ALERT, "Could not apply the time before HOME initialization.");
        LOGF_ERROR("HOME SYUTM apply failed: %s", time.error.c_str());
        return false;
    }

    const auto initialization = client_->initializeMount();
    if (!initialization)
    {
        publish(IPS_ALERT, "Wireless Unit rejected HOME initialization.");
        LOGF_ERROR("HOME MTINI failed: %s", initialization.error.c_str());
        return false;
    }
    if (!*initialization.value)
    {
        // SYUTM immediately above also updates MJD, so a later current-MJD
        // MTSTA cannot turn this rejected initialization into success.
        publish(IPS_ALERT, "MTINI was already consumed; power-cycle the Wireless Unit and retry HOME setup.");
        LOG_ERROR("HOME MTINI returned NG after an uninitialized Connect state.");
        return false;
    }

    // MTINI=1 is authoritative. Commit before verification so a lost MTSTA
    // response can never cause MTINI to be repeated in the same power session.
    initializationState_ = MountInitializationState::initialized;
    trackingKnownStopped_ = true;
    motionRa_ = motionDec_ = 0;
    status_.reset();

    const auto initializedStatus = client_->mountStatus();
    if (!initializedStatus)
    {
        if (!automatic)
            IUResetSwitch(&SetHomePositionSP);
        publish(IPS_OK, "HOME was accepted; position verification will resume on the next MTSTA poll.");
        LOGF_WARN("HOME was accepted, but MTSTA verification failed: %s. "
                  "The next status poll will complete coordinate recovery.",
                  initializedStatus.error.c_str());
        updateHomeProperties();
        return true;
    }
    status_ = initializedStatus.value;

    TrackState = SCOPE_IDLE;
    NewRaDec(status_->rightAscensionHours, status_->declinationDegrees);
    HomePositionNP.s = IPS_OK;
    IDSetNumber(&HomePositionNP, "Current telescope position initialized as Az 270 / Alt 0.");
    if (!automatic)
        IUResetSwitch(&SetHomePositionSP);
    publish(IPS_OK, automatic ? "Automatic HOME initialization completed."
                              : "HOME initialization completed.");
    LOG_INFO("Current telescope position initialized as Wireless Unit HOME (Az 270 / Alt 0).");
    updateHomeProperties();
    return true;
}

bool VixenWirelessUnit::wifiOperationAllowed(const char *operation) const
{
    const bool manualMotion = std::abs(motionRa_) > 1e-9 || std::abs(motionDec_) > 1e-9;
    const bool guideActive = guideNSTimerId_ != 0 || guideWETimerId_ != 0;
    const bool reportedMotion = status_ && (status_->status == 2 || status_->status == 3);
    const bool reportedTracking = status_ && status_->status == 1 && status_->targetObjectType >= 3 &&
                                  !trackingKnownStopped_;
    const bool activeScopeState = TrackState == SCOPE_SLEWING || TrackState == SCOPE_TRACKING ||
                                  TrackState == SCOPE_PARKING;
    if (!manualMotion && !guideActive && !reportedMotion && !reportedTracking && !activeScopeState)
        return true;

    LOGF_WARN("%s is unavailable while the mount is moving, tracking, parking, or pulse guiding. "
              "Stop the mount before changing or querying Wi-Fi settings.", operation);
    return false;
}

bool VixenWirelessUnit::ISNewText(const char *dev, const char *name, char *texts[], char *names[], int n)
{
    if (!dev || std::strcmp(dev, getDeviceName()) != 0)
        return INDI::Telescope::ISNewText(dev, name, texts, names, n);

    if (std::strcmp(name, HotspotSettingsTP.name) == 0)
    {
        std::vector<std::pair<std::string, std::string>> submitted;
        if (n > 0)
            submitted.reserve(static_cast<std::size_t>(n));
        for (int index = 0; index < n; ++index)
        {
            if (!names || !texts || !names[index] || !texts[index])
            {
                HotspotSettingsTP.s = IPS_ALERT;
                IDSetText(&HotspotSettingsTP, "Access point settings request is incomplete.");
                return false;
            }
            submitted.emplace_back(names[index], texts[index]);
        }
        const auto parsed = vixen::wirelessunit::parseWifiApTextUpdate(submitted);
        if (!parsed)
        {
            HotspotSettingsTP.s = IPS_ALERT;
            IDSetText(&HotspotSettingsTP, "%s", parsed.error.c_str());
            return false;
        }
        if (!isConnected() || !client_)
        {
            clearPendingHotspotPassword(false);
            HotspotSettingsTP.s = IPS_ALERT;
            IDSetText(&HotspotSettingsTP, "Connect before setting Access point settings.");
            return false;
        }
        if (!wifiOperationAllowed("Access-point settings"))
        {
            clearPendingHotspotPassword(false);
            HotspotSettingsTP.s = IPS_ALERT;
            IDSetText(&HotspotSettingsTP, "Stop mount activity before changing Access point settings.");
            return false;
        }

        pendingHotspotSsid_.clear();
        std::fill(pendingHotspotPassword_.begin(), pendingHotspotPassword_.end(), '\0');
        pendingHotspotPassword_.clear();
        pendingHotspotChannel_.reset();
        if (parsed.value->ssid)
        {
            pendingHotspotSsid_ = *parsed.value->ssid;
            IUSaveText(&HotspotSettingsT[0], pendingHotspotSsid_.c_str());
        }
        if (parsed.value->password)
        {
            pendingHotspotPassword_ = *parsed.value->password;
            IUSaveText(&HotspotSettingsT[1], parsed.value->password->c_str());
        }
        if (parsed.value->channel)
        {
            pendingHotspotChannel_ = *parsed.value->channel;
            const std::string channelText = std::to_string(*parsed.value->channel);
            IUSaveText(&HotspotSettingsT[2], channelText.c_str());
        }
        HotspotSettingsTP.s = IPS_BUSY;
        IDSetText(&HotspotSettingsTP, "Applying Access point settings...");
        const bool success = applyHotspotSettings();
        HotspotSettingsTP.s = success ? IPS_OK : IPS_ALERT;
        IDSetText(&HotspotSettingsTP, "%s",
                  success ? "Access point settings applied; reconnect or power-cycle if required."
                          : "Access point settings could not be fully applied.");
        return success;
    }
    if (std::strcmp(name, WifiSsidTP.name) == 0)
    {
        const char *requestedSsid = nullptr;
        const char *requestedPassword = nullptr;
        for (int index = 0; index < n; ++index)
        {
            if (names[index] && std::strcmp(names[index], "SSID") == 0)
                requestedSsid = texts[index];
            else if (names[index] && std::strcmp(names[index], "PASSWORD") == 0)
                requestedPassword = texts[index];
        }
        // INDI clients may submit only the edited text element. When Password
        // alone is changed, retain the SSID currently advertised by the driver
        // instead of rejecting the request as an empty SSID.
        const std::string submittedSsid = requestedSsid
                                              ? requestedSsid
                                              : (WifiSsidT[0].text ? WifiSsidT[0].text : "");
        const int selectedNetwork = IUFindOnSwitchIndex(&WifiAccessPointSelectionSP);
        const bool validSelection = selectedNetwork >= 0 &&
                                    selectedNetwork < static_cast<int>(wifiAccessPointSsids_.size());
        const std::string displayedSsid = WifiSsidT[0].text ? WifiSsidT[0].text : "";
        const std::string configuredSsid = WifiInformationT[3].text ? WifiInformationT[3].text : "";
        // KStars may submit either the selected advertised value or its older
        // client-side edit value. Both mean "use the highlighted network";
        // any other submitted SSID is an explicit manual override.
        const bool useSelected = validSelection &&
                                 (submittedSsid == displayedSsid || submittedSsid == configuredSsid);
        const std::string ssid = useSelected
                                     ? wifiAccessPointSsids_[static_cast<std::size_t>(selectedNetwork)]
                                     : submittedSsid;
        const std::string password = requestedPassword ? requestedPassword : "";
        const bool passwordProvided = !password.empty();
        if (!validWifiField(ssid, 32, false))
        {
            WifiSsidTP.s = IPS_ALERT;
            IDSetText(&WifiSsidTP, "SSID must be 1-32 bytes and must not contain comma or a line break.");
            return false;
        }
        if (!validWifiField(password, 64, true))
        {
            clearPendingWifiPassword();
            WifiSsidTP.s = IPS_ALERT;
            IDSetText(&WifiSsidTP,
                      "Password must be at most 64 bytes and must not contain comma or a line break.");
            return false;
        }
        if (!isConnected() || !client_)
        {
            clearPendingWifiPassword();
            WifiSsidTP.s = IPS_ALERT;
            IDSetText(&WifiSsidTP, "Connect before setting Station settings.");
            return false;
        }
        if (!wifiOperationAllowed("Wi-Fi station settings"))
        {
            clearPendingWifiPassword();
            WifiSsidTP.s = IPS_ALERT;
            IDSetText(&WifiSsidTP, "Stop mount activity before changing Station settings.");
            return false;
        }

        pendingWifiSsid_ = ssid;
        clearPendingWifiPassword();
        pendingWifiPassword_ = password;
        IUResetSwitch(&WifiAccessPointSelectionSP);
        WifiAccessPointSelectionSP.s = IPS_IDLE;
        if (wifiAccessPointSelectionDefined_)
            IDSetSwitch(&WifiAccessPointSelectionSP, nullptr);
        LOGF_INFO("Applying Station settings for SSID '%s' with %s Password.",
                  ssid.c_str(), passwordProvided ? "a non-empty" : "an empty");
        WifiSsidTP.s = IPS_BUSY;
        IDSetText(&WifiSsidTP, "Applying Station settings...");
        const bool success = applyWifiStationSettings();
        WifiSsidTP.s = success ? IPS_OK : IPS_ALERT;
        IDSetText(&WifiSsidTP, "%s",
                  success
                      ? (passwordProvided
                             ? "Station settings applied; Password was sent and cleared from the UI."
                             : "Station settings applied; the empty Password retained the saved password.")
                      : "Station settings could not be applied.");
        return success;
    }
    if (std::strcmp(name, EndpointTP.name) == 0)
    {
        if (isConnected())
        {
            // Ekos performs a full automatic CONFIG_LOAD just after Connect.
            // Accept an unchanged saved endpoint so that harmless replay does
            // not leave the Connection tab in Alert. A real endpoint change
            // remains prohibited until Disconnect.
            bool unchanged = true;
            for (int index = 0; index < n; ++index)
            {
                const IText *item = IUFindText(&EndpointTP, names[index]);
                if (!item || std::strcmp(item->text, texts[index]) != 0)
                {
                    unchanged = false;
                    break;
                }
            }
            if (unchanged)
            {
                EndpointTP.s = IPS_OK;
                IDSetText(&EndpointTP, nullptr);
                return true;
            }
            EndpointTP.s = IPS_ALERT;
            IDSetText(&EndpointTP, "Disconnect before changing the endpoint.");
            return false;
        }
        const std::string previousHost = EndpointT[0].text ? EndpointT[0].text : "";
        const std::string previousPort = EndpointT[1].text ? EndpointT[1].text : "";
        IUUpdateText(&EndpointTP, texts, names, n);
        const bool endpointChanged = previousHost != (EndpointT[0].text ? EndpointT[0].text : "") ||
                                     previousPort != (EndpointT[1].text ? EndpointT[1].text : "");
        if (endpointChanged)
        {
            const bool configurationReplay = loadingEndpointConfig_ || isConfigLoading() || loadingDefaults_;
            if (!configurationReplay && automaticConnectPending_)
                cancelAutomaticDiscoveryConnect(
                    "Automatic discovery cancelled because the endpoint was changed.");
            if (automaticDiscoverySelected())
                deviceDiscoveryState_ = DeviceDiscoveryState::notScanned;
        }
        EndpointTP.s = IPS_OK;
        if (endpointChanged && manualIpSelected())
            IDSetText(&EndpointTP, "%s",
                      "Manual IP will use this Host and Port without Device discovery.");
        else if (endpointChanged)
            IDSetText(&EndpointTP, "%s",
                      "Endpoint saved; Automatic discovery will determine the Host when connecting.");
        else
            IDSetText(&EndpointTP, nullptr);
        return true;
    }
    return INDI::Telescope::ISNewText(dev, name, texts, names, n);
}

VixenWirelessUnit::DiscoveryResult VixenWirelessUnit::discoverWirelessUnit(
    bool automatic, unsigned attempt, unsigned maximumAttempts, bool reportProgress)
{
    const bool retryingAutomaticDiscovery =
        automatic && deviceDiscoveryState_ == DeviceDiscoveryState::unavailable;
    char *end = nullptr;
    const long parsedPort = std::strtol(EndpointT[1].text, &end, 10);
    if (!end || *end != '\0' || parsedPort < 1 || parsedPort > 65535)
    {
        IUResetSwitch(&DiscoverySP);
        deviceDiscoveryState_ = DeviceDiscoveryState::unavailable;
        DiscoverySP.s = IPS_ALERT;
        IDSetSwitch(&DiscoverySP, "Port must be an integer from 1 to 65535.");
        LOG_ERROR("Device discovery requires a UDP port from 1 to 65535.");
        return DiscoveryResult::invalidConfiguration;
    }

    DiscoverySP.s = IPS_BUSY;
    if (reportProgress)
    {
        if (automatic && maximumAttempts == 0)
            IDSetSwitch(&DiscoverySP, "Searching for Wireless Unit via UDP (attempt %u)...", attempt);
        else if (automatic && maximumAttempts > 1)
            IDSetSwitch(&DiscoverySP, "Searching for Wireless Unit via UDP (attempt %u/%u)...",
                        attempt, maximumAttempts);
        else
            IDSetSwitch(&DiscoverySP, "%s",
                        retryingAutomaticDiscovery
                            ? "The previous search found no usable Wireless Unit; searching again via UDP..."
                            : automatic ? "No previous scan result; searching local IPv4 networks via UDP..."
                                        : "Searching local IPv4 networks via UDP...");
    }
    std::string error;
    const auto units = vixen::wirelessunit::discoverWirelessUnits(
        { static_cast<std::uint16_t>(parsedPort), std::chrono::milliseconds(800), {} }, error);
    IUResetSwitch(&DiscoverySP);

    if (!error.empty())
    {
        deviceDiscoveryState_ = DeviceDiscoveryState::unavailable;
        DiscoverySP.s = IPS_ALERT;
        IDSetSwitch(&DiscoverySP, "Wireless Unit search failed: %s", error.c_str());
        LOGF_ERROR("Wireless Unit discovery failed: %s", error.c_str());
        return DiscoveryResult::transportError;
    }
    if (units.empty())
    {
        deviceDiscoveryState_ = DeviceDiscoveryState::unavailable;
        const bool willRetry = automatic && (maximumAttempts == 0 || attempt < maximumAttempts);
        DiscoverySP.s = willRetry ? IPS_BUSY : IPS_ALERT;
        if (willRetry && reportProgress)
        {
            if (maximumAttempts == 0)
            {
                IDSetSwitch(&DiscoverySP,
                            "No Wireless Unit responded on attempt %u; continuing every %d seconds.",
                            attempt, kAutomaticDiscoveryRetryMilliseconds / 1000);
                LOGF_WARN("No Wireless Unit responded on UDP port %ld (attempt %u); continuing every %d seconds.",
                          parsedPort, attempt, kAutomaticDiscoveryRetryMilliseconds / 1000);
            }
            else
            {
                IDSetSwitch(&DiscoverySP,
                            "No Wireless Unit responded on attempt %u/%u; retrying in %d seconds.",
                            attempt, maximumAttempts, kAutomaticDiscoveryRetryMilliseconds / 1000);
                LOGF_WARN("No Wireless Unit responded on UDP port %ld (attempt %u/%u); retrying in %d seconds.",
                          parsedPort, attempt, maximumAttempts, kAutomaticDiscoveryRetryMilliseconds / 1000);
            }
        }
        else if (!willRetry)
        {
            IDSetSwitch(&DiscoverySP, "No Wireless Unit responded on UDP port %ld.", parsedPort);
            LOGF_ERROR("No Wireless Unit responded on UDP port %ld. Check the Unit power and local network.",
                       parsedPort);
        }
        return DiscoveryResult::noResponse;
    }
    if (units.size() > 1)
    {
        std::string addresses;
        for (const auto &unit : units)
        {
            if (!addresses.empty())
                addresses += ", ";
            addresses += unit.address;
        }
        deviceDiscoveryState_ = DeviceDiscoveryState::unavailable;
        DiscoverySP.s = IPS_ALERT;
        IDSetSwitch(&DiscoverySP, "Multiple Wireless Units found; Host was not changed: %s", addresses.c_str());
        LOGF_WARN("Multiple Wireless Units discovered: %s", addresses.c_str());
        return DiscoveryResult::multipleUnits;
    }

    IUSaveText(&EndpointT[0], units.front().address.c_str());
    EndpointTP.s = IPS_OK;
    IDSetText(&EndpointTP, "Wireless Unit discovered (firmware %.2f).", units.front().firmwareVersion);
    IUResetSwitch(&TransportSP);
    TransportS[1].s = ISS_ON;
    TransportSP.s = IPS_OK;
    deviceDiscoveryState_ = DeviceDiscoveryState::found;
    DiscoverySP.s = IPS_OK;
    IDSetSwitch(&DiscoverySP, "Wireless Unit found at %s.", units.front().address.c_str());
    LOGF_INFO("%s Wireless Unit %.2f at %s:%ld via UDP.",
              automatic ? "Automatically discovered" : "Discovered", units.front().firmwareVersion,
              units.front().address.c_str(), parsedPort);
    return DiscoveryResult::found;
}

void VixenWirelessUnit::startAutomaticDiscoveryConnect()
{
    if (automaticConnectPending_)
    {
        setAutomaticConnectionBusy("Automatic Wireless Unit discovery is already in progress.");
        return;
    }

    automaticConnectPending_ = true;
    automaticDiscoveryAttempt_ = 0;
    setAutomaticConnectionBusy("Starting automatic Wireless Unit discovery...");
    runAutomaticDiscoveryAttempt();
}

void VixenWirelessUnit::runAutomaticDiscoveryAttempt()
{
    if (!automaticConnectPending_)
        return;

    ++automaticDiscoveryAttempt_;
    const bool reportProgress =
        automaticDiscoveryAttempt_ <= kAutomaticDiscoveryInitialReportedAttempts ||
        (automaticDiscoveryAttempt_ - kAutomaticDiscoveryInitialReportedAttempts) %
            kAutomaticDiscoverySteadyReportAttempts == 0;
    const auto result = discoverWirelessUnit(
        true, automaticDiscoveryAttempt_, 0, reportProgress);
    if (!automaticConnectPending_)
        return;

    if (result == DiscoveryResult::found)
    {
        finishAutomaticDiscoveryConnect();
        return;
    }

    if (result == DiscoveryResult::noResponse)
    {
        if (reportProgress)
            setAutomaticConnectionBusy("Wireless Unit was not found; automatic discovery will retry.");
        automaticDiscoveryTimerId_ = IEAddTimer(
            kAutomaticDiscoveryRetryMilliseconds, automaticDiscoveryRetryHelper, this);
        return;
    }

    const unsigned completedAttempts = automaticDiscoveryAttempt_;
    automaticConnectPending_ = false;
    automaticDiscoveryAttempt_ = 0;
    automaticDiscoveryTimerId_ = 0;
    setConnected(false, IPS_ALERT, "Automatic Wireless Unit discovery failed.");
    LOGF_ERROR("Connection stopped because automatic Device discovery failed on attempt %u.", completedAttempts);
}

void VixenWirelessUnit::finishAutomaticDiscoveryConnect()
{
    automaticConnectPending_ = false;
    automaticDiscoveryAttempt_ = 0;
    automaticDiscoveryTimerId_ = 0;

    if (Connect())
    {
        setConnected(true);
        updateProperties();
        return;
    }

    if (deviceDiscoveryState_ == DeviceDiscoveryState::found)
        deviceDiscoveryState_ = DeviceDiscoveryState::unavailable;
    setConnected(false, IPS_ALERT, "Wireless Unit was found, but the connection or handshake failed.");
}

void VixenWirelessUnit::setAutomaticConnectionBusy(const char *message)
{
    auto connection = getSwitch("CONNECTION");
    if (!connection)
    {
        setConnected(false, IPS_BUSY, message);
        return;
    }

    connection.reset();
    if (auto *connect = connection.findWidgetByName("CONNECT"))
        connect->setState(ISS_ON);
    if (auto *disconnect = connection.findWidgetByName("DISCONNECT"))
        disconnect->setState(ISS_OFF);
    connection.setState(IPS_BUSY);
    connection.apply("%s", message ? message : "Connecting to the Wireless Unit...");
}

void VixenWirelessUnit::cancelAutomaticDiscoveryConnect(const char *message, bool updateConnection)
{
    if (automaticDiscoveryTimerId_ != 0)
    {
        IERmTimer(automaticDiscoveryTimerId_);
        automaticDiscoveryTimerId_ = 0;
    }
    const bool wasPending = automaticConnectPending_;
    automaticConnectPending_ = false;
    automaticDiscoveryAttempt_ = 0;
    if (!wasPending || !updateConnection)
        return;

    IUResetSwitch(&DiscoverySP);
    DiscoverySP.s = IPS_IDLE;
    IDSetSwitch(&DiscoverySP, "%s", message ? message : "Automatic Wireless Unit discovery cancelled.");
    setConnected(false, IPS_IDLE, message ? message : "Automatic Wireless Unit discovery cancelled.");
}

void VixenWirelessUnit::automaticDiscoveryRetryHelper(void *context)
{
    static_cast<VixenWirelessUnit *>(context)->automaticDiscoveryRetry();
}

void VixenWirelessUnit::automaticDiscoveryRetry()
{
    automaticDiscoveryTimerId_ = 0;
    runAutomaticDiscoveryAttempt();
}

bool VixenWirelessUnit::automaticDiscoverySelected() const
{
    return ConnectionModeS[0].s == ISS_ON;
}

bool VixenWirelessUnit::manualIpSelected() const
{
    return ConnectionModeS[1].s == ISS_ON;
}

void VixenWirelessUnit::selectConnectionMode(bool automatic)
{
    IUResetSwitch(&ConnectionModeSP);
    ConnectionModeS[automatic ? 0 : 1].s = ISS_ON;
}

bool VixenWirelessUnit::ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n)
{
    if (!dev || std::strcmp(dev, getDeviceName()) != 0)
        return INDI::Telescope::ISNewSwitch(dev, name, states, names, n);

    if (std::strcmp(name, ConnectionModeSP.name) == 0)
    {
        const char *requested = IUFindOnSwitchName(states, names, n);
        if (!requested || (std::strcmp(requested, "AUTO_DISCOVERY") != 0 &&
                           std::strcmp(requested, "MANUAL_IP") != 0))
        {
            ConnectionModeSP.s = IPS_ALERT;
            IDSetSwitch(&ConnectionModeSP, "Select Automatic discovery or Manual IP.");
            return false;
        }

        const bool requestedAutomatic = std::strcmp(requested, "AUTO_DISCOVERY") == 0;
        const bool requestedManual = !requestedAutomatic;
        if (isConnected())
        {
            const bool unchanged = (requestedAutomatic && automaticDiscoverySelected()) ||
                                   (requestedManual && manualIpSelected());
            if (unchanged)
            {
                ConnectionModeSP.s = IPS_OK;
                IDSetSwitch(&ConnectionModeSP, nullptr);
                return true;
            }
            ConnectionModeSP.s = IPS_ALERT;
            IDSetSwitch(&ConnectionModeSP, "Disconnect before changing the connection mode.");
            return false;
        }

        if (automaticConnectPending_)
            cancelAutomaticDiscoveryConnect("Automatic discovery cancelled because the connection mode was changed.");
        selectConnectionMode(requestedAutomatic);
        if (requestedAutomatic)
            deviceDiscoveryState_ = DeviceDiscoveryState::notScanned;
        ConnectionModeSP.s = IPS_OK;
        IDSetSwitch(&ConnectionModeSP, "%s",
                    requestedAutomatic
                        ? "Connect will discover a Wireless Unit on a local IPv4 network."
                        : "Connect will use the configured Host and Port without Device discovery.");
        return true;
    }

    if (std::strcmp(name, "CONNECTION") == 0)
    {
        const char *requested = IUFindOnSwitchName(states, names, n);
        if (requested && std::strcmp(requested, "DISCONNECT") == 0 && automaticConnectPending_)
        {
            cancelAutomaticDiscoveryConnect("Automatic Wireless Unit discovery cancelled by Disconnect.");
            updateProperties();
            return true;
        }
        if (requested && std::strcmp(requested, "CONNECT") == 0 && !isConnected() && !isSimulation() &&
                automaticDiscoverySelected())
        {
            if (automaticConnectPending_)
            {
                setAutomaticConnectionBusy("Automatic Wireless Unit discovery is already in progress.");
                return true;
            }
            const bool discoveryRequired = deviceDiscoveryState_ == DeviceDiscoveryState::notScanned ||
                                           deviceDiscoveryState_ == DeviceDiscoveryState::unavailable;
            if (discoveryRequired)
            {
                startAutomaticDiscoveryConnect();
                return true;
            }
        }
    }
    if (std::strcmp(name, "SIMULATION") == 0 && automaticConnectPending_)
        cancelAutomaticDiscoveryConnect("Automatic discovery cancelled because Simulation was changed.");

    if (std::strcmp(name, HotspotRefreshSP.name) == 0)
    {
        const char *requested = IUFindOnSwitchName(states, names, n);
        IUResetSwitch(&HotspotRefreshSP);
        if (!isConnected() || !client_)
        {
            HotspotRefreshSP.s = IPS_ALERT;
            IDSetSwitch(&HotspotRefreshSP, "Connect before refreshing access point information.");
            return false;
        }
        if (!requested || std::strcmp(requested, "REFRESH") != 0)
        {
            HotspotRefreshSP.s = IPS_IDLE;
            IDSetSwitch(&HotspotRefreshSP, nullptr);
            return true;
        }
        HotspotRefreshSP.s = IPS_BUSY;
        IDSetSwitch(&HotspotRefreshSP, "Reading Wireless Unit access point information...");
        const bool success = readHotspotInformation();
        HotspotRefreshSP.s = success ? IPS_OK : IPS_ALERT;
        IDSetSwitch(&HotspotRefreshSP, "%s",
                    success ? "Access point information refreshed." : "Access point information refresh failed.");
        return success;
    }
    if (std::strcmp(name, WifiAutoConnectSP.name) == 0)
    {
        const bool previousAutoConnect = WifiAutoConnectS[0].s == ISS_ON;
        IUUpdateSwitch(&WifiAutoConnectSP, states, names, n);
        if (IUFindOnSwitchIndex(&WifiAutoConnectSP) < 0)
            WifiAutoConnectS[0].s = ISS_ON;
        if (!isConnected() || !client_)
        {
            IUResetSwitch(&WifiAutoConnectSP);
            WifiAutoConnectS[previousAutoConnect ? 0 : 1].s = ISS_ON;
            WifiAutoConnectSP.s = IPS_ALERT;
            IDSetSwitch(&WifiAutoConnectSP, "Connect before changing Auto-connect on boot.");
            return false;
        }
        if (WifiSsidTP.s == IPS_BUSY)
        {
            IUResetSwitch(&WifiAutoConnectSP);
            WifiAutoConnectS[previousAutoConnect ? 0 : 1].s = ISS_ON;
            WifiAutoConnectSP.s = IPS_ALERT;
            IDSetSwitch(&WifiAutoConnectSP, "Set the pending Station settings first.");
            return false;
        }
        if (!wifiOperationAllowed("Auto-connect setting"))
        {
            IUResetSwitch(&WifiAutoConnectSP);
            WifiAutoConnectS[previousAutoConnect ? 0 : 1].s = ISS_ON;
            WifiAutoConnectSP.s = IPS_ALERT;
            IDSetSwitch(&WifiAutoConnectSP, "Stop mount activity before changing Auto-connect on boot.");
            return false;
        }

        const std::string ssid = WifiInformationT[3].text ? WifiInformationT[3].text : "";
        const bool autoConnect = WifiAutoConnectS[0].s == ISS_ON;
        WifiAutoConnectSP.s = IPS_BUSY;
        IDSetSwitch(&WifiAutoConnectSP, "Applying Auto-connect on boot...");
        vixen::wirelessunit::Result<void> result { false, "configured Station SSID is unavailable" };
        if (wifiInformationLoaded_ && validWifiField(ssid, 32, false))
            result = client_->setWifiStationSettings(autoConnect, ssid, "");
        bool success = static_cast<bool>(result);
        if (!result)
            LOGF_ERROR("WFSTS auto-connect update failed: %s", result.error.c_str());
        if (success)
            success = readWifiStationInformation();
        if (!success)
        {
            IUResetSwitch(&WifiAutoConnectSP);
            WifiAutoConnectS[previousAutoConnect ? 0 : 1].s = ISS_ON;
        }
        WifiAutoConnectSP.s = success ? IPS_OK : IPS_ALERT;
        IDSetSwitch(&WifiAutoConnectSP, "%s",
                    success ? "Auto-connect on boot applied." : "Auto-connect on boot could not be applied.");
        return success;
    }
    if (std::strcmp(name, WifiStationControlSP.name) == 0)
    {
        const char *requested = IUFindOnSwitchName(states, names, n);
        IUResetSwitch(&WifiStationControlSP);
        if (!isConnected() || !client_)
        {
            WifiStationControlSP.s = IPS_ALERT;
            IDSetSwitch(&WifiStationControlSP, "Connect the INDI driver before changing station connection.");
            return false;
        }
        if (!requested)
        {
            WifiStationControlSP.s = IPS_IDLE;
            IDSetSwitch(&WifiStationControlSP, nullptr);
            return true;
        }

        const bool connectStation = std::strcmp(requested, "CONNECT") == 0;
        if (!connectStation && std::strcmp(requested, "DISCONNECT") != 0)
            return false;

        if (!connectStation && std::strcmp(WifiInformationT[0].text, "Connected") == 0 &&
            std::strcmp(EndpointT[0].text, WifiInformationT[1].text) == 0)
        {
            WifiStationControlSP.s = IPS_ALERT;
            IDSetSwitch(&WifiStationControlSP,
                        "Refused: this driver is using the Station IP. Reconnect through the Access Point IP first.");
            return false;
        }
        if (!wifiOperationAllowed("Station connection operation"))
        {
            WifiStationControlSP.s = IPS_ALERT;
            IDSetSwitch(&WifiStationControlSP, "Stop mount activity before changing the Station connection.");
            return false;
        }

        WifiStationControlSP.s = IPS_BUSY;
        IDSetSwitch(&WifiStationControlSP, "%s", connectStation ? "Connecting Station mode..."
                                                               : "Disconnecting Station mode...");
        if (connectStation && WifiSsidTP.s == IPS_BUSY)
        {
            WifiStationControlSP.s = IPS_ALERT;
            IDSetSwitch(&WifiStationControlSP,
                        "Set the pending Station settings before connecting Station mode.");
            return false;
        }

        const auto result = client_->setWifiStationConnected(connectStation);
        bool success = static_cast<bool>(result);
        if (!result)
            LOGF_ERROR("WFSTC station connection update failed: %s", result.error.c_str());
        if (success)
            success = readWifiStationInformation();
        WifiStationControlSP.s = success ? IPS_OK : IPS_ALERT;
        IDSetSwitch(&WifiStationControlSP, "%s",
                    success ? (connectStation ? "Station mode connected." : "Station mode disconnected.")
                            : "Station connection operation failed.");
        return success;
    }
    if (std::strcmp(name, WifiRefreshSP.name) == 0)
    {
        const char *requested = IUFindOnSwitchName(states, names, n);
        IUResetSwitch(&WifiRefreshSP);
        if (!isConnected() || !client_)
        {
            WifiRefreshSP.s = IPS_ALERT;
            IDSetSwitch(&WifiRefreshSP, "Connect before refreshing Wi-Fi station information.");
            return false;
        }
        if (!requested || std::strcmp(requested, "REFRESH") != 0)
        {
            WifiRefreshSP.s = IPS_IDLE;
            IDSetSwitch(&WifiRefreshSP, nullptr);
            return true;
        }

        WifiRefreshSP.s = IPS_BUSY;
        IDSetSwitch(&WifiRefreshSP, "Reading Wi-Fi station information...");
        const bool success = readWifiStationInformation();
        WifiRefreshSP.s = success ? IPS_OK : IPS_ALERT;
        IDSetSwitch(&WifiRefreshSP, "%s",
                    success ? "Wi-Fi station information refreshed." : "Wi-Fi station refresh failed.");
        return success;
    }
    if (std::strcmp(name, WifiScanSP.name) == 0)
    {
        const char *requested = IUFindOnSwitchName(states, names, n);
        IUResetSwitch(&WifiScanSP);
        if (!isConnected() || !client_)
        {
            WifiScanSP.s = IPS_ALERT;
            IDSetSwitch(&WifiScanSP, "Connect before scanning Wi-Fi access points.");
            return false;
        }
        if (!requested || std::strcmp(requested, "SCAN") != 0)
        {
            WifiScanSP.s = IPS_IDLE;
            IDSetSwitch(&WifiScanSP, nullptr);
            return true;
        }

        WifiScanSP.s = IPS_BUSY;
        IDSetSwitch(&WifiScanSP, "Scanning Wi-Fi access points...");
        const bool success = scanWifiAccessPoints();
        WifiScanSP.s = success ? IPS_OK : IPS_ALERT;
        IDSetSwitch(&WifiScanSP, "%s",
                    success ? "Wi-Fi access-point scan completed." : "Wi-Fi access-point scan failed.");
        return success;
    }
    if (std::strcmp(name, WifiAccessPointSelectionSP.name) == 0)
    {
        IUUpdateSwitch(&WifiAccessPointSelectionSP, states, names, n);
        const int selected = IUFindOnSwitchIndex(&WifiAccessPointSelectionSP);
        if (!isConnected() || selected < 0 ||
            selected >= static_cast<int>(wifiAccessPointSsids_.size()))
        {
            IUResetSwitch(&WifiAccessPointSelectionSP);
            WifiAccessPointSelectionSP.s = isConnected() ? IPS_IDLE : IPS_ALERT;
        IDSetSwitch(&WifiAccessPointSelectionSP, "%s",
                    isConnected() ? "Select one network." : "Connect before selecting a network.");
            return isConnected();
        }

        const std::string &selectedSsid = wifiAccessPointSsids_[static_cast<std::size_t>(selected)];
        pendingWifiSsid_ = selectedSsid;
        IUSaveText(&WifiSsidT[0], selectedSsid.c_str());
        WifiSsidTP.s = IPS_BUSY;
        IDSetText(&WifiSsidTP,
                  "Selected SSID is pending; enter Password if needed, then press Set.");
        WifiAccessPointSelectionSP.s = IPS_OK;
        IDSetSwitch(&WifiAccessPointSelectionSP,
                    "Selected network will be used by the next Station settings Set.");
        return true;
    }
    if (std::strcmp(name, AlignmentSelectionSP.name) == 0)
    {
        IUUpdateSwitch(&AlignmentSelectionSP, states, names, n);
        const int selected = IUFindOnSwitchIndex(&AlignmentSelectionSP);
        if (!isConnected() || selected < 0 ||
            selected >= static_cast<int>(alignmentPoints_.size()) ||
            selected >= static_cast<int>(kMaximumDisplayedAlignmentPoints))
        {
            IUResetSwitch(&AlignmentSelectionSP);
            AlignmentSelectionSP.s = isConnected() ? IPS_IDLE : IPS_ALERT;
        IDSetSwitch(&AlignmentSelectionSP, "%s",
                    isConnected() ? "Select one alignment point." : "Connect before selecting a point.");
            return isConnected();
        }
        AlignmentSelectionSP.s = IPS_OK;
        IDSetSwitch(&AlignmentSelectionSP, "Point %d selected.", selected + 1);
        return true;
    }
    if (std::strcmp(name, AlignmentDeleteSP.name) == 0)
    {
        const char *requested = IUFindOnSwitchName(states, names, n);
        IUResetSwitch(&AlignmentDeleteSP);
        if (!isConnected() || !client_)
        {
            AlignmentDeleteSP.s = IPS_ALERT;
            IDSetSwitch(&AlignmentDeleteSP, "Connect before deleting alignment points.");
            return false;
        }
        if (!requested)
        {
            AlignmentDeleteSP.s = IPS_IDLE;
            IDSetSwitch(&AlignmentDeleteSP, nullptr);
            return true;
        }

        const bool deleteAll = std::strcmp(requested, "DELETE_ALL") == 0;
        const bool deleteSelected = std::strcmp(requested, "DELETE_SELECTED") == 0;
        if (!deleteAll && !deleteSelected)
        {
            AlignmentDeleteSP.s = IPS_ALERT;
            IDSetSwitch(&AlignmentDeleteSP, "Unknown alignment delete action.");
            return false;
        }

        const int selected = IUFindOnSwitchIndex(&AlignmentSelectionSP);
        if (deleteSelected && (selected < 0 || selected >= static_cast<int>(alignmentPoints_.size())))
        {
            AlignmentDeleteSP.s = IPS_ALERT;
            IDSetSwitch(&AlignmentDeleteSP, "Select an alignment point before deleting it.");
            LOG_ERROR("Select an alignment point before deleting it.");
            return false;
        }

        AlignmentDeleteSP.s = IPS_BUSY;
        IDSetSwitch(&AlignmentDeleteSP, "%s",
                    deleteAll ? "Deleting all alignment points..." : "Verifying the selected point...");

        const auto current = client_->alignmentPoints();
        if (!current)
        {
            AlignmentDeleteSP.s = IPS_ALERT;
            IDSetSwitch(&AlignmentDeleteSP, "Could not verify the current alignment list.");
            LOGF_ERROR("ALLST before alignment deletion failed: %s", current.error.c_str());
            return false;
        }

        if (deleteAll && current.value->empty())
        {
            if (!alignmentPoints_.empty() && !readAlignmentPoints())
            {
                AlignmentDeleteSP.s = IPS_ALERT;
                IDSetSwitch(&AlignmentDeleteSP, "Could not refresh the empty alignment list.");
                return false;
            }
            AlignmentDeleteSP.s = IPS_IDLE;
            IDSetSwitch(&AlignmentDeleteSP, "No alignment points to delete.");
            return true;
        }

        if (deleteSelected &&
            (current.value->size() != alignmentPoints_.size() ||
             !alignmentPointMatches(static_cast<std::size_t>(selected), *current.value)))
        {
            readAlignmentPoints();
            AlignmentDeleteSP.s = IPS_ALERT;
            IDSetSwitch(&AlignmentDeleteSP,
                        "The alignment list changed; select the point again. Nothing was deleted.");
            return false;
        }

        std::string deleteError;
        if (deleteAll)
        {
            // ALDEL uses a zero-based index. Delete from the last row toward
            // zero so remaining row numbers do not move between commands.
            for (std::size_t count = current.value->size(); count > 0; --count)
            {
                const auto deleted = client_->deleteAlignmentPoint(count - 1);
                if (!deleted)
                {
                    deleteError = deleted.error;
                    break;
                }
            }
        }
        else
        {
            const auto deleted = client_->deleteAlignmentPoint(static_cast<std::size_t>(selected));
            if (!deleted)
                deleteError = deleted.error;
        }

        const bool refreshed = readAlignmentPoints();
        if (!deleteError.empty() || !refreshed)
        {
            AlignmentDeleteSP.s = IPS_ALERT;
            IDSetSwitch(&AlignmentDeleteSP, "%s",
                        !deleteError.empty() ? "Alignment deletion failed; the list was refreshed."
                                             : "Deleted, but the alignment list could not be refreshed.");
            if (!deleteError.empty())
                LOGF_ERROR("ALDEL failed: %s", deleteError.c_str());
            return false;
        }

        AlignmentDeleteSP.s = IPS_OK;
        IDSetSwitch(&AlignmentDeleteSP, "%s",
                    deleteAll ? "All alignment points were deleted."
                              : "The selected alignment point was deleted.");
        return true;
    }
    if (std::strcmp(name, AlignmentRefreshSP.name) == 0)
    {
        const char *requested = IUFindOnSwitchName(states, names, n);
        IUResetSwitch(&AlignmentRefreshSP);
        if (!isConnected() || !client_)
        {
            AlignmentRefreshSP.s = IPS_ALERT;
            IDSetSwitch(&AlignmentRefreshSP, "Connect before refreshing alignment points.");
            return false;
        }
        if (!requested || std::strcmp(requested, "REFRESH") != 0)
        {
            AlignmentRefreshSP.s = IPS_IDLE;
            IDSetSwitch(&AlignmentRefreshSP, nullptr);
            return true;
        }

        AlignmentRefreshSP.s = IPS_BUSY;
        IDSetSwitch(&AlignmentRefreshSP, "Reading alignment points...");
        const bool success = readAlignmentPoints();
        AlignmentRefreshSP.s = success ? IPS_OK : IPS_ALERT;
        IDSetSwitch(&AlignmentRefreshSP, "%s",
                    success ? "Alignment points refreshed." : "Alignment-point refresh failed.");
        return success;
    }
    if (std::strcmp(name, DiscoverySP.name) == 0)
    {
        IUUpdateSwitch(&DiscoverySP, states, names, n);
        if (automaticConnectPending_)
        {
            IUResetSwitch(&DiscoverySP);
            DiscoverySP.s = IPS_BUSY;
            IDSetSwitch(&DiscoverySP, "Automatic connection discovery is already in progress.");
            return true;
        }
        if (isConnected())
        {
            IUResetSwitch(&DiscoverySP);
            // Discovery is a pre-connection operation. Treat an attempt made
            // while connected as a harmless no-op so the Connection tab does
            // not retain a red Alert indicator.
            DiscoverySP.s = IPS_IDLE;
            IDSetSwitch(&DiscoverySP,
                        "Wireless Unit is connected over the network. Disconnect before using Device discovery.");
            LOG_WARN("Wireless Unit is connected over the network. Disconnect before using Device discovery.");
            return true;
        }
        if (DiscoveryS[0].s != ISS_ON)
        {
            DiscoverySP.s = IPS_IDLE;
            IDSetSwitch(&DiscoverySP, nullptr);
            return true;
        }

        return discoverWirelessUnit(false) == DiscoveryResult::found;
    }
    if (std::strcmp(name, UseDeviceSettingsSP.name) == 0)
    {
        // A saved configuration (or .default file) can contain the opposite
        // source selection. Do not reinterpret that replay as a user switch
        // during an active connection: it could enable live mount writes.
        if (isConnected() && (preservingSettingsSourceDuringLoad_ || isConfigLoading() || loadingDefaults_))
        {
            UseDeviceSettingsS[0].s = connectionUsesDeviceSettings_ ? ISS_ON : ISS_OFF;
            UseDeviceSettingsSP.s = IPS_OK;
            IDSetSwitch(&UseDeviceSettingsSP, "Saved Mount settings source was ignored during this connection.");
            return true;
        }

        IUUpdateSwitch(&UseDeviceSettingsSP, states, names, n);
        connectionUsesDeviceSettings_ = useDeviceSettings();

        if (!isConnected())
        {
            UseDeviceSettingsSP.s = IPS_OK;
            IDSetSwitch(&UseDeviceSettingsSP, nullptr);
            return true;
        }

        if (connectionUsesDeviceSettings_)
        {
            const bool settingsReady = client_ && readMountSettings();
            UseDeviceSettingsSP.s = settingsReady ? IPS_OK : IPS_ALERT;
            IDSetSwitch(&UseDeviceSettingsSP, "%s",
                        settingsReady ? "Device settings enabled; Wireless Unit values were refreshed."
                                      : "Device settings enabled, but one or more values could not be refreshed.");
            return settingsReady;
        }

        UseDeviceSettingsSP.s = IPS_OK;
        IDSetSwitch(&UseDeviceSettingsSP,
                    "Device settings disabled; edit and Set each parameter to apply it to the Wireless Unit.");
        return true;
    }
    if (std::strcmp(name, AssumeHomeSP.name) == 0)
    {
        const bool previous = AssumeHomeS[0].s == ISS_ON;
        IUUpdateSwitch(&AssumeHomeSP, states, names, n);
        const bool requested = AssumeHomeS[0].s == ISS_ON;
        if (isConnected() || automaticConnectPending_)
        {
            if (requested == previous)
            {
                IDSetSwitch(&AssumeHomeSP, "%s",
                            AssumeHomeSP.s == IPS_ALERT
                                ? "Automatic HOME initialization failed; inspect the mount and use Home set."
                                : "Automatic HOME setting is locked while connected.");
                return true;
            }
            AssumeHomeS[0].s = previous ? ISS_ON : ISS_OFF;
            AssumeHomeSP.s = IPS_ALERT;
            IDSetSwitch(&AssumeHomeSP, "Disconnect before changing automatic HOME initialization.");
            return false;
        }

        AssumeHomeSP.s = IPS_OK;
        IDSetSwitch(&AssumeHomeSP, "%s",
                    requested
                        ? "Enabled: place the mount physically at Az 270 / Alt 0 before connecting."
                        : "Automatic HOME initialization is disabled.");
        if (requested)
            LOG_WARN("Assume HOME enabled: before Connect, physically place the mount at Az 270 / Alt 0.");
        return true;
    }
    if (std::strcmp(name, GotoSpeedSP.name) == 0)
    {
        if (isConnected() && connectionUsesDeviceSettings_)
        {
            GotoSpeedSP.s = IPS_OK;
            IDSetSwitch(&GotoSpeedSP, "Use device settings is enabled; the Wireless Unit value was kept.");
            return true;
        }

        const int previousIndex = gotoSpeedIndex_;
        IUUpdateSwitch(&GotoSpeedSP, states, names, n);
        const int index = IUFindOnSwitchIndex(&GotoSpeedSP);
        if (index < 0 || index >= static_cast<int>(gotoSpeeds_.size()))
        {
            IUResetSwitch(&GotoSpeedSP);
            GotoSpeedS[previousIndex].s = ISS_ON;
            GotoSpeedSP.s = IPS_ALERT;
            IDSetSwitch(&GotoSpeedSP, "Select one GoTo speed.");
            return false;
        }
        gotoSpeedIndex_ = index;

        if (isConnected() && !isConfigLoading() && !loadingDefaults_)
        {
            const auto result = client_->setGotoSpeedValue(gotoSpeeds_[gotoSpeedIndex_]);
            const auto retained = result ? client_->gotoSpeed() : vixen::wirelessunit::Result<int> {};
            const auto retainedIt = retained ? std::find(gotoSpeeds_.begin(), gotoSpeeds_.end(), *retained.value)
                                             : gotoSpeeds_.end();
            if (!result || !retained || retainedIt == gotoSpeeds_.end())
            {
                gotoSpeedIndex_ = previousIndex;
                IUResetSwitch(&GotoSpeedSP);
                GotoSpeedS[gotoSpeedIndex_].s = ISS_ON;
                GotoSpeedSP.s = IPS_ALERT;
                IDSetSwitch(&GotoSpeedSP, "GoTo speed could not be applied and verified.");
                LOGF_ERROR("MTGTS immediate apply failed: %s",
                           !result ? result.error.c_str()
                                   : (!retained ? retained.error.c_str() : "unsupported read-back value"));
                return false;
            }
            gotoSpeedIndex_ = static_cast<int>(std::distance(gotoSpeeds_.begin(), retainedIt));
            IUResetSwitch(&GotoSpeedSP);
            GotoSpeedS[gotoSpeedIndex_].s = ISS_ON;
            GotoSpeedSP.s = IPS_OK;
            IDSetSwitch(&GotoSpeedSP, "Applied to the Wireless Unit.");
            return true;
        }

        GotoSpeedSP.s = isConnected() ? IPS_OK : IPS_IDLE;
        IDSetSwitch(&GotoSpeedSP, "%s",
                    useDeviceSettings() ? "UI value changed; Wireless Unit was not updated."
                                        : "Will be applied when connecting.");
        return true;
    }
    if (std::strcmp(name, SetHomePositionSP.name) == 0)
    {
        const char *requested = IUFindOnSwitchName(states, names, n);
        IUResetSwitch(&SetHomePositionSP);
        if (!requested || std::strcmp(requested, "SET") != 0)
        {
            SetHomePositionSP.s = IPS_IDLE;
            IDSetSwitch(&SetHomePositionSP, nullptr);
            return true;
        }
        return initializeHome(false);
    }
    if (std::strcmp(name, MotorPowerSP.name) == 0)
    {
        if (isConnected() && connectionUsesDeviceSettings_)
        {
            MotorPowerSP.s = IPS_OK;
            IDSetSwitch(&MotorPowerSP, "Use device settings is enabled; the Wireless Unit value was kept.");
            return true;
        }
        const int previousPower = motorPower_;
        IUUpdateSwitch(&MotorPowerSP, states, names, n);
        const int index = IUFindOnSwitchIndex(&MotorPowerSP);
        if (index < 0 || index >= 4)
        {
            IUResetSwitch(&MotorPowerSP);
            MotorPowerS[motorPower_ - 1].s = ISS_ON;
            MotorPowerSP.s = IPS_ALERT;
            IDSetSwitch(&MotorPowerSP, "Select one motor power level.");
            return false;
        }
        motorPower_ = index + 1;
        if (isConnected() && !isConfigLoading() && !loadingDefaults_)
        {
            const auto result = client_->setMotorPowerLevel(motorPower_);
            if (!result)
            {
                motorPower_ = previousPower;
                IUResetSwitch(&MotorPowerSP);
                MotorPowerS[motorPower_ - 1].s = ISS_ON;
                MotorPowerSP.s = IPS_ALERT;
                IDSetSwitch(&MotorPowerSP, "Motor power was rejected by the Wireless Unit.");
                LOGF_ERROR("MTPWR immediate apply failed: %s", result.error.c_str());
                return false;
            }
            MotorPowerSP.s = IPS_OK;
            IDSetSwitch(&MotorPowerSP, "Applied to the Wireless Unit.");
            return true;
        }
        MotorPowerSP.s = isConnected() ? IPS_OK : IPS_IDLE;
        IDSetSwitch(&MotorPowerSP, "%s",
                    useDeviceSettings() ? "UI value changed; Wireless Unit was not updated."
                                        : "Will be applied when connecting.");
        return true;
    }
    if (std::strcmp(name, MountTypeSP.name) == 0)
    {
        if (isConnected() && connectionUsesDeviceSettings_)
        {
            MountTypeSP.s = IPS_OK;
            IDSetSwitch(&MountTypeSP, "Use device settings is enabled; the Wireless Unit value was kept.");
            return true;
        }
        const int previousType = mountType_;
        IUUpdateSwitch(&MountTypeSP, states, names, n);
        const int index = IUFindOnSwitchIndex(&MountTypeSP);
        if (index < 0 || index >= 2)
        {
            IUResetSwitch(&MountTypeSP);
            MountTypeS[mountType_].s = ISS_ON;
            MountTypeSP.s = IPS_ALERT;
            IDSetSwitch(&MountTypeSP, "Select one polar-alignment state.");
            return false;
        }
        mountType_ = index;
        if (isConnected() && !isConfigLoading() && !loadingDefaults_)
        {
            const auto result = client_->setMountTypeValue(mountType_);
            if (!result)
            {
                mountType_ = previousType;
                IUResetSwitch(&MountTypeSP);
                MountTypeS[mountType_].s = ISS_ON;
                MountTypeSP.s = IPS_ALERT;
                IDSetSwitch(&MountTypeSP, "Polar alignment was rejected by the Wireless Unit.");
                LOGF_ERROR("MTTYP immediate apply failed: %s", result.error.c_str());
                return false;
            }
            MountTypeSP.s = IPS_OK;
            IDSetSwitch(&MountTypeSP, "Applied to the Wireless Unit.");
            return true;
        }
        MountTypeSP.s = isConnected() ? IPS_OK : IPS_IDLE;
        IDSetSwitch(&MountTypeSP, "%s",
                    useDeviceSettings() ? "UI value changed; Wireless Unit was not updated."
                                        : "Will be applied when connecting.");
        return true;
    }
    if (std::strcmp(name, MeridianDirectionSP.name) == 0)
    {
        if (isConnected() && connectionUsesDeviceSettings_)
        {
            MeridianDirectionSP.s = IPS_OK;
            IDSetSwitch(&MeridianDirectionSP,
                        "Use device settings is enabled; the Wireless Unit value was kept.");
            return true;
        }

        int previousDirection = IUFindOnSwitchIndex(&MeridianDirectionSP);
        if (previousDirection < 0 || previousDirection >= 2)
            previousDirection = automaticGotoDegrees_ < 0 ? 1 : 0;
        const int previousGoto = automaticGotoDegrees_;

        const bool updated = IUUpdateSwitch(&MeridianDirectionSP, states, names, n) >= 0;
        const int direction = IUFindOnSwitchIndex(&MeridianDirectionSP);
        const double requestedStop = MeridianN[0].value;
        const double requestedMagnitude = MeridianN[1].value;
        const auto stop = vixen::wirelessunit::numeric_validation::integerInRange(requestedStop, 0, 90);
        const auto magnitude = vixen::wirelessunit::numeric_validation::integerInRange(requestedMagnitude, 0, 90);
        const int signedGoto = direction == 1 && magnitude ? -*magnitude : (magnitude ? *magnitude : 0);
        const bool valid = updated && direction >= 0 && direction < 2 && stop && magnitude &&
                           vixen::wirelessunit::numeric_validation::validMeridianSettings(*stop, signedGoto);
        if (!valid)
        {
            IUResetSwitch(&MeridianDirectionSP);
            MeridianDirectionS[previousDirection].s = ISS_ON;
            MeridianDirectionSP.s = IPS_ALERT;
            const char *message =
                "Invalid Meridian behavior: select East or West; use integer degrees from 0 to 90, and "
                "GoTo flip point must not exceed Stop point.";
            IDSetSwitch(&MeridianDirectionSP, "%s", message);
            LOGF_ERROR("%s Requested Stop point=%g, GoTo flip point=%g.",
                       message, requestedStop, requestedMagnitude);
            return false;
        }

        automaticGotoDegrees_ = signedGoto;
        if (isConnected() && !isConfigLoading() && !loadingDefaults_)
        {
            const auto result = client_->setMeridianSettings(*stop, automaticGotoDegrees_);
            if (!result)
            {
                automaticGotoDegrees_ = previousGoto;
                IUResetSwitch(&MeridianDirectionSP);
                MeridianDirectionS[previousDirection].s = ISS_ON;
                MeridianDirectionSP.s = IPS_ALERT;
                IDSetSwitch(&MeridianDirectionSP, "Meridian settings were rejected by the Wireless Unit.");
                LOGF_ERROR("MTCOM direction apply failed: %s", result.error.c_str());
                return false;
            }
            MeridianDirectionSP.s = IPS_OK;
            MeridianNP.s = IPS_OK;
            IDSetNumber(&MeridianNP, nullptr);
            IDSetSwitch(&MeridianDirectionSP, "Applied to the Wireless Unit.");
            return true;
        }

        MeridianDirectionSP.s = isConnected() ? IPS_OK : IPS_IDLE;
        MeridianNP.s = MeridianDirectionSP.s;
        IDSetNumber(&MeridianNP, nullptr);
        IDSetSwitch(&MeridianDirectionSP, "%s",
                    useDeviceSettings() ? "UI value changed; Wireless Unit was not updated."
                                        : "Will be applied when connecting.");
        return true;
    }
    if (TrackStateSP.isNameMatch(name) &&
            (TrackState == SCOPE_PARKED || isParked()))
    {
        const char *requestedState = IUFindOnSwitchName(states, names, n);
        if (requestedState && std::strcmp(requestedState, "TRACK_ON") == 0)
        {
            // INDI::Telescope rejects a Tracking change while parked after it
            // has already copied the client's switch selection. It returns
            // before restoring/applying the previous selection, which can
            // leave KStars showing the complete vector in red. Consume the
            // request and publish the unchanged Tracking Off state explicitly.
            trackingKnownStopped_ = true;
            TrackStateSP.reset();
            TrackStateSP[TRACK_OFF].setState(ISS_ON);
            TrackStateSP.setState(IPS_IDLE);
            TrackStateSP.apply("Telescope is parked; unpark before enabling tracking.");
            LOG_WARN("Tracking On was ignored because the telescope is parked.");
            return true;
        }
    }
    if (AbortSP.isNameMatch(name))
    {
        // The INDI base class changes EQUATORIAL_EOD_COORD from Busy to Idle as
        // soon as Abort() returns. The Wireless Unit acknowledges MTSTP before
        // its axes stop, so keep a GoTo Busy until MTSTA status 1 confirms it.
        const bool coordinateMotionBusy = EqNP.getState() == IPS_BUSY;

        AbortSP.reset();
        if (Abort())
        {
            AbortSP.setState(IPS_OK);

            if (ParkSP.getState() == IPS_BUSY)
            {
                ParkSP.reset();
                ParkSP.setState(IPS_ALERT);
                ParkSP.apply();
                LOG_INFO("Parking aborted; waiting for the mount to stop.");
            }

            if (coordinateMotionBusy)
            {
                EqNP.setState(IPS_BUSY);
                EqNP.apply();
                LOG_INFO("Slew abort accepted; waiting for MTSTA to confirm the stop.");
            }

            if (MovementWESP.getState() == IPS_BUSY)
            {
                MovementWESP.reset();
                MovementWESP.setState(IPS_IDLE);
                MovementWESP.apply();
            }
            if (MovementNSSP.getState() == IPS_BUSY)
            {
                MovementNSSP.reset();
                MovementNSSP.setState(IPS_IDLE);
                MovementNSSP.apply();
            }

            last_ns_motion = last_we_motion = -1;
        }
        else
            AbortSP.setState(IPS_ALERT);

        AbortSP.apply();
        return true;
    }
    return INDI::Telescope::ISNewSwitch(dev, name, states, names, n);
}

bool VixenWirelessUnit::ISNewNumber(const char *dev, const char *name, double values[], char *names[], int n)
{
    if (INDI::GuiderInterface::processNumber(dev, name, values, names, n))
        return true;
    if (!dev || std::strcmp(dev, getDeviceName()) != 0)
        return INDI::Telescope::ISNewNumber(dev, name, values, names, n);

    if (std::strcmp(name, GuideRateNP.name) == 0)
    {
        if (isConnected() && connectionUsesDeviceSettings_)
        {
            GuideRateNP.s = IPS_OK;
            IDSetNumber(&GuideRateNP, "Use device settings is enabled; the Wireless Unit values were kept.");
            return true;
        }
        const float previousRa = guideRa_;
        const float previousDec = guideDec_;
        const double previousUiRa = GuideRateN[0].value;
        const double previousUiDec = GuideRateN[1].value;
        double submittedRa = previousUiRa;
        double submittedDec = previousUiDec;
        if (values && names)
            for (int index = 0; index < n; ++index)
            {
                if (names[index] && std::strcmp(names[index], GuideRateN[0].name) == 0)
                    submittedRa = values[index];
                else if (names[index] && std::strcmp(names[index], GuideRateN[1].name) == 0)
                    submittedDec = values[index];
            }
        const bool updated = vixen::wirelessunit::numeric_validation::finiteUpdate(values, n) &&
                             IUUpdateNumber(&GuideRateNP, values, names, n) >= 0;
        if (!updated ||
            !vixen::wirelessunit::numeric_validation::finiteInRange(GuideRateN[0].value, 0.1, 10.0) ||
            !vixen::wirelessunit::numeric_validation::finiteInRange(GuideRateN[1].value, 0.1, 10.0) ||
            !vixen::wirelessunit::numeric_validation::oneDecimalPlace(GuideRateN[0].value) ||
            !vixen::wirelessunit::numeric_validation::oneDecimalPlace(GuideRateN[1].value))
        {
            GuideRateN[0].value = previousUiRa;
            GuideRateN[1].value = previousUiDec;
            GuideRateNP.s = IPS_ALERT;
            const char *message = "Guide rates must be from 0.1 to 10.0 in 0.1 increments.";
            IDSetNumber(&GuideRateNP, "%s", message);
            LOGF_ERROR("Invalid Guide rate: %s Requested W/E=%g, N/S=%g.",
                       message, submittedRa, submittedDec);
            return false;
        }
        // A connected Configuration Load replays saved UI values without
        // transmitting them. Never promote those values to the confirmed
        // guide multipliers used by pulseGuide(). A full load reads MTAGS
        // afterwards; a property-only load keeps the last confirmed values.
        if (isConnected() && (preservingSettingsSourceDuringLoad_ || isConfigLoading() || loadingDefaults_))
        {
            GuideRateN[0].value = previousRa;
            GuideRateN[1].value = previousDec;
            GuideRateNP.s = IPS_OK;
            IDSetNumber(&GuideRateNP,
                        "Saved Guide rate was not applied; confirmed Wireless Unit values were kept.");
            return true;
        }

        guideRa_ = static_cast<float>(GuideRateN[0].value);
        guideDec_ = static_cast<float>(GuideRateN[1].value);
        if (isConnected() && !isConfigLoading() && !loadingDefaults_)
        {
            const auto result = client_->setGuideRates(guideRa_, guideDec_);
            if (!result)
            {
                guideRa_ = previousRa;
                guideDec_ = previousDec;
                GuideRateN[0].value = guideRa_;
                GuideRateN[1].value = guideDec_;
                GuideRateNP.s = IPS_ALERT;
                IDSetNumber(&GuideRateNP, "Guide rates were rejected by the Wireless Unit.");
                LOGF_ERROR("MTAGS immediate apply failed: %s", result.error.c_str());
                return false;
            }
            GuideRateNP.s = IPS_OK;
            IDSetNumber(&GuideRateNP, "Applied to the Wireless Unit.");
            return true;
        }
        GuideRateNP.s = isConnected() ? IPS_OK : IPS_IDLE;
        IDSetNumber(&GuideRateNP, "%s",
                    useDeviceSettings() ? "UI value changed; Wireless Unit was not updated."
                                        : "Will be applied when connecting.");
        return true;
    }
    if (std::strcmp(name, BacklashNP.name) == 0)
    {
        if (isConnected() && connectionUsesDeviceSettings_)
        {
            BacklashNP.s = IPS_OK;
            IDSetNumber(&BacklashNP, "Use device settings is enabled; the Wireless Unit values were kept.");
            return true;
        }
        const int previousRa = backlashRa_;
        const int previousDec = backlashDec_;
        const double previousUiRa = BacklashN[0].value;
        const double previousUiDec = BacklashN[1].value;
        double submittedRa = previousUiRa;
        double submittedDec = previousUiDec;
        if (values && names)
            for (int index = 0; index < n; ++index)
            {
                if (names[index] && std::strcmp(names[index], BacklashN[0].name) == 0)
                    submittedRa = values[index];
                else if (names[index] && std::strcmp(names[index], BacklashN[1].name) == 0)
                    submittedDec = values[index];
            }
        const bool updated = vixen::wirelessunit::numeric_validation::finiteUpdate(values, n) &&
                             IUUpdateNumber(&BacklashNP, values, names, n) >= 0;
        const double requestedRa = BacklashN[0].value;
        const double requestedDec = BacklashN[1].value;
        const auto ra = vixen::wirelessunit::numeric_validation::integerInRange(requestedRa, 0, 99);
        const auto dec = vixen::wirelessunit::numeric_validation::integerInRange(requestedDec, 0, 99);
        if (!updated || !ra || !dec)
        {
            BacklashN[0].value = previousUiRa;
            BacklashN[1].value = previousUiDec;
            BacklashNP.s = IPS_ALERT;
            const char *message = "Backlash values must be integers from 0 to 99.";
            IDSetNumber(&BacklashNP, "%s", message);
            LOGF_ERROR("Invalid Backlash compensation: %s Requested RA=%g, DEC=%g.",
                       message, submittedRa, submittedDec);
            return false;
        }
        backlashRa_ = *ra;
        backlashDec_ = *dec;
        if (isConnected() && !isConfigLoading() && !loadingDefaults_)
        {
            const auto result = client_->setBacklashCompensation(backlashRa_, backlashDec_);
            if (!result)
            {
                backlashRa_ = previousRa;
                backlashDec_ = previousDec;
                BacklashN[0].value = backlashRa_;
                BacklashN[1].value = backlashDec_;
                BacklashNP.s = IPS_ALERT;
                IDSetNumber(&BacklashNP, "Backlash values were rejected by the Wireless Unit.");
                LOGF_ERROR("MTBLC immediate apply failed: %s", result.error.c_str());
                return false;
            }
            BacklashNP.s = IPS_OK;
            IDSetNumber(&BacklashNP, "Applied to the Wireless Unit.");
            return true;
        }
        BacklashNP.s = isConnected() ? IPS_OK : IPS_IDLE;
        IDSetNumber(&BacklashNP, "%s",
                    useDeviceSettings() ? "UI value changed; Wireless Unit was not updated."
                                        : "Will be applied when connecting.");
        return true;
    }
    if (std::strcmp(name, MeridianNP.name) == 0)
    {
        if (isConnected() && connectionUsesDeviceSettings_)
        {
            MeridianNP.s = IPS_OK;
            IDSetNumber(&MeridianNP, "Use device settings is enabled; the Wireless Unit values were kept.");
            return true;
        }

        const int previousStop = forcedTrackingStopDegrees_;
        const int previousGoto = automaticGotoDegrees_;
        const double previousUiStop = MeridianN[0].value;
        const double previousUiMagnitude = MeridianN[1].value;
        const bool updated = vixen::wirelessunit::numeric_validation::finiteUpdate(values, n) &&
                             IUUpdateNumber(&MeridianNP, values, names, n) >= 0;
        const double requestedStop = MeridianN[0].value;
        const double requestedMagnitude = MeridianN[1].value;
        const int direction = IUFindOnSwitchIndex(&MeridianDirectionSP);
        const auto stop = vixen::wirelessunit::numeric_validation::integerInRange(requestedStop, 0, 90);
        const auto magnitude = vixen::wirelessunit::numeric_validation::integerInRange(requestedMagnitude, 0, 90);
        const int signedGoto = direction == 1 && magnitude ? -*magnitude : (magnitude ? *magnitude : 0);
        if (!updated || !stop || !magnitude || direction < 0 || direction >= 2 ||
            !vixen::wirelessunit::numeric_validation::validMeridianSettings(*stop, signedGoto))
        {
            MeridianN[0].value = previousUiStop;
            MeridianN[1].value = previousUiMagnitude;
            MeridianNP.s = IPS_ALERT;
            const char *message =
                "Invalid Meridian behavior: use integer degrees from 0 to 90; GoTo flip point must not exceed "
                "Stop point.";
            IDSetNumber(&MeridianNP, "%s", message);
            LOGF_ERROR("%s Requested Stop point=%g, GoTo flip point=%g.",
                       message, requestedStop, requestedMagnitude);
            return false;
        }

        forcedTrackingStopDegrees_ = *stop;
        automaticGotoDegrees_ = signedGoto;
        if (isConnected() && !isConfigLoading() && !loadingDefaults_)
        {
            const auto result = client_->setMeridianSettings(forcedTrackingStopDegrees_, automaticGotoDegrees_);
            if (!result)
            {
                forcedTrackingStopDegrees_ = previousStop;
                automaticGotoDegrees_ = previousGoto;
                MeridianN[0].value = forcedTrackingStopDegrees_;
                MeridianN[1].value = std::abs(automaticGotoDegrees_);
                MeridianNP.s = IPS_ALERT;
                IDSetNumber(&MeridianNP, "Meridian settings were rejected by the Wireless Unit.");
                LOGF_ERROR("MTCOM immediate apply failed: %s", result.error.c_str());
                return false;
            }
            MeridianNP.s = IPS_OK;
            MeridianDirectionSP.s = IPS_OK;
            IDSetSwitch(&MeridianDirectionSP, nullptr);
            IDSetNumber(&MeridianNP, "Applied to the Wireless Unit.");
            return true;
        }

        MeridianNP.s = isConnected() ? IPS_OK : IPS_IDLE;
        MeridianDirectionSP.s = MeridianNP.s;
        IDSetSwitch(&MeridianDirectionSP, nullptr);
        IDSetNumber(&MeridianNP, "%s",
                    useDeviceSettings() ? "UI values changed; Wireless Unit was not updated."
                                        : "Will be applied when connecting.");
        return true;
    }
    return INDI::Telescope::ISNewNumber(dev, name, values, names, n);
}

bool VixenWirelessUnit::saveConfigItems(FILE *fp)
{
    INDI::Telescope::saveConfigItems(fp);
    IUSaveConfigSwitch(fp, &ConnectionModeSP);
    IUSaveConfigText(fp, &EndpointTP);
    IUSaveConfigSwitch(fp, &AssumeHomeSP);
    IUSaveConfigSwitch(fp, &UseDeviceSettingsSP);
    IUSaveConfigSwitch(fp, &GotoSpeedSP);
    IUSaveConfigNumber(fp, &GuideRateNP);
    IUSaveConfigNumber(fp, &BacklashNP);
    IUSaveConfigSwitch(fp, &MotorPowerSP);
    IUSaveConfigSwitch(fp, &MountTypeSP);
    IUSaveConfigSwitch(fp, &MeridianDirectionSP);
    IUSaveConfigNumber(fp, &MeridianNP);
    return true;
}

bool VixenWirelessUnit::loadConfig(bool silent, const char *property)
{
    // Ekos normally loads the saved INDI configuration immediately after a
    // device connects. Preserve that behaviour for endpoint, site,
    // and other properties, but do not let saved UI copies become the final
    // display of the values actually held by the Wireless Unit.
    const bool activeConnection = isConnected() && client_;
    const bool connectedFullLoad = activeConnection && property == nullptr;
    struct EndpointLoadGuard
    {
        bool &flag;
        bool previous;

        explicit EndpointLoadGuard(bool &loading) : flag(loading), previous(loading)
        {
            flag = true;
        }

        ~EndpointLoadGuard()
        {
            flag = previous;
        }
    } endpointLoadGuard(loadingEndpointConfig_);
    // The base loader replays switch vectors through ISNewSwitch. Keep a
    // separate guard because the base isConfigLoading() flag is not guaranteed
    // to cover every replay path or client implementation. Restore the active
    // source even if an exception interrupts the base call.
    struct SourceLoadGuard
    {
        bool &source;
        bool &guard;
        bool previousSource;
        bool previousGuard;
        bool preserve;

        SourceLoadGuard(bool &activeSource, bool &loadGuard, bool activeConnection) :
            source(activeSource), guard(loadGuard), previousSource(activeSource),
            previousGuard(loadGuard), preserve(activeConnection)
        {
            guard = previousGuard || preserve;
        }

        ~SourceLoadGuard()
        {
            if (preserve)
                source = previousSource;
            guard = previousGuard;
        }
    } sourceGuard(connectionUsesDeviceSettings_, preservingSettingsSourceDuringLoad_, activeConnection);

    const bool result = INDI::Telescope::loadConfig(silent, property);
    if (activeConnection)
        connectionUsesDeviceSettings_ = sourceGuard.previousSource;

    // Keep the UI and the multiplier used by MoveNS/MoveWE synchronized.
    // A missing or invalid selection uses the documented 100x default.
    const int loadedSlewIndex = SlewRateSP.findOnSwitchIndex();
    if (loadedSlewIndex < 0 || loadedSlewIndex >= static_cast<int>(slewMultipliers_.size()))
    {
        slewIndex_ = kDefaultSlewRateIndex;
        SlewRateSP.reset();
        SlewRateSP[slewIndex_].setState(ISS_ON);
        SlewRateSP.setState(IPS_OK);
    }
    else
    {
        slewIndex_ = loadedSlewIndex;
    }

    if (!connectedFullLoad)
        return result;

    // The active selection (including a later manual switch) controls the
    // connection even when Ekos performs an automatic full Configuration Load.
    UseDeviceSettingsS[0].s = connectionUsesDeviceSettings_ ? ISS_ON : ISS_OFF;

    // Refresh in both source modes. In local-settings mode the configured
    // values were already transmitted by Handshake(); this read-back verifies
    // what the Wireless Unit actually accepted and repairs any values replaced
    // on screen by Ekos' automatic Configuration Load.
    const bool settingsReady = readMountSettings();
    // A fresh installation legitimately has no saved INDI configuration, in
    // which case the base load returns false after reporting that fact.  Once
    // the values have been read back successfully, that absence must not turn
    // Mount settings or the automatic Configuration Load red.
    UseDeviceSettingsSP.s = settingsReady ? IPS_OK : IPS_ALERT;
    IDSetSwitch(&UseDeviceSettingsSP, nullptr);
    if (settingsReady)
        LOG_INFO("Configuration load completed; mount settings were refreshed from the Wireless Unit.");
    return settingsReady;
}

bool VixenWirelessUnit::loadDefaultConfig()
{
    // Preserve the standard INDI behaviour for unrelated properties, then
    // restore this driver's documented defaults.
    loadingDefaults_ = true;
    INDI::Telescope::loadDefaultConfig();
    loadingDefaults_ = false;

    if (!isConnected())
    {
        selectConnectionMode(true);
        ConnectionModeSP.s = IPS_IDLE;
        deviceDiscoveryState_ = DeviceDiscoveryState::notScanned;
        IDSetSwitch(&ConnectionModeSP, "Connection mode restored to Automatic discovery.");
        AssumeHomeS[0].s = ISS_OFF;
        AssumeHomeSP.s = IPS_IDLE;
        IDSetSwitch(&AssumeHomeSP, "Automatic HOME initialization restored to Off.");
    }

    // Restore both the published selection and the speed used by MoveNS/MoveWE.
    slewIndex_ = kDefaultSlewRateIndex;
    SlewRateSP.reset();
    SlewRateSP[slewIndex_].setState(ISS_ON);
    SlewRateSP.setState(IPS_OK);
    if (isConnected())
        SlewRateSP.apply("Slew rate restored to the 100x default.");

    // Connected local Default must not replace the last confirmed device
    // value before the batch is sent and read back.
    const bool connected = isConnected() && client_;
    if (!connected)
        gotoSpeedIndex_ = 4;
    IUResetSwitch(&GotoSpeedSP);
    GotoSpeedS[4].s = ISS_ON;
    GotoSpeedSP.s = connected ? IPS_OK : IPS_IDLE;

    if (connected && connectionUsesDeviceSettings_)
    {
        // Defaults must not replace live device values in device-settings
        // mode. Refresh in case the base default file contained these fields.
        UseDeviceSettingsS[0].s = ISS_ON;
        const bool settingsReady = readMountSettings();
        UseDeviceSettingsSP.s = settingsReady ? IPS_OK : IPS_ALERT;
        IDSetSwitch(&UseDeviceSettingsSP, nullptr);
        if (settingsReady)
            LOG_INFO("Default requested while using device settings; Wireless Unit values were kept.");
        return settingsReady;
    }

    // Preserve OFF for the active connection. When disconnected, Default also
    // restores the settings-source checkbox to its documented ON default.
    UseDeviceSettingsS[0].s = connected ? ISS_OFF : ISS_ON;
    UseDeviceSettingsSP.s = IPS_OK;
    resetMountSettingsToDefaults();

    bool settingsReady = true;
    if (connected)
        settingsReady = applyConfiguredMountSettings();

    UseDeviceSettingsSP.s = settingsReady ? IPS_OK : IPS_ALERT;
    IDSetSwitch(&UseDeviceSettingsSP, nullptr);
    if (settingsReady)
    {
        IDSetSwitch(&GotoSpeedSP, nullptr);
        IDSetNumber(&GuideRateNP, nullptr);
        IDSetNumber(&BacklashNP, nullptr);
        IDSetSwitch(&MotorPowerSP, nullptr);
        IDSetSwitch(&MountTypeSP, nullptr);
        IDSetSwitch(&MeridianDirectionSP, nullptr);
        IDSetNumber(&MeridianNP, nullptr);
    }
    if (connected && settingsReady)
        LOG_INFO("Vixen Wireless Unit defaults were applied to the mount.");
    else if (!connected)
        LOG_INFO("Vixen Wireless Unit defaults restored locally; no mount settings were transmitted.");
    return settingsReady;
}

bool VixenWirelessUnit::useDeviceSettings() const
{
    return UseDeviceSettingsS[0].s == ISS_ON;
}

void VixenWirelessUnit::resetMountSettingsToDefaults()
{
    // The UI defaults are requests while connected. Keep the last observed
    // internal values until a successful device readback confirms each item.
    if (!isConnected() || !client_)
    {
        guideRa_ = guideDec_ = 1.0F;
        backlashRa_ = backlashDec_ = 0;
        motorPower_ = 3;
        mountType_ = 1;
        forcedTrackingStopDegrees_ = 3;
        automaticGotoDegrees_ = 0;
        gotoSpeedIndex_ = 4;
    }

    GuideRateN[0].value = GuideRateN[1].value = 1.0;
    BacklashN[0].value = BacklashN[1].value = 0.0;
    IUResetSwitch(&MotorPowerSP);
    MotorPowerS[2].s = ISS_ON;
    IUResetSwitch(&MountTypeSP);
    MountTypeS[1].s = ISS_ON;
    IUResetSwitch(&GotoSpeedSP);
    GotoSpeedS[4].s = ISS_ON;
    IUResetSwitch(&MeridianDirectionSP);
    MeridianDirectionS[0].s = ISS_ON;

    MeridianN[0].value = 3.0;
    MeridianN[1].value = 0.0;

    GotoSpeedSP.s = GuideRateNP.s = BacklashNP.s = MotorPowerSP.s = MountTypeSP.s =
        MeridianDirectionSP.s = MeridianNP.s = IPS_IDLE;
}

bool VixenWirelessUnit::Connect()
{
    // Keep the user's pre-connection choice stable across the automatic
    // CONFIG_LOAD that Ekos sends after the connection becomes ready.
    connectionUsesDeviceSettings_ = useDeviceSettings();

    std::unique_ptr<vixen::wirelessunit::ITransport> transport;
    std::string endpointKey;
    if (isSimulation())
    {
        endpointKey = "simulation";
        // Fault injection is confined to Simulation. It exercises every
        // setting-batch failure and lost-ACK stage without touching a mount.
        static constexpr std::array<std::string_view, 6> testableSetters {
            vixen::wirelessunit::query::gotoSpeedName,
            vixen::wirelessunit::query::autoguideSpeedName,
            vixen::wirelessunit::query::backlashName,
            vixen::wirelessunit::query::motorPowerName,
            vixen::wirelessunit::query::mountTypeName,
            vixen::wirelessunit::query::meridianName
        };
        const auto injectedName = [](const char *variable) -> std::string
        {
            const char *value = std::getenv(variable);
            if (!value)
                return {};
            const std::string_view name { value };
            return std::find(testableSetters.begin(), testableSetters.end(), name) != testableSetters.end()
                       ? std::string(name) : std::string {};
        };
        const std::string rejected = injectedName("VIXEN_SIMULATOR_REJECT_SETTER");
        const std::string lostAck = injectedName("VIXEN_SIMULATOR_LOSE_SETTER_ACK");
        const std::string failedQuery = injectedName("VIXEN_SIMULATOR_FAIL_SETTING_QUERY");
        const bool nondefault = std::getenv("VIXEN_SIMULATOR_NONDEFAULT_MOUNT_SETTINGS") != nullptr;
        transport = std::make_unique<vixen::wirelessunit::SimulatorTransport>(rejected, lostAck,
                                                                          nondefault, failedQuery, true);
        if (!rejected.empty() || !lostAck.empty() || !failedQuery.empty())
            LOGF_WARN("Simulation-only mount-setting fault injection: reject=%s, lost ACK=%s, query timeout=%s.",
                      rejected.c_str(), lostAck.c_str(), failedQuery.c_str());
    }
    else
    {
        if (automaticDiscoverySelected())
        {
            const bool discoveryRequired = deviceDiscoveryState_ == DeviceDiscoveryState::notScanned ||
                                           deviceDiscoveryState_ == DeviceDiscoveryState::unavailable;
            if (discoveryRequired && discoverWirelessUnit(true) != DiscoveryResult::found)
            {
                LOG_ERROR("Connection stopped because automatic Device discovery did not find exactly one Wireless Unit.");
                return false;
            }
        }
        else if (manualIpSelected())
            LOGF_INFO("Using manually selected Wireless Unit endpoint %s:%s without Device discovery.",
                      EndpointT[0].text, EndpointT[1].text);
        else
        {
            ConnectionModeSP.s = IPS_ALERT;
            IDSetSwitch(&ConnectionModeSP, "Select Automatic discovery or Manual IP before connecting.");
            LOG_ERROR("Connection stopped because no connection mode is selected.");
            return false;
        }

        if (std::strcmp(EndpointT[0].text, vixen::wirelessunit::kUdpDefaultHost.data()) == 0)
        {
            EndpointTP.s = IPS_ALERT;
            const char *message = manualIpSelected()
                                      ? "Manual IP requires a specific Wireless Unit Host address."
                                      : "Device discovery completed without a usable Wireless Unit IP address.";
            IDSetText(&EndpointTP, "%s", message);
            LOGF_ERROR("%s", message);
            return false;
        }

        char *end = nullptr;
        const long parsedPort = std::strtol(EndpointT[1].text, &end, 10);
        if (!end || *end != '\0' || parsedPort < 1 || parsedPort > 65535)
        {
            LOG_ERROR("Port must be an integer from 1 to 65535.");
            return false;
        }
        // The currently verified Wireless Unit protocol is UDP-only. Keep the
        // TCP transport implementation in the source for future use.
        IUResetSwitch(&TransportSP);
        TransportS[1].s = ISS_ON;
        const auto kind = vixen::wirelessunit::TransportKind::udp;
        endpointKey = std::string(EndpointT[0].text) + ":" + EndpointT[1].text;
        transport = std::make_unique<vixen::wirelessunit::SocketTransport>(
            vixen::wirelessunit::Endpoint { EndpointT[0].text, static_cast<std::uint16_t>(parsedPort), kind });
    }

    // Preserve an uninitialized result across Disconnect/Connect to the same
    // unit. SYUTM persists a current MJD in the unit, so the second Connect
    // cannot independently recover that state from MJD alone.
    if (endpointKey != initializationEndpoint_)
    {
        initializationEndpoint_ = std::move(endpointKey);
        initializationState_ = MountInitializationState::unknown;
    }

    client_ = std::make_unique<vixen::wirelessunit::ProtocolClient>(std::move(transport));
    std::string error;
    if (!client_->connect(error))
    {
        LOGF_ERROR("Wireless Unit connection failed: %s", error.c_str());
        client_.reset();
        return false;
    }
    if (!Handshake())
    {
        client_->disconnect();
        client_.reset();
        return false;
    }
    // This driver owns its socket instead of using an INDI ConnectionPlugin.
    // DefaultDevice::Connect() therefore does not start Telescope::TimerHit()
    // for us. Start periodic MTSTA polling explicitly so KStars receives live
    // coordinates and state transitions during and after a GoTo.
    setCurrentPollingPeriod(kMountStatusPollingMilliseconds);
    SetTimer(kMountStatusPollingMilliseconds);
    LOG_INFO("Connected to Vixen Wireless Unit.");
    return true;
}

bool VixenWirelessUnit::Disconnect()
{
    cancelAutomaticDiscoveryConnect(nullptr, false);
    const bool reportedTracking = status_ && status_->status == 1 && status_->targetObjectType >= 3 &&
                                  !trackingKnownStopped_;
    const bool reportedActive = status_ && (status_->status == 2 || status_->status == 3 || reportedTracking);
    const bool stopRequired = vixen::wirelessunit::motion_safety::stopRequiredBeforeDisconnect(
                                  manualMotionActive(), guidePulseActive(),
                                  TrackState == SCOPE_SLEWING, TrackState == SCOPE_TRACKING,
                                  TrackState == SCOPE_PARKING, reportedActive);

    // Publish Idle/Tracking Off before the final transport transaction. If
    // this is delayed until after MTSTP, DefaultDevice removes the connected
    // properties immediately when Disconnect() returns and KStars can receive
    // delProperty before the queued EQUATORIAL_EOD_COORD Idle update. The
    // MTSTP round trip below gives the client time to consume this reset.
    resetTransientMountStateForDisconnect();
    if (client_ && stopRequired)
    {
        const auto stopped = stopMountMotion();
        if (stopped)
            LOG_INFO("Stopped active mount motion before disconnecting.");
        else
            LOGF_ERROR("Safety stop before disconnect failed: %s. "
                       "The mount may still be moving; disconnecting anyway.",
                       stopped.error.c_str());
    }
    cancelGuideTimers();
    if (client_)
        client_->disconnect();
    client_.reset();
    status_.reset();
    motionRa_ = motionDec_ = 0;
    trackingKnownStopped_ = false;
    AssumeHomeSP.s = IPS_IDLE;
    IDSetSwitch(&AssumeHomeSP, "%s",
                AssumeHomeS[0].s == ISS_ON
                    ? "Enabled: place the mount physically at Az 270 / Alt 0 before connecting."
                    : "Automatic HOME initialization is disabled.");
    alignmentListLoaded_ = false;
    wifiInformationLoaded_ = false;
    // These values may still be useful as the next connection's requested
    // settings, but they are no longer verified against a live Wireless Unit.
    GotoSpeedSP.s = GuideRateNP.s = BacklashNP.s = MotorPowerSP.s = MountTypeSP.s =
        MeridianDirectionSP.s = MeridianNP.s = IPS_IDLE;
    IDSetSwitch(&GotoSpeedSP, nullptr);
    IDSetNumber(&GuideRateNP, nullptr);
    IDSetNumber(&BacklashNP, nullptr);
    IDSetSwitch(&MotorPowerSP, nullptr);
    IDSetSwitch(&MountTypeSP, nullptr);
    IDSetSwitch(&MeridianDirectionSP, nullptr);
    IDSetNumber(&MeridianNP, nullptr);
    IUResetSwitch(&DiscoverySP);
    DiscoverySP.s = IPS_IDLE;
    IDSetSwitch(&DiscoverySP, nullptr);
    return true;
}

bool VixenWirelessUnit::Handshake()
{
    const auto wirelessVersion = client_->systemVersion();
    if (!wirelessVersion)
    {
        LOGF_ERROR("SYVER handshake failed: %s", wirelessVersion.error.c_str());
        return false;
    }
    if (*wirelessVersion.value < kMinimumWirelessFirmware)
    {
        LOGF_ERROR("Wireless firmware %.2f is older than required %.2f.",
                   *wirelessVersion.value, kMinimumWirelessFirmware);
        return false;
    }

    const auto identity = client_->mountIdentity();
    if (!identity)
    {
        LOGF_ERROR("MTMDL handshake failed: %s", identity.error.c_str());
        return false;
    }
    const int conservativeMaximum = identity.value->maximumSecondarySpeed > 0
                                        ? std::min(identity.value->maximumGotoSpeed,
                                                   identity.value->maximumSecondarySpeed)
                                        : identity.value->maximumGotoSpeed;
    maximumManualSlewMultiplier_ = conservativeMaximum > 0
                                       ? static_cast<double>(conservativeMaximum)
                                       : 100.0;
    if (slewMultipliers_[slewIndex_] > maximumManualSlewMultiplier_)
    {
        int safeIndex = 0;
        for (std::size_t index = 0; index < slewMultipliers_.size(); ++index)
        {
            if (slewMultipliers_[index] <= maximumManualSlewMultiplier_ &&
                slewMultipliers_[index] > slewMultipliers_[safeIndex])
                safeIndex = static_cast<int>(index);
        }
        LOGF_WARN("Saved slew rate %.1fx exceeds this mount's reported maximum %.1fx; using %.1fx.",
                  slewMultipliers_[slewIndex_], maximumManualSlewMultiplier_, slewMultipliers_[safeIndex]);
        slewIndex_ = safeIndex;
        SlewRateSP.reset();
        SlewRateSP[slewIndex_].setState(ISS_ON);
    }
    const auto initialStatus = client_->mountStatus();
    if (!initialStatus)
    {
        LOGF_ERROR("MTSTA handshake failed: %s", initialStatus.error.c_str());
        return false;
    }

    status_ = initialStatus.value;
    updateInitializationState(*status_);
    trackingKnownStopped_ = initializationState_ != MountInitializationState::initialized ||
                            (status_->status == 1 && status_->targetObjectType < 3);
    reconcileMountStateFromStatus();
    synchronizeTransientMountProperties();
    updateFirmwareProperties(*wirelessVersion.value, *identity.value);
    const auto site = client_->location();
    if (site)
    {
        timezoneHours_ = site.value->timezoneHours;
        double indiLongitude = site.value->longitudeDegrees;
        if (indiLongitude < 0)
            indiLongitude += 360;
        LocationNP[LOCATION_LONGITUDE].setValue(indiLongitude);
        LocationNP[LOCATION_LATITUDE].setValue(site.value->latitudeDegrees);
    }
    else
    {
        LOGF_WARN("SYLOC handshake read failed: %s; HOME initialization will use the current INDI site values.",
                  site.error.c_str());
    }

    if (AssumeHomeS[0].s == ISS_ON)
    {
        if (initializationState_ == MountInitializationState::uninitialized)
        {
            LOG_WARN("Assume HOME is enabled; initializing the current physical position as Az 270 / Alt 0.");
            // Failure intentionally does not tear down a valid network
            // connection. The manual Home set control remains available so
            // the operator can inspect the mount and retry safely.
            initializeHome(true);
        }
        else
        {
            AssumeHomeSP.s = IPS_OK;
            IDSetSwitch(&AssumeHomeSP, "Wireless Unit is already initialized; MTINI was not sent.");
        }
    }
    else
    {
        AssumeHomeSP.s = IPS_IDLE;
        IDSetSwitch(&AssumeHomeSP, "Automatic HOME initialization is disabled.");
    }

    bool settingsReady = false;
    if (connectionUsesDeviceSettings_)
    {
        settingsReady = readMountSettings();
    }
    else
    {
        // Always read the settings back after applying local values. The
        // UI then reports the values actually retained by the Wireless Unit,
        // rather than merely echoing the requested values.
        settingsReady = applyConfiguredMountSettings();
    }
    UseDeviceSettingsSP.s = settingsReady ? IPS_OK : IPS_ALERT;
    return settingsReady;
}

bool VixenWirelessUnit::readMountSettings()
{
    bool success = true;
    const auto gotoSpeed = client_->gotoSpeed();
    const auto gotoSpeedIt = gotoSpeed ? std::find(gotoSpeeds_.begin(), gotoSpeeds_.end(), *gotoSpeed.value)
                                       : gotoSpeeds_.end();
    if (gotoSpeed && gotoSpeedIt != gotoSpeeds_.end())
    {
        gotoSpeedIndex_ = static_cast<int>(std::distance(gotoSpeeds_.begin(), gotoSpeedIt));
        IUResetSwitch(&GotoSpeedSP);
        GotoSpeedS[gotoSpeedIndex_].s = ISS_ON;
        GotoSpeedSP.s = IPS_OK;
        IDSetSwitch(&GotoSpeedSP, nullptr);
    }
    else if (!gotoSpeed)
    {
        GotoSpeedSP.s = IPS_ALERT;
        IDSetSwitch(&GotoSpeedSP, "Could not read GoTo speed from the Wireless Unit.");
        LOGF_WARN("MTGTS settings read failed: %s", gotoSpeed.error.c_str());
        success = false;
    }
    else
    {
        GotoSpeedSP.s = IPS_ALERT;
        IDSetSwitch(&GotoSpeedSP, "Wireless Unit returned an unsupported SXP2 GoTo speed.");
        LOGF_WARN("MTGTS returned unsupported SXP2 value %d.", *gotoSpeed.value);
        success = false;
    }

    const auto rates = client_->guideRates();
    if (rates && rates.value->rightAscensionSidereal >= 0.1F && rates.value->rightAscensionSidereal <= 10.0F &&
        rates.value->declinationSidereal >= 0.1F && rates.value->declinationSidereal <= 10.0F)
    {
        guideRa_ = rates.value->rightAscensionSidereal;
        guideDec_ = rates.value->declinationSidereal;
        GuideRateN[0].value = guideRa_;
        GuideRateN[1].value = guideDec_;
        GuideRateNP.s = IPS_OK;
        IDSetNumber(&GuideRateNP, nullptr);
    }
    else if (!rates)
    {
        GuideRateNP.s = IPS_ALERT;
        IDSetNumber(&GuideRateNP, "Guide rate could not be verified; displayed values may be stale.");
        LOGF_WARN("MTAGS settings read failed: %s", rates.error.c_str());
        success = false;
    }
    else
    {
        GuideRateNP.s = IPS_ALERT;
        IDSetNumber(&GuideRateNP, "Wireless Unit returned invalid guide rates; displayed values are unverified.");
        LOG_WARN("MTAGS returned values outside the supported range 0.1..10.0.");
        success = false;
    }

    const auto backlash = client_->backlash();
    if (backlash && backlash.value->rightAscension >= 0 && backlash.value->rightAscension <= 99 &&
        backlash.value->declination >= 0 && backlash.value->declination <= 99)
    {
        backlashRa_ = backlash.value->rightAscension;
        backlashDec_ = backlash.value->declination;
        BacklashN[0].value = backlashRa_;
        BacklashN[1].value = backlashDec_;
        BacklashNP.s = IPS_OK;
        IDSetNumber(&BacklashNP, nullptr);
    }
    else if (!backlash)
    {
        BacklashNP.s = IPS_ALERT;
        IDSetNumber(&BacklashNP, "Backlash could not be verified; displayed values may be stale.");
        LOGF_WARN("MTBLC settings read failed: %s", backlash.error.c_str());
        success = false;
    }
    else
    {
        BacklashNP.s = IPS_ALERT;
        IDSetNumber(&BacklashNP, "Wireless Unit returned invalid backlash; displayed values are unverified.");
        LOG_WARN("MTBLC returned values outside the supported range 0..99.");
        success = false;
    }

    const auto motorPower = client_->motorPower();
    if (motorPower && *motorPower.value >= 1 && *motorPower.value <= 4)
    {
        motorPower_ = *motorPower.value;
        IUResetSwitch(&MotorPowerSP);
        MotorPowerS[motorPower_ - 1].s = ISS_ON;
        MotorPowerSP.s = IPS_OK;
        IDSetSwitch(&MotorPowerSP, nullptr);
    }
    else if (!motorPower)
    {
        MotorPowerSP.s = IPS_ALERT;
        IDSetSwitch(&MotorPowerSP, "Motor power could not be verified; displayed value may be stale.");
        LOGF_WARN("MTPWR settings read failed: %s", motorPower.error.c_str());
        success = false;
    }
    else
    {
        MotorPowerSP.s = IPS_ALERT;
        IDSetSwitch(&MotorPowerSP, "Wireless Unit returned invalid motor power; displayed value is unverified.");
        LOG_WARN("MTPWR returned a value outside the supported range 1..4.");
        success = false;
    }

    const auto mountType = client_->mountType();
    if (mountType && *mountType.value >= 0 && *mountType.value <= 1)
    {
        mountType_ = *mountType.value;
        IUResetSwitch(&MountTypeSP);
        MountTypeS[mountType_].s = ISS_ON;
        MountTypeSP.s = IPS_OK;
        IDSetSwitch(&MountTypeSP, nullptr);
    }
    else if (!mountType)
    {
        MountTypeSP.s = IPS_ALERT;
        IDSetSwitch(&MountTypeSP, "Polar alignment could not be verified; displayed value may be stale.");
        LOGF_WARN("MTTYP settings read failed: %s", mountType.error.c_str());
        success = false;
    }
    else
    {
        MountTypeSP.s = IPS_ALERT;
        IDSetSwitch(&MountTypeSP, "Wireless Unit returned invalid polar alignment; displayed value is unverified.");
        LOG_WARN("MTTYP returned a value outside the supported range 0..1.");
        success = false;
    }

    const auto meridian = client_->meridianSettings();
    if (meridian && vixen::wirelessunit::numeric_validation::validMeridianSettings(
                        meridian.value->forcedTrackingStopDegrees,
                        meridian.value->automaticGotoDegrees))
    {
        forcedTrackingStopDegrees_ = meridian.value->forcedTrackingStopDegrees;
        automaticGotoDegrees_ = meridian.value->automaticGotoDegrees;
        MeridianN[0].value = forcedTrackingStopDegrees_;
        MeridianN[1].value = std::abs(automaticGotoDegrees_);
        // A zero MTCOM value carries no direction. Preserve the current UI
        // choice in that case; otherwise derive East/West from its sign.
        if (automaticGotoDegrees_ != 0)
        {
            IUResetSwitch(&MeridianDirectionSP);
            MeridianDirectionS[automaticGotoDegrees_ < 0 ? 1 : 0].s = ISS_ON;
        }
        MeridianDirectionSP.s = IPS_OK;
        IDSetSwitch(&MeridianDirectionSP, nullptr);
        MeridianNP.s = IPS_OK;
        IDSetNumber(&MeridianNP, nullptr);
    }
    else if (!meridian)
    {
        MeridianNP.s = IPS_ALERT;
        MeridianDirectionSP.s = IPS_ALERT;
        IDSetNumber(&MeridianNP, "Could not read Meridian behavior from the Wireless Unit.");
        IDSetSwitch(&MeridianDirectionSP, nullptr);
        LOGF_WARN("MTCOM settings read failed: %s", meridian.error.c_str());
        success = false;
    }
    else
    {
        MeridianNP.s = IPS_ALERT;
        MeridianDirectionSP.s = IPS_ALERT;
        IDSetNumber(&MeridianNP,
                    "Wireless Unit returned invalid Meridian behavior values; GoTo flip point must not exceed Stop point.");
        IDSetSwitch(&MeridianDirectionSP, nullptr);
        LOG_WARN("MTCOM returned values outside the supported ranges or GoTo flip point was above Stop point.");
        success = false;
    }

    if (!success)
        LOG_ERROR("Could not load all required mount settings from the Wireless Unit.");
    return success;
}

bool VixenWirelessUnit::applyConfiguredMountSettings()
{
    // Keep pending UI values separate from confirmed device members. No
    // member is promoted to current state until a successful readback below.
    const double requestedGuideRaValue = GuideRateN[0].value;
    const double requestedGuideDecValue = GuideRateN[1].value;
    const double requestedBacklashRaValue = BacklashN[0].value;
    const double requestedBacklashDecValue = BacklashN[1].value;
    const double requestedStopValue = MeridianN[0].value;
    const double requestedGotoValue = MeridianN[1].value;
    const int gotoDirectionIndex = IUFindOnSwitchIndex(&MeridianDirectionSP);
    const int motorPowerIndex = IUFindOnSwitchIndex(&MotorPowerSP);
    const int mountTypeIndex = IUFindOnSwitchIndex(&MountTypeSP);
    const int requestedGotoSpeedIndex = IUFindOnSwitchIndex(&GotoSpeedSP);
    const auto backlashRa = vixen::wirelessunit::numeric_validation::integerInRange(requestedBacklashRaValue, 0, 99);
    const auto backlashDec = vixen::wirelessunit::numeric_validation::integerInRange(requestedBacklashDecValue, 0, 99);
    const auto stop = vixen::wirelessunit::numeric_validation::integerInRange(requestedStopValue, 0, 90);
    const auto magnitude = vixen::wirelessunit::numeric_validation::integerInRange(requestedGotoValue, 0, 90);
    const int requestedAutomaticGoto = gotoDirectionIndex == 1 && magnitude
                                           ? -*magnitude : (magnitude ? *magnitude : 0);
    const bool valid = vixen::wirelessunit::numeric_validation::finiteInRange(requestedGuideRaValue, 0.1, 10.0) &&
                       vixen::wirelessunit::numeric_validation::finiteInRange(requestedGuideDecValue, 0.1, 10.0) &&
                       backlashRa && backlashDec && stop && magnitude &&
                       motorPowerIndex >= 0 && motorPowerIndex < 4 && mountTypeIndex >= 0 && mountTypeIndex < 2 &&
                       requestedGotoSpeedIndex >= 0 &&
                       requestedGotoSpeedIndex < static_cast<int>(gotoSpeeds_.size()) &&
                       gotoDirectionIndex >= 0 && gotoDirectionIndex < 2 &&
                       vixen::wirelessunit::numeric_validation::validMeridianSettings(*stop,
                                                                                     requestedAutomaticGoto);
    if (!valid)
    {
        MeridianNP.s = IPS_ALERT;
        MeridianDirectionSP.s = IPS_ALERT;
        IDSetNumber(&MeridianNP,
                    "Configured mount settings are invalid; no values were sent to the Wireless Unit.");
        IDSetSwitch(&MeridianDirectionSP, nullptr);
        LOG_ERROR("Configured mount settings are outside their supported ranges or Meridian behavior is invalid.");
        return false;
    }

    const float requestedGuideRa = static_cast<float>(requestedGuideRaValue);
    const float requestedGuideDec = static_cast<float>(requestedGuideDecValue);
    const int requestedBacklashRa = *backlashRa;
    const int requestedBacklashDec = *backlashDec;
    const int requestedStop = *stop;
    const int requestedMotorPower = motorPowerIndex + 1;

    // Stop the batch after a failed or ambiguous acknowledgement. A later
    // query of all six settings establishes which values the unit actually
    // holds, including setters that were not attempted.
    int failedStep = -1;
    const auto gotoSpeed = client_->setGotoSpeedValue(gotoSpeeds_[requestedGotoSpeedIndex]);
    if (!gotoSpeed)
    {
        failedStep = 0;
        LOGF_ERROR("MTGTS settings apply failed: %s", gotoSpeed.error.c_str());
    }
    if (failedStep < 0)
    {
        const auto rates = client_->setGuideRates(requestedGuideRa, requestedGuideDec);
        if (!rates)
        {
            failedStep = 1;
            LOGF_ERROR("MTAGS settings apply failed: %s", rates.error.c_str());
        }
    }
    if (failedStep < 0)
    {
        const auto backlash = client_->setBacklashCompensation(requestedBacklashRa, requestedBacklashDec);
        if (!backlash)
        {
            failedStep = 2;
            LOGF_ERROR("MTBLC settings apply failed: %s", backlash.error.c_str());
        }
    }
    if (failedStep < 0)
    {
        const auto motorPower = client_->setMotorPowerLevel(requestedMotorPower);
        if (!motorPower)
        {
            failedStep = 3;
            LOGF_ERROR("MTPWR settings apply failed: %s", motorPower.error.c_str());
        }
    }
    if (failedStep < 0)
    {
        const auto mountType = client_->setMountTypeValue(mountTypeIndex);
        if (!mountType)
        {
            failedStep = 4;
            LOGF_ERROR("MTTYP settings apply failed: %s", mountType.error.c_str());
        }
    }
    if (failedStep < 0)
    {
        const auto meridian = client_->setMeridianSettings(requestedStop, requestedAutomaticGoto);
        if (!meridian)
        {
            failedStep = 5;
            LOGF_ERROR("MTCOM settings apply failed: %s", meridian.error.c_str());
        }
    }

    const bool readReady = readMountSettings();
    bool allMatched = readReady;
    if (GotoSpeedSP.s == IPS_OK && gotoSpeedIndex_ != requestedGotoSpeedIndex)
    {
        GotoSpeedSP.s = IPS_ALERT;
        IDSetSwitch(&GotoSpeedSP, "Requested GoTo speed was not applied; current device value is shown.");
        allMatched = false;
    }
    if (GuideRateNP.s == IPS_OK &&
        (std::abs(guideRa_ - requestedGuideRa) > 0.001F ||
         std::abs(guideDec_ - requestedGuideDec) > 0.001F))
    {
        GuideRateNP.s = IPS_ALERT;
        IDSetNumber(&GuideRateNP, "Requested guide rate was not applied; current device values are shown.");
        allMatched = false;
    }
    if (BacklashNP.s == IPS_OK &&
        (backlashRa_ != requestedBacklashRa || backlashDec_ != requestedBacklashDec))
    {
        BacklashNP.s = IPS_ALERT;
        IDSetNumber(&BacklashNP, "Requested backlash was not applied; current device values are shown.");
        allMatched = false;
    }
    if (MotorPowerSP.s == IPS_OK && motorPower_ != requestedMotorPower)
    {
        MotorPowerSP.s = IPS_ALERT;
        IDSetSwitch(&MotorPowerSP, "Requested motor power was not applied; current device value is shown.");
        allMatched = false;
    }
    if (MountTypeSP.s == IPS_OK && mountType_ != mountTypeIndex)
    {
        MountTypeSP.s = IPS_ALERT;
        IDSetSwitch(&MountTypeSP, "Requested polar alignment was not applied; current device value is shown.");
        allMatched = false;
    }
    if (MeridianNP.s == IPS_OK &&
        (forcedTrackingStopDegrees_ != requestedStop || automaticGotoDegrees_ != requestedAutomaticGoto))
    {
        MeridianNP.s = IPS_ALERT;
        MeridianDirectionSP.s = IPS_ALERT;
        IDSetNumber(&MeridianNP, "Requested Meridian behavior was not applied; current device values are shown.");
        IDSetSwitch(&MeridianDirectionSP, nullptr);
        allMatched = false;
    }

    if (failedStep >= 0)
    {
        const char *message = "Setting command failed; read-back value is shown if available, otherwise unverified.";
        switch (failedStep)
        {
            case 0: GotoSpeedSP.s = IPS_ALERT; IDSetSwitch(&GotoSpeedSP, "%s", message); break;
            case 1: GuideRateNP.s = IPS_ALERT; IDSetNumber(&GuideRateNP, "%s", message); break;
            case 2: BacklashNP.s = IPS_ALERT; IDSetNumber(&BacklashNP, "%s", message); break;
            case 3: MotorPowerSP.s = IPS_ALERT; IDSetSwitch(&MotorPowerSP, "%s", message); break;
            case 4: MountTypeSP.s = IPS_ALERT; IDSetSwitch(&MountTypeSP, "%s", message); break;
            case 5:
                MeridianNP.s = MeridianDirectionSP.s = IPS_ALERT;
                IDSetNumber(&MeridianNP, "%s", message);
                IDSetSwitch(&MeridianDirectionSP, nullptr);
                break;
        }
        LOG_ERROR("Mount settings batch stopped after an unconfirmed command; all six values were queried again.");
        return false;
    }
    if (!allMatched)
    {
        LOG_ERROR("Mount settings batch could not verify every requested value after read-back.");
        return false;
    }

    LOG_INFO("Configured mount settings were sent and all six values were verified by read-back.");
    return true;
}

void VixenWirelessUnit::updateFirmwareProperties(double wirelessVersion,
                                              const vixen::wirelessunit::MountIdentity &identity)
{
    char version[32] {};
    char maximum[32] {};
    std::snprintf(version, sizeof(version), "%.2f", wirelessVersion);
    std::snprintf(maximum, sizeof(maximum), "%d", identity.maximumGotoSpeed);
    IUSaveText(&FirmwareT[0], version);
    IUSaveText(&FirmwareT[1], mountModelName(identity.model).c_str());
    IUSaveText(&FirmwareT[2], maximum);
}

bool VixenWirelessUnit::ReadScopeStatus()
{
    if (!client_)
        return false;

    if ((motionRa_ != 0 || motionDec_ != 0) && !sendContinuousMotion())
    {
        status_.reset();
        return false;
    }

    const auto requestStarted = std::chrono::steady_clock::now();
    const auto result = client_->mountStatus();
    const double responseMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - requestStarted).count();
    if (!result)
    {
        if (manualMotionActive())
            abandonManualMotion("MTSTA failed; manual motion will expire and was not queued for retransmission.");
        status_.reset();
        LOGF_ERROR("MTSTA failed after %.3f ms: %s", responseMilliseconds, result.error.c_str());
        return false;
    }
    LOGF_DEBUG("MTSTA response time: %.3f ms", responseMilliseconds);
    status_ = result.value;
    updateInitializationState(*status_);

    if (initializationState_ != MountInitializationState::initialized)
    {
        reconcileMountStateFromStatus();
        if (TrackStateSP[TRACK_ON].getState() == ISS_ON || TrackStateSP.getState() != IPS_IDLE)
        {
            TrackStateSP.reset();
            TrackStateSP[TRACK_OFF].setState(ISS_ON);
            TrackStateSP.setState(IPS_IDLE);
            TrackStateSP.apply("Set HOME before enabling sky-coordinate controls.");
        }
        // Before MTINI the reported RA/Dec are based on the MJD epoch and are
        // not valid sky coordinates. Keep polling mechanical state, but do not
        // publish those coordinates to KStars.
        return true;
    }

    reconcileMountStateFromStatus();

    // The INDI base class marks the complete Tracking switch vector Alert
    // when a tracking command is rejected. That makes both On and Off appear
    // red in KStars. Clear the vector error whenever the mount is known not to
    // be tracking, including while it is parking or after it becomes parked.
    if (trackingKnownStopped_ && TrackState != SCOPE_TRACKING &&
            (TrackStateSP.getState() == IPS_ALERT || TrackStateSP[TRACK_ON].getState() == ISS_ON))
    {
        TrackStateSP.reset();
        TrackStateSP[TRACK_OFF].setState(ISS_ON);
        TrackStateSP.setState(IPS_IDLE);
        TrackStateSP.apply();
    }

    NewRaDec(status_->rightAscensionHours, status_->declinationDegrees);
    return true;
}

bool VixenWirelessUnit::Goto(double ra, double dec)
{
    if (!client_ || !requireInitialized("GoTo"))
        return false;
    if (!targetMotionStartAllowed("GoTo"))
        return false;
    const auto result = client_->slewRaDec(ra, dec);
    if (!result)
    {
        LOGF_ERROR("GoTo failed: %s", result.error.c_str());
        return false;
    }
    trackingKnownStopped_ = false;
    TrackState = SCOPE_SLEWING;
    return true;
}

bool VixenWirelessUnit::Sync(double ra, double dec)
{
    if (!client_ || !requireInitialized("Sync"))
        return false;
    const auto result = client_->syncRaDec(ra, dec);
    if (!result)
    {
        LOGF_ERROR("Sync failed: %s", result.error.c_str());
        return false;
    }
    NewRaDec(ra, dec);
    if (!readAlignmentPoints())
        LOG_WARN("Sync succeeded, but the alignment list could not be refreshed.");
    return true;
}

bool VixenWirelessUnit::Park()
{
    if (!client_ || !requireInitialized("Park"))
        return false;
    if (!targetMotionStartAllowed("Park"))
        return false;

    const auto result = client_->home();
    if (!result)
    {
        LOGF_ERROR("Park to HOME failed: %s", result.error.c_str());
        return false;
    }

    TrackState = SCOPE_PARKING;
    trackingKnownStopped_ = true;

    // HOME motion is not sidereal tracking. Publish a normal Tracking Off
    // state immediately instead of leaving a previous vector Alert visible
    // for the duration of parking.
    TrackStateSP.reset();
    TrackStateSP[TRACK_OFF].setState(ISS_ON);
    TrackStateSP.setState(IPS_IDLE);
    TrackStateSP.apply();

    LOG_INFO("Parking to the Wireless Unit HOME position (west horizon)...");
    return true;
}

bool VixenWirelessUnit::UnPark()
{
    if (!requireInitialized("Unpark"))
        return false;
    // Wireless Unit has no separate mechanical unpark command. Unpark only
    // releases INDI's parked interlock; the next GoTo supplies a new target.
    SetParked(false);
    return true;
}

bool VixenWirelessUnit::Abort()
{
    motionRa_ = motionDec_ = 0;
    const bool guideNSWasActive = guideNSTimerId_ != 0;
    const bool guideWEWasActive = guideWETimerId_ != 0;
    cancelGuideTimers();
    if (guideNSWasActive)
        GuideComplete(AXIS_DE);
    if (guideWEWasActive)
        GuideComplete(AXIS_RA);
    if (!client_)
        return false;
    const auto result = stopMountMotion();
    if (result)
    {
        // MTSTP is acknowledged before an initialized SXP2 finishes
        // decelerating. MTMV0 zero stops the pre-initialization axes directly.
        TrackState = initializationState_ == MountInitializationState::uninitialized
                         ? SCOPE_IDLE
                         : SCOPE_SLEWING;
        trackingKnownStopped_ = true;
    }
    else
        LOGF_ERROR("Abort failed: %s", result.error.c_str());
    return static_cast<bool>(result);
}

vixen::wirelessunit::Result<void> VixenWirelessUnit::stopMountMotion()
{
    if (!client_)
        return { false, "not connected" };
    return initializationState_ == MountInitializationState::uninitialized
               ? client_->stopBeforeInitialization()
               : client_->abortMotion();
}

void VixenWirelessUnit::reconcileMountStateFromStatus()
{
    if (!status_)
        return;

    const bool parkingRequested = TrackState == SCOPE_PARKING;
    const bool parkedWasConfirmed = isParked();
    const bool stoppedAtHome = status_->status == 1 && status_->targetObjectType == 1;
    const bool moving = status_->status == 2 || status_->status == 3;
    const bool parkPropertyIsTransient = ParkSP.getState() == IPS_BUSY || ParkSP.getState() == IPS_ALERT;

    auto clearUnconfirmedPark = [this, parkPropertyIsTransient]()
    {
        if (isParked() || ParkSP[PARK].getState() == ISS_ON || parkPropertyIsTransient)
            SetParked(false);
    };

    if (initializationState_ != MountInitializationState::initialized)
    {
        clearUnconfirmedPark();
        TrackState = SCOPE_IDLE;
        trackingKnownStopped_ = true;
        return;
    }

    // A HOME label by itself is insufficient: MTINI also leaves the mount at
    // HOME without meaning that the operator parked it. Require either the
    // current Park operation or a previously confirmed/persisted Park state.
    if (stoppedAtHome && (parkingRequested || parkedWasConfirmed))
    {
        if (!isParked() || ParkSP.getState() != IPS_OK)
            SetParked(true);
        TrackState = SCOPE_PARKED;
        trackingKnownStopped_ = true;
        if (parkingRequested)
            LOG_INFO("Parking completed at the Wireless Unit HOME position.");
        return;
    }

    if (moving)
    {
        clearUnconfirmedPark();
        TrackState = parkingRequested ? SCOPE_PARKING : SCOPE_SLEWING;
        return;
    }

    clearUnconfirmedPark();
    if (status_->status == 1 && status_->targetObjectType >= 3 && !trackingKnownStopped_)
        TrackState = SCOPE_TRACKING;
    else
    {
        TrackState = SCOPE_IDLE;
        if (status_->status == 1 && status_->targetObjectType < 3)
            trackingKnownStopped_ = true;
    }
}

void VixenWirelessUnit::resetTransientMountStateForDisconnect()
{
    const bool confirmedParked = initializationState_ == MountInitializationState::initialized &&
                                 status_ && status_->status == 1 && status_->targetObjectType == 1 &&
                                 isParked();
    if (confirmedParked)
        TrackState = SCOPE_PARKED;
    else
    {
        if (isParked() || ParkSP[PARK].getState() == ISS_ON ||
                ParkSP.getState() == IPS_BUSY || ParkSP.getState() == IPS_ALERT)
            SetParked(false);
        TrackState = SCOPE_IDLE;
    }

    synchronizeTransientMountProperties();
    // Disconnect() runs before the connected-only properties are removed.
    // Publish the reset now so KStars does not retain its last Tracking,
    // Slewing, Parking, manual-motion, or Abort display after disconnection.
    TrackStateSP.apply();
    EqNP.apply();
    AbortSP.apply();
    MovementNSSP.apply();
    MovementWESP.apply();
}

void VixenWirelessUnit::synchronizeTransientMountProperties()
{
    TrackStateSP.reset();
    if (TrackState == SCOPE_TRACKING)
    {
        TrackStateSP[TRACK_ON].setState(ISS_ON);
        TrackStateSP.setState(IPS_BUSY);
        EqNP.setState(IPS_OK);
    }
    else
    {
        TrackStateSP[TRACK_OFF].setState(ISS_ON);
        TrackStateSP.setState(IPS_IDLE);
        EqNP.setState(TrackState == SCOPE_SLEWING || TrackState == SCOPE_PARKING ? IPS_BUSY : IPS_IDLE);
    }

    AbortSP.reset();
    AbortSP.setState(IPS_IDLE);
    MovementNSSP.reset();
    MovementNSSP.setState(IPS_IDLE);
    MovementWESP.reset();
    MovementWESP.setState(IPS_IDLE);
    RememberTrackState = TrackState;
}

bool VixenWirelessUnit::SetSlewRate(int index)
{
    if (index < 0 || index >= static_cast<int>(slewMultipliers_.size()))
        return false;
    if (isConnected() && slewMultipliers_[index] > maximumManualSlewMultiplier_)
    {
        LOGF_ERROR("Slew rate %.1fx exceeds this mount's reported maximum %.1fx.",
                   slewMultipliers_[index], maximumManualSlewMultiplier_);
        return false;
    }
    slewIndex_ = index;
    return true;
}

bool VixenWirelessUnit::MoveNS(INDI_DIR_NS dir, TelescopeMotionCommand command)
{
    if (command != MOTION_STOP && !manualMotionStartAllowed())
        return false;
    if (command == MOTION_STOP)
        motionDec_ = 0;
    else
        motionDec_ = (dir == DIRECTION_NORTH ? 1.0 : -1.0) * slewMultipliers_[slewIndex_];
    return sendContinuousMotion();
}

bool VixenWirelessUnit::MoveWE(INDI_DIR_WE dir, TelescopeMotionCommand command)
{
    if (command != MOTION_STOP && !manualMotionStartAllowed())
        return false;
    if (command == MOTION_STOP)
        motionRa_ = 0;
    else
        motionRa_ = (dir == DIRECTION_EAST ? 1.0 : -1.0) * slewMultipliers_[slewIndex_];
    return sendContinuousMotion();
}

bool VixenWirelessUnit::manualMotionActive() const
{
    return motionRa_ != 0 || motionDec_ != 0;
}

bool VixenWirelessUnit::guidePulseActive() const
{
    return guideNSTimerId_ != 0 || guideWETimerId_ != 0;
}

bool VixenWirelessUnit::manualMotionStartAllowed() const
{
    if (guidePulseActive())
    {
        LOG_ERROR("Manual motion is unavailable while a pulse guide is active.");
        return false;
    }
    if ((TrackState == SCOPE_SLEWING && !manualMotionActive()) || TrackState == SCOPE_PARKING)
    {
        LOG_ERROR("Manual motion is unavailable while GoTo or parking is active.");
        return false;
    }
    return true;
}

bool VixenWirelessUnit::targetMotionStartAllowed(const char *operation) const
{
    if (manualMotionActive())
    {
        LOGF_ERROR("%s is unavailable while manual motion is active; stop manual motion first.", operation);
        return false;
    }
    if (guidePulseActive())
    {
        LOGF_ERROR("%s is unavailable while a pulse guide is active.", operation);
        return false;
    }
    return true;
}

void VixenWirelessUnit::abandonManualMotion(const char *message)
{
    const bool raWasActive = motionRa_ != 0 || MovementWESP.getState() == IPS_BUSY;
    const bool decWasActive = motionDec_ != 0 || MovementNSSP.getState() == IPS_BUSY;
    motionRa_ = motionDec_ = 0;
    last_ns_motion = last_we_motion = -1;

    if (raWasActive)
    {
        MovementWESP.reset();
        MovementWESP.setState(IPS_ALERT);
        MovementWESP.apply("%s", message);
    }
    if (decWasActive)
    {
        MovementNSSP.reset();
        MovementNSSP.setState(IPS_ALERT);
        MovementNSSP.apply("%s", message);
    }
    LOGF_ERROR("%s", message);
}

bool VixenWirelessUnit::sendContinuousMotion()
{
    if (!client_)
    {
        motionRa_ = motionDec_ = 0;
        return false;
    }
    if (initializationState_ == MountInitializationState::unknown)
    {
        LOG_ERROR("Manual motion is unavailable until MTSTA determines the initialization state.");
        motionRa_ = motionDec_ = 0;
        return false;
    }
    if (initializationState_ == MountInitializationState::initialized &&
            (isParked() || TrackState == SCOPE_PARKED || TrackState == SCOPE_PARKING) &&
            (motionRa_ != 0 || motionDec_ != 0))
    {
        motionRa_ = motionDec_ = 0;
        LOG_ERROR("Unpark before moving a parked mount.");
        return false;
    }

    // MTMV0 uses the opposite RA sign from the standard INDI East/West
    // convention on the tested SXP2: positive is West and negative is East.
    // Refresh once per MTSTA poll. A 3 s command lifetime leaves room for the
    // poll interval plus both UDP round trips, also before HOME initialization.
    const auto result = initializationState_ == MountInitializationState::uninitialized
                            ? client_->moveBeforeInitialization(
                                  vixen::wirelessunit::motion_timing::continuousMotionDurationSeconds,
                                  -motionRa_, motionDec_)
                            : client_->moveRaDec(
                                  vixen::wirelessunit::motion_timing::continuousMotionDurationSeconds,
                                  motionRa_, motionDec_);
    if (!result)
    {
        LOGF_ERROR("Manual motion (%s) failed: %s",
                   initializationState_ == MountInitializationState::uninitialized ? "MTMV0" : "MTMV2",
                   result.error.c_str());
        // A UDP request may reach the unit even when its reply is lost. Forget
        // both axes so a later status poll cannot reconnect and replay it.
        // The finite MTMV0/MTMV2 duration lets the unit stop by itself.
        abandonManualMotion("Manual motion command failed; the request was discarded and will not be retransmitted.");
        status_.reset();
    }
    return static_cast<bool>(result);
}

bool VixenWirelessUnit::SetTrackEnabled(bool enabled)
{
    if (!client_)
        return false;
    if (initializationState_ != MountInitializationState::initialized)
    {
        trackingKnownStopped_ = true;
        if (enabled)
            return requireInitialized("Tracking");
        return true;
    }
    vixen::wirelessunit::Result<void> result;
    if (enabled)
    {
        if (!targetMotionStartAllowed("Tracking"))
            return false;
        // Tracking uses the mount's current RA/Dec as a new MTTGT target. Do
        // not reuse a cached status after a timeout or transparent UDP socket
        // reconnect; require a valid MTSTA immediately before the command.
        const auto currentStatus = client_->mountStatus();
        if (!currentStatus)
        {
            status_.reset();
            trackingKnownStopped_ = true;
            LOGF_ERROR("Tracking was not started because a fresh MTSTA could not be read: %s",
                       currentStatus.error.c_str());
            return false;
        }
        status_ = currentStatus.value;
        updateInitializationState(*status_);
        if (initializationState_ != MountInitializationState::initialized)
        {
            trackingKnownStopped_ = true;
            return requireInitialized("Tracking");
        }
        result = client_->startTrackingAt(status_->rightAscensionHours, status_->declinationDegrees);
    }
    else
    {
        result = client_->stopTracking();
    }
    if (!result)
    {
        LOGF_ERROR("Tracking command failed: %s", result.error.c_str());
        // A rejected MTTGT can leave the previous target type in MTSTA. Do not
        // let the next poll reinterpret that stale target as active tracking.
        if (enabled)
        {
            trackingKnownStopped_ = true;
            TrackState = SCOPE_IDLE;
        }
    }
    else
    {
        trackingKnownStopped_ = !enabled;
    }
    return static_cast<bool>(result);
}

bool VixenWirelessUnit::updateLocation(double latitude, double longitude, double elevation)
{
    INDI_UNUSED(elevation);
    if (longitude > 180)
        longitude -= 360;
    const auto result = client_ ? client_->setSite(longitude, latitude, timezoneHours_)
                                : vixen::wirelessunit::Result<void> { false, "not connected" };
    if (!result)
        LOGF_ERROR("Location update failed: %s", result.error.c_str());
    return static_cast<bool>(result);
}

bool VixenWirelessUnit::updateTime(ln_date *utc, double utcOffset)
{
    timezoneHours_ = static_cast<float>(utcOffset);
    if (initializationState_ == MountInitializationState::uninitialized)
    {
        // Home set sends this time immediately before MTINI. Sending SYUTM as
        // part of INDI's automatic post-Connect update would replace the
        // near-zero MJD and make a later reconnect look initialized.
        LOG_DEBUG("Deferred UTC update until HOME initialization.");
        return true;
    }

    std::tm time {};
    time.tm_year = utc->years - 1900;
    time.tm_mon = utc->months - 1;
    time.tm_mday = utc->days;
    time.tm_hour = utc->hours;
    time.tm_min = utc->minutes;
    time.tm_sec = static_cast<int>(std::floor(utc->seconds));
    const auto fractional = utc->seconds - std::floor(utc->seconds);
    const auto point = std::chrono::system_clock::from_time_t(timegm(&time)) +
                       std::chrono::duration_cast<std::chrono::system_clock::duration>(
                           std::chrono::duration<double>(fractional));
    const auto result = client_ ? client_->setTime(point)
                                : vixen::wirelessunit::Result<void> { false, "not connected" };
    if (!result)
        LOGF_ERROR("UTC update failed: %s", result.error.c_str());
    return static_cast<bool>(result);
}

IPState VixenWirelessUnit::guide(std::uint32_t ms, double raSign, double decSign)
{
    if (!client_ || !requireInitialized("Pulse guide") || ms < 10 || ms > 10000)
        return IPS_ALERT;
    if (isParked() || TrackState == SCOPE_PARKED || TrackState == SCOPE_PARKING)
    {
        LOG_ERROR("Pulse guide is unavailable while the mount is parked or parking.");
        return IPS_ALERT;
    }
    if (manualMotionActive())
    {
        LOG_ERROR("Pulse guide is unavailable while manual motion is active; stop manual motion first.");
        return IPS_ALERT;
    }
    if (TrackState == SCOPE_SLEWING)
    {
        LOG_ERROR("Pulse guide is unavailable while GoTo is active.");
        return IPS_ALERT;
    }
    const double seconds = static_cast<double>(ms) / 1000.0;
    const auto result = client_->pulseGuide(seconds, raSign * guideRa_, decSign * guideDec_);
    if (!result)
    {
        LOGF_ERROR("Pulse guide failed: %s", result.error.c_str());
        return IPS_ALERT;
    }

    // MTMV2 includes the pulse duration, so the Wireless Unit stops the pulse
    // itself.  Keep the INDI property Busy for that duration and clear both
    // direction values locally afterwards.  Leaving the completed value in
    // the property makes a later opposite-direction request contain two
    // non-zero values; GuiderInterface then gives priority to N or W.
    if (decSign != 0)
    {
        if (guideNSTimerId_ != 0)
            IERmTimer(guideNSTimerId_);
        guideNSTimerId_ = IEAddTimer(static_cast<int>(ms), guideNSCompleteHelper, this);
    }
    else
    {
        if (guideWETimerId_ != 0)
            IERmTimer(guideWETimerId_);
        guideWETimerId_ = IEAddTimer(static_cast<int>(ms), guideWECompleteHelper, this);
    }

    return IPS_BUSY;
}

void VixenWirelessUnit::guideNSCompleteHelper(void *context)
{
    static_cast<VixenWirelessUnit *>(context)->guideNSComplete();
}

void VixenWirelessUnit::guideWECompleteHelper(void *context)
{
    static_cast<VixenWirelessUnit *>(context)->guideWEComplete();
}

void VixenWirelessUnit::guideNSComplete()
{
    guideNSTimerId_ = 0;
    GuideNSNP[0].setValue(0);
    GuideNSNP[1].setValue(0);
    GuideComplete(AXIS_DE);
}

void VixenWirelessUnit::guideWEComplete()
{
    guideWETimerId_ = 0;
    GuideWENP[0].setValue(0);
    GuideWENP[1].setValue(0);
    GuideComplete(AXIS_RA);
}

void VixenWirelessUnit::cancelGuideTimers()
{
    if (guideNSTimerId_ != 0)
    {
        IERmTimer(guideNSTimerId_);
        guideNSTimerId_ = 0;
    }
    if (guideWETimerId_ != 0)
    {
        IERmTimer(guideWETimerId_);
        guideWETimerId_ = 0;
    }
    GuideNSNP[0].setValue(0);
    GuideNSNP[1].setValue(0);
    GuideWENP[0].setValue(0);
    GuideWENP[1].setValue(0);
    GuideNSNP.setState(IPS_IDLE);
    GuideWENP.setState(IPS_IDLE);
}

IPState VixenWirelessUnit::GuideNorth(std::uint32_t ms)
{
    return guide(ms, 0, 1);
}

IPState VixenWirelessUnit::GuideSouth(std::uint32_t ms)
{
    return guide(ms, 0, -1);
}

IPState VixenWirelessUnit::GuideEast(std::uint32_t ms)
{
    return guide(ms, 1, 0);
}

IPState VixenWirelessUnit::GuideWest(std::uint32_t ms)
{
    return guide(ms, -1, 0);
}
