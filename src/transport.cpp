/*
 * Copyright (C) 2026 Satoshi Masuko <wakeup1002@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of INDI Vixen Wireless Unit and is licensed under the
 * GNU General Public License, version 3 or later. See COPYING.
 */

#include "transport.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace vixen::wirelessunit
{
namespace
{

constexpr std::size_t kMaximumResponseBytes = 4096;

std::string systemError(std::string_view operation)
{
    return std::string(operation) + ": " + std::strerror(errno);
}

timeval toTimeval(std::chrono::milliseconds timeout)
{
    timeval value {};
    value.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    value.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    return value;
}

} // namespace

SocketTransport::SocketTransport(Endpoint endpoint) : endpoint_(std::move(endpoint))
{
}

SocketTransport::~SocketTransport()
{
    disconnect();
}

bool SocketTransport::connect(std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connectUnlocked(error);
}

bool SocketTransport::connectUnlocked(std::string &error)
{
    if (socket_ >= 0)
        return true;

    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = endpoint_.kind == TransportKind::tcp ? SOCK_STREAM : SOCK_DGRAM;
    hints.ai_protocol = endpoint_.kind == TransportKind::tcp ? IPPROTO_TCP : IPPROTO_UDP;

    addrinfo *addresses = nullptr;
    const auto port = std::to_string(endpoint_.port);
    const int resolveResult = getaddrinfo(endpoint_.host.c_str(), port.c_str(), &hints, &addresses);
    if (resolveResult != 0)
    {
        error = "getaddrinfo: " + std::string(gai_strerror(resolveResult));
        return false;
    }

    int candidate = -1;
    for (auto *address = addresses; address != nullptr; address = address->ai_next)
    {
        candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate < 0)
            continue;

        const int originalFlags = fcntl(candidate, F_GETFL, 0);
        if (originalFlags >= 0)
            fcntl(candidate, F_SETFL, originalFlags | O_NONBLOCK);

        int result = ::connect(candidate, address->ai_addr, address->ai_addrlen);
        if (result < 0 && errno == EINPROGRESS)
        {
            pollfd descriptor { candidate, POLLOUT, 0 };
            result = ::poll(&descriptor, 1, 500);
            if (result > 0)
            {
                int socketError = 0;
                socklen_t length = sizeof(socketError);
                if (getsockopt(candidate, SOL_SOCKET, SO_ERROR, &socketError, &length) < 0 || socketError != 0)
                {
                    errno = socketError == 0 ? errno : socketError;
                    result = -1;
                }
                else
                {
                    result = 0;
                }
            }
            else
            {
                if (result == 0)
                    errno = ETIMEDOUT;
                result = -1;
            }
        }

        if (originalFlags >= 0)
            fcntl(candidate, F_SETFL, originalFlags);

        if (result == 0)
            break;

        ::close(candidate);
        candidate = -1;
    }
    freeaddrinfo(addresses);

    if (candidate < 0)
    {
        error = systemError("connect");
        return false;
    }

    socket_ = candidate;
    const timeval sendTimeout = toTimeval(std::chrono::milliseconds(200));
    const timeval receiveTimeout = toTimeval(std::chrono::milliseconds(500));
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout));
    receiveBuffer_.clear();
    return true;
}

void SocketTransport::disconnect() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_ >= 0)
    {
        ::shutdown(socket_, SHUT_RDWR);
        ::close(socket_);
        socket_ = -1;
    }
    receiveBuffer_.clear();
}

bool SocketTransport::isConnected() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return socket_ >= 0;
}

const Endpoint &SocketTransport::endpoint() const noexcept
{
    return endpoint_;
}

bool SocketTransport::sendAll(std::string_view request, std::string &error)
{
    std::size_t sent = 0;
    while (sent < request.size())
    {
        const auto count = ::send(socket_, request.data() + sent, request.size() - sent, 0);
        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            error = systemError("send");
            return false;
        }
        if (count == 0)
        {
            error = "send: connection closed";
            return false;
        }
        sent += static_cast<std::size_t>(count);
        if (endpoint_.kind == TransportKind::udp)
        {
            if (sent != request.size())
            {
                error = "send: incomplete UDP datagram";
                return false;
            }
            break;
        }
    }
    return true;
}

TransportResult SocketTransport::receiveLine(std::chrono::milliseconds timeout)
{
    if (endpoint_.kind == TransportKind::udp)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return { false, {}, "receive: timeout" };

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            pollfd descriptor { socket_, POLLIN, 0 };
            const int pollResult =
                ::poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(1, remaining.count())));
            if (pollResult == 0)
                return { false, {}, "receive: timeout" };
            if (pollResult < 0)
            {
                if (errno == EINTR)
                    continue;
                return { false, {}, systemError("poll") };
            }
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                return { false, {}, "receive: UDP socket error" };

            // One Wireless Unit reply occupies one UDP datagram. Reserve one
            // byte beyond the documented transport ceiling so an oversize
            // datagram is distinguishable even on platforms that do not set
            // MSG_TRUNC consistently.
            std::array<char, kMaximumResponseBytes + 1> buffer {};
            iovec vector { buffer.data(), buffer.size() };
            msghdr message {};
            message.msg_iov = &vector;
            message.msg_iovlen = 1;
            const auto count = ::recvmsg(socket_, &message, 0);
            if (count < 0)
            {
                if (errno == EINTR)
                    continue;
                return { false, {}, systemError("recvmsg") };
            }
            if ((message.msg_flags & MSG_TRUNC) != 0 ||
                static_cast<std::size_t>(count) > kMaximumResponseBytes)
                return { false, {}, "receive: UDP datagram exceeds 4096 bytes" };

            std::string datagram(buffer.data(), static_cast<std::size_t>(count));
            if (datagram.size() < 2 || datagram.compare(datagram.size() - 2, 2, "\r\n") != 0)
                return { false, {}, "receive: UDP datagram did not end with CRLF" };
            const auto firstTerminator = datagram.find("\r\n");
            if (firstTerminator != datagram.size() - 2)
                return { false, {}, "receive: UDP datagram contains multiple response lines" };
            return { true, std::move(datagram), {} };
        }
    }

    const auto delimiter = receiveBuffer_.find("\r\n");
    if (delimiter != std::string::npos)
    {
        std::string line = receiveBuffer_.substr(0, delimiter + 2);
        receiveBuffer_.erase(0, delimiter + 2);
        return { true, std::move(line), {} };
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (receiveBuffer_.size() < kMaximumResponseBytes)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return { false, {}, "receive: timeout waiting for CRLF" };

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd descriptor { socket_, POLLIN, 0 };
        const int pollResult = ::poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(1, remaining.count())));
        if (pollResult == 0)
            return { false, {}, "receive: timeout" };
        if (pollResult < 0)
        {
            if (errno == EINTR)
                continue;
            return { false, {}, systemError("poll") };
        }

        char buffer[1024];
        const auto available = kMaximumResponseBytes - receiveBuffer_.size();
        const auto count = ::recv(socket_, buffer, std::min(sizeof(buffer), available), 0);
        if (count < 0)
        {
            if (errno == EINTR)
                continue;
            return { false, {}, systemError("recv") };
        }
        if (count == 0)
            return { false, {}, "receive: connection closed" };

        receiveBuffer_.append(buffer, static_cast<std::size_t>(count));
        const auto end = receiveBuffer_.find("\r\n");
        if (end != std::string::npos)
        {
            std::string line = receiveBuffer_.substr(0, end + 2);
            receiveBuffer_.erase(0, end + 2);
            return { true, std::move(line), {} };
        }
    }
    return { false, {}, "receive: response exceeds 4096 bytes" };
}

TransportResult SocketTransport::transact(std::string_view request, std::chrono::milliseconds timeout)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    if (!connectUnlocked(error))
        return { false, {}, std::move(error) };

    timeval socketTimeout = toTimeval(timeout);
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &socketTimeout, sizeof(socketTimeout));

    if (!sendAll(request, error))
    {
        ::close(socket_);
        socket_ = -1;
        return { false, {}, std::move(error) };
    }

    auto result = receiveLine(timeout);
    if (!result.success)
    {
        ::close(socket_);
        socket_ = -1;
        receiveBuffer_.clear();
    }
    return result;
}

} // namespace vixen::wirelessunit
