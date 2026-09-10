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

// Connect/disconnect/operate concurrency stress for the three Gemini drivers
// (issue #101).
//
// Two of them get the full-seam treatment, because pty-backed fakes already
// exist and drive them into the CONNECTED state, where the reader thread and
// the background command paths actually run:
//   - PDH Advanced 3 Switch over fake_gemini_pdh.h
//   - Flat Panel Pro CoverCalibrator over fake_gemini_flatpanel.h
// That is the surface worth storming here: both drivers own a serial reader
// thread that a disconnect has to reap, and the PDH's status cache is
// written by that thread while the Alpaca getters read it.
//
// The focuser has no fake, so it takes the accepted fail-fast bar (the
// ZWO/iOptron precedent): a serial path that cannot exist, so every connect
// fails fast and the storm still covers the AsyncConnectable machinery and
// the connect-failure cleanup.

#include <alpacacore/covercalibrator_driver.h>
#include <alpacacore/focuser_driver.h>
#include <alpacacore/switch_driver.h>
#include <alpacacore/vendor/gemini/gemini_flatpanel_driver.h>
#include <alpacacore/vendor/gemini/gemini_focuser_driver.h>
#include <alpacacore/vendor/gemini/gemini_pdh_switch_driver.h>

#include <chrono>
#include <memory>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_gemini_flatpanel.h"
#include "fake_gemini_pdh.h"

using alpacacore::AlpacaDriver;
using alpacacore::test::FakeGeminiFlatPanel;
using alpacacore::test::FakeGeminiPdh;

namespace {

// A racing disconnect makes any of these throw NotConnected, and the harness
// only swallows the callback as a whole -- per-call guards keep one throw
// from skipping every call below it for that whole iteration. std::exception
// rather than AlpacaException: anything else escaping the serial teardown
// would otherwise unwind past the remaining calls, which is the exact
// failure mode this helper exists to prevent.
template <typename Fn>
void call(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception&) {
    }
}

void pdh_switch_operate(AlpacaDriver& d) {
    auto& sw = static_cast<alpacacore::SwitchDriver&>(d);
    call([&] { static_cast<void>(sw.get_max_switch()); });
    call([&] { static_cast<void>(sw.get_switch_value(0)); });
    call([&] { static_cast<void>(sw.get_switch_name(0)); });
    call([&] { static_cast<void>(sw.get_can_write(1)); });
    // ids 0 and 1 are USB A and USB B, both writable; the always-on
    // pass-through rail that refuses writes is id 6 ("DC1").
    call([&] { sw.set_switch_value(1, 1.0); });
    call([&] { static_cast<void>(sw.get_device_state()); });
}

void flatpanel_operate(AlpacaDriver& d) {
    auto& panel = static_cast<alpacacore::CoverCalibratorDriver&>(d);
    call([&] { static_cast<void>(panel.get_calibrator_state()); });
    call([&] { static_cast<void>(panel.get_cover_state()); });
    call([&] { static_cast<void>(panel.get_brightness()); });
    call([&] { static_cast<void>(panel.get_max_brightness()); });
    call([&] { static_cast<void>(panel.get_calibrator_changing()); });
    call([&] { static_cast<void>(panel.get_cover_moving()); });
    call([&] { panel.calibrator_on(10); });
    call([&] { panel.calibrator_off(); });
    call([&] { static_cast<void>(panel.get_device_state()); });
}

void focuser_operate(AlpacaDriver& d) {
    auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
    call([&] { static_cast<void>(focuser.get_is_moving()); });
    call([&] { static_cast<void>(focuser.get_position()); });
    call([&] { static_cast<void>(focuser.get_max_step()); });
    call([&] { static_cast<void>(focuser.get_max_increment()); });
    call([&] { static_cast<void>(focuser.get_temperature()); });
    call([&] { focuser.move(1234); });
    call([&] { focuser.halt(); });
}

// Never a real device path: the storm connects hundreds of times, so a
// plausible port (/dev/ttyUSB0) could open whatever is actually plugged in
// and take the MyFocuserPro2 handshake to it.
constexpr const char* kAbsentSerialPort = "/dev/gemini-alpacabridge-absent";

}  // namespace

TEST_CASE("Gemini PDH Advanced 3 switch - concurrent connect/disconnect/operate stress", "[gemini][switch][stress]") {
    FakeGeminiPdh hub;
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, hub.slave_path(), 19200);

    // Prove the fake actually connects before storming it -- otherwise this
    // silently degrades into a fail-fast test that never reaches the reader
    // thread (the PR #3 lesson recorded in the SynScan stress file).
    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    driver->set_connected(false);

    alpacacore::test::run_lifecycle_stress(*driver, pdh_switch_operate);

    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);
}

TEST_CASE("Gemini PDH Advanced 3 switch - destruction races an in-flight connect", "[gemini][switch][stress]") {
    FakeGeminiPdh hub;
    const std::string port = hub.slave_path();
    alpacacore::test::run_destruction_during_connect_stress(
        [&port]() { return alpacacore::vendor::gemini::create_gemini_pdh_switch(0, port, 19200); }, 25);
}

TEST_CASE("Gemini PDH Advanced 3 switch - racing disconnect is never dropped", "[gemini][switch][stress]") {
    FakeGeminiPdh hub;
    auto driver = alpacacore::vendor::gemini::create_gemini_pdh_switch(0, hub.slave_path(), 19200);
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}

TEST_CASE("Gemini Flat Panel Pro - concurrent connect/disconnect/operate stress", "[gemini][covercalibrator][stress]") {
    FakeGeminiFlatPanel panel;
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, panel.slave_path(), 9600);

    REQUIRE(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
    driver->set_connected(false);

    alpacacore::test::run_lifecycle_stress(*driver, flatpanel_operate);

    REQUIRE(alpacacore::test::settle_connected(*driver, false, std::chrono::seconds(10)));
    CHECK(driver->get_connected() == false);
}

TEST_CASE("Gemini Flat Panel Pro - destruction races an in-flight connect", "[gemini][covercalibrator][stress]") {
    FakeGeminiFlatPanel panel;
    const std::string port = panel.slave_path();
    alpacacore::test::run_destruction_during_connect_stress(
        [&port]() { return alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, port, 9600); }, 25);
}

TEST_CASE("Gemini Flat Panel Pro - racing disconnect is never dropped", "[gemini][covercalibrator][stress]") {
    FakeGeminiFlatPanel panel;
    auto driver = alpacacore::vendor::gemini::create_gemini_flatpanel_pro(0, panel.slave_path(), 9600);
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}

TEST_CASE("Gemini focuser - concurrent connect/disconnect/operate stress", "[gemini][focuser][stress]") {
    auto driver = alpacacore::vendor::gemini::create_gemini_focuser(0, kAbsentSerialPort);

    alpacacore::test::run_lifecycle_stress(*driver, focuser_operate);

    // The port cannot exist, so no connect in the storm can have succeeded.
    CHECK(driver->get_connected() == false);
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("Gemini focuser - destruction races an in-flight connect", "[gemini][focuser][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::gemini::create_gemini_focuser(0, kAbsentSerialPort); });
}
