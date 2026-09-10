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

#include <atomic>
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
    // "rtc" needs has_rtc(), i.e. agreement as well as provenance: a clock
    // that has drifted away from its own RTC reports "none", not "rtc".
    std::string source(std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const {
        if (synchronized()) {
            return "ntp";
        }
        // The RTC read may be an I2C transaction: never hold mutex_ across it
        // (step_from_client() would block behind a slow bus). Re-check
        // stepped_ afterwards so a step landing meanwhile still wins.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stepped_) {
                return "client";
            }
        }
        const bool rtc = rtc_state(now) == RtcState::Ok;
        std::lock_guard<std::mutex> lock(mutex_);
        if (stepped_) {
            return "client";
        }
        return rtc ? "rtc" : "none";
    }

    // True when all three hold: the kernel loaded system time from a hardware
    // RTC at boot, the RTC's OWN current reading is plausible (not before this
    // library was built), and the system clock still agrees with that reading
    // (see kRtcAgreement -- this one goes false on a healthy RTC after enough
    // free-running uptime, which is the point). The RTC is read, not the
    // system clock: userspace (fake-hwclock, timesyncd) moves the system clock
    // after boot, so a battery-less Raspberry Pi 5 RTC that handed the kernel
    // 2000-01-01 would otherwise pass once the saved timestamp was restored.
    // Reporting only: the stepping decision never looks at this.
    bool has_rtc(std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const {
        return rtc_state(now) == RtcState::Ok;
    }

    // How much the system clock may differ from the RTC and still be called
    // its offspring. since_epoch is 1 s granular and the memo is up to 1 s
    // stale, so ~2 s of the measurement is noise; 5 s is that noise floor plus
    // a small margin and no more, because this module's own relevance
    // threshold is 1 s (1 s of clock error is 15 arcsec of RA on every goto).
    // At a typical ~20 ppm of relative drift a free-running host crosses it
    // after roughly two days and then honestly reports "none".
    static constexpr std::chrono::seconds kRtcAgreement{5};
    // Coming back from Diverged needs the skew inside a tighter window, so a
    // clock sitting on the threshold cannot flap between "rtc" and "none" on
    // consecutive polls. Strictly above the +-1 s that survives the bias
    // correction below, so a clock that genuinely agrees can always leave.
    static constexpr std::chrono::seconds kRtcReagreement{3};
    // The reading lags true RTC time one-sidedly: since_epoch truncates to the
    // RTC's whole second (0-1 s) and the memo is up to 1 s stale (0-1 s). So
    // now - reading carries a +0..2 s bias even for a clock in perfect
    // agreement; half of it is subtracted before comparing, leaving +-1 s of
    // residual uncertainty and windows that are symmetric in TRUE error.
    static constexpr std::chrono::seconds kRtcReadLag{2};

    // The RTC is present and plausible but the system clock no longer agrees
    // with it: after a bypassing setter, or simply after months of NTP-less
    // uptime (SoC timebases drift tens of ppm, seconds per day). Lets the
    // connect-time message say what actually happened.
    bool rtc_diverged(std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const {
        return rtc_state(now) == RtcState::Diverged;
    }

    enum class RtcState : std::uint8_t {
        None,      // no RTC was used at boot, or its reading is implausible
        Ok,        // the system clock still traces to a plausible RTC
        Diverged,  // plausible RTC, but the system clock has walked away
    };

    // One RTC observation answering both questions, so a caller that needs
    // both cannot straddle the memo boundary and see neither. This is the
    // only place the hysteresis latch is updated; has_rtc() and
    // rtc_diverged() are thin wrappers that each perform their own
    // evaluation, so a caller that needs both should ask once, here, as the
    // router does.
    RtcState rtc_state(std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const {
        const auto t = rtc_time_();
        if (!rtc_plausible(t)) {
            // No plausible reading to judge: drop the latch, so the next one
            // is judged under the full window rather than a stale decision.
            rtc_diverged_.store(false, std::memory_order_relaxed);
            return RtcState::None;
        }
        const auto window = rtc_diverged_.load(std::memory_order_relaxed) ? kRtcReagreement : kRtcAgreement;
        const bool agrees = rtc_agrees(t, now, window);
        rtc_diverged_.store(!agrees, std::memory_order_relaxed);
        return agrees ? RtcState::Ok : RtcState::Diverged;
    }

    // The clock was stepped by a path that bypasses step_from_client() (the
    // /management/v1/synctime endpoint behind the web UI's Sync Time button):
    // from now on the system clock's provenance is "client", not the RTC and
    // not "none".
    void note_external_step() {
        std::lock_guard<std::mutex> lock(mutex_);
        stepped_ = true;
    }

    // The RTC the kernel set system time from at boot (its hctosys attribute
    // reads 1; /sys/class/rtc is enumerated once to find it), read
    // through its since_epoch attribute and memoised (one second for a
    // reading, thirty for "no RTC here") so an I2C transaction never runs on
    // every HTTP request or device connect.
    // nullopt when no RTC was used at boot, when the attribute is unreadable,
    // or when CONFIG_RTC_HCTOSYS is off. Defined in host_clock.cpp.
    static std::optional<std::chrono::system_clock::time_point> host_rtc_time();

    // Build-day floor of the library: the configure-time epoch injected by
    // CMake (ALPACACORE_BUILD_EPOCH, floored to 00:00 UTC of that day; honours
    // SOURCE_DATE_EPOCH, so a reproducible package build carries its
    // changelog date instead), falling back to __DATE__'s day when absent.
    // Defined once in host_clock.cpp. Any clock earlier than this is wrong.
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
    // The RTC reads a time at or after this library's build day (an RTC that
    // lost its battery reads 2000-01-01).
    static bool rtc_plausible(const std::optional<std::chrono::system_clock::time_point>& t) {
        // Strictly after: a host whose own clock was reset to 2000-01-01 by a
        // dead RTC could otherwise produce a __DATE__ floor that its RTC's
        // 2000-01-01 reading exactly meets.
        return t.has_value() && *t > build_time();
    }
    // The label describes the SYSTEM clock's provenance, so the two must
    // still agree within kRtcAgreement: a later setter that bypassed this
    // object (manual date -s, a restored saved timestamp) or free-running
    // drift between the two oscillators leaves them apart, and the honest
    // answer is then "none".
    static bool rtc_agrees(const std::optional<std::chrono::system_clock::time_point>& t,
                           std::chrono::system_clock::time_point now, std::chrono::seconds window) {
        if (!t.has_value()) {
            return false;
        }
        const auto skew = (now - t.value()) - kRtcReadLag / 2;
        return skew < window && skew > -window;
    }
    // Uncached sysfs read behind host_rtc_time(); mutates a function-local
    // static and is only safe under host_rtc_time()'s cache mutex.
    static std::optional<std::chrono::system_clock::time_point> read_host_rtc_time();

public:
    // Parse a __DATE__-shaped string ("Mmm dd yyyy") to 00:00 UTC of that day.
    // Always compiled and unit-tested, so build_time()'s fallback branch is
    // never first exercised in the field; pure arithmetic, no timegm(). An
    // unrecognised month (no aligned match in the month table) reads as
    // January and a day outside 1-31 as the 1st,
    // so a malformed string can only move the floor earlier within its year;
    // a missing string or an unparseable year yields the epoch.
    static std::chrono::system_clock::time_point day_from_date_string(const char* date);

private:
    IsSynchronizedFn is_synchronized_;
    SetTimeFn set_time_;
    RtcTimeFn rtc_time_;
    mutable std::mutex mutex_;
    bool enabled_ = true;
    mutable bool stepped_ = false;
    // Hysteresis latch for the agreement window. Independent of mutex_: it is
    // touched from rtc_state(), which deliberately runs unlocked.
    mutable std::atomic<bool> rtc_diverged_{false};
};

}  // namespace alpacacore::util
