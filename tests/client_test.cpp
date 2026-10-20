/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#include "client.hpp"
#include "motion_safety.hpp"
#include "simulator_transport.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

class FakeTransport final : public vixen::wirelessunit::ITransport
{
    public:
        struct Exchange
        {
            std::string expectedRequest;
            vixen::wirelessunit::TransportResult result;
        };

        bool connect(std::string &) override
        {
            connected = true;
            return true;
        }

        void disconnect() noexcept override
        {
            connected = false;
        }

        bool isConnected() const noexcept override
        {
            return connected;
        }

        vixen::wirelessunit::TransportResult transact(
            std::string_view request,
            std::chrono::milliseconds timeout) override
        {
            assert(!exchanges.empty());
            auto exchange = std::move(exchanges.front());
            exchanges.pop_front();
            assert(request == exchange.expectedRequest);
            requests.emplace_back(request);
            timeouts.push_back(timeout);
            return std::move(exchange.result);
        }

        bool connected { false };
        std::deque<Exchange> exchanges;
        std::vector<std::string> requests;
        std::vector<std::chrono::milliseconds> timeouts;
};

} // namespace

int main()
{
    using vixen::wirelessunit::motion_safety::stopRequiredBeforeDisconnect;
    assert(!stopRequiredBeforeDisconnect(false, false, false, false, false, false));
    assert(stopRequiredBeforeDisconnect(true, false, false, false, false, false));
    assert(stopRequiredBeforeDisconnect(false, true, false, false, false, false));
    assert(stopRequiredBeforeDisconnect(false, false, true, false, false, false));
    assert(stopRequiredBeforeDisconnect(false, false, false, true, false, false));
    assert(stopRequiredBeforeDisconnect(false, false, false, false, true, false));
    assert(stopRequiredBeforeDisconnect(false, false, false, false, false, true));

    auto fake = std::make_unique<FakeTransport>();
    auto *transport = fake.get();
    transport->exchanges.push_back({
        "$SYVER?\r\n",
        { true, "$SYVER=2.600000\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTMDL?\r\n",
        { true, "$MTMDL=11,1000,1000\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTSTA?\r\n",
        { true, "$MTSTA=60400.0,1,x,y,1.0,2.0,180.0,45.0,12.5,-20.0,3\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTINI=1\r\n",
        { true, "$MTINI=1\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTINI=1\r\n",
        { true, "$MTINI=NG\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTGTS?\r\n",
        { true, "$MTGTS=700\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTAGS?\r\n",
        { true, "$MTAGS=2.0,3.1\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTBLC?\r\n",
        { true, "$MTBLC=11,15\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTPWR?\r\n",
        { true, "$MTPWR=1\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTTYP?\r\n",
        { true, "$MTTYP=0\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTCOM?\r\n",
        { true, "$MTCOM=19,-18\r\n", {} }
    });
    transport->exchanges.push_back({
        "$ALLST?\r\n",
        { true, "$ALLST=2,2,61292.49527,R20.7061+45.379,2,61292.49373,R21.3207+62.702\r\n", {} }
    });
    transport->exchanges.push_back({
        "$WFSTS?\r\n",
        { true, "$WFSTS=1,1,ExampleNetworkB,\r\n", {} }
    });
    transport->exchanges.push_back({
        "$WFSTC?\r\n",
        { true, "$WFSTC=1,\"10.42.0.60\"\r\n", {} }
    });
    transport->exchanges.push_back({
        "$WFAPL?\r\n",
        { true, "$WFAPL=3,4,\"ExampleNetworkA\",-62,3,\"ExampleNetworkB\",-71,3,\"\",-91\r\n", {} }
    });
    transport->exchanges.push_back({
        "$WFSTS=0,1,ExampleNetworkB,\r\n",
        { true, "$WFSTS=0,1,,\r\n", {} }
    });
    transport->exchanges.push_back({
        "$WFSTC=1\r\n",
        { true, "$WFSTC=1,\"10.42.0.60\"\r\n", {} }
    });
    transport->exchanges.push_back({
        "$WFSTC=0\r\n",
        { true, "$WFSTC=0,\"0.0.0.0\"\r\n", {} }
    });
    transport->exchanges.push_back({
        "$WFSID?\r\n",
        { true, "$WFSID=VixenWirelessUnitTest\r\n", {} }
    });
    transport->exchanges.push_back({
        "$WFPWD?\r\n",
        { true, "$WFPWD=old-password\r\n", {} }
    });
    transport->exchanges.push_back({
        "$WFCHN?\r\n",
        { true, "$WFCHN=6\r\n", {} }
    });
    transport->exchanges.push_back({
        "$WFSID=VixenWirelessUnit-test\r\n",
        { true, "$WFSID=VixenWirelessUnit-test\r\n", {} }
    });
    transport->exchanges.push_back({
        "$WFPWD=test-password\r\n",
        { true, "$WFPWD=test-password\r\n", {} }
    });
    transport->exchanges.push_back({
        "$WFCHN=11\r\n",
        { true, "$WFCHN=11\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTGTS=1000\r\n",
        { true, "$MTGTS=1000\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTAGS=2.000000,3.100000\r\n",
        { true, "$MTAGS=2.0,3.1\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTBLC=11,15\r\n",
        { true, "$MTBLC=11,15\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTPWR=1\r\n",
        { true, "$MTPWR=1\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTTYP=0\r\n",
        { true, "$MTTYP=0\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTCOM=19,-18\r\n",
        { true, "$MTCOM=19,-18\r\n", {} }
    });
    transport->exchanges.push_back({
        "$ALIGN=3,20.706113,45.379394,R20.7061+45.379\r\n",
        { true, "$ALIGN=3,20.706113,45.379394,R20.7061+45.379\r\n", {} }
    });
    transport->exchanges.push_back({
        "$ALIGN=2,270.000000,0.000000,A270.000+00.000\r\n",
        { true, "$ALIGN=OK\r\n", {} }
    });
    transport->exchanges.push_back({
        "$ALDEL=1\r\n",
        { true, "$ALDEL=1\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTMV0=1.0,100.0000,-30.0000\r\n",
        { true, "$MTMV0=1.0,100.0000,-30.0000\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTTGT=3,6.000000,30.000000,INDI\r\n",
        { true, "$MTTGT=OK\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTTGT=1,270,0,HOME\r\n",
        { true, "$MTTGT=1,270.000000,0.000000,HOME\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTSTP=1\r\n",
        { true, "$MTSTP=1\r\n", {} }
    });
    transport->exchanges.push_back({
        "$MTTGT=3,18.000000,-80.000000,INDI\r\n",
        { true, "$MTTGT=BELOWHORIZON\r\n", {} }
    });

    vixen::wirelessunit::ProtocolClient client(std::move(fake));
    std::string error;
    assert(client.connect(error));

    const auto version = client.systemVersion();
    assert(version && *version.value == 2.6);

    const auto identity = client.mountIdentity();
    assert(identity && identity.value->model == 11);
    assert(identity.value->maximumGotoSpeed == 1000);
    assert(identity.value->maximumSecondarySpeed == 1000);

    const auto status = client.mountStatus();
    assert(status && status.value->rightAscensionHours == 12.5);
    assert(client.lastMountStatus());
    const auto initializedNow = client.initializeMount();
    const auto alreadyInitialized = client.initializeMount();
    assert(initializedNow && *initializedNow.value);
    assert(alreadyInitialized && !*alreadyInitialized.value);

    const auto gotoSpeed = client.gotoSpeed();
    assert(gotoSpeed && *gotoSpeed.value == 700);
    const auto rates = client.guideRates();
    assert(rates && rates.value->rightAscensionSidereal == 2.0F);
    assert(rates.value->declinationSidereal == 3.1F);
    const auto backlash = client.backlash();
    assert(backlash && backlash.value->rightAscension == 11);
    assert(backlash.value->declination == 15);
    const auto power = client.motorPower();
    assert(power && *power.value == 1);
    const auto type = client.mountType();
    assert(type && *type.value == 0);
    const auto meridian = client.meridianSettings();
    assert(meridian && meridian.value->forcedTrackingStopDegrees == 19);
    assert(meridian.value->automaticGotoDegrees == -18);
    const auto alignments = client.alignmentPoints();
    assert(alignments && alignments.value->size() == 2);
    assert((*alignments.value)[0].label == "R20.7061+45.379");
    const auto wifiSettings = client.wifiStationSettings();
    assert(wifiSettings && wifiSettings.value->autoConnect);
    assert(wifiSettings.value->ssid == "ExampleNetworkB");
    assert(transport->timeouts.back() == std::chrono::milliseconds(1500));
    const auto wifiConnection = client.wifiStationConnection();
    assert(wifiConnection && wifiConnection.value->connected);
    assert(wifiConnection.value->ipAddress == "10.42.0.60");
    assert(transport->timeouts.back() == std::chrono::milliseconds(1500));
    const auto wifiAccessPoints = client.wifiAccessPoints();
    assert(wifiAccessPoints && wifiAccessPoints.value->size() == 3);
    assert((*wifiAccessPoints.value)[1].signalDbm == -71);
    assert(transport->timeouts.back() == std::chrono::milliseconds(5000));
    assert(client.setWifiStationSettings(false, "ExampleNetworkB", ""));
    assert(transport->timeouts.back() == std::chrono::milliseconds(30000));
    const auto stationConnected = client.setWifiStationConnected(true);
    assert(stationConnected);
    assert(transport->timeouts.back() == std::chrono::milliseconds(30000));
    const auto stationDisconnected = client.setWifiStationConnected(false);
    assert(stationDisconnected);
    assert(transport->timeouts.back() == std::chrono::milliseconds(30000));
    const auto hotspotSsid = client.wifiHotspotSsid();
    assert(hotspotSsid && *hotspotSsid.value == "VixenWirelessUnitTest");
    const auto hotspotPassword = client.wifiHotspotPassword();
    assert(hotspotPassword && *hotspotPassword.value == "old-password");
    const auto hotspotChannel = client.wifiHotspotChannel();
    assert(hotspotChannel && *hotspotChannel.value == 6);
    assert(client.setWifiHotspotSsidValue("VixenWirelessUnit-test"));
    assert(transport->timeouts.back() == std::chrono::milliseconds(1500));
    assert(client.setWifiHotspotPasswordValue("test-password"));
    assert(transport->timeouts.back() == std::chrono::milliseconds(1500));
    assert(client.setWifiHotspotChannelValue(11));
    assert(transport->timeouts.back() == std::chrono::milliseconds(1500));

    assert(client.setGotoSpeedValue(1000));
    assert(client.setGuideRates(2.0F, 3.1F));
    assert(client.setBacklashCompensation(11, 15));
    assert(client.setMotorPowerLevel(1));
    assert(client.setMountTypeValue(0));
    assert(client.setMeridianSettings(19, -18));
    assert(client.syncRaDec(20.706113, 45.379394));
    assert(transport->timeouts.back() == std::chrono::milliseconds(1500));
    assert(client.syncAltAz(270.0, 0.0));
    assert(transport->timeouts.back() == std::chrono::milliseconds(1500));
    assert(client.deleteAlignmentPoint(1));
    assert(!client.deleteAlignmentPoint(20));
    assert(client.moveBeforeInitialization(1.0, 100.0, -30.0));

    assert(client.slewRaDec(6.0, 30.0));
    assert(client.home());
    assert(client.abortMotion());
    const auto belowHorizon = client.slewRaDec(18.0, -80.0);
    assert(!belowHorizon);
    assert(belowHorizon.error == "target is below the horizon");
    assert(transport->exchanges.empty());

    client.disconnect();
    assert(!client.isConnected());
    const auto requestCount = transport->requests.size();
    const float floatNaN = std::numeric_limits<float>::quiet_NaN();
    const float floatInfinity = std::numeric_limits<float>::infinity();
    const double doubleNaN = std::numeric_limits<double>::quiet_NaN();
    const double doubleInfinity = std::numeric_limits<double>::infinity();
    assert(!client.setGuideRates(floatNaN, 1.0F));
    assert(!client.setGuideRates(floatInfinity, 1.0F));
    assert(!client.setGuideRates(-floatInfinity, 1.0F));
    assert(!client.setGuideRates(0.0F, 1.0F));
    assert(!client.setGuideRates(1.0F, 10.1F));
    assert(!client.setGuideRates(1.25F, 1.0F));
    assert(!client.setGuideRates(1.0F, 1.25F));
    assert(!client.setBacklashCompensation(-1, 0));
    assert(!client.setBacklashCompensation(0, 100));
    assert(!client.setMotorPowerLevel(0));
    assert(!client.setMountTypeValue(2));
    assert(!client.setMeridianSettings(91, 0));
    assert(!client.setMeridianSettings(19, 20));
    assert(!client.setMeridianSettings(19, -20));
    assert(!client.setTrackingRates(doubleNaN, 0.0));
    assert(!client.moveBeforeInitialization(1e100, doubleInfinity, 0.0));
    assert(!client.moveRaDec(-1.0, 0.0, 0.0));
    assert(!client.setWifiHotspotChannelValue(12));
    assert(transport->requests.size() == requestCount);

    {
        auto statusFake = std::make_unique<FakeTransport>();
        auto *statusTransport = statusFake.get();
        statusTransport->exchanges.push_back({
            "$MTSTA?\r\n",
            { true, "$MTSTA=60400,1,0,0,90,0,270,0,12,-10,3,ASCOM\r\n", {} }
        });
        statusTransport->exchanges.push_back({
            "$MTSTA?\r\n",
            { false, {}, "receive: timeout" }
        });
        vixen::wirelessunit::ProtocolClient statusClient(std::move(statusFake));
        assert(statusClient.connect(error));
        assert(statusClient.mountStatus());
        assert(statusClient.lastMountStatus());
        assert(!statusClient.mountStatus());
        assert(!statusClient.lastMountStatus());
        assert(statusTransport->exchanges.empty());
    }

    // Setter replies are accepted regardless of payload unless they are NG.
    // A malformed frame, wrong command family, or transport failure still fails.
    {
        auto ackFake = std::make_unique<FakeTransport>();
        auto *ackTransport = ackFake.get();
        ackTransport->exchanges.push_back({
            "$MTSTP=1\r\n",
            { true, "$MTSTP=\r\n", {} }
        });
        ackTransport->exchanges.push_back({
            "$MTSTP=1\r\n",
            { true, "$MTSTP=UNKNOWN\r\n", {} }
        });
        ackTransport->exchanges.push_back({
            "$MTSTP=1\r\n",
            { true, "$OTHER=1\r\n", {} }
        });
        ackTransport->exchanges.push_back({
            "$MTSTP=1\r\n",
            { false, {}, "receive: timeout" }
        });
        ackTransport->exchanges.push_back({
            "$MTSTP=1\r\n",
            { true, "$MTSTP=NG\r\n", {} }
        });
        ackTransport->exchanges.push_back({
            "$MTPWR=3\r\n",
            { true, "$MTPWR=2\r\n", {} }
        });
        ackTransport->exchanges.push_back({
            "$MTPWR=3\r\n",
            { true, "$MTPWR=3\r\n", {} }
        });
        ackTransport->exchanges.push_back({
            "$MTTGT=3,6.000000,30.000000,INDI\r\n",
            { true, "$MTTGT=UNKNOWN\r\n", {} }
        });
        ackTransport->exchanges.push_back({
            "$MTTGT=1,270,0,HOME\r\n",
            { true, "$MTTGT=1,270.000000,1.000000,HOME\r\n", {} }
        });
        ackTransport->exchanges.push_back({
            "$ALIGN=2,270.000000,0.000000,A270.000+00.000\r\n",
            { true, "$ALIGN=\r\n", {} }
        });
        ackTransport->exchanges.push_back({
            "$ALIGN=3,20.706113,45.379394,R20.7061+45.379\r\n",
            { true, "$ALIGN=3,20.706113,45.379394,OTHER\r\n", {} }
        });
        ackTransport->exchanges.push_back({
            "$WFSTS=1,1,ExampleNetworkB,password\r\n",
            { true, "$WFSTS=0,1,,\r\n", {} }
        });

        vixen::wirelessunit::ProtocolClient ackClient(std::move(ackFake));
        assert(ackClient.connect(error));
        assert(ackClient.abortMotion());
        assert(ackClient.abortMotion());
        assert(!ackClient.abortMotion());
        assert(ackClient.setMotorPowerLevel(3));
        assert(ackClient.setMotorPowerLevel(3));
        assert(ackClient.slewRaDec(6.0, 30.0));
        assert(ackClient.home());
        assert(ackClient.syncAltAz(270.0, 0.0));
        assert(ackClient.syncRaDec(20.706113, 45.379394));
        assert(ackClient.setWifiStationSettings(true, "ExampleNetworkB", "password"));
        assert(ackTransport->exchanges.empty());
    }

    // Safety-stop commands are idempotent and retry after a lost response.
    // Normal non-zero motion commands deliberately remain single-shot.
    {
        auto retryFake = std::make_unique<FakeTransport>();
        auto *retryTransport = retryFake.get();
        retryTransport->exchanges.push_back({
            "$MTSTP=1\r\n", { false, {}, "receive: timeout" }
        });
        retryTransport->exchanges.push_back({
            "$MTSTP=1\r\n", { true, "$MTSTP=NG\r\n", {} }
        });
        retryTransport->exchanges.push_back({
            "$MTSTP=1\r\n", { true, "$MTSTP=1\r\n", {} }
        });
        retryTransport->exchanges.push_back({
            "$MTMV0=1.0,0.0000,0.0000\r\n", { false, {}, "receive: timeout" }
        });
        retryTransport->exchanges.push_back({
            "$MTMV0=1.0,0.0000,0.0000\r\n", { true, "$MTMV0=1.0,0.0000,0.0000\r\n", {} }
        });
        retryTransport->exchanges.push_back({
            "$MTMV2=1.00,1.000000,0.000000\r\n", { false, {}, "receive: timeout" }
        });

        vixen::wirelessunit::ProtocolClient retryClient(std::move(retryFake));
        std::string retryError;
        assert(retryClient.connect(retryError));
        assert(retryClient.abortMotion());
        assert(retryClient.stopBeforeInitialization());
        assert(!retryClient.moveRaDec(1.0, 1.0, 0.0));
        assert(retryTransport->requests.size() == 6);
        assert(retryTransport->timeouts[0] == std::chrono::milliseconds(300));
        assert(retryTransport->timeouts[1] == std::chrono::milliseconds(300));
        assert(retryTransport->timeouts[2] == std::chrono::milliseconds(300));
        assert(retryTransport->timeouts[3] == std::chrono::milliseconds(300));
        assert(retryTransport->timeouts[4] == std::chrono::milliseconds(300));
        assert(retryTransport->timeouts[5] == std::chrono::milliseconds(500));
        assert(retryTransport->exchanges.empty());
    }

    // Every batch stage can fail either before application (NG) or after the
    // unit applied the value but before its ACK reached the client. In both
    // cases, all six read-only queries remain available to resolve state.
    for (const std::string_view family : std::array<std::string_view, 6> {
             "MTGTS", "MTAGS", "MTBLC", "MTPWR", "MTTYP", "MTCOM" })
    {
        for (const bool loseAck : { false, true })
        {
            vixen::wirelessunit::ProtocolClient faultClient(
                std::make_unique<vixen::wirelessunit::SimulatorTransport>(
                    loseAck ? "" : std::string(family), loseAck ? std::string(family) : "", true));
            assert(faultClient.connect(error));
            vixen::wirelessunit::Result<void> setting { false, {} };
            if (family == "MTGTS") setting = faultClient.setGotoSpeedValue(1000);
            if (family == "MTAGS") setting = faultClient.setGuideRates(1.0F, 1.0F);
            if (family == "MTBLC") setting = faultClient.setBacklashCompensation(0, 0);
            if (family == "MTPWR") setting = faultClient.setMotorPowerLevel(3);
            if (family == "MTTYP") setting = faultClient.setMountTypeValue(1);
            if (family == "MTCOM") setting = faultClient.setMeridianSettings(3, 0);
            assert(!setting);

            const auto speed = faultClient.gotoSpeed();
            const auto guide = faultClient.guideRates();
            const auto backlash = faultClient.backlash();
            const auto power = faultClient.motorPower();
            const auto mountType = faultClient.mountType();
            const auto meridian = faultClient.meridianSettings();
            assert(speed && guide && backlash && power && mountType && meridian);
            assert(*speed.value == ((family == "MTGTS" && loseAck) ? 1000 : 700));
            assert(guide.value->rightAscensionSidereal ==
                   ((family == "MTAGS" && loseAck) ? 1.0F : 2.0F));
            assert(guide.value->declinationSidereal ==
                   ((family == "MTAGS" && loseAck) ? 1.0F : 3.0F));
            assert(backlash.value->rightAscension ==
                   ((family == "MTBLC" && loseAck) ? 0 : 11));
            assert(backlash.value->declination ==
                   ((family == "MTBLC" && loseAck) ? 0 : 15));
            assert(*power.value == ((family == "MTPWR" && loseAck) ? 3 : 4));
            assert(*mountType.value == ((family == "MTTYP" && loseAck) ? 1 : 0));
            assert(meridian.value->forcedTrackingStopDegrees ==
                   ((family == "MTCOM" && loseAck) ? 3 : 19));
            assert(meridian.value->automaticGotoDegrees ==
                   ((family == "MTCOM" && loseAck) ? 0 : -18));
        }
    }

    for (const std::string_view family : std::array<std::string_view, 6> {
             "MTGTS", "MTAGS", "MTBLC", "MTPWR", "MTTYP", "MTCOM" })
    {
        vixen::wirelessunit::ProtocolClient queryFaultClient(
            std::make_unique<vixen::wirelessunit::SimulatorTransport>("", "", true, std::string(family)));
        assert(queryFaultClient.connect(error));
        assert(static_cast<bool>(queryFaultClient.gotoSpeed()) == (family != "MTGTS"));
        assert(static_cast<bool>(queryFaultClient.guideRates()) == (family != "MTAGS"));
        assert(static_cast<bool>(queryFaultClient.backlash()) == (family != "MTBLC"));
        assert(static_cast<bool>(queryFaultClient.motorPower()) == (family != "MTPWR"));
        assert(static_cast<bool>(queryFaultClient.mountType()) == (family != "MTTYP"));
        assert(static_cast<bool>(queryFaultClient.meridianSettings()) == (family != "MTCOM"));
    }

    {
        vixen::wirelessunit::ProtocolClient delayedQueryFault(
            std::make_unique<vixen::wirelessunit::SimulatorTransport>("", "", true, "MTAGS", true));
        assert(delayedQueryFault.connect(error));
        assert(delayedQueryFault.guideRates());
        assert(!delayedQueryFault.guideRates());
        assert(delayedQueryFault.gotoSpeed());
    }

    {
        vixen::wirelessunit::ProtocolClient simulator(
            std::make_unique<vixen::wirelessunit::SimulatorTransport>());
        assert(simulator.connect(error));

        const auto simulatorUninitializedStatus = simulator.mountStatus();
        assert(simulatorUninitializedStatus);
        assert(!vixen::wirelessunit::mountStatusIsInitialized(*simulatorUninitializedStatus.value));
        assert(simulator.moveBeforeInitialization(1.0, 100.0, 0.0));

        const auto simulatorInitializedNow = simulator.initializeMount();
        const auto simulatorAlreadyInitialized = simulator.initializeMount();
        assert(simulatorInitializedNow && *simulatorInitializedNow.value);
        assert(simulatorAlreadyInitialized && !*simulatorAlreadyInitialized.value);
        const auto simulatorInitializedStatus = simulator.mountStatus();
        assert(simulatorInitializedStatus);
        assert(vixen::wirelessunit::mountStatusIsInitialized(*simulatorInitializedStatus.value));
        assert(simulatorInitializedStatus.value->targetObjectType == 1);

        const auto initialGotoSpeed = simulator.gotoSpeed();
        const auto initialRates = simulator.guideRates();
        const auto initialBacklash = simulator.backlash();
        const auto initialPower = simulator.motorPower();
        const auto initialType = simulator.mountType();
        const auto initialMeridian = simulator.meridianSettings();
        const auto initialAlignments = simulator.alignmentPoints();
        const auto initialWifiSettings = simulator.wifiStationSettings();
        const auto initialWifiConnection = simulator.wifiStationConnection();
        const auto initialWifiAccessPoints = simulator.wifiAccessPoints();
        const auto initialHotspotSsid = simulator.wifiHotspotSsid();
        const auto initialHotspotPassword = simulator.wifiHotspotPassword();
        const auto initialHotspotChannel = simulator.wifiHotspotChannel();
        assert(initialGotoSpeed && *initialGotoSpeed.value == 1000);
        assert(initialRates && initialRates.value->rightAscensionSidereal == 1.0F);
        assert(initialRates.value->declinationSidereal == 1.0F);
        assert(initialBacklash && initialBacklash.value->rightAscension == 0);
        assert(initialBacklash.value->declination == 0);
        assert(initialPower && *initialPower.value == 3);
        assert(initialType && *initialType.value == 0);
        assert(initialMeridian && initialMeridian.value->forcedTrackingStopDegrees == 3);
        assert(initialMeridian.value->automaticGotoDegrees == 0);
        assert(initialAlignments && initialAlignments.value->empty());
        assert(initialWifiSettings && initialWifiSettings.value->autoConnect);
        assert(initialWifiSettings.value->stationModeEnabled);
        assert(initialWifiSettings.value->ssid == "ExampleNetworkB");
        assert(initialWifiConnection && initialWifiConnection.value->connected);
        assert(initialWifiConnection.value->ipAddress == "10.42.0.60");
        assert(initialWifiAccessPoints && initialWifiAccessPoints.value->size() == 3);
        assert((*initialWifiAccessPoints.value)[1].ssid == "ExampleNetworkB");
        assert(initialHotspotSsid && *initialHotspotSsid.value == "VixenWirelessUnitTest");
        assert(initialHotspotPassword && *initialHotspotPassword.value == "simulated-password");
        assert(initialHotspotChannel && *initialHotspotChannel.value == 6);
        assert(simulator.setWifiHotspotSsidValue("VixenWirelessUnit-test"));
        assert(simulator.setWifiHotspotPasswordValue("test-password"));
        assert(simulator.setWifiHotspotChannelValue(11));
        const auto changedHotspotSsid = simulator.wifiHotspotSsid();
        const auto changedHotspotPassword = simulator.wifiHotspotPassword();
        const auto changedHotspotChannel = simulator.wifiHotspotChannel();
        assert(changedHotspotSsid && *changedHotspotSsid.value == "VixenWirelessUnit-test");
        assert(changedHotspotPassword && *changedHotspotPassword.value == "test-password");
        assert(changedHotspotChannel && *changedHotspotChannel.value == 11);
        assert(simulator.setWifiStationSettings(false, "new-network", "new-password"));
        const auto changedWifiSettings = simulator.wifiStationSettings();
        assert(changedWifiSettings && !changedWifiSettings.value->autoConnect);
        assert(changedWifiSettings.value->stationModeEnabled);
        assert(changedWifiSettings.value->ssid == "new-network");
        const auto disconnectedWifi = simulator.setWifiStationConnected(false);
        assert(disconnectedWifi);
        const auto connectedWifi = simulator.setWifiStationConnected(true);
        assert(connectedWifi);
        assert(!simulator.deleteAlignmentPoint(0));

        assert(simulator.setGotoSpeedValue(700));
        assert(simulator.setGuideRates(2.0F, 3.1F));
        assert(simulator.setBacklashCompensation(11, 15));
        assert(simulator.setMotorPowerLevel(4));
        assert(simulator.setMountTypeValue(1));
        assert(simulator.setMeridianSettings(19, -18));
        assert(simulator.syncRaDec(20.706113, 45.379394));

        const auto simulatedGotoSpeed = simulator.gotoSpeed();
        const auto simulatedRates = simulator.guideRates();
        const auto simulatedBacklash = simulator.backlash();
        const auto simulatedPower = simulator.motorPower();
        const auto simulatedType = simulator.mountType();
        const auto simulatedMeridian = simulator.meridianSettings();
        assert(simulatedGotoSpeed && *simulatedGotoSpeed.value == 700);
        assert(simulatedRates && simulatedRates.value->rightAscensionSidereal == 2.0F);
        assert(simulatedRates.value->declinationSidereal == 3.1F);
        assert(simulatedBacklash && simulatedBacklash.value->rightAscension == 11);
        assert(simulatedBacklash.value->declination == 15);
        assert(simulatedPower && *simulatedPower.value == 4);
        assert(simulatedType && *simulatedType.value == 1);
        assert(simulatedMeridian && simulatedMeridian.value->forcedTrackingStopDegrees == 19);
        assert(simulatedMeridian.value->automaticGotoDegrees == -18);

        assert(simulator.syncAltAz(270.0, 0.0));
        const auto alignedHomeStatus = simulator.mountStatus();
        assert(alignedHomeStatus && alignedHomeStatus.value->azimuthDegrees == 270.0);
        assert(alignedHomeStatus.value->altitudeDegrees == 0.0);

        assert(simulator.home());
        const auto homeStatus = simulator.mountStatus();
        assert(homeStatus && homeStatus.value->status == 2);

        assert(simulator.stopTracking());
        const auto stoppedStatus = simulator.mountStatus();
        assert(stoppedStatus && stoppedStatus.value->status == 1);
        assert(stoppedStatus.value->targetObjectType == 2);

        assert(!simulator.setGotoSpeedValue(500));
        assert(!simulator.setGuideRates(0.0F, 1.0F));
        assert(!simulator.setBacklashCompensation(100, 0));
        assert(!simulator.setMotorPowerLevel(5));
        assert(!simulator.setMountTypeValue(2));
        assert(!simulator.setMeridianSettings(19, 20));
        assert(!simulator.setMeridianSettings(91, 0));
        // East/West is encoded by the sign, but the magnitude must remain at
        // or inside the common Stop point in either direction.
        assert(!simulator.setMeridianSettings(0, -90));
        assert(!simulator.setMeridianSettings(19, -20));
    }
    {
        auto mismatch = std::make_unique<FakeTransport>();
        mismatch->exchanges.push_back({
            "$MTAGS=1.200000,1.300000\r\n",
            { true, "$MTAGS=1.2,1.4\r\n", {} }
        });
        vixen::wirelessunit::ProtocolClient mismatchClient(std::move(mismatch));
        std::string error;
        assert(mismatchClient.connect(error));
        assert(mismatchClient.setGuideRates(1.2F, 1.3F));
    }
    {
        auto malformed = std::make_unique<FakeTransport>();
        malformed->exchanges.push_back({
            "$MTAGS=1.200000,1.300000\r\n",
            { true, "$MTAGS=1.2,1.3,extra\r\n", {} }
        });
        vixen::wirelessunit::ProtocolClient malformedClient(std::move(malformed));
        std::string error;
        assert(malformedClient.connect(error));
        assert(malformedClient.setGuideRates(1.2F, 1.3F));
    }
    {
        auto targets = std::make_unique<FakeTransport>();
        targets->exchanges.push_back({
            "$MTTGT=2,270.000000,0.000000,INDI\r\n",
            { true, "$MTTGT=BELOWHORIZON\r\n", {} }
        });
        targets->exchanges.push_back({
            "$MTTGT=1,270,0,HOME\r\n",
            { true, "$MTTGT=BELOWHORIZON\r\n", {} }
        });
        targets->exchanges.push_back({
            "$MTTGT=1,270,0,HOME\r\n",
            { true, "$MTTGT=OTHER\r\n", {} }
        });
        vixen::wirelessunit::ProtocolClient targetClient(std::move(targets));
        std::string error;
        assert(targetClient.connect(error));
        assert(targetClient.slewAltAz(270.0, 0.0).error == "target is below the horizon");
        assert(targetClient.home().error == "target is below the horizon");
        assert(targetClient.home());
    }
    {
        auto negative = std::make_unique<FakeTransport>();
        negative->exchanges.push_back({
            "$MTTGT=1,270,0,HOME\r\n",
            { true, "$MTTGT=NG,BELOWHORIZON\r\n", {} }
        });
        negative->exchanges.push_back({
            "$ALIGN=2,270.000000,0.000000,A270.000+00.000\r\n",
            { true, "$ALIGN=NG\r\n", {} }
        });
        negative->exchanges.push_back({
            "$MTAGS=1.200000,1.300000\r\n",
            { true, "$MTAGS=NG\r\n", {} }
        });
        negative->exchanges.push_back({
            "$WFSTC=1\r\n",
            { true, "$WFSTC=0,\"0.0.0.0\"\r\n", {} }
        });
        negative->exchanges.push_back({
            "$MTINI=1\r\n",
            { true, "$MTINI=OK\r\n", {} }
        });
        vixen::wirelessunit::ProtocolClient negativeClient(std::move(negative));
        std::string error;
        assert(negativeClient.connect(error));
        assert(!negativeClient.home());
        assert(!negativeClient.syncAltAz(270.0, 0.0));
        assert(!negativeClient.setGuideRates(1.2F, 1.3F));
        assert(negativeClient.setWifiStationConnected(true));
        const auto initialized = negativeClient.initializeMount();
        assert(initialized && *initialized.value);
    }
    {
        auto numeric = std::make_unique<FakeTransport>();
        numeric->exchanges.push_back({
            "$MTMV0=3.0,10.0000,0.0000\r\n",
            { true, "$MTMV0=3.0,10.0,0.0\r\n", {} }
        });
        numeric->exchanges.push_back({
            "$MTMV2=0.10,1.000000,0.000000\r\n",
            { true, "$MTMV2=0.1,1.0000,0.0000\r\n", {} }
        });
        numeric->exchanges.push_back({
            "$MTMV2=0.15,1.000000,0.000000\r\n",
            { true, "$MTMV2=0.2,1.0000,0.0000\r\n", {} }
        });
        numeric->exchanges.push_back({
            "$MTMV2=0.15,1.000000,0.000000\r\n",
            { true, "$MTMV2=NG\r\n", {} }
        });
        numeric->exchanges.push_back({
            "$MTMV2=0.15,1.000000,0.000000\r\n",
            { true, "$MTSTA=1\r\n", {} }
        });
        numeric->exchanges.push_back({
            "$MTRAT=1.000000,0.500000\r\n",
            { true, "$MTRAT=1.0000,0.5000\r\n", {} }
        });
        numeric->exchanges.push_back({
            "$SYUTM=1970,1,1,0,0,0.000000\r\n",
            { true, "$SYUTM=1970,1,1,0,0,0.0\r\n", {} }
        });
        vixen::wirelessunit::ProtocolClient numericClient(std::move(numeric));
        std::string error;
        assert(numericClient.connect(error));
        assert(numericClient.moveBeforeInitialization(3.0, 10.0, 0.0));
        assert(numericClient.moveRaDec(0.1, 1.0, 0.0));
        assert(numericClient.pulseGuide(0.15, 1.0, 0.0));
        assert(!numericClient.pulseGuide(0.15, 1.0, 0.0));
        assert(!numericClient.pulseGuide(0.15, 1.0, 0.0));
        assert(numericClient.setTrackingRates(1.0, 0.5));
        assert(numericClient.setTime(std::chrono::system_clock::from_time_t(0)));
    }
    const auto testTimeReply = [](std::string reply, bool expected) {
        auto fakeTime = std::make_unique<FakeTransport>();
        auto *timeTransport = fakeTime.get();
        fakeTime->exchanges.push_back({
            "$SYUTM=1970,1,1,0,0,45.569299\r\n",
            { true, std::move(reply), {} }
        });
        vixen::wirelessunit::ProtocolClient timeClient(std::move(fakeTime));
        std::string error;
        assert(timeClient.connect(error));
        const auto instant = std::chrono::system_clock::time_point {} +
                             std::chrono::seconds(45) + std::chrono::microseconds(569299);
        assert(static_cast<bool>(timeClient.setTime(instant)) == expected);
        assert(timeTransport->exchanges.empty());
    };
    testTimeReply("$SYUTM=1970,1,1,0,0,45.6\r\n", true);
    testTimeReply("$SYUTM=1970,1,1,0,0,45.5\r\n", true);
    testTimeReply("$SYUTM=1970,1,1,0,1,45.6\r\n", true);
    testTimeReply("$SYUTM=1970,1,1,0,0,45.6,extra\r\n", true);
    testTimeReply("$SYUTM=1970,1,1,0,0,nan\r\n", true);
    testTimeReply("$SYUTM=NG\r\n", false);
    return 0;
}
