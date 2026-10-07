/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#include "simulator_transport.hpp"

#include "numeric_validation.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <utility>

namespace vixen::wirelessunit
{

SimulatorTransport::SimulatorTransport(std::string rejectedSetter, std::string lostSetterAck,
                                       bool nondefaultMountSettings, std::string failedQuery,
                                       bool failQueryAfterFirstRead, Clock clock) :
    rejectedSetter_(std::move(rejectedSetter)), lostSetterAck_(std::move(lostSetterAck)),
    failedQuery_(std::move(failedQuery)), failQueryAfterFirstRead_(failQueryAfterFirstRead),
    clock_(std::move(clock))
{
    if (nondefaultMountSettings)
    {
        gotoSpeed_ = 700;
        guideRa_ = 2.0F;
        guideDec_ = 3.0F;
        backlashRa_ = 11;
        backlashDec_ = 15;
        motorPower_ = 4;
        mountType_ = 0;
        forcedTrackingStopDegrees_ = 19;
        automaticGotoDegrees_ = -18;
    }
}

void SimulatorTransport::loseNextResponses(std::string commandName, unsigned count)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lostResponses_[std::move(commandName)] = count;
}

void SimulatorTransport::rejectNextCommands(std::string commandName, unsigned count)
{
    std::lock_guard<std::mutex> lock(mutex_);
    rejectedCommands_[std::move(commandName)] = count;
}

void SimulatorTransport::advanceState()
{
    const auto now = clock_();
    if (slewAxis_ != SlewAxis::none)
    {
        const double elapsed = std::chrono::duration<double>(now - slewStarted_).count();
        const double fraction = std::clamp(elapsed / 2.0, 0.0, 1.0);
        const double first = slewStartFirst_ + (slewTargetFirst_ - slewStartFirst_) * fraction;
        const double second = slewStartSecond_ + (slewTargetSecond_ - slewStartSecond_) * fraction;
        if (slewAxis_ == SlewAxis::raDec)
        {
            rightAscension_ = first;
            declination_ = second;
        }
        else
        {
            azimuth_ = first;
            altitude_ = second;
        }
        if (fraction >= 1.0)
        {
            slewAxis_ = SlewAxis::none;
            mountStatus_ = 1;
            targetObjectType_ = slewTargetObjectType_;
        }
        else
            mountStatus_ = fraction >= 0.9 ? 3 : 2;
    }
    for (auto &pulse : pulses_)
    {
        const double elapsed = std::chrono::duration<double>(now - pulse.started).count();
        const double applied = std::clamp(elapsed, 0.0, pulse.durationSeconds);
        const double delta = applied - pulse.appliedSeconds;
        if (delta > 0)
        {
            rightAscension_ += siderealMultiplierToDegreesPerSecond(pulse.raRate) * delta / 15.0;
            declination_ += siderealMultiplierToDegreesPerSecond(pulse.decRate) * delta;
            pulse.appliedSeconds = applied;
        }
    }
    pulses_.erase(std::remove_if(pulses_.begin(), pulses_.end(), [](const Pulse &pulse)
    {
        return pulse.appliedSeconds >= pulse.durationSeconds;
    }), pulses_.end());
}

bool SimulatorTransport::connect(std::string &)
{
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = true;
    return true;
}

void SimulatorTransport::disconnect() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
}

bool SimulatorTransport::isConnected() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

TransportResult SimulatorTransport::transact(std::string_view request, std::chrono::milliseconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_)
        return { false, {}, "simulator is disconnected" };

    advanceState();
    const auto separator = request.find_first_of("=?");
    const std::string commandName = request.size() > 1 && request.front() == '$' && separator != std::string_view::npos
                                        ? std::string(request.substr(1, separator - 1)) : std::string {};
    if (auto found = lostResponses_.find(commandName);
            found != lostResponses_.end() && found->second > 0 && separator != std::string_view::npos &&
            request[separator] == '?')
    {
        --found->second;
        return { false, {}, "simulated receive: timeout" };
    }
    if (auto found = rejectedCommands_.find(commandName);
            found != rejectedCommands_.end() && found->second > 0 && separator != std::string_view::npos &&
            request[separator] == '=')
    {
        --found->second;
        return { true, "$" + commandName + "=NG\r\n", {} };
    }

    const auto matchesSetter = [request](std::string_view name)
    {
        return !name.empty() && request.size() > name.size() + 2 &&
               request.front() == '$' && request.substr(1, name.size()) == name &&
               request[name.size() + 1] == '=';
    };
    if (matchesSetter(rejectedSetter_))
        return { true, "$" + rejectedSetter_ + "=NG\r\n", {} };
    if (!failedQuery_.empty() && request == "$" + failedQuery_ + "?\r\n")
    {
        if (!failQueryAfterFirstRead_ || failedQueryReadCount_++ > 0)
            return { false, {}, "simulated receive: setting query timeout" };
    }
    const auto setterReply = [this, request](std::string_view name) -> TransportResult
    {
        if (name == lostSetterAck_)
            return { false, {}, "simulated receive: timeout after setting was applied" };
        return { true, std::string(request), {} };
    };

    std::ostringstream response;
    response.imbue(std::locale::classic());
    response << std::fixed << std::setprecision(6);

    if (request == query::systemVersion)
        return { true, "$SYVER=2.600000\r\n", {} };
    if (request == query::mountVersion)
        return { true, "$MTVER=1.000000\r\n", {} };
    if (request == query::mountModel)
        return { true, "$MTMDL=11,1000,1000\r\n", {} };
    if (request == query::gotoSpeed)
        return { true, "$MTGTS=" + std::to_string(gotoSpeed_) + "\r\n", {} };
    if (request == query::location)
    {
        response << "$SYLOC=" << std::setprecision(3) << longitude_ << ',' << latitude_ << ','
                 << std::setprecision(1) << timezone_ << "\r\n";
        return { true, response.str(), {} };
    }
    if (request == query::autoguideSpeed)
    {
        response << "$MTAGS=" << guideRa_ << ',' << guideDec_ << "\r\n";
        return { true, response.str(), {} };
    }
    if (request == query::backlash)
    {
        response << "$MTBLC=" << backlashRa_ << ',' << backlashDec_ << "\r\n";
        return { true, response.str(), {} };
    }
    if (request == query::motorPower)
    {
        response << "$MTPWR=" << motorPower_ << "\r\n";
        return { true, response.str(), {} };
    }
    if (request == query::mountType)
    {
        response << "$MTTYP=" << mountType_ << "\r\n";
        return { true, response.str(), {} };
    }
    if (request == query::meridian)
    {
        response << "$MTCOM=" << forcedTrackingStopDegrees_ << ',' << automaticGotoDegrees_ << "\r\n";
        return { true, response.str(), {} };
    }
    if (request == query::moveRate)
    {
        response << "$MTRAT=" << trackingRaRate_ << ',' << trackingDecRate_ << "\r\n";
        return { true, response.str(), {} };
    }
    if (request == query::alignments)
    {
        response << "$ALLST=" << alignments_.size();
        for (const auto &point : alignments_)
            response << ',' << point.state << ',' << point.mjd << ',' << point.label;
        response << "\r\n";
        return { true, response.str(), {} };
    }
    if (request == query::wifiStationSettings)
        return { true, "$WFSTS=" + std::to_string(wifiAutoConnect_ ? 1 : 0) + ',' +
                       std::to_string(wifiStationModeEnabled_ ? 1 : 0) + ',' + wifiSsid_ + ",\r\n", {} };
    if (request == query::wifiStationConnection)
        return { true, wifiStationConnected_ ? "$WFSTC=1,\"10.42.0.60\"\r\n"
                                             : "$WFSTC=0,\"0.0.0.0\"\r\n", {} };
    if (request == query::wifiAccessPoints)
        return { true, "$WFAPL=3,4,\"ExampleNetworkA\",-62,3,\"ExampleNetworkB\",-71,3,\"\",-91\r\n", {} };
    if (request == query::wifiHotspotSsid)
        return { true, "$WFSID=" + wifiHotspotSsid_ + "\r\n", {} };
    if (request == query::wifiHotspotPassword)
        return { true, "$WFPWD=" + wifiHotspotPassword_ + "\r\n", {} };
    if (request == query::wifiHotspotChannel)
        return { true, "$WFCHN=" + std::to_string(wifiHotspotChannel_) + "\r\n", {} };
    if (request.rfind("$ALDEL=", 0) == 0)
    {
        int index = -1;
        if (std::sscanf(std::string(request).c_str(), "$ALDEL=%d", &index) != 1 || index < 0 ||
            static_cast<std::size_t>(index) >= alignments_.size())
            return { true, "$ALDEL=NG\r\n", {} };
        alignments_.erase(alignments_.begin() + index);
        return { true, std::string(request), {} };
    }
    if (request == query::mountStatus)
    {
        response << "$MTSTA=" << (mountInitialized_ ? 60400.0 : 0.0005) << ',' << mountStatus_
                 << ",0," << (mountInitialized_ ? 0 : -1) << ',' << mechanicalX_ << ',' << mechanicalY_ << ','
                 << azimuth_ << ',' << altitude_ << ',' << rightAscension_ << ',' << declination_ << ','
                 << targetObjectType_ << ',' << targetLabel_ << "\r\n";
        return { true, response.str(), {} };
    }
    if (request == query::initializeMount)
    {
        if (mountInitialized_)
            return { true, "$MTINI=NG\r\n", {} };
        mountInitialized_ = true;
        mechanicalX_ = 90;
        mechanicalY_ = 0;
        azimuth_ = 270;
        altitude_ = 0;
        declination_ = 0;
        targetObjectType_ = 1;
        targetLabel_ = "HOME";
        return { true, "$MTINI=1\r\n", {} };
    }

    if (request.rfind("$WFSTS=", 0) == 0)
    {
        const auto parsed = parseResponse(request);
        if (!parsed || parsed->fields.size() != 4)
            return { true, "$WFSTS=NG\r\n", {} };
        int autoConnect = -1;
        int stationMode = -1;
        if (std::sscanf(parsed->fields[0].c_str(), "%d", &autoConnect) != 1 ||
            std::sscanf(parsed->fields[1].c_str(), "%d", &stationMode) != 1 ||
            (autoConnect != 0 && autoConnect != 1) || (stationMode != 0 && stationMode != 1))
            return { true, "$WFSTS=NG\r\n", {} };
        wifiAutoConnect_ = autoConnect == 1;
        wifiStationModeEnabled_ = stationMode == 1;
        wifiSsid_ = parsed->fields[2];
        return { true, "$WFSTS=" + std::to_string(autoConnect) + ',' + std::to_string(stationMode) + ",,\r\n", {} };
    }
    if (request == query::wifiStationConnect)
    {
        wifiStationConnected_ = true;
        return { true, "$WFSTC=1,\"10.42.0.60\"\r\n", {} };
    }
    if (request == query::wifiStationDisconnect)
    {
        wifiStationConnected_ = false;
        return { true, "$WFSTC=0,\"0.0.0.0\"\r\n", {} };
    }
    if (request.rfind("$WFSID=", 0) == 0)
    {
        const auto parsed = parseResponse(request);
        if (!parsed || parsed->fields.size() != 1 || parsed->fields[0].empty())
            return { true, "$WFSID=NG\r\n", {} };
        wifiHotspotSsid_ = parsed->fields[0];
        return { true, "$WFSID=" + wifiHotspotSsid_ + "\r\n", {} };
    }
    if (request.rfind("$WFPWD=", 0) == 0)
    {
        const auto parsed = parseResponse(request);
        if (!parsed || parsed->fields.size() != 1 || parsed->fields[0].empty())
            return { true, "$WFPWD=NG\r\n", {} };
        wifiHotspotPassword_ = parsed->fields[0];
        return { true, "$WFPWD=" + wifiHotspotPassword_ + "\r\n", {} };
    }
    if (request.rfind("$WFCHN=", 0) == 0)
    {
        int channel = 0;
        if (std::sscanf(std::string(request).c_str(), "$WFCHN=%d", &channel) != 1 ||
            channel < 1 || channel > 11)
            return { true, "$WFCHN=NG\r\n", {} };
        wifiHotspotChannel_ = channel;
        return { true, "$WFCHN=" + std::to_string(wifiHotspotChannel_) + "\r\n", {} };
    }

    double first = 0;
    double second = 0;
    if (request.rfind("$MTTGT=3,", 0) == 0)
    {
        const auto parsed = parseResponse(request);
        if (!parsed || parsed->fields.size() != 4 || parsed->fields[3].empty() ||
            std::sscanf(std::string(request).c_str(), "$MTTGT=3,%lf,%lf,", &first, &second) != 2 ||
            !std::isfinite(first) || !std::isfinite(second))
            return { true, "$MTTGT=NG\r\n", {} };
        slewAxis_ = SlewAxis::raDec;
        slewStarted_ = clock_();
        slewStartFirst_ = rightAscension_;
        slewStartSecond_ = declination_;
        slewTargetFirst_ = first;
        slewTargetSecond_ = second;
        slewTargetObjectType_ = 3;
        targetLabel_ = parsed->fields[3];
        mountStatus_ = 2;
        return { true, "$MTTGT=OK\r\n", {} };
    }
    if (request.rfind("$MTTGT=2,", 0) == 0)
    {
        const auto parsed = parseResponse(request);
        if (!parsed || parsed->fields.size() != 4 || parsed->fields[3].empty() ||
            std::sscanf(std::string(request).c_str(), "$MTTGT=2,%lf,%lf,", &first, &second) != 2 ||
            !std::isfinite(first) || !std::isfinite(second))
            return { true, "$MTTGT=NG\r\n", {} };
        slewAxis_ = SlewAxis::altAz;
        slewStarted_ = clock_();
        slewStartFirst_ = azimuth_;
        slewStartSecond_ = altitude_;
        slewTargetFirst_ = first;
        slewTargetSecond_ = second;
        slewTargetObjectType_ = 2;
        targetLabel_ = parsed->fields[3];
        mountStatus_ = 2;
        return { true, "$MTTGT=OK\r\n", {} };
    }
    if (request.rfind("$ALIGN=3,", 0) == 0)
    {
        const auto parsed = parseResponse(request);
        if (!parsed || parsed->fields.size() != 4 || alignments_.size() >= 20 ||
            std::sscanf(std::string(request).c_str(), "$ALIGN=3,%lf,%lf,", &first, &second) != 2 ||
            !std::isfinite(first) || !std::isfinite(second) || parsed->fields[3].empty())
            return { true, "$ALIGN=NG\r\n", {} };
        rightAscension_ = first;
        declination_ = second;
        alignments_.insert(alignments_.begin(), Alignment { 2, 60400.0 + alignments_.size() * 0.001,
                                                              parsed->fields[3] });
        return { true, std::string(request), {} };
    }
    if (request.rfind("$ALIGN=2,", 0) == 0)
    {
        const auto parsed = parseResponse(request);
        if (!parsed || parsed->fields.size() != 4 || alignments_.size() >= 20 ||
            std::sscanf(std::string(request).c_str(), "$ALIGN=2,%lf,%lf,", &first, &second) != 2 ||
            !std::isfinite(first) || !std::isfinite(second) || parsed->fields[3].empty())
            return { true, "$ALIGN=NG\r\n", {} };
        azimuth_ = first;
        altitude_ = second;
        alignments_.insert(alignments_.begin(), Alignment { 2, 60400.0 + alignments_.size() * 0.001,
                                                              parsed->fields[3] });
        return { true, std::string(request), {} };
    }
    if (request.rfind("$SYLOC=", 0) == 0)
    {
        double longitude = 0;
        double latitude = 0;
        float timezone = 0;
        const auto parsed = parseResponse(request);
        if (!parsed || parsed->fields.size() != 3 ||
            std::sscanf(std::string(request).c_str(), "$SYLOC=%lf,%lf,%f", &longitude, &latitude, &timezone) != 3 ||
            !std::isfinite(longitude) || !std::isfinite(latitude) || !std::isfinite(timezone))
            return { true, "$SYLOC=NG\r\n", {} };
        longitude_ = std::round(longitude * 1000.0) / 1000.0;
        latitude_ = std::round(latitude * 1000.0) / 1000.0;
        timezone_ = std::round(static_cast<double>(timezone) * 10.0) / 10.0;
        response << "$SYLOC=" << std::setprecision(3) << longitude_ << ',' << latitude_ << ','
                 << std::setprecision(1) << timezone_ << "\r\n";
        return { true, response.str(), {} };
    }
    float guideRa = 0;
    float guideDec = 0;
    if (std::sscanf(std::string(request).c_str(), "$MTAGS=%f,%f", &guideRa, &guideDec) == 2)
    {
        if (guideRa < 0.1F || guideRa > 10.0F || guideDec < 0.1F || guideDec > 10.0F)
            return { true, "$MTAGS=NG\r\n", {} };
        guideRa_ = guideRa;
        guideDec_ = guideDec;
        return setterReply(query::autoguideSpeedName);
    }

    int firstInteger = 0;
    int secondInteger = 0;
    if (std::sscanf(std::string(request).c_str(), "$MTGTS=%d", &firstInteger) == 1)
    {
        if (firstInteger != 100 && firstInteger != 200 && firstInteger != 400 &&
            firstInteger != 700 && firstInteger != 1000)
            return { true, "$MTGTS=NG\r\n", {} };
        gotoSpeed_ = firstInteger;
        return setterReply(query::gotoSpeedName);
    }
    if (std::sscanf(std::string(request).c_str(), "$MTBLC=%d,%d", &firstInteger, &secondInteger) == 2)
    {
        if (firstInteger < 0 || firstInteger > 99 || secondInteger < 0 || secondInteger > 99)
            return { true, "$MTBLC=NG\r\n", {} };
        backlashRa_ = firstInteger;
        backlashDec_ = secondInteger;
        return setterReply(query::backlashName);
    }
    if (std::sscanf(std::string(request).c_str(), "$MTPWR=%d", &firstInteger) == 1)
    {
        if (firstInteger < 1 || firstInteger > 4)
            return { true, "$MTPWR=NG\r\n", {} };
        motorPower_ = firstInteger;
        return setterReply(query::motorPowerName);
    }
    if (std::sscanf(std::string(request).c_str(), "$MTTYP=%d", &firstInteger) == 1)
    {
        if (firstInteger < 0 || firstInteger > 1)
            return { true, "$MTTYP=NG\r\n", {} };
        mountType_ = firstInteger;
        return setterReply(query::mountTypeName);
    }
    if (std::sscanf(std::string(request).c_str(), "$MTCOM=%d,%d", &firstInteger, &secondInteger) == 2)
    {
        if (!numeric_validation::validMeridianSettings(firstInteger, secondInteger))
            return { true, "$MTCOM=NG\r\n", {} };
        forcedTrackingStopDegrees_ = firstInteger;
        automaticGotoDegrees_ = secondInteger;
        return setterReply(query::meridianName);
    }

    if (request == query::stopMount)
    {
        // The real SXP2 settles at status 1 / target type 2 after the
        // intermediate status-3 deceleration state. The simulator completes
        // the stop immediately and must still emit a decoder-valid MTSTA.
        mountStatus_ = 1;
        targetObjectType_ = 2;
        targetLabel_ = "--";
        slewAxis_ = SlewAxis::none;
        pulses_.clear();
        return { true, "$MTSTP=1\r\n", {} };
    }
    if (request.rfind("$MTMV2=", 0) == 0)
    {
        double duration = 0;
        double raRate = 0;
        double decRate = 0;
        if (std::sscanf(std::string(request).c_str(), "$MTMV2=%lf,%lf,%lf", &duration, &raRate, &decRate) != 3 ||
            !std::isfinite(duration) || !std::isfinite(raRate) || !std::isfinite(decRate) || duration <= 0)
            return { true, "$MTMV2=NG\r\n", {} };
        pulses_.push_back(Pulse { clock_(), duration, raRate, decRate, 0 });
        return { true, std::string(request), {} };
    }
    double duration = 0;
    double raRate = 0;
    double decRate = 0;
    if (std::sscanf(std::string(request).c_str(), "$MTMV0=%lf,%lf,%lf", &duration, &raRate, &decRate) == 3)
    {
        mechanicalX_ += siderealMultiplierToDegreesPerSecond(raRate) * duration;
        mechanicalY_ += siderealMultiplierToDegreesPerSecond(decRate) * duration;
        declination_ = mechanicalY_;
        return { true, std::string(request), {} };
    }
    if (request.rfind("$MTRAT=", 0) == 0)
    {
        if (std::sscanf(std::string(request).c_str(), "$MTRAT=%lf,%lf", &first, &second) != 2 ||
            !std::isfinite(first) || !std::isfinite(second))
            return { true, "$MTRAT=NG\r\n", {} };
        trackingRaRate_ = first;
        trackingDecRate_ = second;
        return { true, std::string(request), {} };
    }
    if (request.rfind("$SYUTM=", 0) == 0)
        return { true, std::string(request), {} };
    if (request.rfind("$MTTGT=1,", 0) == 0)
    {
        slewAxis_ = SlewAxis::altAz;
        slewStarted_ = clock_();
        slewStartFirst_ = azimuth_;
        slewStartSecond_ = altitude_;
        slewTargetFirst_ = 270;
        slewTargetSecond_ = 0;
        slewTargetObjectType_ = 1;
        targetLabel_ = "HOME";
        mountStatus_ = 2;
        return { true, "$MTTGT=1,270.000000,0.000000,HOME\r\n", {} };
    }

    return { true, "$UNKNOWN=NG\r\n", {} };
}

} // namespace vixen::wirelessunit
