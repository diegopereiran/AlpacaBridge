// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#pragma once

#include <alpacacore/managementdriver.h>
#include <alpacahttp/util/socket_utils.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "config.h"
#include "router.h"

namespace alpacahttp {

class Server {
public:
    explicit Server(const Config& config);
    ~Server();

    // Set management driver (from AlpacaCore)
    void set_management_driver(std::shared_ptr<alpacacore::ManagementDriver> mgmt_driver);

    // Set shutdown callback (called when shutdown endpoint is requested)
    void set_shutdown_callback(std::function<void()> callback);
    // Set restart callback (called when restart endpoint is requested)
    void set_restart_callback(std::function<void()> callback);

    // Start the server (blocking)
    void start();

    // Start the server in background thread
    void start_async();

    // Stop the server
    void stop();

    // Check if server is running
    bool is_running() const { return running_; }

    // Wait for server to stop
    void wait();

private:
    Config config_;
    Router router_;
    std::atomic<bool> running_{false};
    std::atomic<util::SocketHandle> server_fd_{util::kInvalidSocket};
    std::thread server_thread_;

    // One client connection. Owned by exactly one party at a time: the accept
    // loop (briefly, until it is parked), the reactor (while idle, waiting for
    // the peer's next request), the ready queue, or a worker (while a request
    // is being served). Ownership moves with the unique_ptr, so a connection
    // can never be polled and served at the same time.
    struct Connection {
        util::SocketHandle fd{util::kInvalidSocket};
        std::string remote_address;
        // Bytes read past the end of the last request (a pipelining client).
        // A connection with buffered bytes is never parked: the worker keeps
        // serving until it is empty, since the reactor polls the socket and
        // would not see them.
        std::string carried;
        std::uint64_t requests_served{0};
        std::chrono::steady_clock::time_point opened_at;
        // When the reactor gives up waiting on this connection: the first
        // request's slowloris bound, then the keep-alive idle gap, and never
        // past the connection lifetime cap.
        std::chrono::steady_clock::time_point deadline;
        // Set by the reactor when the deadline passed: the worker that picks
        // it up closes it (gracefully, off the reactor thread) instead of
        // reading from it.
        bool close_only{false};
    };
    using ConnectionPtr = std::unique_ptr<Connection>;

    // Workers: fixed pool, each serves one request at a time. thread_pool_size
    // therefore bounds concurrent REQUESTS; idle connections cost no worker.
    std::vector<std::thread> worker_threads_;
    std::deque<ConnectionPtr> ready_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    bool shutdown_workers_{false};

    // Reactor: one thread parks idle connections on a poll set and hands them
    // to the ready queue when their next request arrives. Woken through a
    // self-pipe when a worker parks a connection or stop() begins.
    std::thread reactor_thread_;
    int reactor_wake_fds_[2]{-1, -1};
    std::mutex reactor_mutex_;
    std::vector<ConnectionPtr> reactor_incoming_;
    bool reactor_accepting_{false};

    // Connections alive in any owner. Bounded by Config::max_connections so
    // idle keep-alive clients cannot exhaust the process's descriptors; at
    // the bound the accept loop pauses and new clients wait in the listen
    // backlog.
    std::atomic<std::size_t> live_connections_{0};

    void run_server();
    void reactor_loop();
    void worker_thread();
    enum class ServeResult : std::uint8_t { KeepOpen, Close };
    ServeResult serve_one_request(Connection& conn);
    void park_connection(ConnectionPtr conn);
    void enqueue_ready(ConnectionPtr conn);
    void close_connection(ConnectionPtr conn, bool graceful);
    void wake_reactor();
    void close_wake_pipe();
    void handle_shutdown_request();
    void handle_restart_request();

    std::function<void()> shutdown_callback_;
    std::mutex shutdown_mutex_;
    std::atomic<bool> shutdown_requested_{false};
    std::function<void()> restart_callback_;
    std::mutex restart_mutex_;
    std::atomic<bool> restart_requested_{false};
};

} // namespace alpacahttp
