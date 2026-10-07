/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace vixen::wirelessunit
{

enum class TransportKind
{
    tcp,
    udp
};

struct Endpoint
{
    std::string host;
    std::uint16_t port { 60023 };
    TransportKind kind { TransportKind::tcp };
};

struct TransportResult
{
    bool success { false };
    std::string response;
    std::string error;
};

class ITransport
{
    public:
        virtual ~ITransport() = default;
        virtual bool connect(std::string &error) = 0;
        virtual void disconnect() noexcept = 0;
        virtual bool isConnected() const noexcept = 0;
        virtual TransportResult transact(
            std::string_view request,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) = 0;
};

class SocketTransport final : public ITransport
{
    public:
        explicit SocketTransport(Endpoint endpoint);
        ~SocketTransport() override;

        SocketTransport(const SocketTransport &) = delete;
        SocketTransport &operator=(const SocketTransport &) = delete;

        bool connect(std::string &error) override;
        void disconnect() noexcept override;
        bool isConnected() const noexcept override;
        TransportResult transact(
            std::string_view request,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) override;

        const Endpoint &endpoint() const noexcept;

    private:
        bool connectUnlocked(std::string &error);
        bool sendAll(std::string_view request, std::string &error);
        TransportResult receiveLine(std::chrono::milliseconds timeout);

        Endpoint endpoint_;
        int socket_ { -1 };
        mutable std::mutex mutex_;
        std::string receiveBuffer_;
};

} // namespace vixen::wirelessunit
