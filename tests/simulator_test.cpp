/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#include "client.hpp"
#include "simulator_transport.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>

int main()
{
    using namespace std::chrono_literals;
    using vixen::wirelessunit::ProtocolClient;
    using vixen::wirelessunit::SimulatorTransport;

    auto now = std::chrono::steady_clock::time_point {};
    auto simulator = std::make_unique<SimulatorTransport>("", "", false, "", false, [&now] { return now; });
    auto *transport = simulator.get();
    ProtocolClient client(std::move(simulator));
    std::string error;
    assert(client.connect(error));
    const auto initialized = client.initializeMount();
    assert(initialized && *initialized.value);

    assert(client.slewRaDec(6.0, 30.0));
    const auto labeledStatus = transport->transact(vixen::wirelessunit::query::mountStatus, 500ms);
    const auto parsedLabeledStatus = vixen::wirelessunit::parseResponse(labeledStatus.response);
    assert(labeledStatus.success && parsedLabeledStatus && parsedLabeledStatus->fields.size() == 12);
    assert(parsedLabeledStatus->fields[11] == "INDI");
    auto status = client.mountStatus();
    assert(status && status.value->status == 2);
    assert(std::abs(status.value->rightAscensionHours) < 1e-9);
    now += 1s;
    status = client.mountStatus();
    assert(status && status.value->status == 2);
    assert(std::abs(status.value->rightAscensionHours - 3.0) < 1e-6);
    assert(std::abs(status.value->declinationDegrees - 15.0) < 1e-6);
    now += 900ms;
    status = client.mountStatus();
    assert(status && status.value->status == 3);
    now += 100ms;
    status = client.mountStatus();
    assert(status && status.value->status == 1);
    assert(std::abs(status.value->rightAscensionHours - 6.0) < 1e-6);

    assert(client.slewRaDec(12.0, 40.0));
    now += 500ms;
    status = client.mountStatus();
    assert(status && status.value->status == 2);
    const double abortedRa = status.value->rightAscensionHours;
    assert(client.abortMotion());
    now += 3s;
    status = client.mountStatus();
    assert(status && status.value->status == 1 && status.value->targetObjectType == 2);
    assert(std::abs(status.value->rightAscensionHours - abortedRa) < 1e-6);

    const double beforeGuideRa = status.value->rightAscensionHours;
    const double beforeGuideDec = status.value->declinationDegrees;
    assert(client.pulseGuide(1.0, 1.0, 0.0));
    now += 250ms;
    assert(client.pulseGuide(1.0, 0.0, 1.0));
    now += 750ms;
    status = client.mountStatus();
    assert(status);
    assert(std::abs((status.value->rightAscensionHours - beforeGuideRa) -
                    (vixen::wirelessunit::siderealMultiplierToDegreesPerSecond(1.0) / 15.0)) < 2e-6);
    assert(std::abs((status.value->declinationDegrees - beforeGuideDec) -
                    (vixen::wirelessunit::siderealMultiplierToDegreesPerSecond(1.0) * 0.75)) < 2e-6);

    assert(client.setTrackingRates(1.0, 0.5));
    const auto trackingRates = transport->transact(vixen::wirelessunit::query::moveRate, 500ms);
    const auto parsedTrackingRates = vixen::wirelessunit::parseResponse(trackingRates.response);
    assert(trackingRates.success && parsedTrackingRates && parsedTrackingRates->fields.size() == 2);
    assert(std::abs(std::stod(parsedTrackingRates->fields[0]) - 1.0) < 1e-6);
    assert(std::abs(std::stod(parsedTrackingRates->fields[1]) - 0.5) < 1e-6);

    assert(client.syncRaDec(20.706113, 45.379394));
    assert(client.syncAltAz(270.0, 0.0));
    auto points = client.alignmentPoints();
    assert(points && points.value->size() == 2);
    assert((*points.value)[0].label == "A270.000+00.000");
    assert((*points.value)[1].label == "R20.7061+45.379");
    assert(client.deleteAlignmentPoint(0));
    points = client.alignmentPoints();
    assert(points && points.value->size() == 1);
    assert(client.deleteAlignmentPoint(0));
    points = client.alignmentPoints();
    assert(points && points.value->empty());

    transport->loseNextResponses("MTSTA", 2);
    assert(!client.mountStatus());
    assert(!client.mountStatus());
    assert(client.mountStatus());
    transport->rejectNextCommands("MTGTS", 1);
    assert(!client.setGotoSpeedValue(700));
    const auto speed = client.gotoSpeed();
    assert(speed && *speed.value == 1000);

    assert(client.home());
    status = client.mountStatus();
    assert(status && status.value->status == 2);
    now += 2s;
    status = client.mountStatus();
    assert(status && status.value->status == 1 && status.value->targetObjectType == 1);
    assert(std::abs(status.value->azimuthDegrees - 270.0) < 1e-6);
    assert(std::abs(status.value->altitudeDegrees) < 1e-6);

    assert(client.slewAltAz(180.0, 45.0));
    now += 1s;
    status = client.mountStatus();
    assert(status && status.value->status == 2);
    assert(std::abs(status.value->azimuthDegrees - 225.0) < 1e-6);
    now += 1s;
    status = client.mountStatus();
    assert(status && status.value->status == 1 && status.value->targetObjectType == 2);
    assert(std::abs(status.value->altitudeDegrees - 45.0) < 1e-6);
}
