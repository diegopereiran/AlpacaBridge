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

#include <alpacacore/vendor/qhy/qhy_sdk_wrapper.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace alpacacore::test {

/**
 * Thread-safe decorator over any QHYSDK — every call forwards to the inner
 * implementation under one mutex.
 *
 * Exists for the concurrency stress harness (issue #101): FakeQHYSDK is
 * deliberately NOT thread-hardened (single-connect-path tests don't need it),
 * but the stress tests hammer one driver from many threads, so unguarded fake
 * state would light up ThreadSanitizer with races in TEST code and drown out
 * the driver races the harness exists to catch. The real wrapper serializes
 * internally (a bookkeeping mutex plus a per-handle call_mutex), so production
 * drivers never need this.
 *
 * The mutex is a leaf: the inner SDK never calls back into this interface.
 *
 * This decorator is only sound because no QHYSDK method blocks in a fake —
 * holding one mutex across a blocking call would serialize the storm into a
 * queue. See rule 1 in FakeQHYSDK's class comment.
 *
 * One production rule this decorator deliberately does NOT preserve:
 * QHYSDKWrapper::cancel_exposure() skips the per-handle call_mutex on purpose
 * (AGENTS.md, "Every SDK call is serialized against its physical handle"),
 * because its whole job is to interrupt a GetQHYCCDSingleFrame blocked on
 * another thread — serializing it the same way as every other call would
 * deadlock it behind the very call it needs to cancel. Here, cancel_exposure()
 * goes through the SAME mutex as everything else. That is safe only as long
 * as the rule above holds (no fake method blocks) — the moment a fake gains a
 * deliberate delay (to test a real timeout path, say), this decorator turns
 * that production safety valve into a deadlock instead of a no-op.
 */
class LockedQHYSDK : public vendor::qhy::QHYSDK {
public:
    using QHYCameraInfo = vendor::qhy::QHYCameraInfo;
    using QHYControlRange = vendor::qhy::QHYControlRange;

    explicit LockedQHYSDK(QHYSDK& inner) : inner_(inner) {}

    std::vector<QHYCameraInfo> enumerate_cameras() override {
        return locked([&] { return inner_.enumerate_cameras(); });
    }
    bool get_camera_model(const std::string& camera_id, std::string& model) override {
        return locked([&] { return inner_.get_camera_model(camera_id, model); });
    }

    void open_camera(const std::string& camera_id) override {
        locked([&] { inner_.open_camera(camera_id); });
    }
    void init_camera(const std::string& camera_id) override {
        locked([&] { inner_.init_camera(camera_id); });
    }
    void close_camera(const std::string& camera_id) override {
        locked([&] { inner_.close_camera(camera_id); });
    }
    void register_exposure_worker(const std::string& camera_id,
                                  std::shared_ptr<std::atomic<bool>> running_flag) override {
        locked([&] { inner_.register_exposure_worker(camera_id, std::move(running_flag)); });
    }

    bool get_chip_info(const std::string& camera_id, QHYCameraInfo& info) override {
        return locked([&] { return inner_.get_chip_info(camera_id, info); });
    }

    bool is_control_available(const std::string& camera_id, int control_id) override {
        return locked([&] { return inner_.is_control_available(camera_id, control_id); });
    }
    double get_param(const std::string& camera_id, int control_id) override {
        return locked([&] { return inner_.get_param(camera_id, control_id); });
    }
    QHYControlRange get_param_range(const std::string& camera_id, int control_id) override {
        return locked([&] { return inner_.get_param_range(camera_id, control_id); });
    }
    void set_param(const std::string& camera_id, int control_id, double value) override {
        locked([&] { inner_.set_param(camera_id, control_id, value); });
    }

    void set_resolution(const std::string& camera_id, uint32_t start_x, uint32_t start_y, uint32_t width,
                        uint32_t height) override {
        locked([&] { inner_.set_resolution(camera_id, start_x, start_y, width, height); });
    }
    void set_bin_mode(const std::string& camera_id, uint32_t wbin, uint32_t hbin) override {
        locked([&] { inner_.set_bin_mode(camera_id, wbin, hbin); });
    }
    void set_bits_mode(const std::string& camera_id, uint32_t bits) override {
        locked([&] { inner_.set_bits_mode(camera_id, bits); });
    }
    uint32_t get_mem_length(const std::string& camera_id) override {
        return locked([&] { return inner_.get_mem_length(camera_id); });
    }

    bool start_single_frame(const std::string& camera_id) override {
        return locked([&] { return inner_.start_single_frame(camera_id); });
    }
    bool get_single_frame(const std::string& camera_id, uint8_t* buffer, uint32_t& width, uint32_t& height,
                          uint32_t& bpp, uint32_t& channels) override {
        return locked([&] { return inner_.get_single_frame(camera_id, buffer, width, height, bpp, channels); });
    }
    void cancel_exposure(const std::string& camera_id) override {
        locked([&] { inner_.cancel_exposure(camera_id); });
    }

    void guide(const std::string& camera_id, uint32_t qhy_direction, uint16_t duration_ms) override {
        locked([&] { inner_.guide(camera_id, qhy_direction, duration_ms); });
    }

    void control_temp(const std::string& camera_id, double target_temp_c) override {
        locked([&] { inner_.control_temp(camera_id, target_temp_c); });
    }

    void move_cfw(const std::string& camera_id, int position) override {
        locked([&] { inner_.move_cfw(camera_id, position); });
    }
    int get_cfw_position(const std::string& camera_id) override {
        return locked([&] { return inner_.get_cfw_position(camera_id); });
    }

    uint32_t get_num_readout_modes(const std::string& camera_id) override {
        return locked([&] { return inner_.get_num_readout_modes(camera_id); });
    }
    std::string get_readout_mode_name(const std::string& camera_id, uint32_t mode_index) override {
        return locked([&] { return inner_.get_readout_mode_name(camera_id, mode_index); });
    }
    void set_readout_mode(const std::string& camera_id, uint32_t mode_index) override {
        locked([&] { inner_.set_readout_mode(camera_id, mode_index); });
    }

    std::string get_sdk_version() override {
        return locked([&] { return inner_.get_sdk_version(); });
    }

private:
    template <typename Fn>
    auto locked(Fn&& fn) -> decltype(fn()) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn();
    }

    QHYSDK& inner_;
    std::mutex mutex_;
};

}  // namespace alpacacore::test
