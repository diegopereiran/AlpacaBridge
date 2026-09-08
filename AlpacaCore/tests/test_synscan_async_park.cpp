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
//
// Park is an asynchronous initiator (issue #208): it must return well inside
// the ConformU 4.5 STANDARD 1 s target while the park slew runs in the
// background, Slewing stays true until the mount arrives, and AtPark flips
// true in the same step Slewing drops. A FakeMountServer plays a SynScan
// handset whose GOTO takes ~1.5 s, so the whole lifecycle runs hardware-free.
#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"

namespace {

using Clock = std::chrono::steady_clock;

struct FakeSynScanState {
    std::atomic<bool> goto_seen{false};
    std::atomic<int> goto_count{0};
    std::atomic<Clock::rep> goto_started{0};
    static constexpr auto kGotoDuration = std::chrono::milliseconds(1500);
    bool goto_in_progress() const {
        if (!goto_seen.load()) return false;
        const auto started = Clock::time_point(Clock::duration(goto_started.load()));
        return Clock::now() - started < kGotoDuration;
    }
};

alpacacore::test::FakeMountServer::Responder synscan_responder(std::shared_ptr<FakeSynScanState> st) {
    return [st](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return "12AB0500,20000500#";  // parseable 16/24-bit position pair
            case 'r':
            case 'R':
            case 'b':
            case 'B':
                st->goto_started.store(Clock::now().time_since_epoch().count());
                st->goto_seen.store(true);
                st->goto_count.fetch_add(1);
                return "#";
            case 'L':
                return st->goto_in_progress() ? "1#" : "0#";
            case 'M':  // cancel goto
                st->goto_seen.store(false);
                return "#";
            case 'T':
            case 'P':  // tracking mode write / passthrough
                return "#";
            default:
                return "0#";
        }
    };
}

alpacacore::vendor::synscan::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 200;
    return info;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

}  // namespace

TEST_CASE("SynScan - get_connected() answers at once while a connect is in flight", "[synscan][telescope][async]") {
    // set_connected(true) holds the driver mutex across every handshake round
    // trip, and the router polls get_connected() throughout (the PUT connected
    // wait, every GET connected). With a mutex-taking getter those calls
    // blocked for the whole connect and the router's deadline never fired
    // (issue #130). Stall the firmware query so the mutex is held for a while
    // and prove the getter still returns immediately.
    static constexpr int kStallMs = 400;  // static: odr-used inside the lambda below
    alpacacore::test::FakeMountServer server([](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        switch (chunk[0]) {
            case 'K':  // protocol echo: "K" + byte -> byte + "#"
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'V':
                std::this_thread::sleep_for(std::chrono::milliseconds(kStallMs));
                return "042A00#";
            case 'e':
            case 'E':
            case 'z':
            case 'Z':
                return "12AB0500,20000500#";
            default:
                return "0#";
        }
    });
    REQUIRE(server.ok());
    auto info = endpoint(server.port());
    info.response_timeout_ms = 1000;  // longer than the stall: the firmware query must succeed, not time out
    auto driver =
        alpacacore::vendor::synscan::create_synscan_telescope(0, info, alpacacore::vendor::synscan::SynScanVersion::V4);

    driver->connect();
    REQUIRE(wait_until([&] { return driver->get_connecting(); }, 1000));
    // Past the port open and the echo, inside the stalled firmware query:
    // the connect task holds mutex_ for the next few hundred milliseconds.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // The flag's value mid-task is driver-specific (SynScan raises it before
    // the warm-up queries, see AGENTS.md on why get_connected() is not a
    // completion signal); the contract under test is that the read returns
    // at once while Connecting is still true.
    const auto t0 = Clock::now();
    static_cast<void>(driver->get_connected());
    const auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(read_ms < 100);
    CHECK(driver->get_connecting());

    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    driver->set_connected(false);
}

TEST_CASE("SynScan - a silent handset fails the connect instead of reporting a phantom link",
          "[synscan][telescope][async]") {
    // connect() only opens the port. Before the echo gate a link with nothing
    // listening came up as Connected=true once every handshake query had
    // burnt its full response timeout (all swallowed), and every command then
    // timed out too. Now the echo is the first thing on the wire and its
    // silence fails the connect within a single timeout.
    auto queries = std::make_shared<std::atomic<int>>(0);
    alpacacore::test::FakeMountServer server([queries](const std::string&) -> std::string {
        queries->fetch_add(1);
        return "";  // nothing is ever sent back
    });
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);  // 200 ms response timeout

    const auto t0 = Clock::now();
    driver->connect();
    REQUIRE(wait_until([&] { return !driver->get_connecting(); }, 5000));
    const auto connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK_FALSE(driver->get_connected());
    CHECK(connect_ms < 1000);     // one echo timeout, not five swallowed query timeouts
    CHECK(queries->load() == 1);  // only the echo went out
}

TEST_CASE("SynScan async - Park returns immediately, AtPark flips when the slew ends", "[synscan][telescope][async]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    REQUIRE_FALSE(driver->get_at_park());

    const auto t0 = Clock::now();
    driver->park();
    const auto park_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK(park_ms < 1000);           // ConformU 4.5 STANDARD target for an async initiator
    REQUIRE(driver->get_slewing());  // parking reports Slewing until AtPark
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return st->goto_seen.load(); }, 5000));  // the GOTO was dispatched
    // Park while a park is in flight is a no-op: the running slew is neither
    // cancelled nor restarted (still exactly one GOTO on the wire).
    driver->park();
    REQUIRE(driver->get_slewing());
    REQUIRE_FALSE(driver->get_at_park());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(st->goto_count.load() == 1);

    REQUIRE(wait_until([&] { return driver->get_at_park(); }, 20000));
    REQUIRE_FALSE(driver->get_slewing());
    REQUIRE_FALSE(driver->get_tracking());  // park stops tracking

    driver->park();  // second Park on a parked mount is harmless
    REQUIRE(driver->get_at_park());
    REQUIRE_FALSE(driver->get_slewing());

    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    driver->set_connected(false);
}

TEST_CASE("SynScan async - Unpark during a park cancels it", "[synscan][telescope][async]") {
    auto st = std::make_shared<FakeSynScanState>();
    alpacacore::test::FakeMountServer server(synscan_responder(st));
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::synscan::create_synscan_telescope(
        0, endpoint(server.port()), alpacacore::vendor::synscan::SynScanVersion::V4);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    driver->park();
    REQUIRE(driver->get_slewing());
    driver->unpark();
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 5000));
    // The cancelled park task must never flip AtPark afterwards.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    REQUIRE_FALSE(driver->get_at_park());

    // A park in flight gates motion members like a completed park does, so a
    // slew or axis jog cannot silently clobber it (ParkedException, 0x408).
    driver->park();
    REQUIRE(driver->get_slewing());
    CHECK_THROWS_AS(driver->move_axis(0, 0.5), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->slew_to_coordinates_async(5.0, 20.0), alpacacore::AlpacaException);
    CHECK_THROWS_AS(driver->sync_to_coordinates(5.0, 20.0), alpacacore::AlpacaException);
    try {
        driver->move_axis(0, 0.5);
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == alpacacore::AlpacaError::InvalidWhileParked);
    }
    REQUIRE(driver->get_slewing());  // the park is still in flight
    driver->abort_slew();            // ...but AbortSlew may cancel it
    REQUIRE_FALSE(driver->get_at_park());
    REQUIRE(wait_until([&] { return !driver->get_slewing(); }, 5000));

    // Disconnect with a park in flight joins the task cleanly.
    driver->park();
    driver->set_connected(false);
    REQUIRE_FALSE(driver->get_connected());
}

#endif  // !_WIN32
