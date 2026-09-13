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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::gphoto {

/**
 * @brief One USB-attached camera as reported by libgphoto2 autodetection.
 */
struct GPhotoCameraInfo {
    std::string model;  // e.g. "Nikon DSC D5300"
    std::string port;   // e.g. "usb:001,004"
};

/**
 * @brief Result of a triggered capture: the downloaded file bytes plus the
 * metadata libgphoto2 reported for it.
 */
struct GPhotoCaptureResult {
    std::vector<std::uint8_t> data;
    std::string filename;
    std::string mime_type;
};

/**
 * @brief Thin RAII wrapper around libgphoto2's Camera/GPContext/CameraWidget
 * API, isolating raw libgphoto2 calls from the Alpaca driver logic (the
 * project's SDK-wrapper pattern -- see AlpacaCore/src/vendors/svbony for the
 * sibling shape used by SDK-based cameras).
 *
 * Unlike the proprietary camera SDKs (ZWO/QHY/SVBONY/...), libgphoto2 has no
 * single global init/shutdown call; the shared state that must outlive every
 * open camera is the GPContext, held once by the singleton. Each opened
 * camera gets its own integer handle (mirroring the "camera_id" shape the
 * other camera wrappers use), so driver code looks the same across vendors.
 */
class GPhotoSDKWrapper {
public:
    static GPhotoSDKWrapper& instance();

    /**
     * @brief Enumerate currently attached PTP/MTP cameras via USB autodetect.
     *
     * Index into the returned vector is the Alpaca "cameraIndex" config
     * field, matching the SDK-enumerated convention used by the other camera
     * vendors (ZWO/QHY/SVBONY/PlayerOne/ToupTek).
     */
    std::vector<GPhotoCameraInfo> enumerate_cameras();

    /**
     * @brief Open a session for the camera at the given model/port pair.
     *
     * @return An opaque handle for use with every other method below.
     * @throws AlpacaException on failure (camera not found, busy, etc).
     */
    int open_camera(const std::string& model, const std::string& port);

    /// Close a previously opened camera. Safe to call on an unknown handle
    /// (no-op) so driver disconnect paths never need to track validity.
    void close_camera(int handle);

    std::string get_gphoto_version();
    std::string get_camera_summary(int handle);

    bool has_widget(int handle, const std::string& name);

    /// Radio/menu widgets (e.g. "iso", "shutterspeed2", "imagequality").
    std::vector<std::string> get_choices(int handle, const std::string& name);
    std::string get_choice_value(int handle, const std::string& name);
    void set_choice_value(int handle, const std::string& name, const std::string& value);

    /// Toggle widgets (e.g. the Nikon/Canon PTP "bulb" shutter-open toggle).
    bool get_toggle_value(int handle, const std::string& name);
    void set_toggle_value(int handle, const std::string& name, bool on);

    /// Text widgets (rarely needed; kept for completeness/diagnostics).
    std::string get_text_value(int handle, const std::string& name);

    /// Trigger a normal (non-bulb) capture and download the resulting file.
    GPhotoCaptureResult capture_and_download(int handle);

    /**
     * @brief Wait for the file-added event a just-closed bulb shutter
     * produces, then download and delete it.
     *
     * Callers drive the bulb sequence themselves via set_toggle_value(handle,
     * "bulb", true/false) (see gphoto_camera_driver.cpp's abortable sleep
     * loop) and call this immediately after closing the shutter.
     */
    GPhotoCaptureResult wait_for_bulb_file_and_download(int handle);

private:
    GPhotoSDKWrapper();
    ~GPhotoSDKWrapper();
    GPhotoSDKWrapper(const GPhotoSDKWrapper&) = delete;
    GPhotoSDKWrapper& operator=(const GPhotoSDKWrapper&) = delete;

    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

}  // namespace alpacacore::vendor::gphoto
