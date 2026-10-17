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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/util/thermal_camera_choice.h>

namespace alpacacore::util {

namespace {

std::string join_cameras(const std::vector<ThermalCameraCandidate>& cameras, bool thermal_only) {
    std::string text;
    for (const auto& camera : cameras) {
        if (thermal_only && !camera.has_thermal) {
            continue;
        }
        if (!text.empty()) {
            text += ", ";
        }
        text += describe_thermal_camera(camera);
    }
    return text;
}

std::size_t count_thermal(const std::vector<ThermalCameraCandidate>& cameras) {
    std::size_t count = 0;
    for (const auto& camera : cameras) {
        if (camera.has_thermal) {
            ++count;
        }
    }
    return count;
}

ThermalCameraChoice refuse(std::string message, int code) {
    ThermalCameraChoice choice;
    choice.refusal = std::move(message);
    choice.error_code = code;
    return choice;
}

ThermalCameraChoice pick(std::size_t position) {
    ThermalCameraChoice choice;
    choice.chosen = position;
    return choice;
}

}  // namespace

std::string describe_thermal_camera(const ThermalCameraCandidate& camera) {
    return camera.name + " (index " + std::to_string(camera.index) + ")";
}

ThermalCameraChoice choose_thermal_camera(const ThermalCameraRequest& request,
                                          const std::vector<ThermalCameraCandidate>& cameras,
                                          const ThermalSwitchLabels& labels) {
    const std::string vendor(labels.vendor);
    const std::string controls(labels.controls);
    if (cameras.empty()) {
        return refuse("No " + vendor + " cameras detected", AlpacaError::NotConnected);
    }

    if (!request.serial.empty()) {
        for (std::size_t i = 0; i < cameras.size(); ++i) {
            if (cameras[i].serial == request.serial) {
                return pick(i);
            }
        }
        // Never fall back to the index: after a USB reorder it names another camera.
        return refuse("The " + vendor + " camera with serial " + request.serial +
                          " is not attached. Attached: " + join_cameras(cameras, false) +
                          ". Plug it in, or pick another camera in this switch's settings.",
                      AlpacaError::NotConnected);
    }

    if (request.camera_index == kAutoThermalCamera) {
        const std::size_t thermal = count_thermal(cameras);
        if (thermal == 1) {
            for (std::size_t i = 0; i < cameras.size(); ++i) {
                if (cameras[i].has_thermal) {
                    return pick(i);
                }
            }
        }
        if (thermal == 0) {
            return refuse("No attached " + vendor + " camera has a " + controls +
                              ". Attached: " + join_cameras(cameras, false) + ".",
                          AlpacaError::NotImplemented);
        }
        return refuse(std::to_string(thermal) + " attached " + vendor + " cameras have a " + controls + ": " +
                          join_cameras(cameras, true) + ". Pick one in this switch's settings.",
                      AlpacaError::InvalidValue);
    }

    for (std::size_t i = 0; i < cameras.size(); ++i) {
        if (cameras[i].index == request.camera_index) {
            return pick(i);
        }
    }
    return refuse("Camera index " + std::to_string(request.camera_index) +
                      " not found. Attached: " + join_cameras(cameras, false) + ".",
                  AlpacaError::InvalidValue);
}

std::string thermal_capability_refusal(const ThermalCameraCandidate& bound,
                                       const std::vector<ThermalCameraCandidate>& cameras,
                                       const ThermalSwitchLabels& labels) {
    std::vector<ThermalCameraCandidate> others;
    for (const auto& camera : cameras) {
        if (camera.has_thermal && camera.index != bound.index) {
            others.push_back(camera);
        }
    }
    std::string message = "Camera index " + std::to_string(bound.index) + " is the " + bound.name + ", which has no " +
                          std::string(labels.controls);
    if (others.empty()) {
        return message + ", and no attached " + std::string(labels.vendor) + " camera has one.";
    }
    if (others.size() == 1) {
        return message + ". Pick " + describe_thermal_camera(others.front()) + " in this switch's settings.";
    }
    return message + ". Pick one of these in this switch's settings: " + join_cameras(others, true) + ".";
}

}  // namespace alpacacore::util
