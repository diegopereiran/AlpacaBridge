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

// SynScan getters and DeviceState served from the polled snapshot (decision 0010, SynScan adoption; link-loss
// policy of decision 0009; issue #1247). The driver runs on a FakeTaskClock, so its status poll runs only when a
// case advances the clock, and the fake handset answers on the wall clock after an injected latency per
// transaction: a getter that asked the handset would pay it and show in the transaction count.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"
#include "fake_task_clock.h"

namespace {

namespace sc = alpacacore::vendor::synscan;
using alpacacore::test::FakeTaskClock;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr auto kLatency = milliseconds(100);
constexpr auto kGetterBudget = milliseconds(20);

struct Handset {
    std::atomic<int> transactions{0};
    std::atomic<int> latency_ms{0};
    std::atomic<bool> mute{false};
    std::atomic<bool> slewing{false};
    std::atomic<int> tracking_mode{1};
};

std::string reply(Handset& st, const std::string& chunk) {
    st.transactions.fetch_add(1);
    if (chunk.empty()) return "0#";
    if (st.mute.load() && chunk[0] != 'K' && chunk[0] != 'V') return "";
    if (st.latency_ms.load() > 0) std::this_thread::sleep_for(milliseconds(st.latency_ms.load()));
    switch (chunk[0]) {
        case 'K':
            return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
        case 'V':
            return "042A00#";
        case 'e':
            return "5CECE200,D001B900#";
        case 'z':
            return "8A4FDA00,0D527400#";
        case 'L':
            return st.slewing.load() ? "1#" : "0#";
        case 'm':  // mount model: an EQ mount
            return std::string(1, '\x32') + "#";
        case 'p':
            return "E#";
        case 't':
            return std::string(1, static_cast<char>(st.tracking_mode.load())) + "#";
        case 'T':
            if (chunk.size() > 1) st.tracking_mode.store(chunk[1]);
            return "#";
        default:
            return "#";
    }
}

std::unique_ptr<alpacacore::TelescopeDriver> connected_driver(alpacacore::test::FakeMountServer& server,
                                                              FakeTaskClock& clock) {
    sc::ConnectionInfo info;
    info.type = sc::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = server.port();
    info.response_timeout_ms = 300;
    auto driver = sc::create_synscan_telescope_with_site(0, info, sc::SynScanVersion::V4, -33.856784, 151.215297, 42.0,
                                                         false, sc::SynScanAlignmentSetting::Auto, clock);
    driver->set_connected(true);
    return driver;
}

template <typename Fn>
std::chrono::microseconds timed(Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
}

bool has_entry(const alpacacore::TelescopeDriver& driver, const std::string& name) {
    for (const auto& entry : driver.get_device_state()) {
        if (entry.name == name) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("SynScan snapshot - getters and DeviceState make no handset transaction at 100 ms per transaction",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    REQUIRE(clock.wait_for_waiters(1, milliseconds(2000)));  // the poll is parked: the connect's reads are over

    st->latency_ms = static_cast<int>(kLatency.count());
    const int before = st->transactions.load();

    const std::vector<std::pair<const char*, std::function<void()>>> reads = {
        {"RightAscension", [&] { (void)driver->get_right_ascension(); }},
        {"Declination", [&] { (void)driver->get_declination(); }},
        {"Azimuth", [&] { (void)driver->get_azimuth(); }},
        {"Altitude", [&] { (void)driver->get_altitude(); }},
        {"Slewing", [&] { (void)driver->get_slewing(); }},
        {"Tracking", [&] { (void)driver->get_tracking(); }},
        {"SideOfPier", [&] { (void)driver->get_side_of_pier(); }},
        {"UTCDate", [&] { (void)driver->get_utc_date(); }},
        {"SiderealTime", [&] { (void)driver->get_sidereal_time(); }},
        {"AtPark", [&] { (void)driver->get_at_park(); }},
        {"AtHome", [&] { (void)driver->get_at_home(); }},
        {"IsPulseGuiding", [&] { (void)driver->get_is_pulse_guiding(); }},
        {"TrackingRate", [&] { (void)driver->get_tracking_rate(); }},
        {"DeviceState", [&] { (void)driver->get_device_state(); }},
    };
    for (const auto& [name, read] : reads) {
        CAPTURE(name);
        CHECK(timed(read) < kGetterBudget);
    }
    CHECK(st->transactions.load() == before);
    // DeviceState drops a getter that throws, so a read that quietly failed would still be fast: name what must be in it.
    for (const char* name : {"RightAscension", "Declination", "Slewing", "Tracking", "TimeStamp"}) {
        CAPTURE(name);
        CHECK(has_entry(*driver, name));
    }
    driver->set_connected(false);
}

TEST_CASE("SynScan snapshot - a setter's value is read back at once and a stale poll does not undo it",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    REQUIRE(driver->get_tracking());

    driver->set_tracking(false);
    CHECK_FALSE(driver->get_tracking());  // read-after-write, with no poll between

    // A poll that sampled before a write publishes after it: the write wins.
    driver->set_tracking(true);
    sc::set_status_poll_paused_for_testing(*driver, true);
    std::atomic<bool> once{false};
    sc::set_status_poll_after_sample_hook_for_testing(*driver, [&] {
        if (!once.exchange(true)) driver->set_tracking(false);
    });
    sc::poll_status_now_for_testing(*driver);  // samples tracking on, then the hook writes off
    CHECK_FALSE(driver->get_tracking());
    sc::set_status_poll_after_sample_hook_for_testing(*driver, nullptr);
    sc::poll_status_now_for_testing(*driver);  // the next poll reads the handset, which holds off
    CHECK_FALSE(driver->get_tracking());
    driver->set_connected(false);
}

TEST_CASE("SynScan snapshot - a sync, a slew start and a clock write are read back at once",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);

    // The fake handset keeps answering 08h42m RA / -67 deg Dec. The pulse-guide position override holds the synced
    // value for 10 s of task clock; past it the frame is what answers, so it must carry the synced position too.
    const auto past_override = seconds(11);
    driver->sync_to_coordinates(3.5, 12.25);
    clock.advance(past_override);
    CHECK(std::abs(driver->get_right_ascension() - 3.5) < 1e-9);
    CHECK(std::abs(driver->get_declination() - 12.25) < 1e-9);

    // A poll that sampled before the sync publishes after it: the sync wins.
    std::atomic<bool> once{false};
    sc::set_status_poll_after_sample_hook_for_testing(*driver, [&] {
        if (!once.exchange(true)) driver->sync_to_coordinates(6.0, -20.0);
    });
    sc::poll_status_now_for_testing(*driver);
    clock.advance(past_override);
    CHECK(std::abs(driver->get_right_ascension() - 6.0) < 1e-9);
    CHECK(std::abs(driver->get_declination() - (-20.0)) < 1e-9);
    sc::set_status_poll_after_sample_hook_for_testing(*driver, nullptr);

    // The next poll reads the handset (Dec only: the precise RA read is corrected against the last known RA).
    sc::poll_status_now_for_testing(*driver);
    CHECK(std::abs(driver->get_declination() - (-67.490537)) < 1e-5);

    // A clock write is read back, not re-read from the handset.
    const auto when = std::chrono::system_clock::now() + seconds(3600);
    driver->set_utc_date(when);
    CHECK(std::abs(std::chrono::duration<double>(driver->get_utc_date() - when).count()) < 5.0);

    // A slew start flips Slewing at once, though the handset still answers "not slewing".
    CHECK_FALSE(driver->get_slewing());
    driver->slew_to_coordinates_async(2.0, 30.0);
    CHECK(driver->get_slewing());
    driver->abort_slew();
    CHECK_FALSE(driver->get_slewing());

    // A stop is read back at once: the frame still says the handset was slewing at the last poll.
    st->slewing = true;
    sc::poll_status_now_for_testing(*driver);
    CHECK(driver->get_slewing());
    driver->abort_slew();
    CHECK_FALSE(driver->get_slewing());
    driver->set_connected(false);
}

TEST_CASE("SynScan snapshot - past the staleness bound DeviceState is empty with no TimeStamp",
          "[synscan][telescope][snapshot]") {
    FakeTaskClock clock;
    auto st = std::make_shared<Handset>();
    alpacacore::test::FakeMountServer server([st](const std::string& c) { return reply(*st, c); });
    REQUIRE(server.ok());
    auto driver = connected_driver(server, clock);
    REQUIRE(driver->get_connected());
    sc::set_status_poll_paused_for_testing(*driver, true);
    CHECK(has_entry(*driver, "TimeStamp"));

    st->mute = true;
    for (int i = 0; i < 3; ++i) sc::poll_status_now_for_testing(*driver);
    CHECK_FALSE(driver->get_link_fault().empty());
    CHECK(driver->get_connected());  // a young fault keeps Connected

    clock.advance(alpacacore::util::kLinkStalenessBound + seconds(1));
    sc::poll_status_now_for_testing(*driver);
    CHECK_FALSE(driver->get_connected());
    CHECK(driver->get_device_state().empty());
    CHECK_FALSE(has_entry(*driver, "TimeStamp"));
    try {
        (void)driver->get_right_ascension();
        FAIL("a lost link must refuse the read");
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
    }
    st->mute = false;

    // Decision 0009 point 4: Connected=true against a lost link relinks, on both entry points; it is not the
    // idempotent no-op.
    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(std::abs(driver->get_right_ascension() - 8.711749) < 1e-5);

    st->mute = true;
    for (int i = 0; i < 3; ++i) sc::poll_status_now_for_testing(*driver);
    clock.advance(alpacacore::util::kLinkStalenessBound + seconds(1));
    sc::poll_status_now_for_testing(*driver);
    REQUIRE_FALSE(driver->get_connected());
    st->mute = false;
    driver->connect();  // the async route
    CHECK(alpacacore::test::settle_connected(*driver, true, seconds(10)));
    CHECK(driver->get_connected());
    CHECK(std::abs(driver->get_right_ascension() - 8.711749) < 1e-5);
    driver->set_connected(false);
}

#endif
