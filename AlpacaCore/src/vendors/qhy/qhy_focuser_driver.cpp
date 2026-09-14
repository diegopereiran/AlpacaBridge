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
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/qhy/qhy_focuser_driver.h>
#include <alpacacore/vendor/qhy/qhy_qfocuser_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace alpacacore::vendor::qhy {

namespace {
constexpr const char* kLogTag = "QHY";
// INDI gates the 12 V hold settings on this supply voltage: below it the
// unit is USB-powered and cannot drive the hold current.
constexpr double kHoldVoltageThreshold = 11.5;
// The protocol has no "moving" flag. A move is considered in progress while
// the position is still short of the target AND has changed within this
// window; a stalled or limit-clamped move therefore reads as stopped after
// one grace period instead of forever.
constexpr auto kMotionGrace = std::chrono::milliseconds(1500);
}  // namespace

class QhyFocuserDriver : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    ALPACA_EXPOSE_CONNECT_ERROR()

    QhyFocuserDriver(int device_number, QFocuserConnectionConfig config, QFocuserSettings settings)
        : AsyncConnectable("QHY"),
          device_number_(device_number),
          config_(std::move(config)),
          settings_(std::move(settings)),
          connected_(false) {
        if (settings_.max_step < 1) settings_.max_step = 1;
    }

    ~QhyFocuserDriver() override {
        shutdown_connection();
        if (connected_.load()) {
            try {
                set_connected(false);  // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kLogTag, "Error during Q-Focuser destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override { return "QHY Q-Focuser"; }
    DeviceType get_device_type() const override { return DeviceType::Focuser; }
    std::string get_unique_id() const override { return "QHY_QFOCUSER_" + std::to_string(device_number_); }
    std::string get_description() const override { return "QHY Q-Focuser Driver"; }
    std::string get_driver_info() const override { return "AlpacaCore QHY Q-Focuser Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 4; }

    // Firmware date + board revision from the connect handshake; web UI
    // only, never DriverInfo. Own mutex: set on the connection thread while
    // the base may be joining it.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_.empty()) return std::nullopt;
        return firmware_;
    }

    bool get_connected() const override { return connected_.load(); }
    void connect() override { start_connection_task(true); }
    void disconnect() override {
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN(kLogTag, "Q-Focuser disconnect error: " + std::string(e.what()));
        }
    }
    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // Serialize whole transitions against each other, matching the sibling
        // serial drivers (Gemini focuser open-astro#333, WandererAstro). The
        // router serializes connection ops per device, but the [stress] harness
        // calls set_connected() concurrently from lifecycle threads; without
        // this, two interleaved transitions can leave the wrapper open while
        // the driver reports disconnected, after which every reconnect throws
        // InvalidOperation "already connected". Deliberately NOT the base's
        // connection mutex and not firmware_mutex_ — it guards set_connected()
        // against set_connected() only and leaves the getters free.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) return;
        if (connected && consume_pending_disconnect(connected_.load())) return;
        if (connected == connected_.load()) return;

        if (connected) {
            QFocuserDeviceInfo info = protocol_.connect(config_);
            try {
                apply_settings();
            } catch (...) {
                protocol_.disconnect();
                throw;
            }
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_ = std::to_string(info.firmware);
                if (info.board_version > 0) firmware_ += " (board " + std::to_string(info.board_version) + ")";
            }
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                target_.reset();
                position_cache_.reset();
                telemetry_cache_.reset();
            }
            connected_.store(true);
            ALPACA_LOG_INFO(kLogTag, "Q-Focuser connected (firmware " + std::to_string(info.firmware) + ")");
        } else {
            // Driver state first, port close second (AGENTS.md, issue #387): a
            // throwing close must not leave the driver reporting connected on a
            // closed port. disconnect() cannot throw today; the order is the
            // contract, not the current wrapper's behaviour. Clear firmware
            // first so the store(false) is never observable beside a stale
            // firmware string.
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_.clear();
            }
            connected_.store(false);
            protocol_.disconnect();
            invalidate_caches();
            ALPACA_LOG_INFO(kLogTag, "Q-Focuser disconnected");
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

    // --- Focuser interface ---

    bool get_absolute() const override { return true; }

    bool get_is_moving() const override {
        ensure_connected();
        return sample_position().moving;
    }

    int get_max_step() const override { return settings_.max_step; }
    int get_max_increment() const override { return settings_.max_step; }

    int get_position() const override {
        ensure_connected();
        return sample_position().position;
    }

    double get_step_size() const override {
        // Step size in microns depends on the drawtube the motor drives; the
        // protocol does not expose it. Connection check first (open-astro#309).
        ensure_connected();
        throw AlpacaException("Step size not available for this focuser", AlpacaError::PropertyNotImplemented);
    }

    bool get_temp_comp_available() const override {
        ensure_connected();
        return false;
    }
    bool get_temp_comp() const override {
        ensure_connected();
        return false;
    }
    void set_temp_comp(bool) override {
        ensure_connected();
        throw AlpacaException("Temperature compensation not supported", AlpacaError::NotImplemented);
    }

    double get_temperature() const override {
        ensure_connected();
        const QFocuserTelemetry t = cached_telemetry();
        return settings_.temperature_source == "chip" ? t.chip_temperature_c : t.external_temperature_c;
    }

    void halt() override {
        ensure_connected();
        // Hold status_mutex_ across the wire command so the command and the
        // target_ update are atomic against a concurrent move()/halt() (Alpaca
        // does not serialize device methods). Lock order is status_mutex_ ->
        // wrapper mutex, matching sample_position(), so no deadlock.
        std::lock_guard<std::mutex> lock(status_mutex_);
        protocol_.halt();
        target_.reset();  // no target => not moving, whatever position the motor stopped at
        position_cache_.reset();
    }

    void move(int position) override {
        ensure_connected();
        if (position < 0 || position > settings_.max_step) {
            throw AlpacaException("Focuser position out of range", AlpacaError::InvalidValue);
        }
        // Hold status_mutex_ across move_to() so two concurrent moves cannot
        // reach the device in one order while target_ records the other (see
        // halt() for the lock-order note).
        std::lock_guard<std::mutex> lock(status_mutex_);
        protocol_.move_to(position);
        target_ = position;
        last_change_ = std::chrono::steady_clock::now();
        position_cache_.reset();
    }

private:
    struct PositionSample {
        int position = 0;
        bool moving = false;
    };

    // One {"cmd_id":5} round trip at 9600 baud is ~35 ms; DeviceState reads
    // IsMoving then Position back to back against a 0.1 s FAST target, so a
    // burst of reads is served from one poll. Dropped by move()/halt().
    static constexpr auto kPositionCacheTtl = std::chrono::milliseconds(100);
    static constexpr auto kTelemetryCacheTtl = std::chrono::milliseconds(1000);

    PositionSample sample_position() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!position_cache_ || now - position_cache_time_ > kPositionCacheTtl) {
            const std::int32_t pos = const_cast<QhyFocuserDriver*>(this)->protocol_.get_position();
            if (!position_cache_ || pos != position_cache_->position) {
                last_change_ = now;
            }
            PositionSample sample;
            sample.position = pos;
            sample.moving = target_.has_value() && pos != *target_ && (now - last_change_) < kMotionGrace;
            if (target_.has_value() && pos == *target_) {
                target_.reset();  // arrived
            }
            position_cache_ = sample;
            position_cache_time_ = now;
        }
        return *position_cache_;
    }

    QFocuserTelemetry cached_telemetry() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!telemetry_cache_ || now - telemetry_cache_time_ > kTelemetryCacheTtl) {
            telemetry_cache_ = const_cast<QhyFocuserDriver*>(this)->protocol_.get_telemetry();
            telemetry_cache_time_ = now;
        }
        return *telemetry_cache_;
    }

    void invalidate_caches() {
        std::lock_guard<std::mutex> lock(status_mutex_);
        position_cache_.reset();
        telemetry_cache_.reset();
        target_.reset();
    }

    // Push the user's settings to the firmware right after the handshake.
    // Reverse and speed always; the hold settings only on a 12 V supply.
    void apply_settings() {
        protocol_.set_reverse(settings_.reverse);
        protocol_.set_speed(settings_.speed);

        const QFocuserTelemetry t = protocol_.get_telemetry();
        if (t.voltage_v > kHoldVoltageThreshold) {
            protocol_.set_hold_current(settings_.hold_ihold, settings_.hold_irun);
            protocol_.set_hold_force(settings_.hold_force);
        } else if (settings_.hold_force) {
            ALPACA_LOG_WARN(kLogTag, "Q-Focuser supply is " + std::to_string(t.voltage_v) +
                                         " V; hold force needs a 12 V supply and was not enabled");
        }
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        }
    }

    int device_number_;
    QFocuserConnectionConfig config_;
    QFocuserSettings settings_;
    std::atomic<bool> connected_;
    QFocuserProtocolWrapper protocol_;

    // Serializes whole connect/disconnect transitions; see set_connected().
    // Separate from firmware_mutex_/status_mutex_ and the base connection mutex.
    std::mutex transition_mutex_;
    mutable std::mutex firmware_mutex_;
    std::string firmware_;

    mutable std::mutex status_mutex_;
    mutable std::optional<int> target_;
    mutable std::chrono::steady_clock::time_point last_change_{};
    mutable std::optional<PositionSample> position_cache_;
    mutable std::chrono::steady_clock::time_point position_cache_time_{};
    mutable std::optional<QFocuserTelemetry> telemetry_cache_;
    mutable std::chrono::steady_clock::time_point telemetry_cache_time_{};
};

std::unique_ptr<FocuserDriver> create_qhy_focuser(int device_number, const std::string& serial_port,
                                                  const QFocuserSettings& settings) {
    QFocuserConnectionConfig config;
    config.serial_port = serial_port;
    return std::make_unique<QhyFocuserDriver>(device_number, std::move(config), settings);
}

std::unique_ptr<FocuserDriver> create_qhy_focuser_by_index(int device_number, int focuser_index,
                                                           const QFocuserSettings& settings) {
    auto ports = enumerate_qfocuser_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("QHY Q-Focuser"), AlpacaError::NotConnected);
    }
    if (focuser_index < 0 || focuser_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Focuser index " + std::to_string(focuser_index) + " out of range (detected " +
                                  std::to_string(ports.size()) + ")",
                              AlpacaError::InvalidValue);
    }
    const auto& port = ports[static_cast<std::size_t>(focuser_index)];
    ALPACA_LOG_INFO(kLogTag, "Auto-detected Q-Focuser at " + port.port_path + " (firmware " +
                                 std::to_string(port.info.firmware) + ")");
    return create_qhy_focuser(device_number, port.port_path, settings);
}

}  // namespace alpacacore::vendor::qhy
