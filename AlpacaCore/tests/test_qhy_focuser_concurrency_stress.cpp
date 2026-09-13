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

// [stress] registration for the QHY Q-Focuser. The FakeQhyQFocuser pty
// gives the storm a real connect path: every iteration runs the JSON
// handshake and the settings push through the real protocol wrapper.

#include <alpacacore/focuser_driver.h>
#include <alpacacore/vendor/qhy/qhy_focuser_driver.h>

#include <chrono>
#include <memory>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_qhy_qfocuser.h"

using alpacacore::AlpacaDriver;
using alpacacore::test::FakeQhyQFocuser;

namespace {

void focuser_operate(alpacacore::test::StressCallGuard& guard, AlpacaDriver& d) {
    auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
    guard([&] { static_cast<void>(focuser.get_is_moving()); });
    guard([&] { static_cast<void>(focuser.get_position()); });
    guard([&] { static_cast<void>(focuser.get_max_step()); });
    guard([&] { static_cast<void>(focuser.get_temperature()); });
    guard([&] { static_cast<void>(focuser.get_temp_comp_available()); });
    guard([&] { focuser.move(1234); });
    guard([&] { focuser.halt(); });
    guard([&] { static_cast<void>(focuser.get_device_state()); });
}

}  // namespace

TEST_CASE("QHY Q-Focuser - concurrent connect/disconnect/operate stress", "[qhy][focuser][stress]") {
    FakeQhyQFocuser fake;
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path());

    // Prove the fake connects before storming it, so the case cannot
    // silently degrade into a fail-fast run that never reaches the wire.
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(15)));
    CHECK_NOTHROW(driver->set_connected(false));

    alpacacore::test::StressCallGuard guard;
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) { focuser_operate(guard, d); });

    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("QHY Q-Focuser - destruction races an in-flight connect", "[qhy][focuser][stress]") {
    FakeQhyQFocuser fake;
    const std::string port = fake.slave_path();
    // 25 iterations, not the default 100: every iteration reaches a real
    // handshake whose first attempt sleeps 100 ms before its read.
    alpacacore::test::run_destruction_during_connect_stress(
        [&port]() { return alpacacore::vendor::qhy::create_qhy_focuser(0, port); }, 25);
}

TEST_CASE("QHY Q-Focuser - racing disconnect is never dropped", "[qhy][focuser][stress]") {
    FakeQhyQFocuser fake;
    auto driver = alpacacore::vendor::qhy::create_qhy_focuser(0, fake.slave_path());
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}
