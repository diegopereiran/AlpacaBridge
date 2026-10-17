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

#pragma once

#include <alpacacore/switch_driver.h>
#include <alpacacore/util/thermal_camera_choice.h>

#include <memory>
#include <vector>

namespace alpacacore::vendor::touptek {

class ToupTekSDK;  // fault-injection seam (touptek_sdk_wrapper.h, issue #104)

/**
 * @brief Create a ToupTek thermal switch device (dew heater + cooling fan).
 *
 * Exposes a cooled ToupTek camera's anti-fog dew heater (TOUPCAM_OPTION_HEAT)
 * and radiator fan (TOUPCAM_OPTION_FAN) as multi-value switch elements. Elements
 * are capability-probed per model at connect (TOUPCAM_FLAG_HEAT / _FAN) — an
 * uncooled camera with neither fails to connect with NotImplemented. The switch
 * shares the camera's Toupcam handle via the reference-counted ToupTekSDKWrapper,
 * so it can be connected alongside the camera device or on its own.
 *
 * The cooler itself is intentionally NOT a switch element — it lives on the
 * Camera interface (CoolerOn / SetCCDTemperature / CoolerPower), matching ASCOM
 * and the Player One thermal switch.
 *
 * @param device_number Alpaca device number
 * @param camera_index ToupTek SDK camera index (0-based, enumeration order), or
 *        util::kAutoThermalCamera for the one attached camera with a heater or fan
 */
std::unique_ptr<SwitchDriver> create_touptek_thermal_switch(int device_number, int camera_index);

// Test seam: identical driver wired to an injected SDK implementation.
std::unique_ptr<SwitchDriver> create_touptek_thermal_switch(int device_number, int camera_index, ToupTekSDK& sdk);

/**
 * @brief Every attached ToupTek camera for a thermal switch's camera picker:
 * enumeration index, display name, and whether its flags show a dew heater or
 * fan. Reads the enumeration only (no camera is opened), so no serial.
 */
std::vector<util::ThermalCameraCandidate> list_touptek_thermal_cameras();
std::vector<util::ThermalCameraCandidate> list_touptek_thermal_cameras(ToupTekSDK& sdk);

}  // namespace alpacacore::vendor::touptek
