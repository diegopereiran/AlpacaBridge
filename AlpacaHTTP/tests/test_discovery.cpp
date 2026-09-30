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

// Discovery responder over real UDP on 127.0.0.1:32227 (#562): the exact v1
// probe, the tolerated trailing bytes, the advertised port for http_port 0,
// and sharing the port with another Alpaca process. Skips when 32227 cannot
// be bound on this host.

#include <alpacahttp/config.h>
#include <alpacahttp/discovery.h>
#include <alpacahttp/server.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include "test_assert.h"

namespace {

constexpr std::uint16_t kDiscoveryPort = 32227;

// Send one datagram to the responder on 127.0.0.1:32227 from a fresh
// ephemeral socket and wait up to `timeout_ms` for its reply. nullopt means
// no reply arrived in that time.
std::optional<std::string> send_probe(const std::string& payload, int timeout_ms) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    EXPECT(fd >= 0);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    EXPECT(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kDiscoveryPort);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    const ssize_t sent =
        ::sendto(fd, payload.data(), payload.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    EXPECT(sent == static_cast<ssize_t>(payload.size()));
    char buf[512];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    ::close(fd);
    if (n <= 0) {
        return std::nullopt;
    }
    return std::string(buf, static_cast<std::size_t>(n));
}

// Bind a second UDP socket to 32227 the way another Alpaca process would:
// SO_REUSEPORT, plus SO_REUSEADDR when `with_reuseaddr`. True if the bind
// succeeded; the socket is closed again either way.
bool bind_discovery_peer(bool with_reuseaddr) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    EXPECT(fd >= 0);
    int on = 1;
    if (with_reuseaddr) {
        EXPECT(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0);
    }
    EXPECT(::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) == 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kDiscoveryPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    const int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) {
        std::cerr << "Peer bind of UDP 32227 (SO_REUSEPORT" << (with_reuseaddr ? " + SO_REUSEADDR" : " only")
                  << ") failed: " << std::strerror(errno) << "\n";
    }
    ::close(fd);
    return rc == 0;
}

std::string port_reply(std::uint16_t port) { return "{\"AlpacaPort\":" + std::to_string(port) + "}"; }

// Start `discovery` and wait briefly for it to bind; false if it could not
// (32227 held by another process), so the caller skips.
bool start_and_wait(alpacahttp::Discovery& discovery) {
    discovery.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return discovery.is_running();
}

}  // namespace

int main() {
    std::cout << "Testing discovery...\n";

    alpacahttp::Config config;
    config.set_discovery_enabled(true);
    config.set_http_port(11111);
    config.set_server_name("TestServer");
    config.set_manufacturer("TestManufacturer");
    config.set_location("TestLocation");

    {
        alpacahttp::Discovery discovery(config);
        if (!start_and_wait(discovery)) {
            std::cerr << "Discovery test skipped: unable to bind discovery socket.\n";
            return 0;
        }

        // The v1 request is exactly the 16 ASCII bytes "alpacadiscovery1".
        auto reply = send_probe("alpacadiscovery1", 2000);
        EXPECT(reply.has_value());
        EXPECT(*reply == port_reply(11111));

        // A datagram that only contains the text is not a probe.
        EXPECT(!send_probe("xxalpacadiscovery1yy", 500).has_value());
        EXPECT(!send_probe("alpacadiscovery1yy", 500).has_value());
        EXPECT(!send_probe("xxalpacadiscovery1", 500).has_value());
        EXPECT(!send_probe("alpacadiscovery", 500).has_value());

        // Trailing CR, LF, NUL and space are tolerated, in any mix.
        reply = send_probe("alpacadiscovery1\n", 2000);
        EXPECT(reply.has_value());
        EXPECT(*reply == port_reply(11111));
        reply = send_probe(std::string("alpacadiscovery1\r\n\0 ", 20), 2000);
        EXPECT(reply.has_value());
        EXPECT(*reply == port_reply(11111));

        // While the responder holds 32227, another Alpaca process must be able
        // to share it. On Linux, SO_REUSEADDR on both sides already lets two
        // UDP sockets share a port, so the SO_REUSEADDR + SO_REUSEPORT peer
        // binds even against a responder without SO_REUSEPORT; the peer that
        // sets only SO_REUSEPORT is the one that needs it on the responder.
        // Done last: with two SO_REUSEPORT sockets bound, unicast probes are
        // shared out between them.
        EXPECT(bind_discovery_peer(true));
        EXPECT(bind_discovery_peer(false));

        discovery.stop();
        EXPECT(!discovery.is_running());
    }

    // http_port 0: the responder advertises the port the server actually
    // bound, and says nothing until it knows it.
    {
        alpacahttp::Config ephemeral = config;
        ephemeral.set_http_port(0);

        alpacahttp::Server server(ephemeral);
        server.start_async();
        std::uint16_t bound = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (bound == 0 && std::chrono::steady_clock::now() < deadline) {
            bound = server.bound_port();
            if (bound == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (bound == 0) {
            std::cerr << "Discovery http_port 0 case skipped: server could not bind an ephemeral port.\n";
        } else {
            alpacahttp::Discovery discovery(ephemeral);
            if (!start_and_wait(discovery)) {
                std::cerr << "Discovery http_port 0 case skipped: unable to bind discovery socket.\n";
            } else {
                // Port not known yet: no reply rather than AlpacaPort 0.
                EXPECT(!send_probe("alpacadiscovery1", 500).has_value());

                discovery.set_advertised_port(bound);
                auto reply = send_probe("alpacadiscovery1", 2000);
                EXPECT(reply.has_value());
                EXPECT(*reply == port_reply(bound));
                discovery.stop();
            }
        }
        server.stop();
    }

    std::cout << "All discovery tests passed!\n";
    return 0;
}
