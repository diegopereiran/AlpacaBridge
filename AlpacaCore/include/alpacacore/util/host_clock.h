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

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
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
    using RtcTimeFn = std::function<std::optional<std::chrono::system_clock::time_point>()>;

    // Same window the /management/v1/synctime endpoint enforces.
    static constexpr std::int64_t kMinEpoch = 946684800;   // 2000-01-01T00:00:00Z
    static constexpr std::int64_t kMaxEpoch = 4102444800;  // 2100-01-01T00:00:00Z
    // A client's UTCDate carries the HTTP round trip and its own scheduling
    // jitter; stepping for sub-second deltas would only inject that noise.
    static constexpr std::chrono::milliseconds kMinStep{1000};

    HostClock()
        : HostClock(&HostClock::kernel_is_synchronized, &HostClock::kernel_set_time, &HostClock::host_rtc_time) {}
    HostClock(
        IsSynchronizedFn is_synchronized, SetTimeFn set_time, RtcTimeFn rtc_time = [] { return std::nullopt; })
        : is_synchronized_(std::move(is_synchronized)),
          set_time_(std::move(set_time)),
          rtc_time_(std::move(rtc_time)) {}

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
    // UTCDate write), "rtc" (undisciplined, never stepped, but the kernel
    // loaded system time from a hardware RTC at boot: that is a plain clock
    // set, so STA_UNSYNC stays set — open-astro#292), or "none".
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

    // True when the kernel loaded system time from a hardware RTC at boot AND
    // the RTC's OWN current reading is plausible (not before this library was
    // built). The RTC is read, not the system clock: userspace (fake-hwclock,
    // timesyncd) moves the system clock after boot, so a battery-less
    // Raspberry Pi 5 RTC that handed the kernel 2000-01-01 would otherwise
    // pass once the saved timestamp was restored. Reporting only: the
    // stepping decision never looks at this.
    bool has_rtc() const {
        const auto t = rtc_time_();
        return t.has_value() && *t >= build_time();
    }

    // The RTC the kernel set system time from at boot (its hctosys attribute
    // reads 1; searched across /sys/class/rtc/rtc*), read through its
    // since_epoch attribute. nullopt when no RTC was used at boot, when the
    // attribute is unreadable, or when CONFIG_RTC_HCTOSYS is off. Defined in
    // host_clock.cpp with build_time().
    static std::optional<std::chrono::system_clock::time_point> host_rtc_time();

    // Build month of the library (defined once in host_clock.cpp so __DATE__
    // is captured in exactly one translation unit); any clock earlier than
    // this is certainly wrong.
    static std::chrono::system_clock::time_point build_time();

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
            return r;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stepped_ = true;
        }
        r.outcome = Outcome::Stepped;
        return r;
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
    RtcTimeFn rtc_time_;
    mutable std::mutex mutex_;
    bool enabled_ = true;
    mutable bool stepped_ = false;
};

}  // namespace alpacacore::util
