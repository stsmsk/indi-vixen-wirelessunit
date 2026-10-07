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
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace
{
class SiteReplyTransport final : public vixen::wirelessunit::ITransport
{
    public:
        explicit SiteReplyTransport(std::string reply) : reply_(std::move(reply)) {}
        bool connect(std::string &) override { connected_ = true; return true; }
        void disconnect() noexcept override { connected_ = false; }
        bool isConnected() const noexcept override { return connected_; }
        vixen::wirelessunit::TransportResult transact(std::string_view request,
                                                 std::chrono::milliseconds) override
        {
            assert(request == "$SYLOC=12.345678,-23.456789,0.000000\r\n");
            ++calls;
            return { true, reply_, {} };
        }
        int calls { 0 };

    private:
        std::string reply_;
        bool connected_ { false };
};

bool setWithReply(std::string reply)
{
    auto transport = std::make_unique<SiteReplyTransport>(std::move(reply));
    vixen::wirelessunit::ProtocolClient client(std::move(transport));
    std::string error;
    assert(client.connect(error));
    return static_cast<bool>(client.setSite(12.345678, -23.456789, 0.0F));
}
} // namespace

int main()
{
    assert(setWithReply("$SYLOC=12.346,-23.457,0.0\r\n"));
    assert(setWithReply("$SYLOC=OK\r\n"));
    assert(!setWithReply("$SYLOC=NG\r\n"));
    assert(setWithReply("$SYLOC=12.346,-23.457,0.0,extra\r\n"));
    assert(setWithReply("$SYLOC=12.1,-23.457,0.0\r\n"));
    assert(setWithReply("$SYLOC=12.346,-23.1,0.0\r\n"));
    assert(setWithReply("$SYLOC=12.346,-23.457,1.0\r\n"));
    assert(setWithReply("$SYLOC=12.346,NaN,0.0\r\n"));
    assert(!setWithReply("$SYVER=2.60\r\n"));

    auto simulatorTransport = std::make_unique<vixen::wirelessunit::SimulatorTransport>();
    vixen::wirelessunit::ProtocolClient simulator(std::move(simulatorTransport));
    std::string error;
    assert(simulator.connect(error));
    assert(simulator.setSite(12.345678, -23.456789, 0.0F));
    const auto retained = simulator.location();
    assert(retained);
    assert(std::abs(retained.value->longitudeDegrees - 12.346) < 1e-9);
    assert(std::abs(retained.value->latitudeDegrees - (-23.457)) < 1e-9);
    assert(std::abs(retained.value->timezoneHours - 0.0F) < 1e-6);
    assert(!simulator.setSite(NAN, -23.457, 0.0F));
}
