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

#include <sys/timex.h>
#include <time.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace alpacacore::util {

/**
 * Host clock discipline from Alpaca clients (open-astro#289).
 *
 * An SBC at a dark site has no NTP and (Raspberry Pi) no RTC, so its clock
 * starts wrong after every power cycle. Drivers that compute LST from the
 * host clock (every motor-controller driver whose mount stores no time)
 * then land every goto off in RA by 15 arcsec per second of clock error,
 * while still reporting the commanded target. Every Alpaca client already
 * writes Telescope.UTCDate on connect; this helper turns that write into a
 * system-clock step when, and only when, the kernel reports the clock as
 * undisciplined, so NTP/chrony/GPS always win when present.
 *
 * The decision is a pure function of (enabled, kernel-synchronised flag,
 * sanity window) so it is unit-testable; the two syscalls are injected.
 */
class HostClock {
public:
    enum class Outcome : std::uint8_t {
        Stepped,              // clock_settime succeeded
        SkippedSynchronized,  // the kernel says the clock is disciplined: leave it alone
        SkippedDisabled,      // syncSystemClockFromClients is off
        SkippedOutOfRange,    // outside the 2000-2100 sanity window
        SkippedSmall,         // |delta| below the step threshold: nothing to fix
        Failed,               // clock_settime refused (no CAP_SYS_TIME)
    };

    struct Result {
        Outcome outcome;
        std::chrono::milliseconds delta{0};  // requested - host, before the step
        std::string error;                   // errno text on Failed
    };

    using IsSynchronizedFn = std::function<bool()>;
    using SetTimeFn = std::function<bool(std::chrono::system_clock::time_point, std::string& error)>;
    using HasRtcFn = std::function<bool()>;

    // Same window the /management/v1/synctime endpoint enforces.
    static constexpr std::int64_t kMinEpoch = 946684800;   // 2000-01-01T00:00:00Z
    static constexpr std::int64_t kMaxEpoch = 4102444800;  // 2100-01-01T00:00:00Z
    // A client's UTCDate carries the HTTP round trip and its own scheduling
    // jitter; stepping for sub-second deltas would only inject that noise.
    static constexpr std::chrono::milliseconds kMinStep{1000};

    HostClock()
        : HostClock(&HostClock::kernel_is_synchronized, &HostClock::kernel_set_time, &HostClock::host_booted_from_rtc) {
    }
    HostClock(
        IsSynchronizedFn is_synchronized, SetTimeFn set_time, HasRtcFn has_rtc = [] { return false; })
        : is_synchronized_(std::move(is_synchronized)), set_time_(std::move(set_time)), has_rtc_(std::move(has_rtc)) {}

    void set_enabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = enabled;
    }
    bool enabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return enabled_;
    }

    // True when the kernel's STA_UNSYNC flag is clear (NTP/chrony/PTP have
    // disciplined the clock). Always false on an internet-less SBC. Once a
    // daemon has disciplined the clock, a client's earlier step no longer
    // describes the current value: forget it, so that if discipline is lost
    // again later the state honestly reads "none" and the connect-time
    // warning fires again.
    bool synchronized() const {
        const bool s = is_synchronized_();
        if (s) {
            std::lock_guard<std::mutex> lock(mutex_);
            stepped_ = false;
        }
        return s;
    }

    // "ntp" (kernel-disciplined), "client" (this process stepped it from a
    // UTCDate write), "rtc" (undisciplined and never stepped, but the kernel
    // loaded the clock from a hardware RTC at boot -- open-astro#292), or
    // "none". The RTC read is done outside mutex_: on an I2C RTC it is a bus
    // transaction, and step_from_client() must not queue behind it.
    std::string source() const {
        if (synchronized()) {
            return "ntp";
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stepped_) {
                return "client";
            }
        }
        return has_rtc() ? "rtc" : "none";
    }

    // True when the kernel loaded system time from a hardware RTC at boot and
    // that RTC is not obviously dead. This is a statement about where the
    // clock CAME FROM, not about how accurate it is: nothing on an NTP-less
    // host verifies or rewrites the RTC, so it may still be wrong or drifting.
    // Reporting only -- the stepping decision never looks at it.
    bool has_rtc() const { return has_rtc_(); }

    // Kernel truth, evaluated once per process (positively; a miss is re-probed
    // in case the RTC registers late): the /sys/class/rtc device whose hctosys
    // attribute reads 1 is the one the kernel set the clock from at boot, and
    // its since_epoch reading must be after kMinPlausibleEpoch. A battery-less
    // Raspberry Pi 5 RTC hands the kernel 2000-01-01 and must not count.
    static bool host_booted_from_rtc();

    // A sanity floor, not an accuracy check: comfortably after the dead-RTC
    // defaults (1970 and 2000-01-01) and before any real deployment.
    static constexpr std::int64_t kMinPlausibleEpoch = 1577836800;  // 2020-01-01T00:00:00Z

    /**
     * A step was attempted and refused by the kernel (no CAP_SYS_TIME).
     * Latched, because it means no client will ever correct this clock, which
     * a caller cannot infer from enabled() alone.
     */
    bool step_ever_failed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return step_failed_;
    }

    bool stepped_by_client() const {
        if (synchronized()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return stepped_;
    }

    /**
     * A client wrote Telescope.UTCDate = `requested` at host time `now`.
     * Step the system clock to it if the policy allows. Never throws.
     */
    Result step_from_client(std::chrono::system_clock::time_point requested,
                            std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) {
        Result r{};
        r.delta = std::chrono::duration_cast<std::chrono::milliseconds>(requested - now);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!enabled_) {
                r.outcome = Outcome::SkippedDisabled;
                return r;
            }
        }
        const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(requested.time_since_epoch()).count();
        if (epoch < kMinEpoch || epoch > kMaxEpoch) {
            r.outcome = Outcome::SkippedOutOfRange;
            return r;
        }
        if (is_synchronized_()) {
            r.outcome = Outcome::SkippedSynchronized;
            return r;
        }
        if (r.delta < kMinStep && r.delta > -kMinStep) {
            r.outcome = Outcome::SkippedSmall;
            return r;
        }
        std::string error;
        if (!set_time_(requested, error)) {
            r.outcome = Outcome::Failed;
            r.error = error;
            std::lock_guard<std::mutex> lock(mutex_);
            step_failed_ = true;
            return r;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stepped_ = true;
        }
        r.outcome = Outcome::Stepped;
        return r;
    }

    /**
     * Something other than a UTCDate write set the system clock through
     * this process (the web UI's Sync Time button). clock_settime does not
     * clear STA_UNSYNC, so without this the readout would keep saying the
     * clock was never set and the connect-time warning would keep firing.
     */
    void mark_stepped() {
        std::lock_guard<std::mutex> lock(mutex_);
        stepped_ = true;
    }

    static const char* outcome_name(Outcome o) {
        switch (o) {
            case Outcome::Stepped:
                return "stepped";
            case Outcome::SkippedSynchronized:
                return "skipped: host clock is NTP-disciplined";
            case Outcome::SkippedDisabled:
                return "skipped: syncSystemClockFromClients is off";
            case Outcome::SkippedOutOfRange:
                return "skipped: outside the 2000-2100 sanity window";
            case Outcome::SkippedSmall:
                return "skipped: delta below 1 s";
            case Outcome::Failed:
                return "failed";
        }
        return "unknown";
    }

    // Kernel truth: adjtimex() reports STA_UNSYNC until an NTP/PTP daemon has
    // disciplined the clock. On a Pi without internet this stays set forever.
    static bool kernel_is_synchronized() {
        struct timex tx {};
        tx.modes = 0;  // read-only query
        const int state = adjtimex(&tx);
        if (state < 0) {
            return false;  // cannot tell: treat as undisciplined so a client can fix it
        }
        return (tx.status & STA_UNSYNC) == 0;
    }

    static bool kernel_set_time(std::chrono::system_clock::time_point tp, std::string& error) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
        struct timespec ts {};
        ts.tv_sec = static_cast<time_t>(ns / 1000000000LL);
        ts.tv_nsec = static_cast<long>(ns % 1000000000LL);
        if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
            error = std::string("clock_settime failed (requires CAP_SYS_TIME): errno ") + std::to_string(errno);
            return false;
        }
        return true;
    }

private:
    IsSynchronizedFn is_synchronized_;
    SetTimeFn set_time_;
    HasRtcFn has_rtc_;
    mutable std::mutex mutex_;
    bool enabled_ = true;
    mutable bool stepped_ = false;
    bool step_failed_ = false;
};

}  // namespace alpacacore::util
