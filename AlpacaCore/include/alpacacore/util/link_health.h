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

#include <chrono>
#include <cstring>
#include <optional>
#include <string>

namespace alpacacore::util {

/**
 * @brief Link-health latch for drivers that serve reads from a cache a
 *        background reader fills from a streamed status frame.
 *
 * Issue #237: when the serial link dies (USB re-enumeration, unplugged
 * cable, port stolen by another process) a cache-backed driver otherwise
 * keeps serving the last good frame forever, indistinguishable from live
 * telemetry, while only writes surface the fault as EIO. This helper ties
 * cache validity to the one thing a streaming device guarantees: frames
 * keep arriving.
 *
 * Usage (caller holds whatever mutex guards its cached status):
 *   - reset(now) at connect (the silence clock starts at connect, so a
 *     device that has not sent yet is not faulted on the spot);
 *   - on_frame(now) each time a frame commits; returns true when that frame
 *     cleared a latched fault (log "restored");
 *   - note_read_error(errno) on a persistent read() failure so the fault
 *     reason names it;
 *   - check_silence(now, limit) from the reader loop; returns the reason on
 *     the call that latches the fault (log it, invalidate the cache);
 *   - fault() from the driver's read/write paths: non-empty means refuse to
 *     serve the cache (DriverException "communications compromised").
 *
 * The fault clears on its own when a frame arrives again; Connected stays
 * true throughout so the client decides whether to reconnect.
 */
class StreamLinkHealth {
public:
    using clock = std::chrono::steady_clock;

    void reset(clock::time_point now) {
        last_frame_ = now;
        fault_.clear();
        read_error_.clear();
    }

    bool on_frame(clock::time_point now) {
        last_frame_ = now;
        read_error_.clear();
        const bool restored = !fault_.empty();
        fault_.clear();
        return restored;
    }

    void note_read_error(int err) { read_error_ = std::strerror(err); }

    std::optional<std::string> check_silence(clock::time_point now, std::chrono::milliseconds limit) {
        if (!fault_.empty() || (now - last_frame_) <= limit) {
            return std::nullopt;
        }
        const auto silent_s = std::chrono::duration_cast<std::chrono::seconds>(now - last_frame_).count();
        fault_ = "no status frame for " + std::to_string(silent_s) + " s";
        if (!read_error_.empty()) {
            fault_ += " (last read error: " + read_error_ + ")";
        }
        return fault_;
    }

    bool faulted() const { return !fault_.empty(); }
    const std::string& fault() const { return fault_; }

private:
    clock::time_point last_frame_{};
    std::string fault_;       // non-empty while latched
    std::string read_error_;  // strerror of the last persistent read failure, if any
};

}  // namespace alpacacore::util
