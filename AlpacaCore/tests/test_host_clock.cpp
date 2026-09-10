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
    bool rtc = false;  // the kernel loaded the clock from a plausible RTC at boot
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
                         [this] { return rtc; });
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
    f.rtc = true;
    auto c = f.clock();
    // Booted from the RTC: the kernel still reports the clock undisciplined
    // (loading an RTC is a plain clock set), but the state is not "none".
    CHECK_FALSE(c.synchronized());
    CHECK(c.has_rtc());
    CHECK(c.source() == "rtc");
    // The label is about provenance only: it never changes the stepping rule,
    // so a client more than a second off still corrects the clock, and the
    // source then becomes "client" -- that IS where the time came from.
    CHECK(c.step_from_client(kNow + milliseconds(300), kNow).outcome == Outcome::SkippedSmall);
    CHECK(c.source() == "rtc");
    CHECK(c.step_from_client(kNow + seconds(40), kNow).outcome == Outcome::Stepped);
    CHECK(f.sets.size() == 1);
    CHECK(c.source() == "client");
    // NTP outranks both.
    f.synchronized = true;
    CHECK(c.source() == "ntp");
    // No RTC, and NTP having taken over already forgot the client step (that
    // rule comes from the base branch), so this reads "none".
    f.synchronized = false;
    f.rtc = false;
    CHECK_FALSE(c.has_rtc());
    CHECK(c.source() == "none");
    Fake g;
    CHECK(g.clock().source() == "none");
    // The two-argument constructor means "no RTC probe": never "rtc".
    HostClock no_probe([] { return false; }, [](system_clock::time_point, std::string&) { return true; });
    CHECK_FALSE(no_probe.has_rtc());
    CHECK(no_probe.source() == "none");
}

TEST_CASE("HostClock - an external clock set (synctime endpoint) is recorded as a client step",
          "[util][hostclock][unit]") {
    Fake f;
    f.rtc = true;
    auto c = f.clock();
    CHECK(c.source() == "rtc");
    CHECK_FALSE(c.stepped_by_client());
    c.note_external_step();
    CHECK(c.stepped_by_client());
    CHECK(c.source() == "client");
    CHECK(f.sets.empty());  // nothing was set through this object
    // NTP taking over still clears it, and the RTC label returns underneath.
    f.synchronized = true;
    CHECK(c.source() == "ntp");
    f.synchronized = false;
    CHECK(c.source() == "rtc");
}

TEST_CASE("HostClock - the real kernel RTC probe is callable and self-consistent", "[util][hostclock][unit]") {
    HostClock real;
    const bool probe = HostClock::host_booted_from_rtc();
    CHECK(real.has_rtc() == probe);
    CHECK(HostClock::host_booted_from_rtc() == probe);  // stable across calls
    if (!real.synchronized() && !real.stepped_by_client()) {
        CHECK(real.source() == (probe ? "rtc" : "none"));
    }
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
    CHECK((real.source() == "ntp" || real.source() == "none"));
    real.set_enabled(false);  // never call clock_settime from a unit test
    CHECK(real.step_from_client(kNow, kNow).outcome == Outcome::SkippedDisabled);
}
