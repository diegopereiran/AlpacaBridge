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

#include <alpacacore/util/host_clock.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "catch2_compat.h"

using alpacacore::util::HostClock;
using Outcome = HostClock::Outcome;
using namespace std::chrono;

namespace {

struct Fake {
    bool synchronized = false;
    bool set_ok = true;
    std::optional<system_clock::time_point> rtc_time;  // what the RTC itself reads
    std::vector<system_clock::time_point> sets;

    HostClock clock() {
        return HostClock([this] { return synchronized; },
                         [this](system_clock::time_point tp, std::string& err) {
                             sets.push_back(tp);
                             if (!set_ok) {
                                 err = "EPERM";
                             }
                             return set_ok;
                         },
                         [this] { return rtc_time; });
    }
};

const system_clock::time_point kNow = system_clock::time_point(seconds(1786298276));  // 2026-08

}  // namespace

TEST_CASE("HostClock - undisciplined host takes the client's UTCDate", "[util][hostclock][unit]") {
    Fake f;
    auto c = f.clock();
    CHECK_FALSE(c.synchronized());
    CHECK(c.source() == "none");
    CHECK_FALSE(c.stepped_by_client());

    auto r = c.step_from_client(kNow + minutes(7), kNow);
    REQUIRE(r.outcome == Outcome::Stepped);
    CHECK(r.delta == minutes(7));
    REQUIRE(f.sets.size() == 1);
    CHECK(f.sets[0] == kNow + minutes(7));
    CHECK(c.stepped_by_client());
    CHECK(c.source() == "client");

    // A later, backwards correction is stepped too (the client is the only source).
    r = c.step_from_client(kNow - hours(3), kNow);
    CHECK(r.outcome == Outcome::Stepped);
    CHECK(r.delta == -hours(3));
    CHECK(f.sets.size() == 2);
}

TEST_CASE("HostClock - an NTP-disciplined host is never touched", "[util][hostclock][unit]") {
    Fake f;
    f.synchronized = true;
    auto c = f.clock();
    CHECK(c.synchronized());
    CHECK(c.source() == "ntp");
    auto r = c.step_from_client(kNow + hours(1), kNow);
    CHECK(r.outcome == Outcome::SkippedSynchronized);
    CHECK(r.delta == hours(1));  // the delta is still reported so the caller can log the disagreement
    CHECK(f.sets.empty());
    CHECK_FALSE(c.stepped_by_client());
}

TEST_CASE("HostClock - NTP taking over forgets an earlier client step", "[util][hostclock][unit]") {
    // Field sequence: no NTP, a client steps the clock; later NTP appears
    // (hotspot with internet), then disappears again. The clock is now
    // whatever NTP left plus drift, not the client's value: the state must
    // read "none" again and the connect-time warning must be armed again.
    Fake f;
    auto c = f.clock();
    REQUIRE(c.step_from_client(kNow + minutes(5), kNow).outcome == Outcome::Stepped);
    CHECK(c.source() == "client");
    CHECK(c.stepped_by_client());
    f.synchronized = true;
    CHECK(c.source() == "ntp");
    CHECK_FALSE(c.stepped_by_client());
    f.synchronized = false;
    CHECK(c.source() == "none");
    CHECK_FALSE(c.stepped_by_client());
    // and a fresh client step is accepted again
    CHECK(c.step_from_client(kNow + minutes(5), kNow).outcome == Outcome::Stepped);
    CHECK(c.source() == "client");
}

TEST_CASE("HostClock - opt-out disables the step but keeps the readout", "[util][hostclock][unit]") {
    Fake f;
    auto c = f.clock();
    CHECK(c.enabled());
    c.set_enabled(false);
    CHECK_FALSE(c.enabled());
    auto r = c.step_from_client(kNow + hours(1), kNow);
    CHECK(r.outcome == Outcome::SkippedDisabled);
    CHECK(f.sets.empty());
    CHECK(c.source() == "none");
    c.set_enabled(true);
    CHECK(c.step_from_client(kNow + hours(1), kNow).outcome == Outcome::Stepped);
}

TEST_CASE("HostClock - sanity window and small deltas", "[util][hostclock][unit]") {
    Fake f;
    auto c = f.clock();
    // 1999: below the window (a client with a dead CMOS battery)
    CHECK(c.step_from_client(system_clock::time_point(seconds(915148800)), kNow).outcome == Outcome::SkippedOutOfRange);
    // 2101: above the window
    CHECK(c.step_from_client(system_clock::time_point(seconds(4133980800)), kNow).outcome ==
          Outcome::SkippedOutOfRange);
    // Exactly the bounds are accepted
    CHECK(c.step_from_client(system_clock::time_point(seconds(HostClock::kMinEpoch)), kNow).outcome ==
          Outcome::Stepped);
    CHECK(c.step_from_client(system_clock::time_point(seconds(HostClock::kMaxEpoch)), kNow).outcome ==
          Outcome::Stepped);
    CHECK(f.sets.size() == 2);
    // Sub-second disagreement is HTTP jitter, not clock error
    CHECK(c.step_from_client(kNow + milliseconds(400), kNow).outcome == Outcome::SkippedSmall);
    CHECK(c.step_from_client(kNow - milliseconds(999), kNow).outcome == Outcome::SkippedSmall);
    CHECK(c.step_from_client(kNow + milliseconds(1000), kNow).outcome == Outcome::Stepped);
    CHECK(f.sets.size() == 3);
}

TEST_CASE("HostClock - a refused clock_settime is reported, not thrown", "[util][hostclock][unit]") {
    Fake f;
    f.set_ok = false;
    auto c = f.clock();
    auto r = c.step_from_client(kNow + hours(1), kNow);
    CHECK(r.outcome == Outcome::Failed);
    CHECK(r.error == "EPERM");
    CHECK(r.delta == hours(1));
    CHECK_FALSE(c.stepped_by_client());
    CHECK(c.source() == "none");
    CHECK(f.sets.size() == 1);
}

TEST_CASE("HostClock - a hardware RTC is reported as the source, and stepping is unchanged (#292)",
          "[util][hostclock][unit]") {
    Fake f;
    // Anchored to the library's build day, never to the host's own clock: the
    // suite also runs on NTP-less SBCs whose clock is behind a fresh package.
    const auto rtc_now = HostClock::build_time() + hours(24);
    f.rtc_time = rtc_now;  // an RTC keeping real time
    auto c = f.clock();
    // Booted from the RTC: the kernel still says unsynchronised, but the
    // state is not "none".
    CHECK_FALSE(c.synchronized());
    CHECK(c.has_rtc(rtc_now));
    CHECK(c.source(rtc_now) == "rtc");
    CHECK(c.rtc_state(rtc_now) == HostClock::RtcState::Ok);
    // The RTC's own reading is judged, not the system clock: a battery-less
    // Pi 5 RTC reads 2000-01-01 while userspace may already have restored a
    // recent system time. It is not keeping time: report "none".
    const auto y2000 = system_clock::time_point(seconds(946684800));
    f.rtc_time = y2000;
    CHECK_FALSE(c.has_rtc(y2000));
    CHECK(c.source(y2000) == "none");
    CHECK(c.rtc_state(y2000) == HostClock::RtcState::None);
    CHECK(HostClock::build_time() > y2000);
    f.rtc_time = std::nullopt;  // no RTC was used at boot
    CHECK(c.source(rtc_now) == "none");
    CHECK_FALSE(c.rtc_diverged(rtc_now));  // no RTC is not "diverged"
    f.rtc_time = y2000;
    CHECK_FALSE(c.rtc_diverged(y2000));  // an implausible RTC is not "diverged" either
    // The system clock must still agree with the RTC: a later date -s, a
    // restored saved timestamp, or simply free-running drift between the two
    // oscillators. The window is seconds, not minutes: 1 s of clock error is
    // 15 arcsec of RA.
    f.rtc_time = rtc_now;
    CHECK_FALSE(c.has_rtc(rtc_now + seconds(30)));
    CHECK(c.rtc_diverged(rtc_now + seconds(30)));
    CHECK(c.source(rtc_now + seconds(30)) == "none");
    CHECK_FALSE(c.has_rtc(rtc_now - seconds(30)));
    CHECK(c.rtc_diverged(rtc_now - seconds(30)));
    CHECK_FALSE(c.rtc_diverged(rtc_now));
    CHECK(c.has_rtc(rtc_now + seconds(1)));  // inside the measurement-noise window
    CHECK(c.has_rtc(rtc_now - seconds(1)));
    // Hysteresis: once diverged, coming back needs the tighter window, so a
    // clock parked on the threshold cannot flap between "rtc" and "none".
    CHECK(c.rtc_diverged(rtc_now + seconds(6)));   // latch set
    CHECK_FALSE(c.has_rtc(rtc_now + seconds(4)));  // inside 5 s, still diverged
    CHECK_FALSE(c.has_rtc(rtc_now + seconds(3)));
    CHECK(c.has_rtc(rtc_now + seconds(1)));  // inside 2 s: latch clears
    CHECK(c.has_rtc(rtc_now + seconds(4)));  // and 4 s is agreement again
    // A client that agrees with the RTC changes nothing.
    CHECK(c.step_from_client(kNow + milliseconds(300), kNow).outcome == Outcome::SkippedSmall);
    CHECK(c.source(rtc_now) == "rtc");
    // A client that disagrees by more than a second still wins (it is what
    // corrects RTC drift in the field), and the source becomes "client".
    CHECK(c.step_from_client(kNow + seconds(40), kNow).outcome == Outcome::Stepped);
    CHECK(f.sets.size() == 1);
    CHECK(c.source(rtc_now) == "client");
    // NTP outranks the RTC label.
    f.synchronized = true;
    CHECK(c.source(rtc_now) == "ntp");
    // The two-argument constructor means "no RTC probe": never "rtc".
    HostClock no_probe([] { return false; }, [](system_clock::time_point, std::string&) { return true; });
    CHECK_FALSE(no_probe.has_rtc());
    CHECK(no_probe.source() == "none");
}

TEST_CASE("HostClock - an external clock set (synctime endpoint) is recorded as a client step",
          "[util][hostclock][unit]") {
    Fake f;
    const auto rtc_now = HostClock::build_time() + hours(24);
    f.rtc_time = rtc_now;
    auto c = f.clock();
    CHECK(c.source(rtc_now) == "rtc");
    CHECK_FALSE(c.stepped_by_client());
    c.note_external_step();
    CHECK(c.stepped_by_client());
    CHECK(c.source(rtc_now) == "client");
    CHECK(f.sets.empty());  // nothing was set through this object
    // NTP taking over still clears it.
    f.synchronized = true;
    CHECK(c.source(rtc_now) == "ntp");
    f.synchronized = false;
    CHECK(c.source(rtc_now) == "rtc");
}

TEST_CASE("HostClock - the __DATE__ day parser used by build_time()'s fallback", "[util][hostclock][unit]") {
    // build_time() normally uses the CMake-injected epoch, so this parser is
    // the branch that would otherwise be compiled for the first time in the
    // field. Exercised directly instead.
    const auto day = [](long long d) { return system_clock::time_point(seconds(d * 86400)); };
    CHECK(HostClock::day_from_date_string("Jan  1 1970") == day(0));
    CHECK(HostClock::day_from_date_string("Jan  2 1970") == day(1));
    CHECK(HostClock::day_from_date_string("Dec 31 1969") == day(-1));
    CHECK(HostClock::day_from_date_string("Jan  1 2000") == system_clock::time_point(seconds(946684800)));
    CHECK(HostClock::day_from_date_string("Mar  1 2000") == system_clock::time_point(seconds(951868800)));  // leap year
    CHECK(HostClock::day_from_date_string("Mar  1 1900") ==
          system_clock::time_point(seconds(-2203891200)));  // not a leap year
    CHECK(HostClock::day_from_date_string("Sep 10 2026") == system_clock::time_point(seconds(1788998400)));
    CHECK(HostClock::day_from_date_string("Dec 31 2026") == system_clock::time_point(seconds(1798675200)));
    // Space-padded single-digit day, every month in order.
    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    auto previous = system_clock::time_point::min();
    for (const char* m : months) {
        const std::string d = std::string(m) + "  1 2026";
        const auto tp = HostClock::day_from_date_string(d.c_str());
        CHECK(tp > previous);
        previous = tp;
    }
    // Garbage is inert, never a floor in the future.
    CHECK(HostClock::day_from_date_string(nullptr) == system_clock::time_point{});
    CHECK(HostClock::day_from_date_string("nope") == system_clock::time_point{});
    CHECK(HostClock::day_from_date_string("Zzz 99 0000") == system_clock::time_point{});
    // And the real build floor is a sane day boundary.
    CHECK(HostClock::build_time().time_since_epoch().count() % (86400LL * system_clock::period::den) == 0);
}

TEST_CASE("HostClock - outcome names are stable log text", "[util][hostclock][unit]") {
    CHECK(std::string(HostClock::outcome_name(Outcome::Stepped)) == "stepped");
    CHECK(std::string(HostClock::outcome_name(Outcome::SkippedSynchronized)).find("NTP") != std::string::npos);
    CHECK(std::string(HostClock::outcome_name(Outcome::SkippedDisabled)).find("syncSystemClockFromClients") !=
          std::string::npos);
    CHECK(std::string(HostClock::outcome_name(Outcome::SkippedOutOfRange)).find("2000-2100") != std::string::npos);
    CHECK(std::string(HostClock::outcome_name(Outcome::SkippedSmall)).find("1 s") != std::string::npos);
    CHECK(std::string(HostClock::outcome_name(Outcome::Failed)) == "failed");
}

TEST_CASE("HostClock - default construction queries the real kernel without stepping", "[util][hostclock][unit]") {
    // The real adjtimex() path must be callable unprivileged; whether this
    // host is synchronised depends on the machine, so only the type is checked.
    HostClock real;
    const bool s = real.synchronized();
    CHECK((s == true || s == false));
    CHECK((real.source() == "ntp" || real.source() == "rtc" || real.source() == "none"));
    const auto rtc = HostClock::host_rtc_time();
    // Only the implications: the agreement clause depends on this host's
    // RTC (a local-time RTC is hours off UTC and reads "none", by design).
    if (!rtc.has_value()) {
        CHECK_FALSE(real.has_rtc());
        CHECK(real.rtc_state() == HostClock::RtcState::None);
    }
    if (real.has_rtc()) {
        CHECK(rtc.has_value());
        CHECK(rtc.value() >= HostClock::build_time());
    }
    real.set_enabled(false);  // never call clock_settime from a unit test
    CHECK(real.step_from_client(kNow, kNow).outcome == Outcome::SkippedDisabled);
}
