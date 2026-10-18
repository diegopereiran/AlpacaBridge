// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

#ifndef _WIN32

#include <alpacacore/vendor/celestron/celestron_cgx_protocol.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "fake_pty_write.h"

namespace alpacacore::test {

// Celestron Focus Motor on its own USB port (issue #896): AUX device 0x12
// answering the commands the reporter's script and INDI's driver use. KNOWN
// PARITY GAPS: motion is explicit (complete_move()), not timed; replies to
// commands the fake does not know are silent, as on the bus; the calibration
// commands (0x2A/0x2B) are not modelled.
class FakeCelestronFocuser {
public:
    FakeCelestronFocuser() : pty_("FakeCelestronFocuser"), worker_([this] { run(); }) {}
    ~FakeCelestronFocuser() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    FakeCelestronFocuser(const FakeCelestronFocuser&) = delete;
    FakeCelestronFocuser& operator=(const FakeCelestronFocuser&) = delete;

    bool ok() const { return pty_.master_fd() >= 0; }
    std::string slave_path() const { return pty_.slave_path(); }

    void set_limits(std::uint32_t low, std::uint32_t high) {
        std::lock_guard<std::mutex> lock(mutex_);
        low_ = low;
        high_ = high;
    }
    void set_raw_position(std::uint32_t raw) {
        std::lock_guard<std::mutex> lock(mutex_);
        raw_ = raw;
    }
    void set_version(std::vector<std::uint8_t> version) {
        std::lock_guard<std::mutex> lock(mutex_);
        version_ = std::move(version);
    }
    void set_muted(bool muted) { muted_.store(muted); }

    // Finishes the motion a GOTO started: the encoder reaches the target.
    void complete_move() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (target_) raw_ = *target_;
        target_.reset();
    }

    std::uint32_t raw_position() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return raw_;
    }
    bool moving() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return target_.has_value();
    }
    std::optional<std::uint32_t> last_goto() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_goto_;
    }
    int count(std::uint8_t command) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(std::count_if(requests_.begin(), requests_.end(),
                                              [command](const auto& frame) { return frame.command == command; }));
    }
    std::vector<alpacacore::vendor::celestron::AuxFrame> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    void run() {
        std::vector<std::uint8_t> buffer;
        while (!stop_.load()) {
            struct pollfd descriptor {};
            descriptor.fd = pty_.master_fd();
            descriptor.events = POLLIN;
            if (::poll(&descriptor, 1, 20) <= 0 || (descriptor.revents & POLLIN) == 0) continue;
            std::uint8_t bytes[128];
            const ssize_t count = ::read(pty_.master_fd(), bytes, sizeof(bytes));
            if (count <= 0) continue;
            buffer.insert(buffer.end(), bytes, bytes + count);
            process(buffer);
        }
    }

    void process(std::vector<std::uint8_t>& buffer) {
        using alpacacore::vendor::celestron::AuxFrame;
        using alpacacore::vendor::celestron::decode_aux_frame;
        using alpacacore::vendor::celestron::encode_aux_frame;
        while (!buffer.empty()) {
            buffer.erase(buffer.begin(), std::find(buffer.begin(), buffer.end(), 0x3b));
            if (buffer.size() < 2) return;
            const std::size_t size = static_cast<std::size_t>(buffer[1]) + 3;
            if (size < 6 || size > 32) {
                buffer.erase(buffer.begin());
                continue;
            }
            if (buffer.size() < size) return;
            AuxFrame request;
            try {
                request = decode_aux_frame(std::span<const std::uint8_t>(buffer.data(), size));
            } catch (const std::invalid_argument&) {
                buffer.erase(buffer.begin());
                continue;
            }
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size));
            if (request.destination != 0x12) continue;
            const auto payload = respond(request);
            if (!payload || muted_.load()) continue;
            const auto bytes = encode_aux_frame(
                AuxFrame{.source = 0x12, .destination = request.source, .command = request.command, .payload = *payload});
            static_cast<void>(
                pty_write_bounded(pty_.master_fd(), reinterpret_cast<const char*>(bytes.data()), bytes.size(), stop_));
        }
    }

    static void put_be(std::vector<std::uint8_t>& out, std::uint32_t value, int bytes) {
        for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
    }

    std::optional<std::vector<std::uint8_t>> respond(const alpacacore::vendor::celestron::AuxFrame& request) {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
        std::vector<std::uint8_t> out;
        switch (request.command) {
            case 0x01:
                put_be(out, raw_, 3);
                return out;
            case 0x02:
                if (request.payload.size() != 3) return std::nullopt;
                target_ = (static_cast<std::uint32_t>(request.payload[0]) << 16) |
                          (static_cast<std::uint32_t>(request.payload[1]) << 8) | request.payload[2];
                last_goto_ = target_;
                return out;
            case 0x13:
                out.push_back(target_ ? 0x00 : 0xff);
                return out;
            case 0x24:
                if (request.payload.size() == 1 && request.payload[0] == 0) target_.reset();
                return out;
            case 0x2c:
                put_be(out, low_, 4);
                put_be(out, high_, 4);
                return out;
            case 0xfe:
                return version_;
            default:
                return std::nullopt;
        }
    }

    mutable std::mutex mutex_;
    std::uint32_t low_ = 1000;
    std::uint32_t high_ = 61000;
    std::uint32_t raw_ = 30000;
    std::optional<std::uint32_t> target_;
    std::optional<std::uint32_t> last_goto_;
    std::vector<std::uint8_t> version_{7, 11, 0x0b, 0xb8};
    std::vector<alpacacore::vendor::celestron::AuxFrame> requests_;
    std::atomic<bool> muted_{false};
    std::atomic<bool> stop_{false};
    PtyPair pty_;
    std::thread worker_;
};

}  // namespace alpacacore::test

#endif  // _WIN32
