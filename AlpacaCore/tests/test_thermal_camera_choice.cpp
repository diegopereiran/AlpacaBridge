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

// Which camera a camera-attached thermal switch binds to: Auto, a pinned
// index, a serial, and the refusal text an operator reads in NINA or Ara.
// The shapes come from the 5.0.0~beta4 rig: an ASI290MM Mini (no heater) at
// index 0 beside an ASI2600MM Pro (heater) at index 1.

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/thermal_camera_choice.h>

#include <string>
#include <vector>

#include "catch2_compat.h"

using alpacacore::util::choose_thermal_camera;
using alpacacore::util::kAutoThermalCamera;
using alpacacore::util::thermal_capability_refusal;
using alpacacore::util::ThermalCameraCandidate;
using alpacacore::util::ThermalCameraRequest;
using alpacacore::util::ThermalSwitchLabels;

namespace {

constexpr ThermalSwitchLabels kLabels{"ZWO", "dew heater"};

ThermalCameraCandidate camera(int index, const char* name, const char* serial, bool has_thermal) {
    ThermalCameraCandidate c;
    c.index = index;
    c.name = name;
    c.serial = serial;
    c.has_thermal = has_thermal;
    return c;
}

std::vector<ThermalCameraCandidate> rig() {
    return {camera(0, "ZWO ASI290MM Mini", "2d0bd20224000900", false),
            camera(1, "ZWO ASI2600MM Pro", "2e18870f28000900", true)};
}

ThermalCameraRequest request(int index, const char* serial = "") {
    ThermalCameraRequest r;
    r.camera_index = index;
    r.serial = serial;
    return r;
}

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("Thermal camera choice - Auto binds the one camera with the controls", "[thermal][unit]") {
    const auto cameras = rig();
    const auto choice = choose_thermal_camera(request(kAutoThermalCamera), cameras, kLabels);
    REQUIRE(choice.chosen.has_value());
    CHECK(cameras[*choice.chosen].name == "ZWO ASI2600MM Pro");
    CHECK(choice.refusal.empty());
}

TEST_CASE("Thermal camera choice - Auto refuses when no camera has the controls", "[thermal][unit]") {
    const std::vector<ThermalCameraCandidate> cameras{camera(0, "ZWO ASI290MM Mini", "", false)};
    const auto choice = choose_thermal_camera(request(kAutoThermalCamera), cameras, kLabels);
    CHECK_FALSE(choice.chosen.has_value());
    CHECK(choice.error_code == alpacacore::AlpacaError::NotImplemented);
    CHECK(choice.refusal == "No attached ZWO camera has a dew heater. Attached: ZWO ASI290MM Mini (index 0).");
}

TEST_CASE("Thermal camera choice - Auto refuses two cameras with the controls and lists them", "[thermal][unit]") {
    auto cameras = rig();
    cameras.push_back(camera(2, "ZWO ASI533MC Pro", "11aa", true));
    const auto choice = choose_thermal_camera(request(kAutoThermalCamera), cameras, kLabels);
    CHECK_FALSE(choice.chosen.has_value());
    CHECK(choice.error_code == alpacacore::AlpacaError::InvalidValue);
    CHECK(contains(choice.refusal, "2 attached ZWO cameras have a dew heater"));
    CHECK(contains(choice.refusal, "ZWO ASI2600MM Pro (index 1), ZWO ASI533MC Pro (index 2)"));
    CHECK_FALSE(contains(choice.refusal, "ASI290MM Mini"));
}

TEST_CASE("Thermal camera choice - a pinned index binds that camera whatever its controls", "[thermal][unit]") {
    const auto cameras = rig();
    const auto mini = choose_thermal_camera(request(0), cameras, kLabels);
    REQUIRE(mini.chosen.has_value());
    CHECK(cameras[*mini.chosen].index == 0);
    const auto missing = choose_thermal_camera(request(5), cameras, kLabels);
    CHECK_FALSE(missing.chosen.has_value());
    CHECK(missing.error_code == alpacacore::AlpacaError::InvalidValue);
    CHECK(contains(missing.refusal, "Camera index 5 not found"));
    CHECK(contains(missing.refusal, "ZWO ASI2600MM Pro (index 1)"));
}

TEST_CASE("Thermal camera choice - a serial binds only its camera, never the index", "[thermal][unit]") {
    // The two cameras swapped slots after a reboot: the serial still finds the Pro.
    const std::vector<ThermalCameraCandidate> swapped{camera(0, "ZWO ASI2600MM Pro", "2e18870f28000900", true),
                                                      camera(1, "ZWO ASI290MM Mini", "2d0bd20224000900", false)};
    const auto found = choose_thermal_camera(request(1, "2e18870f28000900"), swapped, kLabels);
    REQUIRE(found.chosen.has_value());
    CHECK(swapped[*found.chosen].name == "ZWO ASI2600MM Pro");

    const auto absent = choose_thermal_camera(request(1, "ffff"), swapped, kLabels);
    CHECK_FALSE(absent.chosen.has_value());
    CHECK(absent.error_code == alpacacore::AlpacaError::NotConnected);
    CHECK(contains(absent.refusal, "serial ffff is not attached"));
}

TEST_CASE("Thermal camera choice - no cameras at all", "[thermal][unit]") {
    const auto choice = choose_thermal_camera(request(0), {}, kLabels);
    CHECK_FALSE(choice.chosen.has_value());
    CHECK(choice.error_code == alpacacore::AlpacaError::NotConnected);
    CHECK(choice.refusal == "No ZWO cameras detected");
}

TEST_CASE("Thermal camera choice - the capability refusal names the camera to pick", "[thermal][unit]") {
    const auto cameras = rig();
    CHECK(thermal_capability_refusal(cameras[0], cameras, kLabels) ==
          "Camera index 0 is the ZWO ASI290MM Mini, which has no dew heater. Pick ZWO ASI2600MM Pro (index 1) "
          "in this switch's settings.");

    const std::vector<ThermalCameraCandidate> alone{cameras[0]};
    CHECK(thermal_capability_refusal(alone[0], alone, kLabels) ==
          "Camera index 0 is the ZWO ASI290MM Mini, which has no dew heater, and no attached ZWO camera has one.");

    auto three = cameras;
    three.push_back(camera(2, "ZWO ASI533MC Pro", "", true));
    const auto many = thermal_capability_refusal(three[0], three, kLabels);
    CHECK(contains(many,
                   "Pick one of these in this switch's settings: ZWO ASI2600MM Pro (index 1), "
                   "ZWO ASI533MC Pro (index 2)."));
}
