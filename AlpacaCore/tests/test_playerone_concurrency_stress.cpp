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

// Connect/disconnect/operate concurrency stress for the Player One drivers --
// Phoenix filter wheel, camera, and thermal switch (issue #101). Same
// rationale as the ZWO EFW stress file: no fake seam, so hardware-free hosts
// storm the failure path and the AsyncConnectable machinery; hosts with the
// hardware attached exercise the full connect.

#include <alpacacore/camera_driver.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/switch_driver.h>
#include <alpacacore/vendor/playerone/playerone_camera_driver.h>
#include <alpacacore/vendor/playerone/playerone_filterwheel_driver.h>
#include <alpacacore/vendor/playerone/playerone_switch_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;

TEST_CASE("Player One Phoenix - concurrent connect/disconnect/operate stress", "[playerone][filterwheel][stress]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0);

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& wheel = static_cast<alpacacore::FilterWheelDriver&>(d);
        static_cast<void>(wheel.get_position());
        wheel.set_position(1);
        static_cast<void>(wheel.get_names());
        static_cast<void>(wheel.get_focus_offsets());
    });

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("Player One Phoenix - destruction races an in-flight connect", "[playerone][filterwheel][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::playerone::create_playerone_filterwheel(0, 0); });
}

// Camera lifecycle storm (issue #116): the Player One camera's operational
// calls were converted from snapshot-then-call (camera_id_copy) to the
// held-mutex_ with_camera shape, and its disconnect now publishes
// disconnected before the SDK close. Hardware-free hosts storm the
// connect-failure and gate paths; with a camera attached the same test
// exercises the full path.
TEST_CASE("Player One camera - concurrent connect/disconnect/operate stress", "[playerone][camera][stress]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_camera(0, 0);

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& camera = static_cast<alpacacore::CameraDriver&>(d);
        static_cast<void>(camera.get_camera_state());
        static_cast<void>(camera.get_ccd_temperature());
        static_cast<void>(camera.get_gain());
        static_cast<void>(camera.get_cooler_on());
        camera.stop_exposure();
    });

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("Player One camera - destruction races an in-flight connect", "[playerone][camera][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::playerone::create_playerone_camera(0, 0); });
}

// The thermal Switch driver (DewHeater + Fan; cooling deliberately lives on
// the Camera interface, see AGENTS.md) shares the camera's SDK handle, so on
// a hardware-free host it fails fast at the same enumeration; with a camera
// attached the same cases exercise the full connect path.
TEST_CASE("Player One switch - concurrent connect/disconnect/operate stress", "[playerone][switch][stress]") {
    auto driver = alpacacore::vendor::playerone::create_playerone_switch(0, 0);

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& sw = static_cast<alpacacore::SwitchDriver&>(d);
        // Wrapped per call: with no camera attached most of these throw
        // NotConnected (get_max_switch and get_device_state answer without
        // the device), and the harness only swallows the callback as a whole
        // -- a single outer catch would let the first throw skip all the
        // rest, leaving them unexercised. std::exception, not just
        // AlpacaException: anything escaping the SDK layer would otherwise
        // unwind past the remaining calls too.
        auto call = [](auto&& fn) {
            try {
                fn();
            } catch (const std::exception&) {
            }
        };
        call([&] { static_cast<void>(sw.get_max_switch()); });
        call([&] { static_cast<void>(sw.get_can_write(0)); });
        call([&] { static_cast<void>(sw.get_switch(0)); });
        call([&] { static_cast<void>(sw.get_switch_value(0)); });
        // On a host with a real Player One camera attached, this drives the
        // physical dew heater to 1% repeatedly for the storm's duration --
        // intended, but worth knowing before running [stress] on a live rig.
        call([&] { sw.set_switch_value(0, 1.0); });
        call([&] { static_cast<void>(sw.get_switch_name(0)); });
        call([&] { static_cast<void>(sw.get_switch_description(0)); });
        call([&] { static_cast<void>(sw.get_device_state()); });
    });

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("Player One switch - destruction races an in-flight connect", "[playerone][switch][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::playerone::create_playerone_switch(0, 0); });
}
