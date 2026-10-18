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

// Celestron Focus Motor on its own USB port (issue #896), against a PTY fake of
// AUX device 0x12. The driver runs on a FakeTaskClock, so a poll tick is an
// advance(), not a sleep.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/celestron/celestron_focuser_driver.h>
#include <alpacacore/version.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "catch2_compat.h"

#ifndef _WIN32
#include "fake_celestron_focuser.h"
#include "fake_task_clock.h"
#endif

using alpacacore::AlpacaError;
using alpacacore::vendor::celestron::create_celestron_focuser;
using namespace std::chrono_literals;

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

}  // namespace

TEST_CASE("Celestron Focuser Driver - Defaults", "[celestron][focuser][unit]") {
    auto driver = create_celestron_focuser(0, "/dev/ttyUSB0");

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    REQUIRE(driver->get_connecting() == false);
    CHECK(driver->get_name() == "Celestron Focus Motor");
    CHECK(driver->get_absolute() == true);
}

TEST_CASE("Celestron Focuser Driver - Device metadata", "[celestron][focuser][unit]") {
    auto driver = create_celestron_focuser(3, "/dev/ttyUSB0");

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Celestron Focus Motor (USB)");
    CHECK(driver->get_driver_info() == "AlpacaCore Celestron Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "CELESTRON_FOCUSER_3");
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("Celestron Focuser Driver - Disconnected behavior", "[celestron][focuser][unit]") {
    auto driver = create_celestron_focuser(0, "/dev/ttyUSB0");

    require_alpaca_error([&] { (void)driver->get_is_moving(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_position(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_max_step(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_max_increment(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_step_size(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_temp_comp_available(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_temp_comp(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->set_temp_comp(false); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_temperature(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->halt(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->move(100); }, AlpacaError::NotConnected);
    // Platform 7: a disconnected driver reports an empty DeviceState.
    CHECK(driver->get_device_state().empty());
}

TEST_CASE("Celestron Focuser Driver - Unsupported actions", "[celestron][focuser][unit]") {
    auto driver = create_celestron_focuser(0, "/dev/ttyUSB0");

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&] { driver->action("noop", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { driver->command_blind("noop", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->command_bool("noop", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&] { driver->command_string("noop", false); }, AlpacaError::MethodNotImplemented);
}

TEST_CASE("Celestron Focuser Driver - Connect to a missing port fails", "[celestron][focuser][unit]") {
    auto driver = create_celestron_focuser(0, "/dev/alpacabridge-no-such-celestron-focuser");
    CHECK_THROWS_AS(driver->set_connected(true), alpacacore::AlpacaException);
    CHECK(driver->get_connected() == false);
}

#ifndef _WIN32

using alpacacore::test::FakeCelestronFocuser;
using alpacacore::test::FakeTaskClock;

namespace {

constexpr auto kTick = 300ms;  // longer than the 250 ms poll period
constexpr auto kBound = 5000ms;

void tick(FakeTaskClock& clock) {
    clock.advance(kTick);
    REQUIRE(clock.wait_for_woken_settled(kBound));
    REQUIRE(clock.wait_for_waiters(1, kBound));
}

}  // namespace

TEST_CASE("Celestron Focuser Driver - Position and MaxStep are referenced to the high limit",
          "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(1000, 61000);
    fake.set_raw_position(30000);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    CHECK(driver->get_max_step() == 60000);
    CHECK(driver->get_max_increment() == 60000);
    CHECK(driver->get_position() == 31000);  // 61000 - 30000
    CHECK(driver->get_is_moving() == false);

    fake.set_raw_position(61000);
    tick(clock);
    CHECK(driver->get_position() == 0);
    fake.set_raw_position(1000);
    tick(clock);
    CHECK(driver->get_position() == 60000);

    // Every request goes from the application address to the focus motor.
    for (const auto& frame : fake.requests()) {
        CHECK(frame.source == 0x20);
        CHECK(frame.destination == 0x12);
    }
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - Move sends the raw target and IsMoving follows the motor",
          "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(1000, 61000);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    driver->move(5000);
    REQUIRE(fake.last_goto().has_value());
    CHECK(*fake.last_goto() == 56000U);  // 61000 - 5000
    // Write-through: true before the next poll.
    CHECK(driver->get_is_moving() == true);
    tick(clock);
    CHECK(driver->get_is_moving() == true);

    fake.complete_move();
    tick(clock);
    CHECK(driver->get_is_moving() == false);
    CHECK(driver->get_position() == 5000);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - Move clamps to the calibrated range", "[celestron][focuser][unit][fake]") {
    // ConformU expects graceful clamping for Move outside 0..MaxStep, as the
    // other focuser drivers do, rather than InvalidValue.
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(1000, 61000);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    driver->move(-50);
    CHECK(*fake.last_goto() == 61000U);
    driver->move(999999);
    CHECK(*fake.last_goto() == 1000U);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - Halt stops the motor", "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    driver->move(100);
    REQUIRE(fake.moving());
    driver->halt();
    CHECK(fake.count(0x24) == 1);
    const auto requests = fake.requests();
    REQUIRE(requests.back().command == 0x24);
    CHECK(requests.back().payload == std::vector<std::uint8_t>{0x00});
    CHECK_FALSE(fake.moving());
    CHECK(driver->get_is_moving() == false);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - an uncalibrated motor refuses the connect", "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_limits(5000, 5000);
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);

    try {
        driver->set_connected(true);
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::DriverException);
        CHECK(std::string(ex.what()).find("not calibrated") != std::string::npos);
    }
    CHECK(driver->get_connected() == false);
    CHECK(fake.count(0x02) == 0);

    // The port was released: once calibrated, the same driver connects.
    fake.set_limits(1000, 61000);
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - capabilities, firmware and DeviceState when connected",
          "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    fake.set_version({7, 11, 0x0b, 0xb8});
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    CHECK(driver->get_absolute() == true);
    CHECK(driver->get_temp_comp_available() == false);
    CHECK(driver->get_temp_comp() == false);
    CHECK(driver->get_device_firmware() == std::optional<std::string>("7.11.3000"));

    bool has_position = false;
    bool has_moving = false;
    bool has_temperature = false;
    for (const auto& entry : driver->get_device_state()) {
        has_position = has_position || entry.name == "Position";
        has_moving = has_moving || entry.name == "IsMoving";
        has_temperature = has_temperature || entry.name == "Temperature";
    }
    CHECK(has_position);
    CHECK(has_moving);
    CHECK_FALSE(has_temperature);  // no sensor: omitted, not invented

    driver->set_connected(false);
    CHECK_FALSE(driver->get_device_firmware().has_value());
}

TEST_CASE("Celestron Focuser Driver - unsupported members report not implemented", "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    require_alpaca_error([&] { (void)driver->get_step_size(); }, AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)driver->get_temperature(); }, AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { driver->set_temp_comp(true); }, AlpacaError::NotImplemented);
    driver->set_connected(false);
}

TEST_CASE("Celestron Focuser Driver - a silent motor faults the link and a reply restores it",
          "[celestron][focuser][unit][fake]") {
    FakeCelestronFocuser fake;
    REQUIRE(fake.ok());
    FakeTaskClock clock;
    auto driver = create_celestron_focuser(0, fake.slave_path(), clock);
    driver->set_connected(true);
    REQUIRE(clock.wait_for_waiters(1, kBound));

    fake.set_muted(true);
    for (int i = 0; i < 3; ++i) tick(clock);
    require_alpaca_error([&] { (void)driver->get_position(); }, AlpacaError::DriverException);
    require_alpaca_error([&] { driver->move(10); }, AlpacaError::DriverException);
    CHECK(driver->get_connected() == true);

    // The poll resynchronizes the timed-out stream before asking again.
    fake.set_muted(false);
    tick(clock);
    CHECK(driver->get_position() == 31000);
    driver->set_connected(false);
}

#endif  // _WIN32
