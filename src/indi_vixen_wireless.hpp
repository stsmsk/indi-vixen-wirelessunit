/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#pragma once

#include "client.hpp"

#include <indiguiderinterface.h>
#include <inditelescope.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

class VixenWirelessUnit final : public INDI::Telescope, public INDI::GuiderInterface
{
    public:
        VixenWirelessUnit();
        ~VixenWirelessUnit() override;

        const char *getDefaultName() override;
        bool initProperties() override;
        bool updateProperties() override;
        void ISGetProperties(const char *dev) override;
        bool ISNewText(const char *dev, const char *name, char *texts[], char *names[], int n) override;
        bool ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n) override;
        bool ISNewNumber(const char *dev, const char *name, double values[], char *names[], int n) override;
        bool saveConfigItems(FILE *fp) override;
        bool loadConfig(bool silent = false, const char *property = nullptr) override;
        bool loadDefaultConfig() override;
        bool Connect() override;
        bool Disconnect() override;

    protected:
        bool Handshake() override;
        bool ReadScopeStatus() override;
        bool Goto(double ra, double dec) override;
        bool Sync(double ra, double dec) override;
        bool Park() override;
        bool UnPark() override;
        bool Abort() override;
        bool MoveNS(INDI_DIR_NS dir, TelescopeMotionCommand command) override;
        bool MoveWE(INDI_DIR_WE dir, TelescopeMotionCommand command) override;
        bool SetSlewRate(int index) override;
        bool SetTrackEnabled(bool enabled) override;
        bool updateLocation(double latitude, double longitude, double elevation) override;
        bool updateTime(ln_date *utc, double utcOffset) override;
        IPState GuideNorth(std::uint32_t ms) override;
        IPState GuideSouth(std::uint32_t ms) override;
        IPState GuideEast(std::uint32_t ms) override;
        IPState GuideWest(std::uint32_t ms) override;

    private:
        enum class MountInitializationState
        {
            unknown,
            uninitialized,
            initialized
        };

        enum class DeviceDiscoveryState
        {
            notScanned,
            found,
            manualEndpoint,
            unavailable
        };

        enum class DiscoveryResult
        {
            found,
            noResponse,
            multipleUnits,
            invalidConfiguration,
            transportError
        };

        DiscoveryResult discoverWirelessUnit(bool automatic, unsigned attempt = 1, unsigned maximumAttempts = 1);
        void startAutomaticDiscoveryConnect();
        void runAutomaticDiscoveryAttempt();
        void finishAutomaticDiscoveryConnect();
        void cancelAutomaticDiscoveryConnect(const char *message = nullptr, bool updateConnection = true);
        static void automaticDiscoveryRetryHelper(void *context);
        void automaticDiscoveryRetry();
        bool sendContinuousMotion();
        bool manualMotionActive() const;
        bool guidePulseActive() const;
        bool manualMotionStartAllowed() const;
        bool targetMotionStartAllowed(const char *operation) const;
        vixen::wirelessunit::Result<void> stopMountMotion();
        void reconcileMountStateFromStatus();
        void synchronizeTransientMountProperties();
        void resetTransientMountStateForDisconnect();
        void abandonManualMotion(const char *message);
        bool requireInitialized(const char *operation);
        bool wifiOperationAllowed(const char *operation) const;
        bool initializeHome(bool automatic);
        void updateHomeProperties();
        void updateInitializationState(const vixen::wirelessunit::MountStatus &status);
        bool readMountSettings();
        bool readAlignmentPoints();
        void updateAlignmentProperties();
        bool readWifiStationInformation();
        bool scanWifiAccessPoints();
        void updateWifiProperties();
        bool applyWifiStationSettings(bool refreshAfterApply = true);
        void clearPendingWifiPassword();
        bool readHotspotInformation();
        bool applyHotspotSettings();
        void updateHotspotProperties();
        void clearPendingHotspotPassword(bool clearUi = true);
        bool alignmentPointMatches(std::size_t index,
                                   const std::vector<vixen::wirelessunit::AlignmentPoint> &current) const;
        bool applyConfiguredMountSettings();
        bool useDeviceSettings() const;
        void resetMountSettingsToDefaults();
        IPState guide(std::uint32_t ms, double raSign, double decSign);
        static void guideNSCompleteHelper(void *context);
        static void guideWECompleteHelper(void *context);
        void guideNSComplete();
        void guideWEComplete();
        void cancelGuideTimers();
        void updateFirmwareProperties(double wirelessVersion, const vixen::wirelessunit::MountIdentity &identity);

        IText EndpointT[2] {};
        ITextVectorProperty EndpointTP {};
        ISwitch TransportS[2] {};
        ISwitchVectorProperty TransportSP {};
        ISwitch DiscoveryS[1] {};
        ISwitchVectorProperty DiscoverySP {};
        ISwitch UseDeviceSettingsS[1] {};
        ISwitchVectorProperty UseDeviceSettingsSP {};
        ISwitch GotoSpeedS[5] {};
        ISwitchVectorProperty GotoSpeedSP {};
        INumber HomePositionN[2] {};
        INumberVectorProperty HomePositionNP {};
        ISwitch SetHomePositionS[1] {};
        ISwitchVectorProperty SetHomePositionSP {};
        ISwitch AssumeHomeS[1] {};
        ISwitchVectorProperty AssumeHomeSP {};
        IText FirmwareT[3] {};
        ITextVectorProperty FirmwareTP {};
        IText DriverBuildT[1] {};
        ITextVectorProperty DriverBuildTP {};
        INumber GuideRateN[2] {};
        INumberVectorProperty GuideRateNP {};
        INumber BacklashN[2] {};
        INumberVectorProperty BacklashNP {};
        ISwitch MotorPowerS[4] {};
        ISwitchVectorProperty MotorPowerSP {};
        ISwitch MountTypeS[2] {};
        ISwitchVectorProperty MountTypeSP {};
        ISwitch MeridianDirectionS[2] {};
        ISwitchVectorProperty MeridianDirectionSP {};
        INumber MeridianN[2] {};
        INumberVectorProperty MeridianNP {};
        static constexpr std::size_t kMaximumDisplayedAlignmentPoints = 20;
        IText AlignmentT[kMaximumDisplayedAlignmentPoints + 1] {};
        ITextVectorProperty AlignmentTP {};
        ISwitch AlignmentRefreshS[1] {};
        ISwitchVectorProperty AlignmentRefreshSP {};
        ISwitch AlignmentSelectionS[kMaximumDisplayedAlignmentPoints] {};
        ISwitchVectorProperty AlignmentSelectionSP {};
        ISwitch AlignmentDeleteS[2] {};
        ISwitchVectorProperty AlignmentDeleteSP {};
        IText WifiInformationT[4] {};
        ITextVectorProperty WifiInformationTP {};
        ISwitch WifiRefreshS[1] {};
        ISwitchVectorProperty WifiRefreshSP {};
        IText WifiSsidT[2] {};
        ITextVectorProperty WifiSsidTP {};
        ISwitch WifiAutoConnectS[2] {};
        ISwitchVectorProperty WifiAutoConnectSP {};
        ISwitch WifiStationControlS[2] {};
        ISwitchVectorProperty WifiStationControlSP {};
        static constexpr std::size_t kMaximumDisplayedWifiAccessPoints = 32;
        IText WifiAccessPointsT[kMaximumDisplayedWifiAccessPoints + 1] {};
        ITextVectorProperty WifiAccessPointsTP {};
        ISwitch WifiAccessPointSelectionS[kMaximumDisplayedWifiAccessPoints] {};
        ISwitchVectorProperty WifiAccessPointSelectionSP {};
        ISwitch WifiScanS[1] {};
        ISwitchVectorProperty WifiScanSP {};
        IText HotspotInformationT[3] {};
        ITextVectorProperty HotspotInformationTP {};
        ISwitch HotspotRefreshS[1] {};
        ISwitchVectorProperty HotspotRefreshSP {};
        IText HotspotSettingsT[3] {};
        ITextVectorProperty HotspotSettingsTP {};

        std::unique_ptr<vixen::wirelessunit::ProtocolClient> client_;
        std::optional<vixen::wirelessunit::MountStatus> status_;
        std::string initializationEndpoint_;
        DeviceDiscoveryState deviceDiscoveryState_ { DeviceDiscoveryState::notScanned };
        int automaticDiscoveryTimerId_ { 0 };
        unsigned automaticDiscoveryAttempt_ { 0 };
        bool automaticConnectPending_ { false };
        bool mainControlOrderApplied_ { false };
        std::array<double, 10> slewMultipliers_ { 0.5, 1.0, 2.0, 4.0, 10.0, 20.0, 40.0, 100.0, 200.0, 400.0 };
        int slewIndex_ { 7 };
        std::array<int, 5> gotoSpeeds_ { 100, 200, 400, 700, 1000 };
        int gotoSpeedIndex_ { 4 };
        double maximumManualSlewMultiplier_ { 100.0 };
        double motionRa_ { 0 };
        double motionDec_ { 0 };
        float guideRa_ { 1.0F };
        float guideDec_ { 1.0F };
        int backlashRa_ { 0 };
        int backlashDec_ { 0 };
        int motorPower_ { 3 };
        int mountType_ { 1 };
        int forcedTrackingStopDegrees_ { 3 };
        int automaticGotoDegrees_ { 0 };
        int guideNSTimerId_ { 0 };
        int guideWETimerId_ { 0 };
        bool trackingKnownStopped_ { false };
        bool endpointConfigLoaded_ { false };
        bool assumeHomeConfigLoaded_ { false };
        bool connectionUsesDeviceSettings_ { true };
        bool preservingSettingsSourceDuringLoad_ { false };
        bool loadingEndpointConfig_ { false };
        bool loadingDefaults_ { false };
        bool homePositionDefined_ { false };
        bool homeSetDefined_ { false };
        bool alignmentPropertiesDefined_ { false };
        bool alignmentSelectionDefined_ { false };
        bool alignmentListLoaded_ { false };
        std::vector<vixen::wirelessunit::AlignmentPoint> alignmentPoints_;
        bool wifiPropertiesDefined_ { false };
        bool wifiAccessPointSelectionDefined_ { false };
        bool wifiInformationLoaded_ { false };
        std::vector<std::string> wifiAccessPointSsids_;
        std::string pendingWifiSsid_;
        std::string pendingWifiPassword_;
        bool hotspotPropertiesDefined_ { false };
        bool hotspotInformationLoaded_ { false };
        std::string pendingHotspotSsid_;
        std::string pendingHotspotPassword_;
        std::optional<int> pendingHotspotChannel_;
        std::string currentHotspotPassword_;
        int currentHotspotChannel_ { 1 };
        MountInitializationState initializationState_ { MountInitializationState::unknown };
        float timezoneHours_ { 0 };
};
