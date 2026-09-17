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

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/gphoto/gphoto_sdk_wrapper.h>
#include <unistd.h>

#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace alpacacore::test {

/**
 * Drop the driver's on-disk sensor-geometry cache
 * (gphoto_camera_driver.cpp's kSensorCacheRelativePath, CWD-relative).
 *
 * Every fake-SDK connect that reaches the priming capture writes this file.
 * Left in place it makes the *next* run of these tests take the cache-hit
 * branch of set_connected(), which publishes Connected without ever calling
 * prime_sensor_geometry_and_cache -- so the priming/decode coverage these
 * tests exist for would evaporate silently on run 2 in a persistent build
 * directory. Call this first in any gphoto test case that connects.
 *
 * Deleting rather than filtering is safe: the cache is pure derived state
 * that the next priming capture rebuilds.
 */
inline void reset_gphoto_sensor_cache() {
    std::error_code ec;
    std::filesystem::remove("config/gphoto_sensor_cache.tsv", ec);
}

/**
 * Suffix a fake camera model so it is unique to this process.
 *
 * The sensor cache is keyed by model string and lives in one CWD-relative
 * file, but catch_discover_tests() gives every TEST_CASE its own process and
 * run_all_tests.sh drives ctest with -j. Without this, a sibling test case
 * priming the same model concurrently could satisfy this one's cache lookup
 * between its reset_gphoto_sensor_cache() and its connect, silently skipping
 * the priming path it means to exercise. (AlpacaHTTP solves the same
 * CWD-sharing problem with per-test WORKING_DIRECTORY; the Catch2 tests here
 * are all one binary, so key separation is the equivalent.)
 */
inline std::string unique_test_model(const std::string& base) {
    return base + " [pid " + std::to_string(::getpid()) + "]";
}

/**
 * Scripted fake for GPhotoSDK (issue #489 fault-injection seam).
 *
 * Callers push one or more FakeCamera entries into `cameras` before
 * connecting (index == the Alpaca "cameraIndex"). Each fake camera carries
 * its own widget state (choices/current value, toggles, text) so tests can
 * script the widget-probing fallback behavior configure_after_connect_locked
 * relies on (e.g. "shutterspeed2" absent but "shutterspeed" present).
 *
 * Failure injection: add a method name to `throw_from` and every call to
 * that method throws AlpacaException regardless of arguments.
 */
class FakeGPhotoSDK : public vendor::gphoto::GPhotoSDK {
public:
    struct FakeCamera {
        std::string model;
        std::string port;
        // widget name -> ordered choice list
        std::unordered_map<std::string, std::vector<std::string>> choices;
        // widget name -> current choice value
        std::unordered_map<std::string, std::string> choice_value;
        // widget name -> toggle state
        std::unordered_map<std::string, bool> toggle_value;
        // widget name -> text value
        std::unordered_map<std::string, std::string> text_value;
    };

    std::vector<FakeCamera> cameras;
    std::set<std::string> throw_from;
    std::vector<std::string> call_log;

    int open_count{0};
    int close_count{0};
    std::string gphoto_version{"2.5.31"};

    // Canned result returned by capture_and_download / wait_for_bulb_file_and_download.
    vendor::gphoto::GPhotoCaptureResult capture_result;

    // Records every set_toggle_value(handle, "bulb", on) call, in order, so
    // bulb-sequence tests can assert true-then-false-then-download ordering.
    std::vector<bool> bulb_toggle_history;

    std::vector<vendor::gphoto::GPhotoCameraInfo> enumerate_cameras() override {
        log_and_maybe_throw("enumerate_cameras");
        std::vector<vendor::gphoto::GPhotoCameraInfo> out;
        for (const auto& cam : cameras) {
            out.push_back({cam.model, cam.port});
        }
        return out;
    }

    int open_camera(const std::string& model, const std::string& port) override {
        log_and_maybe_throw("open_camera");
        for (std::size_t i = 0; i < cameras.size(); ++i) {
            if (cameras[i].model == model && cameras[i].port == port) {
                int handle = static_cast<int>(i);
                ++open_count;
                open_balance_[handle] = true;
                return handle;
            }
        }
        throw AlpacaException("FakeGPhotoSDK: no matching camera for " + model + "/" + port, AlpacaError::NotConnected);
    }

    void close_camera(int handle) override {
        // Never throws (matches the contract: safe on an unknown handle).
        call_log.push_back("close_camera");
        auto it = open_balance_.find(handle);
        if (it != open_balance_.end() && it->second) {
            it->second = false;
            ++close_count;
        }
    }

    std::string get_gphoto_version() override {
        log_and_maybe_throw("get_gphoto_version");
        return gphoto_version;
    }

    std::string get_camera_summary(int handle) override {
        log_and_maybe_throw("get_camera_summary");
        return "Fake camera summary for " + camera_for(handle).model;
    }

    bool has_widget(int handle, const std::string& name) override {
        log_and_maybe_throw("has_widget");
        const auto& cam = camera_for(handle);
        return cam.choices.count(name) != 0 || cam.toggle_value.count(name) != 0 || cam.text_value.count(name) != 0;
    }

    std::vector<std::string> get_choices(int handle, const std::string& name) override {
        log_and_maybe_throw("get_choices");
        const auto& cam = camera_for(handle);
        auto it = cam.choices.find(name);
        return it != cam.choices.end() ? it->second : std::vector<std::string>{};
    }

    std::string get_choice_value(int handle, const std::string& name) override {
        log_and_maybe_throw("get_choice_value");
        auto& cam = camera_for(handle);
        auto it = cam.choice_value.find(name);
        if (it == cam.choice_value.end()) {
            throw AlpacaException("Widget not present: " + name, AlpacaError::PropertyNotImplemented);
        }
        return it->second;
    }

    void set_choice_value(int handle, const std::string& name, const std::string& value) override {
        log_and_maybe_throw("set_choice_value");
        auto& cam = camera_for(handle);
        cam.choice_value[name] = value;
    }

    bool get_toggle_value(int handle, const std::string& name) override {
        log_and_maybe_throw("get_toggle_value");
        auto& cam = camera_for(handle);
        auto it = cam.toggle_value.find(name);
        if (it == cam.toggle_value.end()) {
            throw AlpacaException("Widget not present: " + name, AlpacaError::PropertyNotImplemented);
        }
        return it->second;
    }

    void set_toggle_value(int handle, const std::string& name, bool on) override {
        log_and_maybe_throw("set_toggle_value");
        auto& cam = camera_for(handle);
        cam.toggle_value[name] = on;
        if (name == "bulb") {
            bulb_toggle_history.push_back(on);
        }
    }

    std::string get_text_value(int handle, const std::string& name) override {
        log_and_maybe_throw("get_text_value");
        auto& cam = camera_for(handle);
        auto it = cam.text_value.find(name);
        if (it == cam.text_value.end()) {
            throw AlpacaException("Widget not present: " + name, AlpacaError::PropertyNotImplemented);
        }
        return it->second;
    }

    vendor::gphoto::GPhotoCaptureResult capture_and_download(int handle) override {
        log_and_maybe_throw("capture_and_download");
        camera_for(handle);  // validates handle
        return capture_result;
    }

    vendor::gphoto::GPhotoCaptureResult wait_for_bulb_file_and_download(int handle) override {
        log_and_maybe_throw("wait_for_bulb_file_and_download");
        camera_for(handle);
        return capture_result;
    }

private:
    std::unordered_map<int, bool> open_balance_;

    FakeCamera& camera_for(int handle) {
        call_log.push_back("camera_for");
        if (handle < 0 || static_cast<std::size_t>(handle) >= cameras.size()) {
            throw AlpacaException("FakeGPhotoSDK: unknown handle", AlpacaError::NotConnected);
        }
        return cameras[static_cast<std::size_t>(handle)];
    }

    void log_and_maybe_throw(const std::string& name) {
        call_log.push_back(name);
        if (throw_from.count(name) != 0) {
            throw AlpacaException("FakeGPhotoSDK: injected failure in " + name, AlpacaError::DriverException);
        }
    }
};

}  // namespace alpacacore::test
