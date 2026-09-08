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

#include <alpacahttp/server.h>
#include <alpacahttp/util/logging_adapter.h>
#include <alpacahttp/util/socket_utils.h>
#include <alpacahttp/version.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <queue>
#include <stdexcept>
#include <utility>

namespace alpacahttp {

Server::Server(const Config& config)
    : config_(config)
{
    router_.set_shutdown_callback([this]() { handle_shutdown_request(); });
    router_.set_restart_callback([this]() { handle_restart_request(); });
    router_.set_server_info(config_.server_name(), config_.manufacturer(), alpacahttp::kVersion, config_.location(),
                            config_.profile_name());
    router_.set_config_path(config_.config_path());
}

void Server::set_management_driver(std::shared_ptr<alpacacore::ManagementDriver> mgmt_driver) {
    router_.set_management_driver(mgmt_driver);
}

void Server::set_shutdown_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(shutdown_mutex_);
    shutdown_callback_ = std::move(callback);
}

void Server::set_restart_callback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(restart_mutex_);
    restart_callback_ = std::move(callback);
}

Server::~Server() {
    stop();
}

void Server::start() {
    if (running_) {
        return;
    }

    shutdown_requested_ = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_workers_ = false;
        while (!connection_queue_.empty()) {
            connection_queue_.pop();
        }
    }
    running_ = true;
    run_server();
}

void Server::start_async() {
    if (running_) {
        return;
    }

    shutdown_requested_ = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_workers_ = false;
        while (!connection_queue_.empty()) {
            connection_queue_.pop();
        }
    }
    running_ = true;
    server_thread_ = std::thread(&Server::run_server, this);
}

void Server::stop() {
    if (!running_) {
        return;
    }

    util::log_info("Stopping HTTP server...");
    running_ = false;
    
    // Shutdown worker threads
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_workers_ = true;
    }
    queue_condition_.notify_all();
    
    // Wait for all worker threads to finish
    const auto current_id = std::this_thread::get_id();
    for (auto& thread : worker_threads_) {
        if (!thread.joinable()) {
            continue;
        }
        if (thread.get_id() == current_id) {
            thread.detach();
            continue;
        }
        thread.join();
    }
    worker_threads_.clear();
    
    // Shutdown and close the server socket to interrupt accept() call
    auto fd = server_fd_.exchange(util::kInvalidSocket);
    if (fd != util::kInvalidSocket) {
        util::socket_shutdown(fd);  // Shutdown before close to ensure accept() wakes up
        util::socket_close(fd);
    }
    
    if (server_thread_.joinable()) {
        if (server_thread_.get_id() == current_id) {
            server_thread_.detach();
        } else {
            server_thread_.join();
        }
    }
    util::log_info("HTTP server stopped");
}

void Server::wait() {
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

namespace {

// Create a bound, listening HTTP socket on `port`. Prefers a dual-stack IPv6
// socket (IPV6_V6ONLY off) so both ::1 and 127.0.0.1 connect directly; falls
// back to IPv4-only where IPv6 is unavailable.
//
// Why dual-stack: .NET clients (ConformU, NINA on Linux/macOS) connecting to
// "127.0.0.1" or "localhost" try ::1 first and only then 127.0.0.1. With an
// IPv4-only listener every request pays a refused IPv6 SYN before the real
// connect, and on a Raspberry Pi that fallback showed up in ConformU as a
// consistent ~100 ms on a handful of otherwise 1 ms members (captured with
// tcpdump on the HAE16 EQ validation, 2026-08-25).
util::SocketHandle create_listener(int port) {
    util::SocketHandle fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd != util::kInvalidSocket) {
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
        int v6only = 0;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only)) != 0) {
            // Without a confirmed dual-stack socket, IPv4 clients could be
            // locked out on a host whose default is v6-only; fall back to the
            // IPv4 listener rather than guess.
            util::log_warning("IPV6_V6ONLY could not be cleared (" +
                              util::socket_error_message(util::socket_get_last_error()) +
                              "); falling back to an IPv4-only HTTP listener");
            util::socket_close(fd);
            fd = util::kInvalidSocket;
        }
        struct sockaddr_in6 address6 {};
        address6.sin6_family = AF_INET6;
        address6.sin6_addr = in6addr_any;
        address6.sin6_port = htons(static_cast<u_short>(port));
        if (fd != util::kInvalidSocket) {
            if (bind(fd, reinterpret_cast<struct sockaddr*>(&address6), sizeof(address6)) == 0 && listen(fd, 10) == 0) {
                return fd;
            }
            util::socket_close(fd);
            util::log_warning("Dual-stack HTTP listener unavailable, falling back to IPv4 only");
        }
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == util::kInvalidSocket) {
        return util::kInvalidSocket;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<u_short>(port));
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0 || listen(fd, 10) < 0) {
        util::socket_close(fd);
        return util::kInvalidSocket;
    }
    return fd;
}

}  // namespace

void Server::run_server() {
    util::ensure_winsock();
    const int port = config_.http_port();
    if (port < 0 || port > static_cast<int>(std::numeric_limits<u_short>::max())) {
        util::log_error("Invalid HTTP port: " + std::to_string(port));
        running_ = false;
        return;
    }

    util::SocketHandle server_fd = create_listener(port);
    if (server_fd == util::kInvalidSocket) {
        util::log_error("Failed to bind HTTP listener to port " + std::to_string(port));
        running_ = false;
        return;
    }

    // Store server_fd so we can close it from stop()
    server_fd_.store(server_fd);

    util::log_info("Server listening on port " + std::to_string(port));

    // The listener fd can go bad underneath us without stop() being called (a
    // stray double-close elsewhere in the process can free and then re-close
    // our fd number). Observed once in the field: the accept loop broke
    // silently and the whole server shut down "successfully" mid ConformU
    // run. Recreate the listener instead of dying.
    auto last_rebind = std::chrono::steady_clock::time_point::min();
    auto rebind_listener = [&]() -> bool {
        // Backoff ACROSS rebind cycles too: if the fd-loss condition recurs
        // immediately after a successful rebind, sleep instead of spinning
        // select-fail -> rebind -> select-fail with continuous error logging.
        auto now = std::chrono::steady_clock::now();
        if (now - last_rebind < std::chrono::seconds(2)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        last_rebind = now;
        auto old = server_fd_.exchange(util::kInvalidSocket);
        if (old != util::kInvalidSocket) {
            // The fd number may have been recycled to an UNRELATED live socket
            // by whatever double-closed the listener. Close it only if it is
            // still a listening socket; otherwise leak the number rather than
            // sever an innocent connection.
            int acc = 0;
            socklen_t len = sizeof(acc);
            if (getsockopt(old, SOL_SOCKET, SO_ACCEPTCONN, &acc, &len) == 0 && acc != 0) {
                util::socket_close(old);
            }
        }
        for (int attempt = 0; attempt < 10 && running_; ++attempt) {
            util::SocketHandle fd = create_listener(port);
            if (fd != util::kInvalidSocket) {
                server_fd_.store(fd);
                server_fd = fd;
                util::log_error("HTTP listener recreated after descriptor loss");
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return false;
    };

    // Start worker thread pool for handling concurrent requests
    std::size_t pool_size = config_.thread_pool_size();
    worker_threads_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) {
        worker_threads_.emplace_back(&Server::worker_thread, this);
    }
    util::log_info("Started " + std::to_string(pool_size) + " worker threads for concurrent request handling");

    // Accept connections using select() to allow checking running_ flag periodically
    while (running_) {
        // Use select() to wait for connections with a timeout, so we can check running_ periodically
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;  // 500ms timeout
        
        int select_result = util::socket_select(server_fd, &read_fds, nullptr, nullptr, &timeout);
        
        if (select_result < 0) {
            // Error in select
            int err = util::socket_get_last_error();
            if (util::socket_interrupted(err)) {
                // Interrupted by signal - continue
                continue;
            } else if (util::socket_bad_descriptor(err) || util::socket_not_socket(err)) {
                if (!running_) {
                    break;  // stop() closed the listener deliberately
                }
                util::log_error("HTTP listener descriptor went bad in select(); rebinding");
                if (!rebind_listener()) {
                    break;
                }
                continue;
            } else {
                if (running_) {
                    util::log_error("Server select error: " + util::socket_error_message(err));
                }
                break;
            }
        } else if (select_result == 0) {
            // Timeout - check running_ flag and continue
            continue;
        }
        
        // Connection available - accept it
        if (FD_ISSET(server_fd, &read_fds)) {
            struct sockaddr_storage client_address {};
            util::SocketLen client_len = sizeof(client_address);

            util::SocketHandle client_fd =
                accept(server_fd, reinterpret_cast<struct sockaddr*>(&client_address), &client_len);
            if (client_fd == util::kInvalidSocket) {
                int err = util::socket_get_last_error();
                if (util::socket_interrupted(err) || util::socket_would_block(err)) {
                    // Interrupted or would block - continue
                    continue;
                } else if (util::socket_bad_descriptor(err) || util::socket_not_socket(err)) {
                    if (!running_) {
                        break;  // stop() closed the listener deliberately
                    }
                    util::log_error("HTTP listener descriptor went bad in accept(); rebinding");
                    if (!rebind_listener()) {
                        break;
                    }
                    continue;
                } else {
                    if (running_) {
                        util::log_error("Failed to accept connection: " + util::socket_error_message(err));
                    }
                    continue;
                }
            }

            // Dispatch connection to worker thread pool for concurrent handling
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                connection_queue_.push(client_fd);
            }
            queue_condition_.notify_one();
        }
    }

    // Clean up socket if not already closed
    auto fd = server_fd_.exchange(util::kInvalidSocket);
    if (fd != util::kInvalidSocket) {
        util::socket_close(fd);
    }
    util::log_info("Server stopped");
}

namespace {

// Per-connection socket timeout: a peer that stalls mid-request (slowloris)
// or mid-response is disconnected after this many seconds of inactivity.
constexpr int kSocketTimeoutSeconds = 30;

// Upper bound on how long a whole request may take to arrive (headers +
// body). Complements SO_RCVTIMEO, which only bounds the gap between bytes —
// large ImageArray responses are unaffected (this bounds the read side only).
constexpr int kRequestDeadlineSeconds = 120;

// How long a keep-alive connection may sit idle between requests before the
// worker gives it up. Persistent connections matter for timing: ConformU's
// .NET client stalled ~175 ms before opening each new TCP connection on a
// Raspberry Pi 3B, and with every response marked "Connection: close" that
// stall landed inside its FAST-target measurements (CameraState, CameraXSize,
// SensorType on a ZWO camera; DeviceState, AlignmentMode, EquatorialSystem on
// a mount) while the server itself answered in 2-8 ms. Kept well under the
// per-request slowloris bound so idle clients cannot pin the pool.
constexpr int kKeepAliveIdleSeconds = 15;

// Upper bound on requests served over one keep-alive connection. Without
// this, a small number of clients that simply send a request at least every
// kKeepAliveIdleSeconds (accidentally -- several long-lived Alpaca clients --
// or adversarially) can each pin one worker thread indefinitely, since the
// thread pool is fixed-size and the accept queue has no backpressure of its
// own (PR #2 review). Closing after N requests bounds how long any single
// connection can hold a worker, forcing well-behaved clients to reconnect
// (cheap: this is what keep-alive was added to avoid *per-request*, not
// forbid outright) and adversarial ones to give up a worker periodically.
constexpr std::uint64_t kMaxRequestsPerConnection = 1000;

// Upper bound on how long a single connection may stay persistent, regardless
// of request count. kMaxRequestsPerConnection alone still lets a connection
// that sends one request every kKeepAliveIdleSeconds legitimately hold a
// worker for up to ~4 hours (1000 * 15s), and a client that simply reconnects
// immediately afterward repeats that indefinitely (PR #2 review). This is a
// second, independent cap on the same failure mode: once a connection has
// been open this long, the NEXT response forces a reconnect no matter how few
// requests it has served. Well under the request-count cap's worst case, so
// it is the tighter bound in practice; a well-behaved long-lived client
// (autoguiding, ConformU) pays one extra handshake every few minutes, which
// is negligible next to the per-request handshake keep-alive exists to avoid.
constexpr int kMaxConnectionLifetimeSeconds = 300;

// Upper bound on the request line + headers; larger header blocks are
// rejected before any body is read.
constexpr std::size_t kMaxHeaderBytes = std::size_t{64} * 1024;

void send_error(util::SocketHandle socket_fd, int status, const char* reason, const char* body) {
    Response error_response;
    error_response.set_status(status, reason);
    error_response.set_body(body);
    std::string response_str = error_response.to_string();
    util::socket_send_all(socket_fd, response_str.c_str(), response_str.size());
}

// True when the client wants the connection kept open after this request
// (RFC 7230 §6.3): HTTP/1.1 persists unless it says "Connection: close";
// anything else (HTTP/1.0, or no version at all) closes unless it says
// "Connection: keep-alive". The header is a comma-separated token list, so
// match whole tokens rather than substrings.
bool wants_keep_alive(const Request& request) {
    std::string connection = request.get_header("connection");
    std::transform(connection.begin(), connection.end(), connection.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool says_close = false;
    bool says_keep_alive = false;
    std::size_t start = 0;
    while (start <= connection.size()) {
        std::size_t comma = connection.find(',', start);
        std::string token = connection.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (token == "close") {
            says_close = true;
        } else if (token == "keep-alive") {
            says_keep_alive = true;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (says_close) {
        return false;
    }
    if (request.http_version() != "HTTP/1.1") {
        return says_keep_alive;
    }
    return true;
}

// Read one full HTTP request: loop until the end-of-headers marker, then read
// exactly Content-Length body bytes (bounded by Request::kMaxBodyBytes).
// `raw_request` may arrive holding bytes left over from the previous request
// on a keep-alive connection (a pipelining client); any bytes past the end of
// this request are handed back in `surplus` for the next call.
// Returns false after sending an error response where possible (on a dead or
// timed-out socket nothing can be sent); the caller closes the connection.
//
// `idle_timeout_pending`, when non-null, means the socket's SO_RCVTIMEO is
// currently set to the short kKeepAliveIdleSeconds bound (the caller is
// between requests on a keep-alive connection and does not yet know whether
// the peer is idle or has already started sending). That short bound must
// only govern the WAIT for the next request's first byte -- once it arrives,
// this read is a normal in-progress request like any other and deserves the
// same kSocketTimeoutSeconds per-recv budget request 1 gets, not a tighter
// one just because it happens to be request 2+. So the first successful recv
// below restores the normal timeout and clears the flag; if `raw_request`
// already holds a complete request from pipelined carry-over, no recv occurs
// here at all and the flag is left for the caller to resolve on its own next
// read (nothing was ever idle-timed against this request).
bool read_request(util::SocketHandle socket_fd, std::string& raw_request, std::string& surplus,
                  bool* idle_timeout_pending) {
    char buffer[8192];

    // Total-request wall-clock deadline. SO_RCVTIMEO bounds each individual
    // recv, but a peer trickling one byte per just-under-timeout interval
    // would pass every per-recv check and pin this worker indefinitely.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kRequestDeadlineSeconds);

    // Returns false (fail closed, matching every other timeout-setting call
    // on this connection) if the restore itself fails -- silently leaving the
    // socket on the tighter idle timeout for the rest of this request would
    // reintroduce the exact bug this restore exists to fix.
    auto note_recv = [&]() {
        if (idle_timeout_pending != nullptr && *idle_timeout_pending) {
            if (!util::socket_set_recv_timeout(socket_fd, kSocketTimeoutSeconds)) {
                return false;
            }
            *idle_timeout_pending = false;
        }
        return true;
    };

    // Read until \r\n\r\n (end of headers); SO_RCVTIMEO bounds each recv.
    // Carried-over bytes may already hold the terminator, so look before the
    // first recv.
    std::size_t header_end = raw_request.find("\r\n\r\n");
    while (header_end == std::string::npos) {
        // Enforce the header-size cap before reading more: without a
        // terminator the whole buffer is header so far.
        if (raw_request.size() > kMaxHeaderBytes) {
            send_error(socket_fd, 431, "Request Header Fields Too Large", "Request headers too large");
            return false;
        }
        int bytes_read = util::socket_recv(socket_fd, buffer, static_cast<int>(sizeof(buffer)));
        if (bytes_read <= 0) {
            // Peer closed, error, or receive timeout — drop the connection.
            // Between keep-alive requests this is the normal way out.
            return false;
        }
        if (!note_recv()) {
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            send_error(socket_fd, 408, "Request Timeout", "Request took too long to arrive");
            return false;
        }
        raw_request.append(buffer, static_cast<std::size_t>(bytes_read));
        header_end = raw_request.find("\r\n\r\n");
    }
    // The chunk that finds the terminator is size-checked too — otherwise it
    // could push the header block up to one recv buffer past the cap
    // unchecked. Only the bytes up to the terminator count as headers.
    if (header_end > kMaxHeaderBytes) {
        send_error(socket_fd, 431, "Request Header Fields Too Large", "Request headers too large");
        return false;
    }

    // Parse Content-Length (case-insensitive) out of the header block
    std::size_t content_length = 0;
    {
        std::string headers = raw_request.substr(0, header_end + 2);
        std::transform(headers.begin(), headers.end(), headers.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto pos = headers.find("\r\ncontent-length:");
        if (pos != std::string::npos) {
            // Reject duplicate Content-Length headers outright (RFC 7230 §3.3.2)
            // instead of one layer using the first and another the last.
            if (headers.find("\r\ncontent-length:", pos + 1) != std::string::npos) {
                send_error(socket_fd, 400, "Bad Request", "Duplicate Content-Length");
                return false;
            }
            pos += std::strlen("\r\ncontent-length:");
            auto eol = headers.find("\r\n", pos);
            std::string value = headers.substr(pos, eol - pos);
            // Trim surrounding whitespace
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            try {
                std::size_t consumed = 0;
                content_length = std::stoul(value, &consumed);
                if (consumed != value.size()) {
                    throw std::invalid_argument("trailing garbage");
                }
            } catch (...) {
                send_error(socket_fd, 400, "Bad Request", "Invalid Content-Length");
                return false;
            }
            if (content_length > Request::kMaxBodyBytes) {
                send_error(socket_fd, 413, "Payload Too Large", "Request body too large");
                return false;
            }
        }
    }

    // Read exactly Content-Length body bytes
    const std::size_t expected_total = header_end + 4 + content_length;
    while (raw_request.size() < expected_total) {
        std::size_t remaining = expected_total - raw_request.size();
        int chunk = static_cast<int>(std::min(remaining, sizeof(buffer)));
        int bytes_read = util::socket_recv(socket_fd, buffer, chunk);
        if (bytes_read <= 0) {
            return false;
        }
        if (!note_recv()) {
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            send_error(socket_fd, 408, "Request Timeout", "Request took too long to arrive");
            return false;
        }
        raw_request.append(buffer, static_cast<std::size_t>(bytes_read));
    }

    // The body loop never over-reads, but the header phase can pull in the
    // start of a pipelined next request; hand those bytes back to the caller.
    if (raw_request.size() > expected_total) {
        surplus.assign(raw_request, expected_total, std::string::npos);
        raw_request.resize(expected_total);
    }

    return true;
}

}  // namespace

void Server::handle_connection(util::SocketHandle socket_fd) {
    // Bound how long a slow or stalled peer can hold this worker (slowloris)
    if (!util::socket_set_timeouts(socket_fd, kSocketTimeoutSeconds)) {
        // Fail closed: without recv/send timeouts this connection could pin a
        // worker thread forever (the slowloris hole the timeouts exist to plug).
        util::log_warning("Dropping connection, failed to set socket timeouts: " +
                          util::socket_error_message(util::socket_get_last_error()));
        return;
    }

    // Resolve the peer address once per connection so the router can
    // discriminate clients that send no ClientID in the per-client Connected
    // registry (issue #163).
    std::string remote_address;
    {
        struct sockaddr_storage peer {};
        util::SocketLen peer_len = sizeof(peer);
        char addr_buf[INET6_ADDRSTRLEN] = {0};
        if (getpeername(socket_fd, reinterpret_cast<struct sockaddr*>(&peer), &peer_len) == 0) {
            if (peer.ss_family == AF_INET) {
                inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in*>(&peer)->sin_addr, addr_buf, sizeof(addr_buf));
            } else if (peer.ss_family == AF_INET6) {
                // On the dual-stack listener an IPv4 client arrives as a
                // v4-mapped IPv6 peer (::ffff:a.b.c.d); report the plain
                // dotted form so the registry key and logs keep the format
                // the IPv4-only listener produced.
                const struct in6_addr* a6 = &reinterpret_cast<struct sockaddr_in6*>(&peer)->sin6_addr;
                if (IN6_IS_ADDR_V4MAPPED(a6)) {
                    struct in_addr a4 {};
                    std::memcpy(&a4, &a6->s6_addr[12], sizeof(a4));
                    inet_ntop(AF_INET, &a4, addr_buf, sizeof(addr_buf));
                } else {
                    inet_ntop(AF_INET6, a6, addr_buf, sizeof(addr_buf));
                }
            }
        }
        remote_address = addr_buf;
    }

    // Serve requests on this connection until the client asks to close, the
    // request is malformed, the idle gap runs out, or the send fails. Bytes
    // read past the end of one request (a pipelining client) seed the next.
    std::string carried;
    bool first_request = true;
    std::uint64_t requests_served = 0;
    const auto connection_opened_at = std::chrono::steady_clock::now();
    while (true) {
        // Whether the socket's SO_RCVTIMEO is currently the short idle bound
        // rather than the normal per-request one; read_request clears this
        // (restoring kSocketTimeoutSeconds) the moment the peer's first byte
        // of the new request actually arrives, so the 15s bound covers only
        // the wait between requests, never the request itself.
        bool idle_timeout_pending = false;
        if (!first_request) {
            // Between requests the peer may legitimately go quiet; bound how
            // long an idle keep-alive connection can hold this worker.
            if (!util::socket_set_recv_timeout(socket_fd, kKeepAliveIdleSeconds)) {
                return;
            }
            idle_timeout_pending = true;
        }

        // Read request (headers, then exactly Content-Length body bytes)
        std::string raw_request = std::move(carried);
        carried.clear();
        if (!read_request(socket_fd, raw_request, carried, &idle_timeout_pending)) {
            return;
        }

        // Parse request
        Request request;
        if (!request.parse(raw_request)) {
            send_error(socket_fd, 400, "Bad Request", "Invalid request");
            return;
        }
        request.set_remote_address(remote_address);

        bool keep_alive = wants_keep_alive(request);
        ++requests_served;
        const auto connection_age = std::chrono::steady_clock::now() - connection_opened_at;
        if (requests_served >= kMaxRequestsPerConnection ||
            connection_age >= std::chrono::seconds(kMaxConnectionLifetimeSeconds)) {
            // Force a reconnect so this connection can't hold the worker
            // forever -- by request count or by wall clock, whichever comes
            // first. A fresh TCP handshake at either bound is negligible next
            // to the per-request handshake this feature exists to avoid.
            keep_alive = false;
        }

        // Generate transaction ID (thread-safe)
        static std::atomic<std::uint32_t> transaction_counter{0};
        std::uint32_t server_tx_id = ++transaction_counter;

        // Route request
        Response response = router_.route(request, server_tx_id);

        // A handler that set its own Connection header can only narrow
        // keep_alive to false, never widen it back to true past the count/
        // lifetime caps above. Matched case-insensitively for consistency
        // with how the request-side Connection header is parsed in
        // wants_keep_alive -- no handler sets this today, but a
        // differently-cased "Keep-Alive" would otherwise be silently treated
        // as a close.
        const std::string& connection_header = response.get_header("Connection");
        if (!connection_header.empty()) {
            std::string lower_connection_header = connection_header;
            std::transform(lower_connection_header.begin(), lower_connection_header.end(),
                           lower_connection_header.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            keep_alive = keep_alive && lower_connection_header == "keep-alive";
        }
        // Always rewrite the header to match the final decision (rather than
        // only setting it when absent) -- otherwise a handler that had set
        // "Connection: keep-alive" before the count/lifetime caps forced
        // keep_alive to false would leave that stale header on the wire: the
        // client would read "keep-alive" while the server closes the socket
        // right after sending, a protocol-violating response (review round
        // 3). Explicitly writing "close" here is identical to leaving the
        // header unset, since Response::to_string() defaults to "close".
        response.set_header("Connection", keep_alive ? "keep-alive" : "close");

        // Send response (loop until fully sent; MSG_NOSIGNAL prevents SIGPIPE)
        std::string response_str = response.to_string();
        if (!util::socket_send_all(socket_fd, response_str.c_str(), response_str.size())) {
            util::log_warning("Failed to send full response: " +
                              util::socket_error_message(util::socket_get_last_error()));
            return;
        }

        if (!keep_alive) {
            return;
        }
        first_request = false;
    }
}

void Server::worker_thread() {
    while (true) {
        util::SocketHandle client_fd = util::kInvalidSocket;
        
        // Wait for a connection to handle
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_condition_.wait(lock, [this] {
                return !connection_queue_.empty() || shutdown_workers_;
            });
            
            if (shutdown_workers_ && connection_queue_.empty()) {
                // Shutdown requested and no more work
                break;
            }
            
            if (!connection_queue_.empty()) {
                client_fd = connection_queue_.front();
                connection_queue_.pop();
            }
        }
        
        if (client_fd != util::kInvalidSocket) {
            // Handle the connection
            handle_connection(client_fd);
            util::socket_close(client_fd);
        }
    }
}

void Server::handle_shutdown_request() {
    bool expected = false;
    if (!shutdown_requested_.compare_exchange_strong(expected, true)) {
        util::log_info("Shutdown request already in progress, ignoring duplicate request");
        return;
    }

    util::log_info("Shutdown requested via management endpoint");

    std::function<void()> callback_copy;
    {
        std::lock_guard<std::mutex> lock(shutdown_mutex_);
        callback_copy = shutdown_callback_;
    }

    if (callback_copy) {
        try {
            callback_copy();
        } catch (const std::exception& e) {
            util::log_error("Shutdown callback threw exception: " + std::string(e.what()));
        } catch (...) {
            util::log_error("Shutdown callback threw unknown exception");
        }
    }

    stop();
}

void Server::handle_restart_request() {
    bool expected = false;
    if (!restart_requested_.compare_exchange_strong(expected, true)) {
        util::log_info("Restart request already in progress, ignoring duplicate request");
        return;
    }

    util::log_info("Restart requested via management endpoint");

    std::function<void()> callback_copy;
    {
        std::lock_guard<std::mutex> lock(restart_mutex_);
        callback_copy = restart_callback_;
    }

    if (callback_copy) {
        try {
            callback_copy();
        } catch (const std::exception& e) {
            util::log_error("Restart callback threw exception: " + std::string(e.what()));
        } catch (...) {
            util::log_error("Restart callback threw unknown exception");
        }
    }

    util::log_info("Restarting HTTP server");
    stop();
    start_async();
    restart_requested_ = false;
}

} // namespace alpacahttp
