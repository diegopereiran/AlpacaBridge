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

// Which camera a camera-attached thermal switch (ZWO dew heater, ToupTek and
// Player One heater/fan) binds to. A bare enumeration index let a switch bind
// to a camera with no heater (an ASI290MM Mini at index 0 beside an ASI2600MM
// Pro at index 1) and fail with a message that named neither camera. The
// vendor drivers list the attached cameras; this decides and words the refusal.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alpacacore::util {

/// cameraIndex value meaning "the one attached camera that has the controls".
inline constexpr int kAutoThermalCamera = -1;

/// One attached camera as a vendor driver lists it.
struct ThermalCameraCandidate {
    int index{};         // the cameraIndex value that selects this camera
    std::string name;    // model name as the SDK reports it
    std::string serial;  // empty when the camera reports none or it was not read
    bool has_thermal{};  // has the controls this switch drives
};

/// What a switch's config asks for.
struct ThermalCameraRequest {
    int camera_index{kAutoThermalCamera};  // >= 0 pins an index; kAutoThermalCamera picks
    std::string serial;                    // non-empty pins this camera; the index is then ignored
};

/// User-facing words for one switch type, e.g. {"ZWO", "dew heater"}.
struct ThermalSwitchLabels {
    std::string_view vendor;
    std::string_view controls;
};

struct ThermalCameraChoice {
    std::optional<std::size_t> chosen;  // position in the candidate list
    std::string refusal;                // empty when chosen
    int error_code{};                   // an AlpacaError value when refused
};

/// "ZWO ASI2600MM Pro (index 1)".
std::string describe_thermal_camera(const ThermalCameraCandidate& camera);

/// Pick the camera `request` means. A serial binds only the camera carrying it.
/// Auto binds the single camera with has_thermal and refuses, listing them,
/// when there are none or several. A pinned index binds that camera whatever
/// has_thermal says, so a driver whose listing cannot see every control (the
/// ToupTek tail light has no capability flag) still decides by its own probe.
ThermalCameraChoice choose_thermal_camera(const ThermalCameraRequest& request,
                                          const std::vector<ThermalCameraCandidate>& cameras,
                                          const ThermalSwitchLabels& labels);

/// The connect refusal for a bound camera whose probe found none of the
/// controls: names it, and the attached cameras that do have them.
std::string thermal_capability_refusal(const ThermalCameraCandidate& bound,
                                       const std::vector<ThermalCameraCandidate>& cameras,
                                       const ThermalSwitchLabels& labels);

}  // namespace alpacacore::util
