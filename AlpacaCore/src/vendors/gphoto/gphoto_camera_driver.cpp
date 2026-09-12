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

#include <alpacacore/async_connectable.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/gphoto/gphoto_camera_driver.h>
#include <alpacacore/vendor/gphoto/gphoto_sdk_wrapper.h>
#include <alpacacore/version.h>

#include <libraw/libraw.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace alpacacore::vendor::gphoto {

namespace {

constexpr const char* kLogTag = "GPhoto";

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Parses a libgphoto2 shutter-speed choice string ("1/200", "30", "bulb", or
// the raw PTP fraction form "65535/65535") into seconds. Returns nullopt for
// the bulb sentinel -- callers treat that as "no fixed duration".
std::optional<double> parse_shutter_speed_seconds(const std::string& choice) {
    std::string lower = to_lower(choice);
    if (lower == "bulb" || lower == "65535/65535") {
        return std::nullopt;
    }
    auto slash = choice.find('/');
    try {
        if (slash != std::string::npos) {
            double numerator = std::stod(choice.substr(0, slash));
            double denominator = std::stod(choice.substr(slash + 1));
            if (denominator == 0.0) return std::nullopt;
            return numerator / denominator;
        }
        return std::stod(choice);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Picks the RAW image-quality choice from a camera's "imageformat"/
// "imagequality" widget: prefers a pure-RAW choice (e.g. "NEF (Raw)") over a
// combined RAW+JPEG one (e.g. "NEF+JPEG Fine"), so only one file per capture
// needs downloading and decoding.
std::optional<std::string> pick_raw_format_choice(const std::vector<std::string>& choices) {
    std::optional<std::string> raw_plus_other;
    for (const auto& choice : choices) {
        std::string lower = to_lower(choice);
        bool has_raw = lower.find("raw") != std::string::npos || lower.find("nef") != std::string::npos ||
                       lower.find("cr2") != std::string::npos || lower.find("cr3") != std::string::npos ||
                       lower.find("arw") != std::string::npos;
        if (!has_raw) continue;
        bool combined = lower.find('+') != std::string::npos || lower.find("jpeg") != std::string::npos ||
                        lower.find("jpg") != std::string::npos;
        if (!combined) {
            return choice;
        }
        if (!raw_plus_other) {
            raw_plus_other = choice;
        }
    }
    return raw_plus_other;
}

} // namespace

class GPhotoCameraDriver : public CameraDriver, protected alpacacore::AsyncConnectable {
public:
    GPhotoCameraDriver(int device_number, int camera_index)
        : AsyncConnectable("GPhoto"),
          device_number_(device_number),
          camera_index_(camera_index) {
        preload_camera_info_locked();
    }

    ~GPhotoCameraDriver() override {
        shutdown_connection();
        stop_exposure_thread();
        if (connected_.load()) {
            try {
                set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag, "Error during destruction: " + std::string(e.what()));
            }
        }
    }

    // --- AlpacaDriver ---

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        const_cast<GPhotoCameraDriver*>(this)->refresh_cached_camera_info_if_needed();
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.model : "gphoto2 Camera";
    }

    DeviceType get_device_type() const override { return DeviceType::Camera; }

    std::string get_unique_id() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_info_valid_ && !camera_info_.port.empty()) {
            return "GPHOTO_" + camera_info_.port;
        }
        return "GPHOTO_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "libgphoto2 DSLR/Mirrorless Camera Driver"; }
    std::string get_driver_info() const override { return "AlpacaCore GPhoto Camera Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }

    std::optional<std::string> get_device_sdk_version() const override {
        auto version = GPhotoSDKWrapper::instance().get_gphoto_version();
        if (version.empty()) return std::nullopt;
        return "libgphoto2 " + version;
    }

    int get_interface_version() const override { return 4; } // ICameraV4 (Platform 7)

    bool get_connected() const override { return connected_.load(); }
    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::unique_lock<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_, std::defer_lock);
        if (!connected) {
            lifecycle_lock.lock();
            stop_exposure_thread();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected == connected_.load()) {
            return; // Idempotent (ASCOM)
        }

        auto& sdk = GPhotoSDKWrapper::instance();

        if (connected) {
            auto cameras = sdk.enumerate_cameras();
            if (camera_index_ < 0 || camera_index_ >= static_cast<int>(cameras.size())) {
                throw AlpacaException("gphoto camera index not found (is it plugged in and powered on?)",
                                      AlpacaError::NotConnected);
            }
            const auto& info = cameras[static_cast<std::size_t>(camera_index_)];
            int opened_handle = sdk.open_camera(info.model, info.port);
            try {
                configure_after_connect_locked(sdk, opened_handle);
            } catch (const AlpacaException&) {
                sdk.close_camera(opened_handle);
                throw;
            } catch (const std::exception& e) {
                sdk.close_camera(opened_handle);
                throw AlpacaException(std::string("Failed to configure gphoto camera: ") + e.what(),
                                      AlpacaError::DriverException);
            }
            handle_ = opened_handle;
            camera_info_ = info;
            camera_info_valid_ = true;
            reset_exposure_state_locked();
            connected_.store(true);
            return;
        }

        // Disconnecting: publish disconnected before closing the SDK handle
        // so a racing operational call fails fast at its connection check
        // instead of hitting a just-closed camera (AGENTS.md disconnect
        // rule). Exposure worker is already joined (lifecycle lock above).
        const int close_handle = handle_;
        handle_ = -1;
        reset_exposure_state_locked();
        connected_.store(false);
        if (close_handle >= 0) {
            sdk.close_camera(close_handle);
        }
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }

    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
    }

    bool can_action(std::string_view) const override { return false; }

    std::string command_blind(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    bool command_bool(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    std::string command_string(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    // --- CameraDriver: sensor geometry ---
    //
    // Unlike SDK-enumerated CMOS cameras, libgphoto2 does not report sensor
    // dimensions, pixel pitch, or Bayer phase up front -- those are only
    // knowable once a RAW frame has actually been decoded. Every property
    // below that depends on them throws InvalidOperation ("not yet known")
    // until the first successful exposure; after that they report the
    // decoded frame's real values for the rest of the session. This is the
    // same limitation other RAW-over-gphoto2 ASCOM drivers (e.g.
    // ASCOM.DSLR) have; see AGENTS.md for the follow-up options considered.

    int get_bayer_offset_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_geometry_known_locked();
        return bayer_offset_x_;
    }

    int get_bayer_offset_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        require_geometry_known_locked();
        return bayer_offset_y_;
    }

    int get_bin_x() const override { return 1; }
    void set_bin_x(int bin_x) override { reject_non_unity_bin(bin_x); }
    int get_bin_y() const override { return 1; }
    void set_bin_y(int bin_y) override { reject_non_unity_bin(bin_y); }

    CameraState get_camera_state() const override {
        if (!connected_.load()) return CameraState::Idle;
        if (exposure_active_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exposure_deadline_valid_ && std::chrono::steady_clock::now() >= exposure_deadline_) {
                ALPACA_LOG_WARN(kLogTag, "Exposure deadline exceeded; forcing CameraState=Idle. "
                                         "Exposure thread may still be blocked inside libgphoto2.");
                exposure_active_.store(false);
                exposure_deadline_valid_ = false;
                return CameraState::Idle;
            }
            return CameraState::Exposing;
        }
        return CameraState::Idle;
    }

    int get_camera_x_size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_x_size_;
    }

    int get_camera_y_size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_y_size_;
    }

    bool get_can_abort_exposure() const override { return true; }
    bool get_can_asymmetric_bin() const override { return false; }
    bool get_can_fast_readout() const override { return false; }
    bool get_can_get_cooler_power() const override { return false; }
    bool get_can_pulse_guide() const override { return false; }
    bool get_can_set_ccd_temperature() const override { return false; }
    bool get_can_stop_exposure() const override { return true; }

    double get_ccd_temperature() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sensor_temperature_.has_value()) {
            // Most DSLR RAW files (and Nikon's specifically) do not embed a
            // sensor-temperature tag; this only ever becomes available if
            // LibRaw finds one (observed mainly on Canon frames).
            throw AlpacaException("Sensor temperature not reported by this camera/frame",
                                  AlpacaError::PropertyNotImplemented);
        }
        return *sensor_temperature_;
    }

    bool get_cooler_on() const override { return false; }
    void set_cooler_on(bool cooler_on) override {
        ensure_connected();
        if (cooler_on) {
            throw AlpacaException("Cooler not supported", AlpacaError::NotImplemented);
        }
    }

    double get_cooler_power() const override { return 0.0; }
    double get_electrons_per_adu() const override { return 1.0; } // unknown; ConformU rejects 0

    double get_exposure_max() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        // No hard ceiling exists once bulb mode is available; report a
        // generous practical cap rather than an unbounded value (ConformU
        // and most Alpaca clients expect a finite ExposureMax). Revisit
        // this constant if hardware validation shows a tighter camera-side
        // limit (e.g. a battery/thermal cutoff during long bulb captures).
        return has_bulb_ ? 3600.0 : max_native_shutter_seconds_;
    }

    double get_exposure_min() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return min_native_shutter_seconds_;
    }

    double get_exposure_resolution() const override { return 0.001; }

    bool get_fast_readout() const override {
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }
    void set_fast_readout(bool) override {
        throw AlpacaException("Fast readout not supported", AlpacaError::NotImplemented);
    }

    double get_full_well_capacity() const override { return 0.0; } // unknown for DSLR sensors

    int get_gain() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_iso_supported_locked();
        return current_iso_index_;
    }

    void set_gain(int gain) override {
        ensure_connected();
        std::string choice;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            require_iso_supported_locked();
            if (gain < 0 || gain >= static_cast<int>(iso_choices_.size())) {
                throw AlpacaException("Gain index out of range", AlpacaError::InvalidValue);
            }
            if (exposure_active_.load()) {
                throw AlpacaException("Cannot change ISO during an exposure", AlpacaError::InvalidOperation);
            }
            choice = iso_choices_[static_cast<std::size_t>(gain)];
        }
        GPhotoSDKWrapper::instance().set_choice_value(handle_value(), "iso", choice);
        std::lock_guard<std::mutex> lock(mutex_);
        current_iso_index_ = gain;
    }

    int get_gain_max() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_iso_supported_locked();
        return static_cast<int>(iso_choices_.size()) - 1;
    }

    int get_gain_min() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_iso_supported_locked();
        return 0;
    }

    std::vector<std::string> get_gains() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        require_iso_supported_locked();
        return iso_choices_;
    }

    bool get_has_shutter() const override { return true; } // DSLRs have a real mechanical shutter

    double get_heat_sink_temperature() const override { return get_ccd_temperature(); }

    ImageArray get_image_array() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_ || !image_ready_ || !image_cached_) {
            throw AlpacaException("Image not ready", AlpacaError::InvalidOperation);
        }
        return last_image_;
    }

    std::string get_image_array_variant() const override { return "Int32"; }

    bool get_image_ready() const override {
        if (!connected_.load()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        return last_exposure_valid_ && image_ready_ && image_cached_;
    }

    bool get_is_pulse_guiding() const override { return false; }

    double get_last_exposure_duration() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            throw AlpacaException("Last exposure duration not set", AlpacaError::ValueNotSet);
        }
        return last_exposure_duration_;
    }

    std::chrono::system_clock::time_point get_last_exposure_start_time() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!last_exposure_valid_) {
            throw AlpacaException("Last exposure start time not set", AlpacaError::ValueNotSet);
        }
        return last_exposure_start_;
    }

    int get_max_adu() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return max_adu_;
    }

    int get_max_bin_x() const override { return 1; }
    int get_max_bin_y() const override { return 1; }

    int get_num_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_x_;
    }
    void set_num_x(int num_x) override { set_roi_dimension_locked(&num_x_, num_x); }

    int get_num_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return num_y_;
    }
    void set_num_y(int num_y) override { set_roi_dimension_locked(&num_y_, num_y); }

    int get_offset() const override {
        throw AlpacaException("Offset not supported", AlpacaError::NotImplemented);
    }
    void set_offset(int) override {
        throw AlpacaException("Offset not supported", AlpacaError::NotImplemented);
    }
    int get_offset_max() const override {
        throw AlpacaException("Offset not supported", AlpacaError::NotImplemented);
    }
    int get_offset_min() const override {
        throw AlpacaException("Offset not supported", AlpacaError::NotImplemented);
    }
    std::vector<std::string> get_offsets() const override {
        throw AlpacaException("Offset not supported", AlpacaError::PropertyNotImplemented);
    }

    double get_percent_completed() const override {
        if (!connected_.load()) return 0.0;
        if (!exposure_active_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            return image_ready_ ? 100.0 : 0.0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (last_exposure_duration_ <= 0.0) return 0.0;
        double elapsed = std::chrono::duration<double>(std::chrono::system_clock::now() - last_exposure_start_).count();
        double percent = (elapsed / last_exposure_duration_) * 100.0;
        return std::clamp(percent, 0.0, 100.0);
    }

    double get_pixel_size_x() const override { return 0.0; } // unknown; libgphoto2 has no pixel-pitch query
    double get_pixel_size_y() const override { return 0.0; }

    int get_readout_mode() const override { return 0; }
    void set_readout_mode(int mode) override {
        if (mode != 0) {
            throw AlpacaException("Invalid readout mode", AlpacaError::InvalidValue);
        }
    }
    std::vector<std::string> get_readout_modes() const override { return {"Normal"}; }

    std::string get_sensor_name() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return camera_info_valid_ ? camera_info_.model : "gphoto2 Sensor";
    }

    SensorType get_sensor_type() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return sensor_type_;
    }

    double get_set_ccd_temperature() const override {
        throw AlpacaException("Cooler not supported", AlpacaError::NotImplemented);
    }
    void set_set_ccd_temperature(double) override {
        throw AlpacaException("Cooler not supported", AlpacaError::NotImplemented);
    }

    int get_start_x() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_x_;
    }
    void set_start_x(int start_x) override { set_roi_dimension_locked(&start_x_, start_x); }

    int get_start_y() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return start_y_;
    }
    void set_start_y(int start_y) override { set_roi_dimension_locked(&start_y_, start_y); }

    double get_sub_exposure_duration() const override {
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }
    void set_sub_exposure_duration(double) override {
        throw AlpacaException("Sub-exposure duration not supported", AlpacaError::NotImplemented);
    }

    void abort_exposure() override { stop_exposure(); }

    void pulse_guide(int, int) override {
        ensure_connected();
        throw AlpacaException("Pulse guide not supported (no autoguider port on a plain gphoto2 camera)",
                              AlpacaError::NotImplemented);
    }

    void start_exposure(double duration, bool light) override {
        ensure_connected();
        (void)light; // A DSLR's mechanical shutter always opens; dark frames require capping the lens.

        if (duration < 0.0) {
            throw AlpacaException("Exposure duration must be non-negative", AlpacaError::InvalidValue);
        }

        std::lock_guard<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_);
        stop_exposure_thread();

        int active_handle;
        std::string shutter_choice;
        std::string shutter_widget_name;
        bool use_bulb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (handle_ < 0) {
                throw AlpacaException("Camera handle not set", AlpacaError::NotConnected);
            }
            if (duration > get_exposure_max_locked()) {
                throw AlpacaException("Exposure duration out of range", AlpacaError::InvalidValue);
            }
            active_handle = handle_;
            use_bulb = has_bulb_ && duration > max_native_shutter_seconds_ + 1e-9;
            shutter_choice = use_bulb ? bulb_choice_ : nearest_shutter_choice_locked(duration);
            shutter_widget_name = shutter_widget_name_;

            last_exposure_duration_ = duration;
            last_exposure_start_ = std::chrono::system_clock::now();
            last_exposure_valid_ = true;
            image_ready_ = false;
            image_cached_ = false;
            abort_requested_.store(false);
            // Generous watchdog margin over the requested duration: USB
            // transfer of a 20+MB RAW file plus libgphoto2/PTP overhead.
            auto duration_margin = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>(duration)) +
                                   std::chrono::seconds(60);
            exposure_deadline_ = std::chrono::steady_clock::now() + duration_margin;
            exposure_deadline_valid_ = true;
            exposure_active_.store(true);
        }

        exposure_thread_ = std::thread([this, active_handle, shutter_choice, shutter_widget_name, use_bulb, duration]() {
            run_exposure(active_handle, shutter_choice, shutter_widget_name, use_bulb, duration);
        });
    }

    void stop_exposure() override {
        ensure_connected();
        std::lock_guard<std::mutex> lifecycle_lock(exposure_lifecycle_mutex_);
        // For a bulb capture this closes the shutter early (checked inside
        // the sleep loop in run_exposure). For a native shutter-speed
        // capture, libgphoto2 has no capture-cancel primitive: gp_camera_
        // capture() is a single blocking call, so "abort" here can only wait
        // for it to finish naturally (bounded by the longest native shutter
        // choice, typically <=30s) rather than truly interrupt it.
        abort_requested_.store(true);
        exposure_active_.store(false);
        if (exposure_thread_.joinable()) {
            exposure_thread_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        image_ready_ = false;
        image_cached_ = false;
        exposure_deadline_valid_ = false;
    }

private:
    int device_number_;
    int camera_index_;
    int handle_{-1};

    mutable std::mutex mutex_;
    std::atomic<bool> connected_{false};

    GPhotoCameraInfo camera_info_{};
    bool camera_info_valid_{false};

    // Widget capability caches, populated at connect (configure_after_connect_locked).
    std::vector<std::string> iso_choices_;
    int current_iso_index_{0};
    bool has_bulb_{false};
    std::string bulb_choice_;              // shutter-speed choice string selecting bulb mode
    std::vector<std::pair<std::string, double>> native_shutter_choices_; // sorted ascending by seconds
    std::string shutter_widget_name_{"shutterspeed2"}; // whichever of the fallback names was found at connect
    double min_native_shutter_seconds_{0.001};
    double max_native_shutter_seconds_{30.0};
    std::optional<std::string> format_widget_name_;
    std::optional<std::string> raw_format_choice_;

    // Sensor geometry -- unknown until the first successful exposure (see
    // the class-level comment on get_bayer_offset_x above).
    bool geometry_known_{false};
    int camera_x_size_{0};
    int camera_y_size_{0};
    int bayer_offset_x_{0};
    int bayer_offset_y_{0};
    int max_adu_{65535};
    SensorType sensor_type_{SensorType::RGGB};

    int num_x_{0};
    int num_y_{0};
    int start_x_{0};
    int start_y_{0};

    mutable bool image_ready_{false};
    mutable bool image_cached_{false};
    mutable ImageArray last_image_{};
    double last_exposure_duration_{0.0};
    std::chrono::system_clock::time_point last_exposure_start_{};
    bool last_exposure_valid_{false};
    std::optional<double> sensor_temperature_;

    mutable std::atomic<bool> exposure_active_{false};
    std::atomic<bool> abort_requested_{false};
    std::thread exposure_thread_;
    std::mutex exposure_lifecycle_mutex_;
    mutable std::chrono::steady_clock::time_point exposure_deadline_{};
    mutable bool exposure_deadline_valid_{false};

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
    }

    int handle_value() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (handle_ < 0) {
            throw AlpacaException("Camera not connected", AlpacaError::NotConnected);
        }
        return handle_;
    }

    void require_geometry_known_locked() const {
        if (!geometry_known_) {
            throw AlpacaException("Sensor geometry not yet known -- take one exposure first",
                                  AlpacaError::InvalidOperation);
        }
    }

    void require_iso_supported_locked() const {
        if (iso_choices_.empty()) {
            throw AlpacaException("ISO control not exposed by this camera", AlpacaError::NotImplemented);
        }
    }

    static void reject_non_unity_bin(int value) {
        if (value != 1) {
            throw AlpacaException("Binning not supported by DSLR cameras", AlpacaError::InvalidValue);
        }
    }

    void reset_exposure_state_locked() {
        image_ready_ = false;
        image_cached_ = false;
        last_exposure_duration_ = 0.0;
        last_exposure_start_ = std::chrono::system_clock::time_point{};
        last_exposure_valid_ = false;
        exposure_active_.store(false);
        exposure_deadline_valid_ = false;
        abort_requested_.store(false);
    }

    void stop_exposure_thread() {
        abort_requested_.store(true);
        exposure_active_.store(false);
        if (exposure_thread_.joinable()) {
            exposure_thread_.join();
        }
    }

    double get_exposure_max_locked() const {
        return has_bulb_ ? 3600.0 : max_native_shutter_seconds_;
    }

    std::string nearest_shutter_choice_locked(double duration) const {
        if (native_shutter_choices_.empty()) {
            throw AlpacaException("No shutter speed control exposed by this camera", AlpacaError::NotImplemented);
        }
        const auto* best = &native_shutter_choices_.front();
        double best_diff = std::abs(best->second - duration);
        for (const auto& entry : native_shutter_choices_) {
            double diff = std::abs(entry.second - duration);
            if (diff < best_diff) {
                best = &entry;
                best_diff = diff;
            }
        }
        return best->first;
    }

    void set_roi_dimension_locked(int* field, int value) {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (value < 0) {
            throw AlpacaException("ROI value must be non-negative", AlpacaError::InvalidValue);
        }
        *field = value;
    }

    void preload_camera_info_locked() {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            auto cameras = GPhotoSDKWrapper::instance().enumerate_cameras();
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
                camera_info_valid_ = true;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG(kLogTag, "Unable to preload camera info: " + std::string(e.what()));
        }
    }

    void refresh_cached_camera_info_if_needed() {
        if (connected_.load()) return;
        try {
            auto cameras = GPhotoSDKWrapper::instance().enumerate_cameras();
            std::lock_guard<std::mutex> lock(mutex_);
            if (camera_index_ >= 0 && camera_index_ < static_cast<int>(cameras.size())) {
                camera_info_ = cameras[static_cast<std::size_t>(camera_index_)];
                camera_info_valid_ = true;
            }
        } catch (const std::exception& e) {
            ALPACA_LOG_DEBUG(kLogTag, "Unable to refresh camera info: " + std::string(e.what()));
        }
    }

    // Reads widget capabilities from the freshly opened camera and caches
    // them for the rest of the session. Requires mutex_ held by the caller
    // (set_connected holds it for the whole connect sequence).
    void configure_after_connect_locked(GPhotoSDKWrapper& sdk, int handle) {
        iso_choices_.clear();
        current_iso_index_ = 0;
        if (sdk.has_widget(handle, "iso")) {
            iso_choices_ = sdk.get_choices(handle, "iso");
            std::string current = sdk.get_choice_value(handle, "iso");
            auto it = std::find(iso_choices_.begin(), iso_choices_.end(), current);
            if (it != iso_choices_.end()) {
                current_iso_index_ = static_cast<int>(std::distance(iso_choices_.begin(), it));
            }
        }

        native_shutter_choices_.clear();
        has_bulb_ = false;
        bulb_choice_.clear();
        min_native_shutter_seconds_ = 0.001;
        max_native_shutter_seconds_ = 30.0;
        const char* shutter_widget_names[] = {"shutterspeed2", "shutterspeed", "eos-shutterspeed"};
        for (const char* name : shutter_widget_names) {
            if (!sdk.has_widget(handle, name)) continue;
            shutter_widget_name_ = name;
            for (const auto& choice : sdk.get_choices(handle, name)) {
                auto seconds = parse_shutter_speed_seconds(choice);
                if (!seconds.has_value()) {
                    // A "bulb"/fraction-sentinel entry in the shutter-speed
                    // choice list: remember it so run_exposure can prime the
                    // dial into bulb mode before driving the toggle below.
                    bulb_choice_ = choice;
                    continue;
                }
                native_shutter_choices_.emplace_back(choice, *seconds);
            }
            break;
        }
        // Bulb mode is only implementable when the standalone "bulb" toggle
        // widget is present -- that is the only mechanism this driver drives
        // (see bulb_capture_with_abort). A shutter-speed "bulb" choice with
        // no toggle widget (the classic Canon eosremoterelease press/release
        // sequence) is not supported yet; ExposureMax stays capped at the
        // longest native shutter speed for such a camera. TODO(gphoto/canon):
        // add the press/release path if/when tested against real hardware.
        has_bulb_ = sdk.has_widget(handle, "bulb");
        if (!has_bulb_) {
            bulb_choice_.clear();
        }
        std::sort(native_shutter_choices_.begin(), native_shutter_choices_.end(),
                 [](const auto& a, const auto& b) { return a.second < b.second; });
        if (!native_shutter_choices_.empty()) {
            min_native_shutter_seconds_ = native_shutter_choices_.front().second;
            max_native_shutter_seconds_ = native_shutter_choices_.back().second;
        }

        format_widget_name_.reset();
        raw_format_choice_.reset();
        const char* format_widget_names[] = {"imageformat", "imagequality"};
        for (const char* name : format_widget_names) {
            if (!sdk.has_widget(handle, name)) continue;
            auto choices = sdk.get_choices(handle, name);
            auto raw_choice = pick_raw_format_choice(choices);
            if (raw_choice.has_value()) {
                format_widget_name_ = name;
                raw_format_choice_ = raw_choice;
                sdk.set_choice_value(handle, name, *raw_choice);
                break;
            }
        }
        if (!raw_format_choice_.has_value()) {
            ALPACA_LOG_WARN(kLogTag,
                "No RAW image-quality choice found on this camera; captures will fail to "
                "decode unless the camera's current format is already RAW.");
        }

        // Geometry (sensor size, Bayer phase, max ADU) is not knowable until
        // a frame has actually been decoded -- reset here so a reconnect
        // never serves stale dimensions from a previous camera.
        geometry_known_ = false;
        camera_x_size_ = 0;
        camera_y_size_ = 0;
        num_x_ = 0;
        num_y_ = 0;
        start_x_ = 0;
        start_y_ = 0;
        max_adu_ = 65535;
        sensor_type_ = SensorType::RGGB;
        sensor_temperature_.reset();
    }

    void run_exposure(int handle, const std::string& shutter_choice, const std::string& shutter_widget_name,
                      bool use_bulb, double duration) {
        auto& sdk = GPhotoSDKWrapper::instance();
        try {
            if (!shutter_choice.empty() && !shutter_widget_name.empty()) {
                sdk.set_choice_value(handle, shutter_widget_name, shutter_choice);
            }

            GPhotoCaptureResult capture;
            if (use_bulb) {
                capture = bulb_capture_with_abort(handle, duration);
            } else {
                capture = sdk.capture_and_download(handle);
            }

            if (!exposure_active_.load()) {
                // Aborted while the (uncancellable) capture call was still
                // in flight; discard the frame that arrived after the fact.
                return;
            }

            DecodedFrame decoded = decode_raw_frame(capture.data);

            std::lock_guard<std::mutex> lock(mutex_);
            apply_decoded_frame_locked(decoded);
            image_cached_ = true;
            image_ready_ = true;
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogTag, "Exposure failed: " + std::string(e.what()));
        }
        exposure_active_.store(false);
    }

    // Bulb capture with early-abort support: sleeps in short slices so
    // stop_exposure()/abort_exposure() can close the shutter well before the
    // full requested duration elapses.
    GPhotoCaptureResult bulb_capture_with_abort(int handle, double duration_s) {
        auto& sdk = GPhotoSDKWrapper::instance();
        sdk.set_toggle_value(handle, "bulb", true);
        constexpr double kSliceSeconds = 0.1;
        double remaining = duration_s;
        while (remaining > 0.0) {
            double slice = std::min(remaining, kSliceSeconds);
            std::this_thread::sleep_for(std::chrono::duration<double>(slice));
            remaining -= slice;
            if (abort_requested_.load()) {
                break;
            }
        }
        sdk.set_toggle_value(handle, "bulb", false);
        return sdk.wait_for_bulb_file_and_download(handle);
    }

    struct DecodedFrame {
        std::vector<std::int32_t> pixels;
        int width{};
        int height{};
        int bayer_offset_x{};
        int bayer_offset_y{};
        int max_adu{65535};
        SensorType sensor_type{SensorType::RGGB};
        std::optional<double> sensor_temperature;
    };

    DecodedFrame decode_raw_frame(const std::vector<std::uint8_t>& raw_bytes) {
        LibRaw processor;
        int rc = processor.open_buffer(raw_bytes.data(), raw_bytes.size());
        if (rc != LIBRAW_SUCCESS) {
            throw AlpacaException(std::string("libraw failed to open captured frame: ") + libraw_strerror(rc),
                                  AlpacaError::DriverException);
        }
        rc = processor.unpack();
        if (rc != LIBRAW_SUCCESS) {
            throw AlpacaException(std::string("libraw failed to unpack captured frame: ") + libraw_strerror(rc),
                                  AlpacaError::DriverException);
        }

        const auto& sizes = processor.imgdata.sizes;
        const ushort* raw_image = processor.imgdata.rawdata.raw_image;
        if (raw_image == nullptr || sizes.width == 0 || sizes.height == 0) {
            throw AlpacaException("libraw produced no Bayer data for this frame", AlpacaError::DriverException);
        }

        DecodedFrame frame;
        frame.width = sizes.width;
        frame.height = sizes.height;
        frame.pixels.resize(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height));
        for (int row = 0; row < frame.height; ++row) {
            const ushort* src_row = raw_image + static_cast<std::size_t>(row + sizes.top_margin) * sizes.raw_width +
                                    sizes.left_margin;
            std::int32_t* dst_row = frame.pixels.data() + static_cast<std::size_t>(row) * frame.width;
            for (int col = 0; col < frame.width; ++col) {
                dst_row[col] = static_cast<std::int32_t>(src_row[col]);
            }
        }

        // Bayer phase of the top-left 2x2 tile: find which cell libraw
        // reports as the red channel.
        const char* cdesc = processor.imgdata.idata.cdesc;
        frame.bayer_offset_x = 0;
        frame.bayer_offset_y = 0;
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                int color_index = processor.COLOR(y, x);
                if (color_index >= 0 && color_index < 4 && cdesc[color_index] == 'R') {
                    frame.bayer_offset_x = x;
                    frame.bayer_offset_y = y;
                }
            }
        }
        frame.sensor_type = SensorType::RGGB; // Nikon/Canon/Sony DSLR sensors are all standard Bayer RGGB variants

        frame.max_adu = processor.imgdata.color.maximum > 0
                            ? static_cast<int>(processor.imgdata.color.maximum)
                            : 65535;

        float sensor_temp = processor.imgdata.makernotes.common.SensorTemperature;
        if (sensor_temp > -273.15f) {
            frame.sensor_temperature = static_cast<double>(sensor_temp);
        }

        return frame;
    }

    // Requires mutex_ held; applies a freshly decoded frame, cropping to the
    // client-requested software ROI (StartX/StartY/NumX/NumY), clamped to
    // the frame bounds.
    void apply_decoded_frame_locked(const DecodedFrame& frame) {
        bool first_frame = !geometry_known_;
        camera_x_size_ = frame.width;
        camera_y_size_ = frame.height;
        bayer_offset_x_ = frame.bayer_offset_x;
        bayer_offset_y_ = frame.bayer_offset_y;
        max_adu_ = frame.max_adu;
        sensor_type_ = frame.sensor_type;
        sensor_temperature_ = frame.sensor_temperature;
        geometry_known_ = true;

        if (first_frame || num_x_ <= 0 || num_y_ <= 0) {
            num_x_ = frame.width;
            num_y_ = frame.height;
            start_x_ = 0;
            start_y_ = 0;
        }

        int crop_w = std::clamp(num_x_, 1, frame.width - std::clamp(start_x_, 0, frame.width - 1));
        int crop_h = std::clamp(num_y_, 1, frame.height - std::clamp(start_y_, 0, frame.height - 1));
        int origin_x = std::clamp(start_x_, 0, frame.width - 1);
        int origin_y = std::clamp(start_y_, 0, frame.height - 1);

        if (crop_w == frame.width && crop_h == frame.height && origin_x == 0 && origin_y == 0) {
            last_image_.data = frame.pixels;
        } else {
            last_image_.data.resize(static_cast<std::size_t>(crop_w) * crop_h);
            for (int row = 0; row < crop_h; ++row) {
                const std::int32_t* src_row = frame.pixels.data() +
                    static_cast<std::size_t>(row + origin_y) * frame.width + origin_x;
                std::int32_t* dst_row = last_image_.data.data() + static_cast<std::size_t>(row) * crop_w;
                std::copy(src_row, src_row + crop_w, dst_row);
            }
        }
        last_image_.width = crop_w;
        last_image_.height = crop_h;
        last_image_.rank = 2;
    }
};

std::unique_ptr<CameraDriver> create_gphoto_camera(int device_number, int camera_index) {
    return std::make_unique<GPhotoCameraDriver>(device_number, camera_index);
}

} // namespace alpacacore::vendor::gphoto
