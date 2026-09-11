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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <variant>
#include <vector>

#include "catch2_compat.h"
#include "fake_gemini_focuser.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

} // namespace

TEST_CASE("Gemini Focuser Driver - Defaults", "[gemini][focuser][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "Gemini Automatic Astro Focuser Pro");
}

TEST_CASE("Gemini Focuser Driver - Metadata", "[gemini][focuser][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");

    CHECK(driver->get_description() == "Gemini Automatic Astro Focuser Pro Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore Gemini Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "GEMINI_FOCUSER_0");
}

TEST_CASE("Gemini Focuser Driver - Disconnected Behavior", "[gemini][focuser][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(1, "/dev/ttyUSB0");

    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_absolute() == true);
    REQUIRE(driver->get_temp_comp_available() == true);
    REQUIRE(driver->get_temp_comp() == false);
    REQUIRE(driver->get_supported_actions().empty());

    // Platform 7 DeviceState: while disconnected the operational getters throw
    // and are omitted, leaving just the TimeStamp; the old non-compliant
    // "Connected" entry is gone.
    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") {
            has_timestamp = true;
        }
    }
    REQUIRE(has_timestamp);

    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_step(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_max_increment(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(0); }, alpacacore::AlpacaError::NotConnected);

    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->action("noop", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("noop", false); }, alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("Gemini Focuser Driver - Connecting State", "[gemini][focuser][unit]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_connecting() == false);
    REQUIRE(driver->get_connected() == false);
}

TEST_CASE("Gemini Focuser Driver - Device Number Assignment", "[gemini][focuser][unit]") {
    auto driver0 = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");
    auto driver1 = alpacacore::vendor::gemini::create_gemini_focuser(1, "/dev/ttyUSB1");
    auto driver5 = alpacacore::vendor::gemini::create_gemini_focuser(5, "/dev/ttyUSB2");

    REQUIRE(driver0->get_device_number() == 0);
    REQUIRE(driver1->get_device_number() == 1);
    REQUIRE(driver5->get_device_number() == 5);
}

TEST_CASE("Gemini Focuser Driver - Unique IDs", "[gemini][focuser][unit]") {
    auto driver0 = alpacacore::vendor::gemini::create_gemini_focuser(0, "/dev/ttyUSB0");
    auto driver1 = alpacacore::vendor::gemini::create_gemini_focuser(1, "/dev/ttyUSB1");

    REQUIRE(driver0->get_unique_id() != driver1->get_unique_id());
    CHECK(driver0->get_unique_id() == "GEMINI_FOCUSER_0");
    CHECK(driver1->get_unique_id() == "GEMINI_FOCUSER_1");
}

#ifndef _WIN32

TEST_CASE("Gemini Focuser Driver - concurrent set_connected(true) is one transition (#333)",
          "[gemini][focuser][concurrency]") {
    // The focuser was the only Gemini driver without a transition mutex, so
    // set_connected() was a check-then-act on a plain atomic. Two HTTP workers
    // could both observe connected_ == false and both reach
    // protocol_.connect(), which assigns serial_fd_ with no prior close: the
    // first descriptor leaks for the life of the process, and on a real
    // MyFocuserPro2 the second open() re-asserts DTR and resets the MCU
    // mid-session.
    //
    // The window is the handshake ladder, up to ~9.1 s on hardware. The fake
    // holds its handshake reply so the race is reproducible rather than
    // timing-dependent.
    alpacacore::test::FakeGeminiFocuser fake;
    fake.set_handshake_delay(std::chrono::milliseconds(300));
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path());

    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 2; ++i) {
        workers.emplace_back([&] {
            try {
                driver->set_connected(true);
            } catch (const std::exception&) {
                ++failures;
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    // Neither caller may fail: both asked for a state the driver is already
    // reaching. Without the transition mutex the loser reaches the wrapper,
    // whose already-connected guard throws.
    CHECK(failures.load() == 0);
    CHECK(driver->get_connected());
    // Exactly one connect reached the wire. Without either fix this is 2, and
    // the first descriptor is the one that leaks.
    CHECK(fake.connects() == 1);

    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Gemini Focuser Driver - a connect racing a disconnect settles once (#333)",
          "[gemini][focuser][concurrency]") {
    // The same unguarded serial_fd_ a second connect overwrites is the one a
    // concurrent disconnect closes, so the two must not interleave either.
    alpacacore::test::FakeGeminiFocuser fake;
    fake.set_handshake_delay(std::chrono::milliseconds(200));
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path());

    std::atomic<int> failures{0};
    auto flip = [&](bool target) {
        try {
            driver->set_connected(target);
        } catch (const std::exception&) {
            ++failures;
        }
    };

    for (int round = 0; round < 5; ++round) {
        std::thread up(flip, true);
        std::thread down(flip, false);
        up.join();
        down.join();
        CHECK(failures.load() == 0);
        // Whichever won, the driver is in a definite state and the link
        // agrees with it: a getter must not throw NotConnected while
        // get_connected() reports true.
        if (driver->get_connected()) {
            CHECK_NOTHROW(driver->get_position());
        }
        driver->set_connected(false);
    }
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Gemini Focuser Driver - connect/disconnect cycles reuse the port cleanly (#333)",
          "[gemini][focuser][concurrency]") {
    // The already-connected guard must not break the ordinary sequential
    // reconnect the web UI does every time a device is re-enabled.
    alpacacore::test::FakeGeminiFocuser fake;
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, fake.slave_path());

    for (int i = 0; i < 3; ++i) {
        driver->set_connected(true);
        REQUIRE(driver->get_connected());
        CHECK_NOTHROW(driver->get_position());
        driver->set_connected(false);
        REQUIRE_FALSE(driver->get_connected());
    }
    // One handshake per connect, no more.
    CHECK(fake.connects() == 3);
}

#endif  // _WIN32
