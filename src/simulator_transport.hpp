/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#pragma once

#include "transport.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vixen::wirelessunit
{

class SimulatorTransport final : public ITransport
{
    public:
        using Clock = std::function<std::chrono::steady_clock::time_point()>;
        explicit SimulatorTransport(std::string rejectedSetter = {}, std::string lostSetterAck = {},
                                    bool nondefaultMountSettings = false, std::string failedQuery = {},
                                    bool failQueryAfterFirstRead = false,
                                    Clock clock = [] { return std::chrono::steady_clock::now(); });
        void loseNextResponses(std::string commandName, unsigned count);
        void rejectNextCommands(std::string commandName, unsigned count);
        bool connect(std::string &error) override;
        void disconnect() noexcept override;
        bool isConnected() const noexcept override;
        TransportResult transact(
            std::string_view request,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) override;

    private:
        std::string rejectedSetter_;
        std::string lostSetterAck_;
        std::string failedQuery_;
        bool failQueryAfterFirstRead_ { false };
        int failedQueryReadCount_ { 0 };
        Clock clock_;
        std::unordered_map<std::string, unsigned> lostResponses_;
        std::unordered_map<std::string, unsigned> rejectedCommands_;
        mutable std::mutex mutex_;
        bool connected_ { false };
        double rightAscension_ { 0 };
        double declination_ { 90 };
        double azimuth_ { 180 };
        double altitude_ { 45 };
        double longitude_ { 0.0 };
        double latitude_ { 0.0 };
        float timezone_ { 0.0F };
        float guideRa_ { 1.0F };
        float guideDec_ { 1.0F };
        int gotoSpeed_ { 1000 };
        int backlashRa_ { 0 };
        int backlashDec_ { 0 };
        int motorPower_ { 3 };
        int mountType_ { 0 };
        int forcedTrackingStopDegrees_ { 3 };
        int automaticGotoDegrees_ { 0 };
        int mountStatus_ { 1 };
        int targetObjectType_ { 2 };
        std::string targetLabel_ { "--" };
        struct Alignment
        {
            int state { 2 };
            double mjd { 60400.0 };
            std::string label;
        };
        std::vector<Alignment> alignments_;
        enum class SlewAxis { none, raDec, altAz };
        SlewAxis slewAxis_ { SlewAxis::none };
        std::chrono::steady_clock::time_point slewStarted_ {};
        double slewStartFirst_ { 0 };
        double slewStartSecond_ { 0 };
        double slewTargetFirst_ { 0 };
        double slewTargetSecond_ { 0 };
        int slewTargetObjectType_ { 3 };
        struct Pulse
        {
            std::chrono::steady_clock::time_point started;
            double durationSeconds { 0 };
            double raRate { 0 };
            double decRate { 0 };
            double appliedSeconds { 0 };
        };
        std::vector<Pulse> pulses_;
        double trackingRaRate_ { 0 };
        double trackingDecRate_ { 0 };
        double mechanicalX_ { 90 };
        double mechanicalY_ { 0 };
        bool mountInitialized_ { false };
        bool wifiAutoConnect_ { true };
        bool wifiStationModeEnabled_ { true };
        bool wifiStationConnected_ { true };
        std::string wifiSsid_ { "ExampleNetworkB" };
        std::string wifiHotspotSsid_ { "VixenWirelessUnitTest" };
        std::string wifiHotspotPassword_ { "simulated-password" };
        int wifiHotspotChannel_ { 6 };
        void advanceState();
};

} // namespace vixen::wirelessunit
