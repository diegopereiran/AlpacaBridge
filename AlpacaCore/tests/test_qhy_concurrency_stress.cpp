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

// Connect/disconnect/operate concurrency stress for the QHY camera and its
// paired filter wheel (issue #101). No fake seam exists for the QHY SDK, so
// on a hardware-free host every connect fails fast at enumeration -- which
// still storms the AsyncConnectable machinery and the connect-failure
// cleanup. With a camera attached the same cases exercise the full connect
// path, which for this driver family is where the exposure-worker lifetime
// bugs lived (the generation-tracking and reap-ordering work in 3.6.0).
//
// The two drivers share one physical QHY handle through QHYSDKWrapper, so
// the last case storms both at once: that shared open/close ledger is the
// surface a single-driver storm cannot reach.

#include <alpacacore/camera_driver.h>
#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/vendor/qhy/qhy_camera_driver.h>
#include <alpacacore/vendor/qhy/qhy_filterwheel_driver.h>

#include <thread>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;

namespace {

// Most calls throw NotConnected on a hardware-free host, and the harness
// only swallows the callback as a whole -- without a per-call guard the
// first throw would skip everything after it, leaving those paths
// unexercised rather than merely unconnected. std::exception rather than
// AlpacaException: anything else escaping the SDK layer would unwind past
// the remaining calls just the same.
template <typename Fn>
void call(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception&) {
    }
}

void camera_operate(AlpacaDriver& d) {
    auto& camera = static_cast<alpacacore::CameraDriver&>(d);
    call([&] { static_cast<void>(camera.get_camera_state()); });
    call([&] { static_cast<void>(camera.get_ccd_temperature()); });
    call([&] { static_cast<void>(camera.get_gain()); });
    call([&] { static_cast<void>(camera.get_cooler_on()); });
    call([&] { static_cast<void>(camera.get_image_ready()); });
    call([&] { static_cast<void>(camera.get_device_state()); });
    // stop_exposure() rather than start_exposure(): a started exposure on
    // this driver spawns the download worker whose reap is bounded by a
    // multi-second watchdog, which would dominate the storm window without
    // adding lifecycle coverage the connect/disconnect races don't already
    // give. The stop path is the one that races a disconnect.
    call([&] { camera.stop_exposure(); });
}

void filterwheel_operate(AlpacaDriver& d) {
    auto& wheel = static_cast<alpacacore::FilterWheelDriver&>(d);
    call([&] { static_cast<void>(wheel.get_position()); });
    call([&] { wheel.set_position(1); });
    call([&] { static_cast<void>(wheel.get_names()); });
    call([&] { static_cast<void>(wheel.get_focus_offsets()); });
    call([&] { static_cast<void>(wheel.get_device_state()); });
}

}  // namespace

TEST_CASE("QHY camera - concurrent connect/disconnect/operate stress", "[qhy][camera][stress]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);

    alpacacore::test::run_lifecycle_stress(*driver, camera_operate);

    // Connected reflects whether a QHY camera is attached; both outcomes are
    // valid here, so only the settle back to disconnected is asserted.
    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("QHY camera - destruction races an in-flight connect", "[qhy][camera][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0); });
}

TEST_CASE("QHY filter wheel - concurrent connect/disconnect/operate stress", "[qhy][filterwheel][stress]") {
    auto driver = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0);

    alpacacore::test::run_lifecycle_stress(*driver, filterwheel_operate);

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("QHY filter wheel - destruction races an in-flight connect", "[qhy][filterwheel][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(0, 0); });
}

TEST_CASE("QHY camera + filter wheel - shared handle under concurrent lifecycle storms",
          "[qhy][camera][filterwheel][stress]") {
    // Both drivers resolve to the same physical camera handle via
    // QHYSDKWrapper's handle map, so storming them together is what races
    // open_camera()/close_camera() from two drivers against one ledger --
    // the shape the ToupTek camera + thermal switch case covers for that
    // vendor.
    auto camera = alpacacore::vendor::qhy::create_qhy_camera_by_index(0, 0);
    auto wheel = alpacacore::vendor::qhy::create_qhy_filterwheel_by_index(1, 0);

    std::thread camera_storm([&]() { alpacacore::test::run_lifecycle_stress(*camera, camera_operate); });
    alpacacore::test::run_lifecycle_stress(*wheel, filterwheel_operate);
    camera_storm.join();

    camera->set_connected(false);
    wheel->set_connected(false);
    CHECK(camera->get_connected() == false);
    CHECK(wheel->get_connected() == false);
}
