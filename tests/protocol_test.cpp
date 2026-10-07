/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#include "protocol.hpp"
#include "motion_timing.hpp"
#include "numeric_validation.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <limits>
#include <string>

using vixen::wirelessunit::parseResponse;

int main()
{
    assert(vixen::wirelessunit::kTcpDefaultHost == "0.0.0.0");
    assert(vixen::wirelessunit::kUdpDefaultHost == "0.0.0.0");

    using vixen::wirelessunit::numeric_validation::integerInRange;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    assert(!integerInRange(nan, 0, 90));
    assert(!integerInRange(infinity, 0, 90));
    assert(!integerInRange(-infinity, 0, 90));
    assert(!integerInRange(1e100, 0, 90));
    assert(!integerInRange(-1.0, 0, 90));
    assert(!integerInRange(90.5, 0, 90));
    assert(integerInRange(0.0, 0, 90) == 0);
    assert(integerInRange(90.0, 0, 90) == 90);
    const double validUpdate[] { 0.1, 10.0 };
    const double invalidUpdate[] { 0.1, nan };
    assert(vixen::wirelessunit::numeric_validation::finiteUpdate(validUpdate, 2));
    assert(!vixen::wirelessunit::numeric_validation::finiteUpdate(invalidUpdate, 2));
    assert(vixen::wirelessunit::numeric_validation::oneDecimalPlace(0.1));
    assert(vixen::wirelessunit::numeric_validation::oneDecimalPlace(1.2));
    assert(vixen::wirelessunit::numeric_validation::oneDecimalPlace(9.9F));
    assert(vixen::wirelessunit::numeric_validation::oneDecimalPlace(10.0));
    assert(!vixen::wirelessunit::numeric_validation::oneDecimalPlace(1.25));
    assert(!vixen::wirelessunit::numeric_validation::oneDecimalPlace(nan));
    using vixen::wirelessunit::numeric_validation::validMeridianSettings;
    assert(validMeridianSettings(0, 0));
    assert(validMeridianSettings(19, 18));
    assert(validMeridianSettings(19, -18));
    assert(!validMeridianSettings(0, -90));
    assert(!validMeridianSettings(19, 20));
    assert(!validMeridianSettings(19, -20));
    assert(!validMeridianSettings(91, 0));

    {
        const auto response = parseResponse("$SYVER=1.300000\r\n");
        assert(response);
        assert(response->command == "$SYVER");
        assert(response->accepted);
        assert(response->fields.size() == 1);
        assert(response->fields[0] == "1.300000");
    }

    {
        const auto response = parseResponse("$MTTGT=NG,BELOWHORIZON\r\n");
        assert(response);
        assert(!response->accepted);
        assert(response->fields.size() == 2);
    }

    assert(!parseResponse("$SYVER=1.3"));
    assert(!parseResponse("SYVER=1.3\r\n"));
    assert(!parseResponse("$SYVER?\r\n"));

    {
        const auto response = parseResponse(
            "$ALLST=4,2,61292.49527,R20.7061+45.379,2,61292.49373,R21.3207+62.702,"
            "2,61292.47277,ASCOM,2,61292.46268,ASCOM\r\n");
        assert(response);
        const auto points = vixen::wirelessunit::decodeAlignmentPoints(*response);
        assert(points && points->size() == 4);
        assert((*points)[0].state == 2);
        assert((*points)[0].modifiedJulianDate == 61292.49527);
        assert((*points)[0].label == "R20.7061+45.379");
        assert((*points)[3].label == "ASCOM");
    }
    {
        const auto response = parseResponse("$ALLST=0\r\n");
        assert(response);
        const auto points = vixen::wirelessunit::decodeAlignmentPoints(*response);
        assert(points && points->empty());
    }
    {
        const auto response = parseResponse("$ALLST=2,2,61292.0,OnlyOneRecord\r\n");
        assert(response);
        assert(!vixen::wirelessunit::decodeAlignmentPoints(*response));
    }

    {
        const auto response = parseResponse(
            "$MTSTA=60400.125000,2,x,y,100.000000,200.000000,180.000000,45.000000,12.500000,-20.250000,3\r\n");
        assert(response);
        const auto status = vixen::wirelessunit::decodeMountStatus(*response);
        assert(status);
        assert(status->rightAscensionHours == 12.5);
        assert(status->declinationDegrees == -20.25);
        assert(status->targetObjectType == 3);
    }
    for (const char *invalid : {
             "$MTSTA=nan,1,0,0,100,200,180,45,12.5,-20.25,3,--\r\n",
             "$MTSTA=-1,1,0,0,100,200,180,45,12.5,-20.25,3,--\r\n",
             "$MTSTA=60400,99,0,0,100,200,180,45,12.5,-20.25,3,--\r\n",
             "$MTSTA=60400,1,0,0,inf,200,180,45,12.5,-20.25,3,--\r\n",
             "$MTSTA=60400,1,0,0,100,200,361,45,12.5,-20.25,3,--\r\n",
             "$MTSTA=60400,1,0,0,100,200,180,91,12.5,-20.25,3,--\r\n",
             "$MTSTA=60400,1,0,0,100,200,180,45,25,-20.25,3,--\r\n",
             "$MTSTA=60400,1,0,0,100,200,180,45,12.5,-91,3,--\r\n",
             "$MTSTA=60400,1,0,0,100,200,180,45,12.5,-20.25,99,--\r\n",
         })
    {
        const auto response = parseResponse(invalid);
        assert(response);
        assert(!vixen::wirelessunit::decodeMountStatus(*response));
    }

    assert(vixen::wirelessunit::gotoRaDec(12.5, -20.25) ==
           "$MTTGT=3,12.500000,-20.250000,INDI\r\n");
    assert(vixen::wirelessunit::gotoAltAz(270.0, 0.0) ==
           "$MTTGT=2,270.000000,0.000000,INDI\r\n");
    assert(vixen::wirelessunit::gotoHome() == "$MTTGT=1,270,0,HOME\r\n");
    assert(vixen::wirelessunit::initializeMountCommand() == "$MTINI=1\r\n");
    assert(vixen::wirelessunit::alignmentLabelRaDec(20.706113, 45.379394) == "R20.7061+45.379");
    assert(vixen::wirelessunit::alignmentLabelRaDec(5.5, -20.25) == "R05.5000-20.250");
    assert(vixen::wirelessunit::alignmentLabelRaDec(20.21176, -0.73975) == "R20.2118-00.740");
    assert(vixen::wirelessunit::alignmentLabelAltAz(270.0, 0.0) == "A270.000+00.000");
    assert(vixen::wirelessunit::alignmentLabelAltAz(5.5, -20.25) == "A005.500-20.250");
    assert(vixen::wirelessunit::alignmentLabelAltAz(360.0, 90.0) == "A360.000+90.000");
    assert(vixen::wirelessunit::alignRaDec(1.25, 30.0) ==
           "$ALIGN=3,1.250000,30.000000,R01.2500+30.000\r\n");
    assert(vixen::wirelessunit::alignAltAz(270.0, 0.0) ==
           "$ALIGN=2,270.000000,0.000000,A270.000+00.000\r\n");
    assert(vixen::wirelessunit::deleteAlignmentPoint(7) == "$ALDEL=7\r\n");
    assert(vixen::wirelessunit::moveBeforeInitialization(1.0, 100.0, -30.0) ==
           "$MTMV0=1.0,100.0000,-30.0000\r\n");
    assert(vixen::wirelessunit::moveBeforeInitialization(
               vixen::wirelessunit::motion_timing::continuousMotionDurationSeconds, 0.5, 0.0) ==
           "$MTMV0=3.0,0.5000,0.0000\r\n");
    // Even if MTMV0 and MTSTA each consume their normal 500 ms deadline,
    // the next refresh is scheduled before the previous 3 s command expires.
    constexpr auto delayedRefreshMilliseconds =
        vixen::wirelessunit::motion_timing::statusPollingMilliseconds +
        2 * vixen::wirelessunit::motion_timing::normalTransactionTimeoutMilliseconds;
    assert(delayedRefreshMilliseconds <
           vixen::wirelessunit::motion_timing::continuousMotionDurationSeconds * 1000.0);
    assert(vixen::wirelessunit::moveRaDec(3.0, 1.0, -0.5) ==
           "$MTMV2=3.00,1.000000,-0.500000\r\n");

    vixen::wirelessunit::MountStatus uninitializedStatus;
    uninitializedStatus.modifiedJulianDate = 0.000442;
    assert(!vixen::wirelessunit::mountStatusIsInitialized(uninitializedStatus));
    uninitializedStatus.modifiedJulianDate = 61290.345730;
    assert(vixen::wirelessunit::mountStatusIsInitialized(uninitializedStatus));
    assert(vixen::wirelessunit::destinationPierSide(12.0, -10.0) ==
           "$MTCPI?3,12.000000,-10.000000\r\n");
    assert(vixen::wirelessunit::setAutoguideSpeed(2.0F, 3.1F) ==
           "$MTAGS=2.000000,3.100000\r\n");
    assert(vixen::wirelessunit::setGotoSpeed(1000) == "$MTGTS=1000\r\n");
    assert(vixen::wirelessunit::setBacklash(11, 15) == "$MTBLC=11,15\r\n");
    assert(vixen::wirelessunit::setMotorPower(1) == "$MTPWR=1\r\n");
    assert(vixen::wirelessunit::setMountType(0) == "$MTTYP=0\r\n");
    assert(vixen::wirelessunit::setMeridianSettings(19, -18) == "$MTCOM=19,-18\r\n");

    {
        const auto response = parseResponse("$MTGTS=700\r\n");
        assert(response);
        const auto speed = vixen::wirelessunit::decodeGotoSpeed(*response);
        assert(speed && *speed == 700);
    }

    {
        const auto response = parseResponse("$MTBLC=11,15\r\n");
        assert(response);
        const auto backlash = vixen::wirelessunit::decodeBacklash(*response);
        assert(backlash);
        assert(backlash->rightAscension == 11);
        assert(backlash->declination == 15);
    }

    {
        const auto powerResponse = parseResponse("$MTPWR=4\r\n");
        const auto typeResponse = parseResponse("$MTTYP=1\r\n");
        assert(powerResponse && typeResponse);
        const auto power = vixen::wirelessunit::decodeMotorPower(*powerResponse);
        const auto type = vixen::wirelessunit::decodeMountType(*typeResponse);
        assert(power && *power == 4);
        assert(type && *type == 1);
    }

    {
        const auto response = parseResponse("$MTCOM=19,-18\r\n");
        assert(response);
        const auto meridian = vixen::wirelessunit::decodeMeridianSettings(*response);
        assert(meridian);
        assert(meridian->forcedTrackingStopDegrees == 19);
        assert(meridian->automaticGotoDegrees == -18);
    }

    {
        const auto response = parseResponse("$WFSTS=1,1,ExampleNetworkB,\r\n");
        assert(response);
        assert(response->fields.size() == 4);
        const auto settings = vixen::wirelessunit::decodeWifiStationSettings(*response);
        assert(settings && settings->autoConnect);
        assert(settings->stationModeEnabled);
        assert(settings->ssid == "ExampleNetworkB");
    }
    {
        const auto response = parseResponse("$WFSTS=0,1,ExampleNetworkB,\r\n");
        assert(response);
        const auto settings = vixen::wirelessunit::decodeWifiStationSettings(*response);
        assert(settings && !settings->autoConnect);
        assert(settings->stationModeEnabled);
        assert(vixen::wirelessunit::setWifiStationSettings(true, "ExampleNetworkB", "secret") ==
               "$WFSTS=1,1,ExampleNetworkB,secret\r\n");
        assert(vixen::wirelessunit::setWifiStationSettings(false, "ExampleNetworkB", "") ==
               "$WFSTS=0,1,ExampleNetworkB,\r\n");
        assert(vixen::wirelessunit::setWifiStationConnection(true) == "$WFSTC=1\r\n");
        assert(vixen::wirelessunit::setWifiStationConnection(false) == "$WFSTC=0\r\n");
    }
    {
        const auto response = parseResponse("$WFSTC=1,\"10.42.0.60\"\r\n");
        assert(response);
        const auto connection = vixen::wirelessunit::decodeWifiStationConnection(*response);
        assert(connection && connection->connected);
        assert(connection->ipAddress == "10.42.0.60");
    }
    {
        const auto response = parseResponse(
            "$WFAPL=3,4,\"ExampleNetworkA\",-62,3,\"ExampleNetworkB\",-71,3,\"\",-91\r\n");
        assert(response);
        const auto accessPoints = vixen::wirelessunit::decodeWifiAccessPoints(*response);
        assert(accessPoints && accessPoints->size() == 3);
        assert((*accessPoints)[0].type == 4);
        assert((*accessPoints)[0].ssid == "ExampleNetworkA");
        assert((*accessPoints)[0].signalDbm == -62);
        assert((*accessPoints)[1].ssid == "ExampleNetworkB");
        assert((*accessPoints)[2].ssid.empty());
        assert((*accessPoints)[2].signalDbm == -91);
    }
    {
        const auto malformed = parseResponse("$WFAPL=2,4,one,-60\r\n");
        assert(malformed && !vixen::wirelessunit::decodeWifiAccessPoints(*malformed));
    }
    {
        const auto response = parseResponse("$MTMDL=11,1000,400\r\n");
        assert(response);
        const auto identity = vixen::wirelessunit::decodeMountIdentity(*response);
        assert(identity && identity->model == 11);
        assert(identity->maximumGotoSpeed == 1000);
        assert(identity->maximumSecondarySpeed == 400);
    }
    {
        const auto response = parseResponse("$MTMDL=2,800\r\n");
        assert(response);
        const auto identity = vixen::wirelessunit::decodeMountIdentity(*response);
        assert(identity && identity->maximumSecondarySpeed == 800);
    }
    {
        const auto response = parseResponse("$WFSID=VixenWirelessUnitTest\r\n");
        assert(response);
        const auto ssid = vixen::wirelessunit::decodeWifiHotspotSsid(*response);
        assert(ssid && *ssid == "VixenWirelessUnitTest");
        assert(vixen::wirelessunit::setWifiHotspotSsid("VixenWirelessUnit-test") ==
               "$WFSID=VixenWirelessUnit-test\r\n");
        assert(vixen::wirelessunit::setWifiHotspotPassword("test-password") ==
               "$WFPWD=test-password\r\n");
        const auto passwordResponse = parseResponse("$WFPWD=old-password\r\n");
        assert(passwordResponse);
        const auto password = vixen::wirelessunit::decodeWifiHotspotPassword(*passwordResponse);
        assert(password && *password == "old-password");
        const auto channelResponse = parseResponse("$WFCHN=11\r\n");
        assert(channelResponse);
        const auto channel = vixen::wirelessunit::decodeWifiHotspotChannel(*channelResponse);
        assert(channel && *channel == 11);
        assert(vixen::wirelessunit::setWifiHotspotChannel(1) == "$WFCHN=1\r\n");
    }

    {
        using namespace std::chrono;
        const auto utc = system_clock::from_time_t(0) + milliseconds(500);
        assert(vixen::wirelessunit::setUtc(utc) == "$SYUTM=1970,1,1,0,0,0.500000\r\n");
    }

    return 0;
}
