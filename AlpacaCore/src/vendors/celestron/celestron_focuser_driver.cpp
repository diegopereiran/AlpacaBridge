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

// Celestron Focus Motor on its own USB port (issue #896). The motor answers AUX
// frames as device 0x12 at 9600 baud, over the same framing and serial
// transport as the CGX direct-USB mount (CelestronCgxProtocol). Command set
// from the reporter's tested script (issue #896) and INDI's celestron SCT
// focuser driver, which agree:
//
//   0x01 MC_GET_POSITION       -> 24-bit raw encoder count
//   0x02 MC_GOTO_FAST  u24     -> empty ack
//   0x13 MC_SLEW_DONE          -> 0xFF when idle, anything else while moving
//   0x24 MC_MOVE_POS   rate 0  -> stops the motor (Halt)
//   0x2C FOC_GET_HS_POSITIONS  -> two big-endian u32: low and high limit;
//                                 high <= low means not calibrated
//   0xFE GET_VER               -> major, minor[, build hi, build lo]
//
// The raw count rises as the mirror moves toward the corrector, the reverse of
// a refractor's drawtube, so Position = high - raw (INDI's absPos): 0 is the
// high raw limit and MaxStep is high - low.

#include <alpacacore/async_connectable.h>
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/link_health.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/state_snapshot.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/celestron/celestron_cgx_protocol_wrapper.h>
#include <alpacacore/vendor/celestron/celestron_focuser_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace alpacacore::vendor::celestron {

namespace {

constexpr std::uint8_t kApp = 0x20;
constexpr std::uint8_t kFocuser = 0x12;
constexpr std::uint8_t kGetPosition = 0x01;
constexpr std::uint8_t kGotoFast = 0x02;
constexpr std::uint8_t kSlewDone = 0x13;
constexpr std::uint8_t kMovePositive = 0x24;
constexpr std::uint8_t kGetLimits = 0x2c;
constexpr std::uint8_t kGetVersion = 0xfe;
constexpr std::uint32_t kMaxRawPosition = 0x00ffffffU;

constexpr auto kExchangeTimeout = std::chrono::milliseconds(500);
// One poll is two exchanges of about 10 bytes each way at 9600 baud, so the
// period leaves the wire mostly free for Move/Halt (same shape as Gemini).
constexpr auto kStatePollPeriod = std::chrono::milliseconds(250);
constexpr auto kSnapshotMaxAge = 2 * kStatePollPeriod;
constexpr int kFaultThreshold = 3;

std::uint32_t read_be(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t count) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        value = (value << 8) | bytes[offset + i];
    }
    return value;
}

}  // namespace

class CelestronFocuserDriver final : public FocuserDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    CelestronFocuserDriver(int device_number, std::string port_path,
                           util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("Celestron Focuser"),
          clock_(clock),
          device_number_(device_number),
          port_path_(std::move(port_path)) {}

    ~CelestronFocuserDriver() override {
        // Blocks new connection tasks, then joins the in-flight one; MUST be
        // first, before members the task touches are destroyed (base contract).
        shutdown_connection();
        state_poll_.cancel_all_and_join();
        if (connected_.load()) {
            try {
                // Qualified: virtual dispatch is gone in a destructor anyway;
                // saying so explicitly keeps clang-analyzer's VirtualCall happy.
                CelestronFocuserDriver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("Celestron", "Error during focuser destruction: " + std::string(e.what()));
            }
        }
    }

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override { return "Celestron Focus Motor"; }
    DeviceType get_device_type() const override { return DeviceType::Focuser; }
    std::string get_unique_id() const override { return "CELESTRON_FOCUSER_" + std::to_string(device_number_); }
    std::string get_description() const override { return "Celestron Focus Motor (USB)"; }
    std::string get_driver_info() const override { return "AlpacaCore Celestron Focuser Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 4; }

    // Motor firmware captured at connect; web UI only, never DriverInfo.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_.empty()) {
            return std::nullopt;
        }
        return firmware_;
    }

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }

    void disconnect() override {
        stop_connection_thread();
        try {
            set_connected(false);
        } catch (const std::exception& e) {
            ALPACA_LOG_WARN("Celestron", "Focuser disconnect error: " + std::string(e.what()));
        }
    }

    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        // One transition at a time (open-astro#333); getters never take this.
        std::lock_guard<std::mutex> transition(transition_mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected == connected_.load()) {
            return;
        }

        if (connected) {
            protocol_.connect_serial(port_path_, AuxLinkSpeed::Baud9600);
            try {
                read_limits();
            } catch (...) {
                protocol_.disconnect();
                throw;
            }
            read_firmware();
            snapshot_.reset();
            {
                std::lock_guard<std::mutex> lock(health_mutex_);
                health_.reset();
            }
            // First frame before Connected reads true, so a getter has an
            // answer the moment Connect returns.
            poll_state_once();
            connected_.store(true);
            start_state_poll();
            ALPACA_LOG_INFO("Celestron", "Focuser connected (limits " + std::to_string(low_limit_.load()) + ".." +
                                             std::to_string(high_limit_.load()) + ")");
        } else {
            // Driver state first, transport close second (AGENTS.md, issue #387).
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_.clear();
            }
            connected_.store(false);
            state_poll_.cancel_all_and_join();
            snapshot_.reset();
            {
                std::lock_guard<std::mutex> lock(health_mutex_);
                health_.reset();
            }
            protocol_.disconnect();
            ALPACA_LOG_INFO("Celestron", "Focuser disconnected");
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

    bool get_is_moving() const override { return served_frame().moving; }

    int get_max_step() const override {
        ensure_connected();
        return static_cast<int>(high_limit_.load() - low_limit_.load());
    }

    int get_max_increment() const override { return get_max_step(); }

    int get_position() const override { return served_frame().position; }

    double get_step_size() const override {
        // The motor reports encoder counts only; microns per count depend on
        // the telescope it drives.
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
        throw AlpacaException("The Celestron focus motor has no temperature sensor",
                              AlpacaError::PropertyNotImplemented);
    }

    void halt() override {
        ensure_connected();
        // Never refused for a link fault: Halt is the safety stop. A link left
        // desynchronized by an earlier timeout is resynchronized first (no
        // reopen); this sends a stop, never a repeat of the motion that timed out.
        resync_if_needed();
        exchange(kMovePositive, {0x00});
        snapshot_.write([](StateFrame& frame) { frame.moving = false; });
    }

    void move(int position) override {
        ensure_connected();
        throw_if_faulted();
        const int max_step = get_max_step();
        // ConformU expects graceful clamping, not an exception (as Gemini).
        if (position < 0) {
            position = 0;
        } else if (position > max_step) {
            position = max_step;
        }
        const std::uint32_t raw = high_limit_.load() - static_cast<std::uint32_t>(position);
        exchange(kGotoFast, {static_cast<std::uint8_t>(raw >> 16), static_cast<std::uint8_t>(raw >> 8),
                             static_cast<std::uint8_t>(raw)});
        // Write-through: IsMoving reads true right after a successful Move.
        snapshot_.write([](StateFrame& frame) { frame.moving = true; });
    }

private:
    struct StateFrame {
        int position = 0;
        bool moving = false;
    };

    std::vector<std::uint8_t> exchange(std::uint8_t command, std::vector<std::uint8_t> payload = {}) {
        return protocol_
            .exchange(
                AuxFrame{.source = kApp, .destination = kFocuser, .command = command, .payload = std::move(payload)},
                kExchangeTimeout)
            .payload;
    }

    // A timed-out exchange leaves the AUX stream desynchronized; resync()
    // flushes stale input on the port already held. It never reopens one, so a
    // Halt racing a disconnect cannot leave the port open behind it.
    void resync_if_needed() {
        if (!protocol_.is_healthy()) {
            protocol_.resync();
        }
    }

    void read_limits() {
        const auto reply = exchange(kGetLimits);
        if (reply.size() != 8) {
            throw AlpacaException(
                "Celestron focuser: limit query returned " + std::to_string(reply.size()) + " bytes, expected 8",
                AlpacaError::DriverException);
        }
        const std::uint32_t low = read_be(reply, 0, 4);
        const std::uint32_t high = read_be(reply, 4, 4);
        if (high <= low || high > kMaxRawPosition) {
            throw AlpacaException(
                "Celestron focus motor is not calibrated (limits " + std::to_string(low) + ".." + std::to_string(high) +
                    "); run the calibration from the hand control or the Celestron Focuser Utility, then connect again",
                AlpacaError::DriverException);
        }
        low_limit_.store(low);
        high_limit_.store(high);
    }

    // A failed version query must not fail the connect.
    void read_firmware() {
        std::string text;
        try {
            const auto reply = exchange(kGetVersion);
            if (reply.size() >= 2) {
                text = std::to_string(reply[0]) + "." + std::to_string(reply[1]);
                if (reply.size() >= 4) {
                    text += "." + std::to_string(read_be(reply, 2, 2));
                }
            }
        } catch (const std::exception&) {
            ALPACA_LOG_WARN("Celestron", "Focuser firmware query failed");
            resync_if_needed();
        }
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        firmware_ = text;
    }

    void throw_if_faulted() const {
        std::lock_guard<std::mutex> lock(health_mutex_);
        if (health_.faulted()) {
            throw AlpacaException("Celestron focuser communications compromised: " + health_.fault(),
                                  AlpacaError::DriverException);
        }
    }

    StateFrame served_frame() const {
        ensure_connected();
        throw_if_faulted();
        auto reading = snapshot_.read();
        if (!reading.has_value()) {
            // Connected but no frame: the connect's first poll was lost, or a
            // racing Move/Halt write dropped it. Measure the motor now rather
            // than wait up to a poll period. A disconnect in between makes the
            // exchange fail and the check below report NotConnected.
            const_cast<CelestronFocuserDriver*>(this)->poll_state_once();
            reading = snapshot_.read();
        }
        if (!reading.has_value()) {
            ensure_connected();
            throw_if_faulted();
            throw AlpacaException("Focuser state not available yet", AlpacaError::DriverException);
        }
        return reading->value;
    }

    void poll_state_once() {
        const auto token = snapshot_.begin_poll();
        StateFrame frame;
        try {
            resync_if_needed();
            const auto position = exchange(kGetPosition);
            if (position.size() != 3) {
                throw AlpacaException("position reply is not 24 bits", AlpacaError::DriverException);
            }
            const auto done = exchange(kSlewDone);
            if (done.size() != 1) {
                throw AlpacaException("slew-done reply is not one byte", AlpacaError::DriverException);
            }
            frame.position = static_cast<int>(static_cast<std::int64_t>(high_limit_.load()) -
                                              static_cast<std::int64_t>(read_be(position, 0, 3)));
            frame.moving = done[0] != 0xff;
        } catch (const std::exception&) {
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.note_failure("no valid reply to the state poll", kFaultThreshold, clock_.now())) {
                ALPACA_LOG_ERROR("Celestron", "Focuser communications compromised: " + health_.fault());
            } else {
                ALPACA_LOG_DEBUG("Celestron", "Focuser state poll failed; the frame keeps its age");
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(health_mutex_);
            if (health_.on_reply()) {
                ALPACA_LOG_INFO("Celestron", "Focuser communications restored");
            }
        }
        snapshot_.publish(token, frame);
    }

    void run_state_poll(util::OperationContext& ctx) {
        if (!ctx.wait_for(kStatePollPeriod)) {
            return;
        }
        for (;;) {
            const auto started = clock_.now();
            poll_state_once();
            const auto spent = clock_.now() - started;
            const auto ticks = spent / kStatePollPeriod + 1;
            if (!ctx.wait_for(ticks * kStatePollPeriod - spent)) {
                return;
            }
        }
    }

    void start_state_poll() {
        try {
            state_poll_.start([this](util::OperationContext& ctx) { run_state_poll(ctx); });
        } catch (const std::exception&) {
            ALPACA_LOG_ERROR("Celestron", "Focuser state poll did not start");
        }
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Focuser not connected", AlpacaError::NotConnected);
        }
    }

    util::TaskClock& clock_;
    int device_number_;
    std::string port_path_;
    std::atomic<bool> connected_{false};
    CelestronCgxProtocol protocol_;
    // Written at connect before connected_ is stored true; read-only after.
    std::atomic<std::uint32_t> low_limit_{0};
    std::atomic<std::uint32_t> high_limit_{0};
    mutable util::StateSnapshot<StateFrame> snapshot_{clock_, kSnapshotMaxAge};
    mutable std::mutex health_mutex_;
    util::PolledLinkHealth health_;
    util::OperationGeneration state_poll_generation_;
    util::AsyncOperation state_poll_{"Celestron focuser state poll", state_poll_generation_, clock_};
    // Own mutex, not the base's connection mutex (see the Gemini focuser).
    mutable std::mutex firmware_mutex_;
    std::string firmware_;
    std::mutex transition_mutex_;
};

std::unique_ptr<FocuserDriver> create_celestron_focuser(int device_number, const std::string& port_path) {
    return std::make_unique<CelestronFocuserDriver>(device_number, port_path);
}

std::unique_ptr<FocuserDriver> create_celestron_focuser(int device_number, const std::string& port_path,
                                                        util::TaskClock& clock) {
    return std::make_unique<CelestronFocuserDriver>(device_number, port_path, clock);
}

}  // namespace alpacacore::vendor::celestron
