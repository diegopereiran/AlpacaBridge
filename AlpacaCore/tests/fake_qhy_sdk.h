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
#include <alpacacore/vendor/qhy/qhy_sdk_wrapper.h>

#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace alpacacore::test {

/**
 * Scripted fake for the QHYSDK seam (issue #321).
 *
 * QHY is the one vendor whose real SDK cannot run at all on a test runner:
 * the first libqhyccd call spawns PnpEventListenerThread, which segfaults in
 * libusb_hotplug_register_callback when libusb_init failed. Before this fake
 * the QHY drivers had NO automated connect coverage of any kind — neither
 * test_qhy_camera.cpp nor test_qhy_filterwheel.cpp ever reached
 * set_connected(true).
 *
 * Capabilities:
 * - Fault injection: add a method name to `throw_from` and that call throws
 *   AlpacaException(DriverException).
 * - `sdk_resource_available = false` makes the three resource-dependent entry
 *   points (enumerate_cameras, get_camera_model, open_camera) throw the
 *   wrapper's "QHY SDK resource not initialized" DriverException — the path
 *   that on real hardware can only be reached by crashing.
 * - Canned devices: fill `cameras`; enumeration returns them verbatim and
 *   opens resolve ids against them.
 * - Ref-counted opens per camera_id, matching the wrapper's open_count: the
 *   camera driver and the CFW driver share ONE handle for one physical
 *   device. `physical_opens`/`physical_closes` count real transitions;
 *   `underflow_closes` counts closes of a camera that was not open (must stay
 *   0 in a correct driver).
 * - Scripted wheel positions: `cfw_position_script` is consumed one read at a
 *   time (last value repeats), so a homing/transit sequence is testable. -1
 *   means "still moving", matching GetQHYCCDCFWStatus.
 *
 * TWO RULES THIS FAKE MUST KEEP (they are not stylistic):
 *
 * 1. NOTHING HERE MAY BLOCK. The camera driver's exposure, temperature and
 *    cooler-off workers join with a bounded timeout and DETACH on expiry, and
 *    its pulse-guide worker is detached by design. A fake that blocks turns
 *    those into detached threads still calling into it after the test body
 *    has moved on — i.e. a use-after-free of the fake itself.
 * 2. THE FAKE MUST OUTLIVE EVERY DRIVER BUILT ON IT, including those
 *    detachable workers, which hold a QHYSDK* rather than `this`. Declare the
 *    fake before the driver (locals destroy in reverse order); never stash a
 *    driver beyond the fake's scope.
 *
 * default_camera() reports NO cooler. That is deliberate: has_cooler starts
 * the driver's telemetry thread, whose loop sleeps 1s between polls, so every
 * disconnect then blocks up to ~1s in the join. Use default_cooled_camera()
 * when the thermal paths are what's under test, and keep it out of anything
 * that connects in a loop.
 *
 * Not thread-hardened, by design — wrap it in LockedQHYSDK for the [stress]
 * suite so ThreadSanitizer reports point at driver code, not at this file.
 */
class FakeQHYSDK : public vendor::qhy::QHYSDK {
public:
    using QHYCameraInfo = vendor::qhy::QHYCameraInfo;
    using QHYControlRange = vendor::qhy::QHYControlRange;

    // --- scripting knobs ---------------------------------------------------
    std::set<std::string> throw_from;
    bool sdk_resource_available = true;
    std::vector<QHYCameraInfo> cameras;
    // Controls the drivers probe. CFWPORT present by default so the filter
    // wheel connects; add/remove to steer capability branches.
    std::set<int> controls_available{
        vendor::qhy::control::BITS16,  vendor::qhy::control::BIN1X1,      vendor::qhy::control::BIN2X2,
        vendor::qhy::control::GAIN,    vendor::qhy::control::OFFSET,      vendor::qhy::control::EXPOSURE,
        vendor::qhy::control::CFWPORT, vendor::qhy::control::CFWSLOTSNUM,
    };
    std::map<int, double> params{
        {vendor::qhy::control::GAIN, 10.0},         {vendor::qhy::control::OFFSET, 20.0},
        {vendor::qhy::control::EXPOSURE, 100000.0}, {vendor::qhy::control::CURTEMP, -5.0},
        {vendor::qhy::control::CURPWM, 30.0},       {vendor::qhy::control::CFWSLOTSNUM, 5.0},
    };
    std::map<int, QHYControlRange> param_ranges{
        {vendor::qhy::control::GAIN, {0.0, 100.0, 1.0, true}},
        {vendor::qhy::control::OFFSET, {0.0, 255.0, 1.0, true}},
        {vendor::qhy::control::EXPOSURE, {100.0, 3600000000.0, 100.0, true}},
    };
    std::vector<std::string> readout_modes{"Full Resolution", "Linearity HDR"};
    // Consumed by get_cfw_position; -1 = in motion. Last entry repeats.
    std::deque<int> cfw_position_script;
    bool read_directly = false;  // start_single_frame's return
    bool frame_ok = true;        // get_single_frame's return

    // --- observability -----------------------------------------------------
    std::map<std::string, int> calls;
    int physical_opens = 0;
    int physical_closes = 0;
    int underflow_closes = 0;
    int init_calls = 0;
    std::string last_opened_id;
    std::string last_guide_id;
    uint32_t last_guide_direction = 0;
    uint16_t last_guide_duration_ms = 0;
    double last_temp_target = 0.0;
    int last_cfw_target = -1;

    int ref_count(const std::string& id) const {
        auto it = ref_counts_.find(id);
        return it == ref_counts_.end() ? 0 : it->second;
    }

    /// Thread-safe view of `calls` — driver workers hit the fake concurrently
    /// with the test body.
    int call_count(const char* fn) const {
        std::lock_guard<std::mutex> lock(sync_.calls_mutex);
        auto it = calls.find(fn);
        return it == calls.end() ? 0 : it->second;
    }

    /// A plausible uncooled mono camera, sized small so get_mem_length() and
    /// the driver's frame buffer stay cheap under a storm.
    static QHYCameraInfo default_camera(const std::string& id, const std::string& model) {
        QHYCameraInfo info;
        info.camera_id = id;
        info.model = model;
        info.max_width = 64;
        info.max_height = 48;
        info.pixel_size_x_um = 3.76;
        info.pixel_size_y_um = 3.76;
        info.bpp = 16;
        info.is_color = false;
        info.bayer_pattern = 0;
        info.has_cooler = false;
        info.has_st4_port = true;
        info.has_shutter = false;
        return info;
    }

    /// Same, but with the TEC — starts the driver's telemetry and temp-control
    /// threads. See the class comment before using this in a connect loop.
    static QHYCameraInfo default_cooled_camera(const std::string& id, const std::string& model) {
        QHYCameraInfo info = default_camera(id, model);
        info.has_cooler = true;
        return info;
    }

    // --- QHYSDK implementation ---------------------------------------------
    std::vector<QHYCameraInfo> enumerate_cameras() override {
        hit("enumerate_cameras");
        require_resource();
        return cameras;
    }

    bool get_camera_model(const std::string& camera_id, std::string& model) override {
        hit("get_camera_model");
        require_resource();
        for (const auto& cam : cameras) {
            if (cam.camera_id == camera_id) {
                model = cam.model;
                return true;
            }
        }
        return false;
    }

    void open_camera(const std::string& camera_id) override {
        hit("open_camera");
        require_resource();
        if (!known_id(camera_id)) {
            // Matches QHYSDKWrapper::open_camera(): OpenQHYCCD returning null
            // for an id it doesn't recognize throws DriverException, not
            // NotConnected -- the real SDK has no concept of "not connected"
            // at this call, only "the open failed".
            throw AlpacaException("fake: unknown QHY camera id '" + camera_id + "'", AlpacaError::DriverException);
        }
        last_opened_id = camera_id;
        auto& count = ref_counts_[camera_id];
        if (count == 0) {
            ++physical_opens;
        }
        ++count;
    }

    void init_camera(const std::string& camera_id) override {
        hit("init_camera");
        require_open(camera_id);
        ++init_calls;
    }

    void close_camera(const std::string& camera_id) override {
        hit("close_camera");
        auto it = ref_counts_.find(camera_id);
        if (it == ref_counts_.end() || it->second <= 0) {
            ++underflow_closes;
            return;
        }
        if (--it->second == 0) {
            ++physical_closes;
            ref_counts_.erase(it);
        }
    }

    void register_exposure_worker(const std::string& camera_id,
                                  std::shared_ptr<std::atomic<bool>> running_flag) override {
        hit("register_exposure_worker");
        exposure_workers_[camera_id] = std::move(running_flag);
    }

    bool get_chip_info(const std::string& camera_id, QHYCameraInfo& info) override {
        hit("get_chip_info");
        require_open(camera_id);
        // QHYSDKWrapper::get_chip_info() never writes info.model at all -- the
        // driver gets the model exclusively from the separate
        // get_camera_model() call, made earlier in the connect sequence.
        // Back-filling it here (an earlier version of this fake did) would
        // hide a regression that dropped that call: the test would still see
        // a correct name under the fake and an empty one on real hardware.
        const std::string model = info.model;
        for (const auto& cam : cameras) {
            if (cam.camera_id == camera_id) {
                info = cam;
                info.model = model;
                return true;
            }
        }
        return false;
    }

    bool is_control_available(const std::string& camera_id, int control_id) override {
        hit("is_control_available");
        require_open(camera_id);
        return controls_available.count(control_id) != 0;
    }

    double get_param(const std::string& camera_id, int control_id) override {
        hit("get_param");
        require_open(camera_id);
        auto it = params.find(control_id);
        return it == params.end() ? 0.0 : it->second;
    }

    QHYControlRange get_param_range(const std::string& camera_id, int control_id) override {
        hit("get_param_range");
        require_open(camera_id);
        auto it = param_ranges.find(control_id);
        if (it == param_ranges.end()) {
            return QHYControlRange{0.0, 0.0, 0.0, false};
        }
        return it->second;
    }

    void set_param(const std::string& camera_id, int control_id, double value) override {
        hit("set_param");
        require_open(camera_id);
        params[control_id] = value;
    }

    void set_resolution(const std::string& camera_id, uint32_t start_x, uint32_t start_y, uint32_t width,
                        uint32_t height) override {
        hit("set_resolution");
        require_open(camera_id);
        roi_ = {start_x, start_y, width, height};
    }

    void set_bin_mode(const std::string& camera_id, uint32_t wbin, uint32_t hbin) override {
        hit("set_bin_mode");
        require_open(camera_id);
        wbin_ = wbin;
        hbin_ = hbin;
    }

    void set_bits_mode(const std::string& camera_id, uint32_t bits) override {
        hit("set_bits_mode");
        require_open(camera_id);
        bits_ = bits;
    }

    uint32_t get_mem_length(const std::string& camera_id) override {
        hit("get_mem_length");
        require_open(camera_id);
        const uint32_t bytes_per_px = (bits_ > 8) ? 2U : 1U;
        return roi_.width * roi_.height * bytes_per_px;
    }

    bool start_single_frame(const std::string& camera_id) override {
        hit("start_single_frame");
        require_open(camera_id);
        return read_directly;
    }

    bool get_single_frame(const std::string& camera_id, uint8_t* buffer, uint32_t& width, uint32_t& height,
                          uint32_t& bpp, uint32_t& channels) override {
        hit("get_single_frame");
        require_open(camera_id);
        // Returns IMMEDIATELY — see rule 1 in the class comment. The real call
        // blocks for the whole exposure; a fake that did would strand the
        // driver's detachable exposure worker.
        width = roi_.width;
        height = roi_.height;
        bpp = bits_;
        channels = 1;
        if (frame_ok && buffer != nullptr) {
            const uint32_t bytes_per_px = (bits_ > 8) ? 2U : 1U;
            std::memset(buffer, 0, static_cast<std::size_t>(width) * height * bytes_per_px);
        }
        return frame_ok;
    }

    void cancel_exposure(const std::string& camera_id) override {
        hit("cancel_exposure");
        require_open(camera_id);
    }

    void guide(const std::string& camera_id, uint32_t qhy_direction, uint16_t duration_ms) override {
        hit("guide");
        require_open(camera_id);
        // The real call blocks for the full pulse duration; this must not.
        last_guide_id = camera_id;
        last_guide_direction = qhy_direction;
        last_guide_duration_ms = duration_ms;
    }

    void control_temp(const std::string& camera_id, double target_temp_c) override {
        hit("control_temp");
        require_open(camera_id);
        last_temp_target = target_temp_c;
        params[vendor::qhy::control::CURTEMP] = target_temp_c;
    }

    void move_cfw(const std::string& camera_id, int position) override {
        hit("move_cfw");
        require_open(camera_id);
        last_cfw_target = position;
        cfw_position_ = position;
        cfw_position_script.clear();
    }

    int get_cfw_position(const std::string& camera_id) override {
        hit("get_cfw_position");
        require_open(camera_id);
        if (cfw_position_script.empty()) {
            return cfw_position_;
        }
        cfw_position_ = cfw_position_script.front();
        if (cfw_position_script.size() > 1) {
            cfw_position_script.pop_front();  // last entry repeats forever
        }
        return cfw_position_;
    }

    uint32_t get_num_readout_modes(const std::string& camera_id) override {
        hit("get_num_readout_modes");
        require_open(camera_id);
        return static_cast<uint32_t>(readout_modes.size());
    }

    std::string get_readout_mode_name(const std::string& camera_id, uint32_t mode_index) override {
        hit("get_readout_mode_name");
        require_open(camera_id);
        if (mode_index >= readout_modes.size()) {
            throw AlpacaException("fake: readout mode index out of range", AlpacaError::InvalidValue);
        }
        return readout_modes[mode_index];
    }

    void set_readout_mode(const std::string& camera_id, uint32_t mode_index) override {
        hit("set_readout_mode");
        require_open(camera_id);
        if (mode_index >= readout_modes.size()) {
            throw AlpacaException("fake: readout mode index out of range", AlpacaError::InvalidValue);
        }
        readout_mode_ = mode_index;
    }

    std::string get_sdk_version() override {
        hit("get_sdk_version");
        return "fake-qhy-1.0";
    }

private:
    struct ROI {
        uint32_t start_x{};
        uint32_t start_y{};
        uint32_t width{64};
        uint32_t height{48};
    };

    // A std::mutex has no move constructor, so the naive `std::mutex
    // calls_mutex;` member would make FakeQHYSDK non-movable -- and
    // test_qhy_fake_sdk.cpp's own static_assert, plus every helper that
    // builds a fake and returns it by value, depends on movability. A
    // std::unique_ptr<Sync> wrapper would restore movability but leaves the
    // moved-from object's sync_ null, so a stray hit()/call_count() on it
    // (a caller holding a moved-from fake past the move, say) segfaults
    // instead of misbehaving loudly. Sync's own hand-written move
    // constructor sidesteps both: there is nothing meaningful to transfer
    // out of a mutex-only struct, so moving one just re-defaults a fresh
    // mutex in place, and the moved-from object stays fully usable.
    struct Sync {
        mutable std::mutex calls_mutex;

        Sync() = default;
        Sync(Sync&&) noexcept {}
        Sync& operator=(Sync&&) noexcept { return *this; }
        Sync(const Sync&) = delete;
        Sync& operator=(const Sync&) = delete;
    };
    Sync sync_;

    void hit(const char* fn) {
        {
            std::lock_guard<std::mutex> lock(sync_.calls_mutex);
            ++calls[fn];
        }
        if (throw_from.count(fn) != 0) {
            throw AlpacaException(std::string("fake: injected failure in ") + fn, AlpacaError::DriverException);
        }
    }

    void require_resource() const {
        if (!sdk_resource_available) {
            throw AlpacaException("QHY SDK resource not initialized", AlpacaError::DriverException);
        }
    }

    void require_open(const std::string& camera_id) const {
        auto it = ref_counts_.find(camera_id);
        if (it == ref_counts_.end() || it->second <= 0) {
            throw AlpacaException("QHY camera not open: " + camera_id, AlpacaError::NotConnected);
        }
    }

    bool known_id(const std::string& id) const {
        for (const auto& cam : cameras) {
            if (cam.camera_id == id) {
                return true;
            }
        }
        return false;
    }

    std::map<std::string, int> ref_counts_;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> exposure_workers_;
    ROI roi_{};
    uint32_t wbin_ = 1;
    uint32_t hbin_ = 1;
    uint32_t bits_ = 16;
    uint32_t readout_mode_ = 0;
    int cfw_position_ = 0;
};

}  // namespace alpacacore::test
