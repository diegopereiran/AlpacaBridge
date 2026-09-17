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

// Protocol reference: QHY's published QHYCFW3 serial command table and INDI
// drivers/filter_wheel/qhycfw3.cpp, cross-checked against a 7-slot CFW3
// (firmware 20181114, CP2102 bridge) on 2026-09-15. Summary in
// AlpacaCore/external/QHY/QHYCFW3-USB-protocol.md.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_by_id_scan.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/qhy/qhy_cfw3_protocol_wrapper.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace alpacacore::vendor::qhy {

namespace {

constexpr const char* kLogTag = "QHY";
constexpr int kFirmwareReplyLen = 8;  // "yyyymmdd"
constexpr int kReadPollMs = 10;
// Replies carry no terminator, so a multi-byte reply (VRS) is complete when
// the line goes quiet. At 9600 baud a byte takes ~1 ms; 50 ms of silence is
// a comfortable end-of-reply mark and far below the arrival wait it paces.
constexpr int kReplyIdleMs = 50;
// The goto wait is sliced so the cancel flag is polled while the wheel turns.
constexpr int kMoveSliceMs = 100;
constexpr int kMaxSlot = 15;  // 'F'

std::string printable(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char ch : raw) {
        out += std::isprint(ch) ? static_cast<char>(ch) : '.';
    }
    return out;
}

#ifndef _WIN32
bool configure_tty(int fd) {
    struct termios tty {};
    if (tcgetattr(fd, &tty) != 0) return false;
    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;  // read() returns at once; read_reply() paces with poll()
    if (tcsetattr(fd, TCSANOW, &tty) != 0) return false;
    // Opened O_NONBLOCK so open() cannot hang on a half-enumerated node; the
    // port must not stay that way (the Q-Focuser lesson: a non-blocking USB
    // serial port can answer the first write() with EAGAIN).
    return util::clear_nonblocking(fd);
}

// Read up to `max_len` bytes: wait up to `first_byte_ms` for the first byte,
// then keep collecting until `max_len` is reached or the line has been idle
// for kReplyIdleMs. Sets link_dead when the port is gone (POLLHUP/POLLERR/
// POLLNVAL with nothing to read, EOF, or EIO/ENXIO/ENODEV/EBADF) so the caller
// fails the transaction at once instead of re-polling to the deadline.
std::string read_reply(int fd, int first_byte_ms, std::size_t max_len, bool& link_dead) {
    link_dead = false;
    std::string buf;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(first_byte_ms);
    while (buf.size() < max_len) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        struct pollfd pfd {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int r = poll(&pfd, 1, static_cast<int>(remaining < kReadPollMs ? remaining : kReadPollMs));
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EBADF) link_dead = true;
            return buf;
        }
        if (r == 0) continue;
        if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 && (pfd.revents & POLLIN) == 0) {
            link_dead = true;
            return buf;
        }
        char ch = 0;
        const ssize_t n = read(fd, &ch, 1);  // NOLINT(clang-analyzer-unix.BlockInCriticalSection)
        if (n == 0) {
            link_dead = true;
            return buf;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            if (errno == EIO || errno == ENXIO || errno == ENODEV || errno == EBADF) link_dead = true;
            return buf;
        }
        buf += ch;
        // First byte in: the rest of the reply follows within the idle window.
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kReplyIdleMs);
    }
    return buf;
}

// Wait for the boot byte the wheel emits once it has homed after the DTR
// reset that opening the port caused. Returns the position it reported, or
// nullopt when nothing arrived (the port did not reset -- a pty, or a bridge
// that kept DTR steady). Any preceding noise is skipped; the LAST valid slot
// character wins so a repeated byte is harmless.
std::optional<int> wait_for_boot_byte(int fd, int timeout_ms, bool& link_dead) {
    link_dead = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::optional<int> position;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const std::string chunk = read_reply(fd, static_cast<int>(remaining), 8, link_dead);
        if (link_dead) return std::nullopt;
        if (chunk.empty()) break;  // deadline with nothing more
        for (char ch : chunk) {
            if (auto slot = cfw3_parse_slot(ch)) position = slot;
        }
        if (position) return position;
    }
    return position;
}
#endif

}  // namespace

std::optional<char> cfw3_slot_to_command(int slot) {
    if (slot < 0 || slot > kMaxSlot) return std::nullopt;
    if (slot < 10) return static_cast<char>('0' + slot);
    return static_cast<char>('A' + (slot - 10));
}

std::optional<int> cfw3_parse_slot(char reply) {
    if (reply >= '0' && reply <= '9') return reply - '0';
    if (reply >= 'A' && reply <= 'F') return 10 + (reply - 'A');
    return std::nullopt;
}

// Probe one port: open, sit out the boot, ask MXP then NOW. VRS is read too
// but optional. Returns true only when both single-character replies parse.
static bool probe_port(const std::string& port_path, int boot_timeout_ms, Cfw3DeviceInfo& info) {
#ifndef _WIN32
    int fd = open(port_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return false;
    if (!configure_tty(fd)) {
        close(fd);
        return false;
    }
    tcflush(fd, TCIOFLUSH);
    bool link_dead = false;
    const auto boot = wait_for_boot_byte(fd, boot_timeout_ms, link_dead);
    if (link_dead) {
        close(fd);
        return false;
    }
    auto ask = [&](const char* cmd, std::size_t max_len) -> std::string {
        tcflush(fd, TCIFLUSH);
        if (!util::write_all(fd, cmd, std::char_traits<char>::length(cmd))) return {};
        tcdrain(fd);
        return read_reply(fd, 3000, max_len, link_dead);
    };
    const std::string vrs = ask("VRS", kFirmwareReplyLen);
    const std::string mxp = link_dead ? std::string() : ask("MXP", 1);
    const std::string now = link_dead ? std::string() : ask("NOW", 1);
    close(fd);
    if (mxp.size() != 1 || now.size() != 1) return false;
    const auto slots = cfw3_parse_slot(mxp[0]);
    const auto pos = cfw3_parse_slot(now[0]);
    if (!slots || !pos) return false;
    info.firmware = vrs.size() == kFirmwareReplyLen ? vrs : std::string();
    info.slot_count = *slots;
    info.position = *pos;  // NOW is the fresher of the two; the boot byte only proves the reset happened
    (void)boot;
    return true;
#else
    (void)port_path;
    (void)boot_timeout_ms;
    (void)info;
    return false;
#endif
}

#ifndef _WIN32
namespace {
// The raw /dev/ttyUSBn fallback has no by-id name to filter on; restrict it
// to the Silicon Labs CP210x class the wheel ships with so the scan never
// DTR-resets an unrelated adapter (a CH340 mount cable, an FTDI focuser).
bool raw_port_looks_like_cfw3_candidate(const std::string& port_path) {
    auto descriptor = alpacacore::util::read_raw_tty_usb_descriptor(port_path);
    if (!descriptor) return false;
    return alpacacore::util::usb_tty_descriptor_matches(*descriptor, {"10c4", "Silicon Labs", "CP210"});
}
}  // namespace
#endif

std::vector<Cfw3PortInfo> enumerate_cfw3_ports(int boot_timeout_ms) {
    std::vector<Cfw3PortInfo> results;

#ifndef _WIN32
    std::set<std::string> probed;

    struct Candidate {
        std::string path;
        std::string name;
    };
    std::vector<Candidate> candidates;

    const std::filesystem::path serial_by_id("/dev/serial/by-id");
    if (alpacacore::util::path_exists(serial_by_id)) {
        for (const auto& sym : alpacacore::util::list_serial_by_id(serial_by_id)) {
            const std::string& name = sym.name;
            // udev name seen on hardware:
            // usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0
            const bool is_candidate = (name.find("Silicon_Labs") != std::string::npos) ||
                                      (name.find("CP210") != std::string::npos) ||
                                      (name.find("10c4") != std::string::npos);
            if (!is_candidate) continue;

            std::error_code canon_ec;
            std::string resolved = std::filesystem::canonical(sym.path, canon_ec).string();
            if (canon_ec) continue;
            if (alpacacore::util::is_serial_port_in_use(resolved)) continue;
            probed.insert(resolved);
            candidates.push_back({resolved, name});
        }
    }

    // Raw /dev/ttyUSB* pass, run unconditionally: two CP2102s with the same
    // (absent) serial number collide on their by-id name and only one gets a
    // symlink. Descriptor-filtered and deduped against the by-id pass.
    for (int i = 0; i < 10; ++i) {
        std::string port = "/dev/ttyUSB" + std::to_string(i);
        if (!alpacacore::util::path_exists(port)) continue;
        std::error_code canon_ec;
        std::string resolved = std::filesystem::canonical(port, canon_ec).string();
        if (canon_ec) continue;
        if (probed.count(resolved) != 0) continue;
        if (alpacacore::util::is_serial_port_in_use(resolved)) continue;
        if (!raw_port_looks_like_cfw3_candidate(resolved)) continue;
        probed.insert(resolved);
        candidates.push_back({resolved, ""});
    }

    if (!candidates.empty()) {
        ALPACA_LOG_WARN(kLogTag, "Probing " + std::to_string(candidates.size()) +
                                     " CP210x port(s) for a QHYCFW3; opening a port resets the device behind "
                                     "it and a wheel takes ~17 s to home");
    }

    // Probe concurrently so the wall time is one boot, not one per adapter.
    std::vector<Cfw3DeviceInfo> infos(candidates.size());
    std::vector<char> found(candidates.size(), 0);
    std::vector<std::thread> workers;
    workers.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        ALPACA_LOG_INFO(kLogTag, "Probing " + c.path + (c.name.empty() ? "" : " (" + c.name + ")") + " for QHYCFW3...");
        workers.emplace_back([&, i] { found[i] = probe_port(candidates[i].path, boot_timeout_ms, infos[i]) ? 1 : 0; });
    }
    for (auto& w : workers) w.join();

    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!found[i]) continue;
        const auto& c = candidates[i];
        ALPACA_LOG_INFO(kLogTag, "Found QHYCFW3 on " + c.path + " (" + std::to_string(infos[i].slot_count) +
                                     " slots, firmware " + (infos[i].firmware.empty() ? "unknown" : infos[i].firmware) +
                                     ")");
        results.push_back({c.path, c.name, infos[i]});
    }
#else
    (void)boot_timeout_ms;
#endif

    return results;
}

class Cfw3ProtocolWrapper::Impl {
public:
    Impl() = default;
    ~Impl() {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        close_port_locked();
    }

    Cfw3DeviceInfo connect(const Cfw3ConnectionConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connected_) {
            throw AlpacaException("QHYCFW3 already connected", AlpacaError::InvalidOperation);
        }
        // Opening the port DTR-resets the wheel and costs its ~17 s boot, and
        // ConformU's Platform 7 Connect() abandons the device after 5 s of
        // Connecting. So the fd is HELD across logical disconnects: the first
        // connect of a process pays the boot, every later one on the same
        // port skips straight to the handshake (~10 ms). A different port, a
        // link loss (which released the fd) or a failed handshake (which
        // closes it) reopens and pays the boot again.
        const bool held = serial_fd_ >= 0 && opened_port_config_ == config.serial_port;
        config_ = config;
        link_lost_.store(false);
        if (!held) {
            close_port_locked();
            open_port_locked();
        }

        Cfw3DeviceInfo info;
        in_connect_ = true;
        try {
#ifndef _WIN32
            if (held) {
                tcflush(serial_fd_, TCIOFLUSH);  // a stale arrival reply from the last session
                ALPACA_LOG_DEBUG(kLogTag, "QHYCFW3 port " + config_.serial_port +
                                              " held open since the last session; no reset, no boot wait");
            } else {
                bool link_dead = false;
                const auto boot = wait_for_boot_byte(serial_fd_, config_.boot_timeout_ms, link_dead);
                if (link_dead) fail_link_locked();
                if (boot) {
                    ALPACA_LOG_INFO(kLogTag,
                                    "QHYCFW3 finished its post-reset homing at slot " + std::to_string(*boot + 1));
                } else {
                    ALPACA_LOG_DEBUG(kLogTag, "QHYCFW3 sent no boot byte within " +
                                                  std::to_string(config_.boot_timeout_ms) +
                                                  " ms; assuming the port did not reset it");
                }
            }
#endif
            // VRS is optional: firmware before 201409 answers none of the
            // three queries, and a wheel that answers MXP/NOW but not VRS
            // still works. MXP and NOW are required -- without them there is
            // no slot count to validate against and no position to report.
            const std::string vrs = transact_locked("VRS", kFirmwareReplyLen, config_.reply_timeout_ms);
            if (vrs.size() == kFirmwareReplyLen) {
                info.firmware = vrs;
            } else if (!vrs.empty()) {
                ALPACA_LOG_WARN(kLogTag, "QHYCFW3 VRS reply has an unexpected shape: " + printable(vrs));
            }
            const std::string mxp = transact_locked("MXP", 1, config_.reply_timeout_ms);
            const std::string now = transact_locked("NOW", 1, config_.reply_timeout_ms);
            const auto slots = mxp.size() == 1 ? cfw3_parse_slot(mxp[0]) : std::nullopt;
            const auto pos = now.size() == 1 ? cfw3_parse_slot(now[0]) : std::nullopt;
            if (!slots || !pos) {
                throw AlpacaException(
                    "No QHYCFW3 answered on " + config_.serial_port +
                        ": check that the wheel's mode switch is in USB mode (red LED flash at power-on) and "
                        "that the port is the wheel's CP2102 bridge",
                    AlpacaError::NotConnected);
            }
            if (*slots <= 0) {
                throw AlpacaException("QHYCFW3 reported an invalid slot count", AlpacaError::DriverException);
            }
            info.slot_count = *slots;
            info.position = *pos;
        } catch (...) {
            in_connect_ = false;
            close_port_locked();
            throw;
        }
        in_connect_ = false;
        connected_ = true;
        ALPACA_LOG_INFO(kLogTag, "QHYCFW3 connected on " + config_.serial_port + " (" +
                                     std::to_string(info.slot_count) + " slots, firmware " +
                                     (info.firmware.empty() ? "unknown" : info.firmware) + ", at slot " +
                                     std::to_string(info.position + 1) + ")");
        return info;
    }

    // Logical only: the fd stays open so the next connect() does not reset
    // the wheel (see connect()). The port is released by the destructor, a
    // link loss, a failed handshake, or a connect() on a different port.
    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
    }

    bool is_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    bool link_alive() const noexcept { return !link_lost_.load(); }

    int get_position() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        const std::string now = transact_locked("NOW", 1, config_.reply_timeout_ms);
        const auto pos = now.size() == 1 ? cfw3_parse_slot(now[0]) : std::nullopt;
        if (!pos) {
            throw AlpacaException(
                "QHYCFW3 did not answer NOW" + (now.empty() ? std::string() : " (" + printable(now) + ")"),
                AlpacaError::DriverException);
        }
        return *pos;
    }

    int goto_slot(int slot, const std::atomic<bool>& cancel) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        const auto cmd = cfw3_slot_to_command(slot);
        if (!cmd) {
            throw AlpacaException("Filter position out of range for the QHYCFW3 protocol (max 16 slots)",
                                  AlpacaError::InvalidValue);
        }
#ifndef _WIN32
        if (serial_fd_ < 0) fail_link_locked();
        tcflush(serial_fd_, TCIFLUSH);
        write_locked(&*cmd, 1);
        // The reply is the arrival report; the wheel is silent while turning.
        // Slice the wait so a disconnect can cancel it -- the wheel finishes
        // the move on its own, and the next connect's NOW reports where it is.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.move_timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (cancel.load()) {
                throw AlpacaException("QHYCFW3 move wait cancelled", AlpacaError::InvalidOperation);
            }
            bool link_dead = false;
            const std::string reply = read_reply(serial_fd_, kMoveSliceMs, 1, link_dead);
            if (link_dead) fail_link_locked();
            if (reply.empty()) continue;
            const auto arrived = cfw3_parse_slot(reply[0]);
            if (!arrived) {
                ALPACA_LOG_DEBUG(kLogTag, "QHYCFW3 ignoring non-slot byte during move: " + printable(reply));
                continue;
            }
            if (*arrived != slot) {
                // Not seen on hardware; the firmware answers with the slot it
                // stopped at, so report what it said rather than what we asked.
                ALPACA_LOG_WARN(kLogTag, "QHYCFW3 reported arrival at slot " + std::to_string(*arrived + 1) +
                                             " after a goto to slot " + std::to_string(slot + 1));
            }
            return *arrived;
        }
        throw AlpacaException("QHYCFW3 did not report arrival at slot " + std::to_string(slot + 1) + " within " +
                                  std::to_string(config_.move_timeout_ms) + " ms",
                              AlpacaError::DriverException);
#else
        (void)cancel;
        throw AlpacaException("QHYCFW3 driver is Linux-only", AlpacaError::DriverException);
#endif
    }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("QHYCFW3 not connected", AlpacaError::NotConnected);
        }
    }

    void open_port_locked() {
#ifndef _WIN32
        std::error_code canon_ec;
        std::string resolved = std::filesystem::canonical(config_.serial_port, canon_ec).string();
        if (canon_ec) resolved = config_.serial_port;
        if (!util::try_mark_serial_port_open(resolved)) {
            throw AlpacaException("Serial port " + config_.serial_port + " is already in use by another device",
                                  AlpacaError::NotConnected);
        }
        opened_port_ = resolved;
        opened_port_config_ = config_.serial_port;

        serial_fd_ = open(config_.serial_port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (serial_fd_ < 0) {
            const int err = errno;
            util::mark_serial_port_closed(opened_port_);
            opened_port_.clear();
            throw AlpacaException(
                "Failed to open serial port: " + config_.serial_port + " (" + util::errno_string(err) + ")",
                AlpacaError::NotConnected);
        }
        if (!configure_tty(serial_fd_)) {
            close_port_locked();
            throw AlpacaException("Failed to configure serial port " + config_.serial_port,
                                  AlpacaError::DriverException);
        }
        tcflush(serial_fd_, TCIOFLUSH);
#else
        throw AlpacaException("QHYCFW3 driver is Linux-only", AlpacaError::DriverException);
#endif
    }

    void close_port_locked() {
#ifndef _WIN32
        if (serial_fd_ >= 0) {
            close(serial_fd_);
            serial_fd_ = -1;
        }
        if (!opened_port_.empty()) {
            util::mark_serial_port_closed(opened_port_);
            opened_port_.clear();
        }
        opened_port_config_.clear();
#endif
    }

    // The port is gone (unplug / re-enumeration): release the fd so a
    // replugged wheel can come back under the same name, latch link_lost_
    // for the driver's Connected getter, and fail with NotConnected. During
    // connect() nothing is latched; connect() closes the port itself.
    [[noreturn]] void fail_link_locked() {
        if (!in_connect_) {
            ALPACA_LOG_ERROR(kLogTag, "QHYCFW3 link lost on " + config_.serial_port + " (port released)");
            link_lost_.store(true);
            connected_ = false;
            close_port_locked();
        }
        throw AlpacaException("QHYCFW3 link lost on " + config_.serial_port, AlpacaError::NotConnected);
    }

#ifndef _WIN32
    void write_locked(const char* data, std::size_t len) {
        if (!util::write_all(serial_fd_, data, len)) {
            const int err = errno;
            if (err == EIO || err == ENXIO || err == ENODEV || err == EBADF) fail_link_locked();
            throw AlpacaException("QHYCFW3 write failed: " + util::errno_string(err), AlpacaError::DriverException);
        }
        tcdrain(serial_fd_);
    }
#endif

    // Send a query and read its reply (up to `max_len` bytes, ending on line
    // idle). Empty when the wheel stayed silent for `timeout_ms`.
    std::string transact_locked(const char* cmd, std::size_t max_len, int timeout_ms) {
#ifndef _WIN32
        if (serial_fd_ < 0) fail_link_locked();
        ALPACA_LOG_TRACE(kLogTag, std::string("QHYCFW3 command: ") + cmd);
        tcflush(serial_fd_, TCIFLUSH);
        write_locked(cmd, std::char_traits<char>::length(cmd));
        bool link_dead = false;
        const std::string reply = read_reply(serial_fd_, timeout_ms, max_len, link_dead);
        if (link_dead) fail_link_locked();
        ALPACA_LOG_TRACE(kLogTag, "QHYCFW3 reply: " + printable(reply));
        return reply;
#else
        (void)cmd;
        (void)max_len;
        (void)timeout_ms;
        throw AlpacaException("QHYCFW3 driver is Linux-only", AlpacaError::DriverException);
#endif
    }

    mutable std::mutex mutex_;
    Cfw3ConnectionConfig config_;
    bool connected_ = false;
    std::atomic<bool> link_lost_{false};
    bool in_connect_ = false;  // guarded by mutex_
#ifndef _WIN32
    int serial_fd_ = -1;
    std::string opened_port_;         // canonical path registered in the serial-port registry
    std::string opened_port_config_;  // config path the held fd was opened from
#endif
};

Cfw3ProtocolWrapper::Cfw3ProtocolWrapper() : impl_(std::make_unique<Impl>()) {}
Cfw3ProtocolWrapper::~Cfw3ProtocolWrapper() = default;

Cfw3DeviceInfo Cfw3ProtocolWrapper::connect(const Cfw3ConnectionConfig& config) { return impl_->connect(config); }
void Cfw3ProtocolWrapper::disconnect() { impl_->disconnect(); }
bool Cfw3ProtocolWrapper::is_connected() const { return impl_->is_connected(); }
bool Cfw3ProtocolWrapper::link_alive() const noexcept { return impl_->link_alive(); }
int Cfw3ProtocolWrapper::get_position() { return impl_->get_position(); }
int Cfw3ProtocolWrapper::goto_slot(int slot, const std::atomic<bool>& cancel) { return impl_->goto_slot(slot, cancel); }

}  // namespace alpacacore::vendor::qhy
