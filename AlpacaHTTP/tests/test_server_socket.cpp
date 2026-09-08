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

// Socket-level tests for the Server request-read path (read_request), which the
// Router-level test_routing.cpp cannot reach because it feeds Router::route
// directly. Covers the header-size (431) boundary fixed in #128: the cap must
// be enforced on the recv chunk that contains the \r\n\r\n terminator, not only
// on earlier chunks. See issue #129.

#include <alpacahttp/config.h>
#include <alpacahttp/server.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "test_assert.h"

namespace {

// Must match kMaxHeaderBytes in AlpacaHTTP/src/http/server.cpp (not exported).
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

// Must match kMaxRequestsPerConnection in AlpacaHTTP/src/http/server.cpp (not exported).
constexpr std::uint64_t kMaxRequestsPerConnection = 1000;

// Send `data` to 127.0.0.1:port in two writes — the terminator-bearing tail
// goes in the second write so we exercise the fixed path (the chunk that finds
// \r\n\r\n must itself be size-checked). Returns the first line of the response,
// or "" if the connection produced nothing.
std::string send_split_request(std::uint16_t port, const std::string& data, std::size_t first_chunk) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return "";
    }

    first_chunk = std::min(first_chunk, data.size());
    ::send(fd, data.data(), first_chunk, 0);
    // Brief gap so the two writes tend to arrive as separate recvs on the server.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ::send(fd, data.data() + first_chunk, data.size() - first_chunk, 0);

    std::string response;
    char buf[2048];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        response.append(buf, static_cast<std::size_t>(n));
        if (response.find("\r\n") != std::string::npos) {
            break;  // have the status line
        }
    }
    ::close(fd);

    auto eol = response.find("\r\n");
    return eol == std::string::npos ? response : response.substr(0, eol);
}

// Build a request whose header block (bytes before the terminating \r\n\r\n) is
// exactly `header_bytes` long, padding a single X-Pad header to hit the target.
std::string make_request_with_header_size(std::size_t header_bytes) {
    const std::string prefix = "GET / HTTP/1.1\r\nX-Pad: ";
    EXPECT(header_bytes >= prefix.size());
    std::string req = prefix;
    req.append(header_bytes - prefix.size(), 'a');
    req.append("\r\n\r\n");  // ends X-Pad line + empty line => \r\n\r\n terminator
    return req;
}

// --- keep-alive helpers ------------------------------------------------------

int connect_local(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void send_all(int fd, const std::string& data) {
    EXPECT(::send(fd, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size()));
}

// Read exactly one HTTP response (status line + headers + Content-Length body)
// from `fd`. `carry` holds bytes already received past the previous response
// (pipelined replies) and is updated for the next call. Returns "" if the
// peer closed before a full header block arrived.
std::string read_one_response(int fd, std::string& carry) {
    char tmp[4096];
    std::size_t header_end = carry.find("\r\n\r\n");
    while (header_end == std::string::npos) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            return "";
        }
        carry.append(tmp, static_cast<std::size_t>(n));
        header_end = carry.find("\r\n\r\n");
    }
    std::string headers = carry.substr(0, header_end);
    std::transform(headers.begin(), headers.end(), headers.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::size_t content_length = 0;
    auto pos = headers.find("content-length:");
    if (pos != std::string::npos) {
        content_length = std::stoul(headers.substr(pos + std::strlen("content-length:")));
    }
    const std::size_t total = header_end + 4 + content_length;
    while (carry.size() < total) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            break;
        }
        carry.append(tmp, static_cast<std::size_t>(n));
    }
    std::string response = carry.substr(0, total);
    carry.erase(0, total);
    return response;
}

// True if the server has closed the connection (EOF within `ms`); false if it
// is still open (the peek times out).
bool peer_closed(int fd, int ms) {
    struct timeval tv {};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char c = 0;
    return ::recv(fd, &c, 1, MSG_PEEK) == 0;
}

}  // namespace

int main() {
    std::cout << "Testing Server socket read path and keep-alive...\n";

    alpacahttp::Config config;
    config.set_http_port(6871);
    config.set_discovery_enabled(false);
    config.set_server_name("TestServer");

    alpacahttp::Server server(config);
    server.start_async();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    if (!server.is_running()) {
        std::cerr << "Server socket test skipped: unable to bind port 6871.\n";
        return 0;  // tolerate a busy/unavailable port, like the discovery test
    }

    const std::uint16_t port = config.http_port();

    // Over the cap by one byte, terminator in the second write => 431.
    {
        std::string req = make_request_with_header_size(kMaxHeaderBytes + 1);
        std::string status = send_split_request(port, req, req.size() - 8);
        EXPECT(status.find(" 431") != std::string::npos);
    }

    // Exactly at the cap, same split => accepted (parsed and routed, NOT 431).
    {
        std::string req = make_request_with_header_size(kMaxHeaderBytes);
        std::string status = send_split_request(port, req, req.size() - 8);
        EXPECT(!status.empty());
        EXPECT(status.find(" 431") == std::string::npos);
    }

    // A normal small request still gets a well-formed response (sanity).
    {
        std::string req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
        std::string status = send_split_request(port, req, req.size() - 4);
        EXPECT(status.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(status.find(" 431") == std::string::npos);
    }

    // --- Keep-alive ---------------------------------------------------------
    const std::string kGet11 = "GET /management/apiversions HTTP/1.1\r\nHost: localhost\r\n\r\n";

    // HTTP/1.1 is persistent by default: two requests on one connection, each
    // answered with "Connection: keep-alive", then "Connection: close" ends it.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11);
        std::string r1 = read_one_response(fd, carry);
        EXPECT(r1.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(r1.find("Connection: keep-alive\r\n") != std::string::npos);
        EXPECT(!peer_closed(fd, 200));
        send_all(fd, kGet11);
        std::string r2 = read_one_response(fd, carry);
        EXPECT(r2.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(r2.find("Connection: keep-alive\r\n") != std::string::npos);
        send_all(fd, "GET /management/apiversions HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        std::string r3 = read_one_response(fd, carry);
        EXPECT(r3.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // HTTP/1.0 closes by default...
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, "GET /management/apiversions HTTP/1.0\r\nHost: localhost\r\n\r\n");
        std::string r = read_one_response(fd, carry);
        EXPECT(r.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // ...unless the client asks for keep-alive.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        const std::string get10_ka =
            "GET /management/apiversions HTTP/1.0\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
        send_all(fd, get10_ka);
        std::string r1 = read_one_response(fd, carry);
        EXPECT(r1.find("Connection: keep-alive\r\n") != std::string::npos);
        EXPECT(!peer_closed(fd, 200));
        send_all(fd, get10_ka);
        std::string r2 = read_one_response(fd, carry);
        EXPECT(r2.find("Connection: keep-alive\r\n") != std::string::npos);
        ::close(fd);
    }

    // Pipelined: two requests in a single write are answered in order on the
    // same connection (the bytes past the first request are carried over).
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11 + kGet11);
        std::string r1 = read_one_response(fd, carry);
        std::string r2 = read_one_response(fd, carry);
        EXPECT(r1.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(r2.rfind("HTTP/1.1 ", 0) == 0);
        EXPECT(r2.find("Connection: keep-alive\r\n") != std::string::npos);
        EXPECT(!peer_closed(fd, 200));
        ::close(fd);
    }

    // A connection is force-closed after kMaxRequestsPerConnection requests,
    // even though every one of them individually asked to keep the
    // connection alive -- the worker-pinning mitigation added in response to
    // the PR #2 review must actually fire, not just exist as an unused cap.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        for (std::uint64_t i = 1; i < kMaxRequestsPerConnection; ++i) {
            send_all(fd, kGet11);
            std::string r = read_one_response(fd, carry);
            EXPECT(r.find("Connection: keep-alive\r\n") != std::string::npos);
        }
        EXPECT(!peer_closed(fd, 200));
        send_all(fd, kGet11);
        std::string last = read_one_response(fd, carry);
        EXPECT(last.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // A malformed request on a persistent connection gets 400 and a close.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11);
        std::string r1 = read_one_response(fd, carry);
        EXPECT(r1.find("Connection: keep-alive\r\n") != std::string::npos);
        send_all(fd, "GARBAGE\r\n\r\n");
        std::string r2 = read_one_response(fd, carry);
        EXPECT(r2.find(" 400 ") != std::string::npos);
        EXPECT(r2.find("Connection: close\r\n") != std::string::npos);
        EXPECT(peer_closed(fd, 2000));
        ::close(fd);
    }

    // A slow-but-legitimate request body on a keep-alive connection's SECOND
    // request must not be held to the short idle-wait bound. Before the fix,
    // SO_RCVTIMEO was set to kKeepAliveIdleSeconds (15s) for the whole of
    // request 2+ and never restored once the peer started sending, so a body
    // arriving in two writes >15s apart -- fine on request 1, which gets the
    // full 30s kSocketTimeoutSeconds per recv -- would time out and drop the
    // connection purely because it happened to be request 2. The timeout is
    // now restored the moment the peer's first byte of the new request
    // arrives, so only the true gap BETWEEN requests is 15s-limited.
    {
        int fd = connect_local(port);
        EXPECT(fd >= 0);
        std::string carry;
        send_all(fd, kGet11);
        std::string r1 = read_one_response(fd, carry);
        EXPECT(r1.find("Connection: keep-alive\r\n") != std::string::npos);

        const std::string body = "{}";
        std::string headers =
            "POST /nonexistent HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\n\r\n";
        send_all(fd, headers + body.substr(0, 1));
        // Longer than the 15s idle bound, shorter than the 30s per-request one.
        std::this_thread::sleep_for(std::chrono::seconds(16));
        send_all(fd, body.substr(1));
        std::string r2 = read_one_response(fd, carry);
        EXPECT(!r2.empty());
        ::close(fd);
    }

    // stop() must not wait out an ACTIVE keep-alive client. stop() joins every
    // worker, and a worker only leaves handle_connection's loop when the
    // connection ends -- so before the running_ check in that loop, a client
    // that kept sending (NINA/PHD2 polling) held its worker, and therefore
    // stop(), until the 300s lifetime cap; systemd would SIGKILL the service
    // at its 90s TimeoutStopSec first. Measured 26s of stop() latency behind
    // a client sending every 2s. Now the first response after stop() carries
    // "Connection: close" and the worker exits. This must be the last test:
    // it stops the server.
    {
        std::atomic<bool> got_close{false};
        std::atomic<int> served{0};
        std::thread client([&] {
            int fd = connect_local(port);
            EXPECT(fd >= 0);
            std::string carry;
            for (int i = 0; i < 40; ++i) {  // up to ~20s of activity, every 500 ms
                send_all(fd, kGet11);
                std::string r = read_one_response(fd, carry);
                if (r.empty()) {
                    break;  // server closed the socket
                }
                ++served;
                if (r.find("Connection: close\r\n") != std::string::npos) {
                    got_close = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            ::close(fd);
        });
        // Let the client get a couple of keep-alive responses in first.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        const int served_before_stop = served.load();
        EXPECT(served_before_stop >= 2);

        const auto t0 = std::chrono::steady_clock::now();
        server.stop();
        const auto stop_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        client.join();

        EXPECT(!server.is_running());
        // One in-flight idle gap (<= 500 ms here) plus scheduling slack, not
        // the ~19 s the client was prepared to keep going.
        EXPECT(stop_ms < 5000);
        EXPECT(got_close.load());
        // stop() answered at most one more request after being called.
        EXPECT(served.load() <= served_before_stop + 1);
    }

    std::cout << "All server socket tests passed!\n";
    return 0;
}
