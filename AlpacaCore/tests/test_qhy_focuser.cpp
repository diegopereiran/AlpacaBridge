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
#include <alpacacore/vendor/qhy/qhy_focuser_driver.h>
#include <alpacacore/vendor/qhy/qhy_qfocuser_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <chrono>
#include <functional>
#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_qhy_qfocuser.h"

namespace {

void require_alpaca_error(const std::function<void()>& fn, int expected_code) {
    try {
        fn();
        FAIL("Expected AlpacaException");
    } catch (const alpacacore::AlpacaException& ex) {
        REQUIRE(ex.error_code() == expected_code);
    }
}

// Never a real device path: a plausible /dev/ttyACM0 could be the actual focuser.
constexpr const char* kAbsentPort = "/dev/qhy-qfocuser-absent";

}  // namespace

TEST_CASE("QHY Q-Focuser Driver - Defaults", "[qhy][focuser][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, kAbsentPort);

    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Focuser);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "QHY Q-Focuser");
    CHECK(driver->get_absolute() == true);
    CHECK(driver->get_max_step() == 64000);
    CHECK(driver->get_max_increment() == 64000);
    CHECK(driver->get_device_firmware().has_value() == false);
}

TEST_CASE("QHY Q-Focuser Driver - Device metadata", "[qhy][focuser][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(3, kAbsentPort);

    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "QHY Q-Focuser Driver");
    CHECK(driver->get_driver_info() == "AlpacaCore QHY Q-Focuser Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 4);
    CHECK(driver->get_unique_id() == "QHY_QFOCUSER_3");
    auto other = alpacacore::vendor::qhy::create_qhy_focuser(0, kAbsentPort);
    CHECK(other->get_unique_id() != driver->get_unique_id());
}

TEST_CASE("QHY Q-Focuser Driver - Not connected throws", "[qhy][focuser][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, kAbsentPort);

    require_alpaca_error([&]() { driver->get_is_moving(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_position(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_temperature(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_temp_comp_available(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_temp_comp(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->set_temp_comp(false); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->halt(); }, alpacacore::AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->move(1000); }, alpacacore::AlpacaError::NotConnected);
}

TEST_CASE("QHY Q-Focuser Driver - Unsupported actions", "[qhy][focuser][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, kAbsentPort);

    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&]() { driver->action("test", ""); }, alpacacore::AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, alpacacore::AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); },
                         alpacacore::AlpacaError::MethodNotImplemented);
}

TEST_CASE("QHY Q-Focuser Driver - Reply parser", "[qhy][focuser][unit]") {
    using alpacacore::vendor::qhy::parse_qfocuser_reply;

    auto f = parse_qfocuser_reply("{\"idx\":1,\"id\":\"\\u001f@SL3KG\\u0018TH2C\",\"version\":20231207,\"bv\":208}");
    REQUIRE(f.size() == 4);
    CHECK(f["idx"] == "1");
    CHECK(f["version"] == "20231207");
    CHECK(f["bv"] == "208");
    CHECK(f["id"] == ".@SL3KG.TH2C");

    auto t = parse_qfocuser_reply("{\"idx\":4,\"temp\":120683,\"c_t\":18727,\"c_r\":125,\"o_t\":-5250,\"sg\":0}");
    CHECK(t["o_t"] == "-5250");
    CHECK(t["c_r"] == "125");

    CHECK(parse_qfocuser_reply("{\"idx\":-1}")["idx"] == "-1");
    CHECK(parse_qfocuser_reply("garbage").empty());
    CHECK(parse_qfocuser_reply("{\"idx\":5,\"pos\":").empty());
    CHECK(parse_qfocuser_reply("{}").empty());
}

TEST_CASE("QHY Q-Focuser Driver - Value range validation", "[qhy][focuser][unit]") {
    alpacacore::test::FakeQhyQFocuser fake;
    alpacacore::vendor::qhy::QFocuserSettings settings;
    settings.max_step = 5000;
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path(), settings);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    CHECK(driver->get_max_step() == 5000);
    require_alpaca_error([&]() { driver->move(-1); }, alpacacore::AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->move(5001); }, alpacacore::AlpacaError::InvalidValue);
    CHECK(fake.count("\"cmd_id\":6") == 0);  // rejected before reaching the wire
    CHECK_NOTHROW(driver->move(5000));
    CHECK(fake.count("\"cmd_id\":6,\"tar\":5000}") == 1);

    driver->set_connected(false);
}

TEST_CASE("QHY Q-Focuser Driver - State machine", "[qhy][focuser][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, kAbsentPort);

    REQUIRE(driver->get_connecting() == false);
    REQUIRE(driver->get_connected() == false);

    const auto state = driver->get_device_state();
    bool has_timestamp = false;
    for (const auto& entry : state) {
        REQUIRE(entry.name != "Connected");
        if (entry.name == "TimeStamp") has_timestamp = true;
    }
    REQUIRE(has_timestamp);
}

TEST_CASE("QHY Q-Focuser Driver - Connect applies settings and reports motion", "[qhy][focuser][unit]") {
    alpacacore::test::FakeQhyQFocuser fake;
    fake.set_position(1000);
    fake.set_steps_per_poll(400);

    alpacacore::vendor::qhy::QFocuserSettings settings;
    settings.reverse = true;
    settings.speed = 3;
    settings.hold_force = true;
    settings.hold_ihold = 6;
    settings.hold_irun = 10;
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path(), settings);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    CHECK(fake.connects() == 1);
    CHECK(fake.count("\"cmd_id\":7,\"rev\":1}") == 1);
    CHECK(fake.count("\"cmd_id\":13,\"speed\":3}") == 1);
    CHECK(fake.count("\"cmd_id\":16,\"ihold\":6,\"irun\":10}") == 1);
    CHECK(fake.count("\"cmd_id\":12,\"force\":1}") == 1);
    CHECK(driver->get_device_firmware().value_or("") == "20231207 (board 208)");

    CHECK(driver->get_position() == 1000);
    CHECK(driver->get_is_moving() == false);
    CHECK(driver->get_temperature() == Catch::Approx(23.881));
    CHECK(driver->get_temp_comp_available() == false);
    require_alpaca_error([&]() { driver->get_step_size(); }, alpacacore::AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&]() { driver->set_temp_comp(true); }, alpacacore::AlpacaError::NotImplemented);

    driver->move(2000);
    CHECK(driver->get_is_moving() == true);  // first poll: 1400 of 2000
    CHECK(driver->get_position() == 1400);   // served from the same 100 ms sample
    for (int i = 0; i < 20 && driver->get_is_moving(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    CHECK(driver->get_is_moving() == false);
    CHECK(driver->get_position() == 2000);

    // Halt: the fake stops mid-move and the driver drops its target.
    fake.set_steps_per_poll(1);
    driver->move(3000);
    CHECK(driver->get_is_moving() == true);
    driver->halt();
    CHECK(fake.count("\"cmd_id\":3}") == 1);
    CHECK(driver->get_is_moving() == false);

    // Platform 7 DeviceState carries the operational trio while connected.
    bool has_moving = false, has_position = false, has_temperature = false;
    for (const auto& entry : driver->get_device_state()) {
        if (entry.name == "IsMoving") has_moving = true;
        if (entry.name == "Position") has_position = true;
        if (entry.name == "Temperature") has_temperature = true;
    }
    CHECK(has_moving);
    CHECK(has_position);
    CHECK(has_temperature);

    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
    CHECK(driver->get_device_firmware().has_value() == false);
}

TEST_CASE("QHY Q-Focuser Driver - Hold settings skipped on USB power", "[qhy][focuser][unit]") {
    alpacacore::test::FakeQhyQFocuser fake;
    fake.set_voltage_tenths(50);  // 5.0 V: USB-powered
    alpacacore::vendor::qhy::QFocuserSettings settings;
    settings.hold_force = true;
    settings.temperature_source = "chip";
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path(), settings);
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));

    CHECK(fake.count("\"cmd_id\":16") == 0);
    CHECK(fake.count("\"cmd_id\":12") == 0);
    CHECK(driver->get_temperature() == Catch::Approx(18.727));

    driver->set_connected(false);
}

TEST_CASE("QHY Q-Focuser Driver - Absent port fails connect with NotConnected", "[qhy][focuser][unit]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, kAbsentPort);
    require_alpaca_error([&]() { driver->set_connected(true); }, alpacacore::AlpacaError::NotConnected);
    CHECK(driver->get_connected() == false);
}

// Regression guard for the one-reply-behind USB behaviour of the real GD32
// firmware (PR #526 review). With the fake in one-behind mode, a reply is held
// until the next OUT packet, so the driver's connect and reads only succeed
// because transact_locked() drains the command out as its own packet and sends
// a newline kick. Deleting the tcdrain or the kicks in
// qhy_qfocuser_protocol_wrapper.cpp makes this case hang and fail, which the
// instant-answer default fake could not catch.
TEST_CASE("QHY Q-Focuser Driver - one-behind firmware needs the tcdrain+kick", "[qhy][focuser][unit]") {
    alpacacore::test::FakeQhyQFocuser fake;
    fake.set_one_behind(true);
    fake.set_position(1500);
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path());

    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(10)));
    CHECK(fake.connects() >= 1);
    // Reads must reach the awaited reply despite every reply arriving one OUT
    // packet late; the kick is what clocks each one out.
    CHECK(driver->get_position() == 1500);
    CHECK(driver->get_is_moving() == false);
    CHECK(driver->get_temperature() == Catch::Approx(23.881));

    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}
