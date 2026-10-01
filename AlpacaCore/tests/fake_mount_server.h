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

// Minimal loopback TCP "mount" for the telescope [stress] tests (POSIX-only,
// like the rest of the test suite's socket helpers). The telescope protocol
// wrappers (ZWO / Celestron / SynScan / iOptron) treat a successful TCP
// connect as a successful mount connect — every post-connect query is
// individually fault-tolerant — so a server that accepts connections and
// answers each read with a canned reply is enough to drive the drivers into
// the CONNECTED state on a hardware-free host. That is the seam the stress
// tests need: only a connected driver spawns its poll/pulse/GOTO/teardown
// threads, and those threads racing disconnect and destruction are exactly
// where the audited detached-thread bugs lived.
//
// The responder callback receives each received chunk verbatim and returns
// the bytes to send back (empty = no reply). Command framing/parsing is the
// caller's business; per-command parse failures in the drivers are tolerated
// by design, so a dumb default reply already exercises every thread path.
//
// Optional knobs, all off by default so every existing user sees the fake it
// always had (issue #617 added them for the SynScan position poller cases):
//   - set_reply_delay(command, d): hold the reply to chunks starting with that
//     command byte for d, the way a 9600-baud hand controller takes ~230 ms to
//     answer a position read. The responder still runs at RECEIPT, before the
//     delay, so a test responder can timestamp when a command reached the
//     "mount".
//   - exchange_count(command): chunks received that start with that byte,
//     muted or not. Commands the driver sends blind and back-to-back can
//     coalesce into one TCP chunk; only the first byte of a chunk is counted.
//   - set_muted(true): keep reading but never answer (a hung handset on a
//     healthy link), the same vocabulary as the pty fakes.
//   - connection_count(): connections accepted so far, so a test can prove a
//     recovery happened with no reconnect.

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::test {

class FakeMountServer {
public:
    using Responder = std::function<std::string(const std::string& chunk)>;

    /// Starts listening on an ephemeral loopback port immediately.
    explicit FakeMountServer(Responder responder = default_responder()) : responder_(std::move(responder)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return;
        }
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // ephemeral
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 16) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port_ = static_cast<int>(ntohs(addr.sin_port));
        }
        accept_thread_ = std::thread([this]() { accept_loop(); });
    }

    ~FakeMountServer() {
        stop_.store(true);
        if (listen_fd_ >= 0) {
            // shutdown() unblocks the accept(); close() alone is not portable
            // for that.
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        // Unblock every handler recv() first: the vendor protocol wrappers
        // are process-lifetime singletons, so a still-connected wrapper can
        // legitimately outlive the test that owns this server — without the
        // shutdown() the matching handler would block in recv() forever and
        // the join below would hang.
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            for (const int fd : conn_fds_) {
                ::shutdown(fd, SHUT_RDWR);
            }
        }
        // Swap the thread list out, then join WITHOUT the mutex — the
        // handlers take conn_mutex_ on exit to deregister their fd, so
        // joining under the lock would deadlock.
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            threads.swap(conn_threads_);
        }
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    FakeMountServer(const FakeMountServer&) = delete;
    FakeMountServer& operator=(const FakeMountServer&) = delete;

    bool ok() const { return listen_fd_ >= 0 && port_ > 0; }
    int port() const { return port_; }

    /// Delay the reply to every chunk whose first byte is `command`. Zero (the
    /// default for every command) replies at once.
    void set_reply_delay(char command, std::chrono::milliseconds delay) {
        reply_delay_ms_[index_of(command)].store(static_cast<int>(delay.count()));
    }

    /// Chunks received so far whose first byte is `command` (counted while
    /// muted too).
    int exchange_count(char command) const { return exchange_counts_[index_of(command)].load(); }

    /// While muted the fake reads and counts but answers nothing.
    void set_muted(bool muted) { muted_.store(muted); }

    /// Connections accepted since construction.
    int connection_count() const { return connection_count_.load(); }

    /// Replies "0#" to anything: valid terminator for every '#'-framed
    /// protocol in the family; per-command parse failures are tolerated by
    /// the drivers (and swallowed by the stress harness).
    // Canned "0#" for everything, EXCEPT the SynScan/NexStar protocol echo
    // ("K" + byte -> byte + "#"): the SynScan driver's connect now gates on
    // that echo matching, so a fake that never answers it leaves the driver
    // permanently unconnected - and the concurrency stress tests, whose whole
    // point is to race worker threads on a CONNECTED driver, silently turn
    // into no-ops (review finding on PR #3). No other protocol served by this
    // fake sends a bare 'K' (iOptron and OnStep commands start with ':'), so
    // the echo branch is invisible to them.
    static Responder default_responder() {
        return [](const std::string& chunk) {
            if (chunk.size() >= 2 && chunk[0] == 'K') {
                return std::string(1, chunk[1]) + "#";
            }
            return std::string("0#");
        };
    }

private:
    void accept_loop() {
        while (!stop_.load()) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (stop_.load()) {
                    return;
                }
                continue;
            }
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            connection_count_.fetch_add(1);
            std::lock_guard<std::mutex> lock(conn_mutex_);
            conn_fds_.push_back(fd);
            conn_threads_.emplace_back([this, fd]() { serve(fd); });
        }
    }

    void serve(int fd) {
        char buf[1024];
        while (!stop_.load()) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;  // peer disconnected (the storm churns connections)
            }
            const std::size_t command = index_of(buf[0]);
            exchange_counts_[command].fetch_add(1);
            if (muted_.load()) {
                continue;
            }
            const std::string reply = responder_(std::string(buf, static_cast<std::size_t>(n)));
            if (!wait_reply_delay(reply_delay_ms_[command].load())) {
                break;
            }
            if (!reply.empty()) {
                static_cast<void>(::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL));
            }
        }
        // Deregister before close so the destructor can never shutdown() a
        // recycled fd number; the storm churns hundreds of short connections.
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            conn_fds_.erase(std::remove(conn_fds_.begin(), conn_fds_.end(), fd), conn_fds_.end());
        }
        ::close(fd);
    }

    static std::size_t index_of(char command) { return static_cast<unsigned char>(command); }

    // Sliced so the destructor never waits out a long delay. Returns false
    // when the server is stopping.
    bool wait_reply_delay(int delay_ms) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (stop_.load()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return !stop_.load();
    }

    Responder responder_;
    std::array<std::atomic<int>, 256> reply_delay_ms_{};
    std::array<std::atomic<int>, 256> exchange_counts_{};
    std::atomic<bool> muted_{false};
    std::atomic<int> connection_count_{0};
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread accept_thread_;
    std::mutex conn_mutex_;
    std::vector<int> conn_fds_;
    std::vector<std::thread> conn_threads_;
};

}  // namespace alpacacore::test

#endif  // !_WIN32
