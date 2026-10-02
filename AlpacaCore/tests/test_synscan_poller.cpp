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
//
// Background position poller (issue #617). On the EQM-35 Pro a DeviceState
// read cost four hand-controller round trips ("z" and "e" ~230 ms each, "p"
// and "L" ~30 ms each), 0.56 s against ConformU's 0.1 s FAST target. The
// driver now fills Slewing, both position caches and SideOfPier from a
// background thread, so the getters answer from the cache. A FakeMountServer
// plays a handset with those reply delays, which makes every contract below
// measurable hardware-free:
//   - DeviceState, SideOfPier and Slewing cause no handset exchange;
//   - a muted handset latches a link fault instead of serving a stale cache;
//   - a command gets the link after at most one in-flight exchange;
//   - a position read that was in flight when a command invalidated the cache
//     is never published over that invalidation.
#ifndef _WIN32

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/synscan/synscan_protocol_wrapper.h>
#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "fake_mount_server.h"

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// ConformU's FAST response time target.
constexpr int kFastTargetMs = 100;
// A command may wait out one in-flight exchange (~230 ms), never a cycle
// (L + e + z + p = 520 ms).
constexpr int kCommandLinkBudgetMs = 400;

struct FakeHandsetState {
    // Position reads answered so far. Each "e" reply encodes its own ordinal
    // in the RA field, so a test can tell WHICH exchange a cached RA came from.
    std::atomic<int> ra_ordinal{0};
    std::atomic<bool> goto_active{false};
    // The "p" answer: 'E' (pointing east) is ASCOM pierWest (1).
    std::atomic<char> pointing{'E'};
    // Commands the handset leaves unanswered (the exchange times out).
    std::atomic<bool> p_silent{false};
    std::atomic<bool> l_silent{false};
    // Receipt time of the latest chunk starting with each command byte.
    std::array<std::atomic<Clock::rep>, 256> received_at{};

    void clear_receipts() {
        for (auto& stamp : received_at) {
            stamp.store(0);
        }
    }
};

// The RA field is one byte repeated ("2A2A2A2A"), so the first six and the
// last six hex digits agree and the 16-bit form ("2A2A") decodes to the same
// hours: RA = byte / 255 * 24 h whichever digits the wrapper keeps.
std::string ra_field(int ordinal, int digits) {
    char byte[3];
    std::snprintf(byte, sizeof(byte), "%02X", ordinal & 0xFF);
    std::string field;
    while (static_cast<int>(field.size()) < digits) {
        field += byte;
    }
    return field;
}

int ra_ordinal_of(double ra_hours) { return static_cast<int>(std::lround(ra_hours / 24.0 * 255.0)); }

alpacacore::test::FakeMountServer::Responder handset_responder(std::shared_ptr<FakeHandsetState> st) {
    return [st](const std::string& chunk) -> std::string {
        if (chunk.empty()) return "0#";
        st->received_at[static_cast<unsigned char>(chunk[0])].store(Clock::now().time_since_epoch().count());
        switch (chunk[0]) {
            case 'K':  // protocol echo: "K" + byte -> byte + "#" (the connect-time link check)
                return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
            case 'V':
                return "042A00#";
            case 'e':
                return ra_field(st->ra_ordinal.fetch_add(1) + 1, 8) + ",20000500#";
            case 'E':
                return ra_field(st->ra_ordinal.fetch_add(1) + 1, 4) + ",2000#";
            case 'z':
                return "12AB0500,20000500#";
            case 'Z':
                return "12AB,2000#";
            case 'L':
                if (st->l_silent.load()) return "";
                return st->goto_active.load() ? "1#" : "0#";
            case 'p':
                if (st->p_silent.load()) return "";
                return std::string(1, st->pointing.load()) + "#";
            case 'J':  // aligned
                return std::string(1, static_cast<char>(1)) + "#";
            case 't':  // tracking mode: chr(mode) + "#"
                return std::string(1, static_cast<char>(2)) + "#";
            case 'm':  // model id: chr(model) + "#"; 50 = EQM-35 Pro
                return std::string(1, static_cast<char>(50)) + "#";
            case 'w':  // location: 36d51m S, 174d45m E
                return std::string({36, 51, 0, 1, static_cast<char>(174), 45, 0, 0}) + "#";
            case 'h':  // time: 12:00:00, 1 October 2026, UTC, no DST
                return std::string({12, 0, 0, 10, 1, 26, 0, 0}) + "#";
            case 'r':
            case 'R':
            case 's':
            case 'S':
            case 'T':
            case 'M':
            case 'P':
            case 'W':
            case 'H':
                return "#";
            default:
                return "0#";
        }
    };
}

// The #617 trace: position reads ~230 ms, the single-byte reads ~30 ms.
void apply_handset_delays(alpacacore::test::FakeMountServer& server) {
    for (const char c : {'e', 'E', 'z', 'Z'}) {
        server.set_reply_delay(c, milliseconds(230));
    }
    for (const char c : {'p', 'L'}) {
        server.set_reply_delay(c, milliseconds(30));
    }
}

alpacacore::vendor::synscan::ConnectionInfo endpoint(int port) {
    alpacacore::vendor::synscan::ConnectionInfo info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 600;  // above the 230 ms reply delay, short enough to fail a muted cycle fast
    return info;
}

struct Rig {
    std::shared_ptr<FakeHandsetState> state = std::make_shared<FakeHandsetState>();
    alpacacore::test::FakeMountServer server{handset_responder(state)};
    std::unique_ptr<alpacacore::TelescopeDriver> driver;

    // A driver that is not connected yet.
    bool make() {
        if (!server.ok()) return false;
        apply_handset_delays(server);
        driver = alpacacore::vendor::synscan::create_synscan_telescope(0, endpoint(server.port()),
                                                                       alpacacore::vendor::synscan::SynScanVersion::V4);
        return true;
    }

    bool connect() {
        if (!make()) return false;
        driver->set_connected(true);
        return driver->get_connected();
    }

    // Exchanges of either spelling of a position read, or of a single-byte read.
    int count(char command) const {
        switch (command) {
            case 'e':
                return server.exchange_count('e') + server.exchange_count('E');
            case 'z':
                return server.exchange_count('z') + server.exchange_count('Z');
            default:
                return server.exchange_count(command);
        }
    }

    std::array<int, 4> link_counts() const { return {count('L'), count('e'), count('z'), count('p')}; }

    // Waits for the handset to receive `command` while the test thread is
    // idle: only the background poller can have sent it. The exchange is then
    // in flight for its whole reply delay.
    bool wait_for_poller_exchange(char command, int timeout_ms) const {
        const int before = count(command);
        const auto deadline = Clock::now() + milliseconds(timeout_ms);
        while (Clock::now() < deadline) {
            if (count(command) > before) return true;
            std::this_thread::sleep_for(milliseconds(1));
        }
        return false;
    }

    // Milliseconds from `since` until the handset received a chunk starting
    // with any byte of `commands`, or nullopt when none arrived in time.
    std::optional<long> arrival_ms(const std::string& commands, Clock::time_point since, int timeout_ms) const {
        const auto deadline = Clock::now() + milliseconds(timeout_ms);
        for (;;) {
            for (const char c : commands) {
                const Clock::rep stamp = state->received_at[static_cast<unsigned char>(c)].load();
                if (stamp != 0) {
                    const auto at = Clock::time_point(Clock::duration(stamp));
                    return static_cast<long>(std::chrono::duration_cast<milliseconds>(at - since).count());
                }
            }
            if (Clock::now() >= deadline) return std::nullopt;
            std::this_thread::sleep_for(milliseconds(1));
        }
    }
};

long elapsed_ms(Clock::time_point since) {
    return static_cast<long>(std::chrono::duration_cast<milliseconds>(Clock::now() - since).count());
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    const auto deadline = Clock::now() + milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(milliseconds(50));
    }
    return pred();
}

struct Thrown {
    int code = 0;
    std::string message;
};

// The Alpaca error a call throws, or nullopt when it returns normally.
std::optional<Thrown> thrown_by(const std::function<void()>& call) {
    try {
        call();
    } catch (const alpacacore::AlpacaException& ex) {
        return Thrown{static_cast<int>(ex.error_code()), ex.what()};
    }
    return std::nullopt;
}

bool has_state(const std::vector<alpacacore::DeviceState>& state, const std::string& name) {
    return std::any_of(state.begin(), state.end(), [&](const alpacacore::DeviceState& s) { return s.name == name; });
}

// The six reads rule 5 of the design puts behind the link-fault latch.
std::vector<std::pair<std::string, std::function<void()>>> link_reads(alpacacore::TelescopeDriver& d) {
    return {
        {"RightAscension", [&d] { static_cast<void>(d.get_right_ascension()); }},
        {"Declination", [&d] { static_cast<void>(d.get_declination()); }},
        {"Altitude", [&d] { static_cast<void>(d.get_altitude()); }},
        {"Azimuth", [&d] { static_cast<void>(d.get_azimuth()); }},
        {"Slewing", [&d] { static_cast<void>(d.get_slewing()); }},
        {"SideOfPier", [&d] { static_cast<void>(d.get_side_of_pier()); }},
    };
}

}  // namespace

// T1. RED before the poller: with the caches past their 2 s TTL the read does
// "z", "e", "p" and "L" itself, ~520 ms.
TEST_CASE("SynScan poller - DeviceState is answered from the cache with no handset exchange",
          "[synscan][telescope][poller]") {
    Rig rig;
    REQUIRE(rig.connect());
    auto& scope = *rig.driver;

    // The synchronous fallback (design rule 2): straight after connect, before
    // the first poll has landed, the reads still answer. This held before the
    // poller too; it is here so the poller cannot take it away.
    CHECK(scope.get_side_of_pier() == 1);
    CHECK_FALSE(scope.get_slewing());
    CHECK(std::isfinite(scope.get_right_ascension()));

    // The poller runs with no client asking: every link value is exchanged
    // again and again while this thread sleeps.
    const auto idle_before = rig.link_counts();
    std::this_thread::sleep_for(milliseconds(3000));
    const auto idle_after = rig.link_counts();
    CHECK(idle_after[0] - idle_before[0] >= 2);  // L
    CHECK(idle_after[1] - idle_before[1] >= 2);  // e
    CHECK(idle_after[2] - idle_before[2] >= 2);  // z
    CHECK(idle_after[3] - idle_before[3] >= 2);  // p

    // Tracking is read from the handset once and cached from then on (a "t"
    // that is not one of the four link values); take it out of the measurement.
    static_cast<void>(scope.get_tracking());

    // Each read waits past the TTL first. Every one must be inside the FAST
    // target. The exchange count is the minimum over three reads because a
    // poll cycle can begin, on its own schedule, inside the read's window; a
    // read that CAUSES exchanges causes them every time.
    int fewest_exchanges = 1000;
    std::vector<alpacacore::DeviceState> state;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::this_thread::sleep_for(milliseconds(2200));
        const auto before = rig.link_counts();
        const auto t0 = Clock::now();
        state = scope.get_device_state();
        const long read_ms = elapsed_ms(t0);
        const auto after = rig.link_counts();
        INFO("attempt " << attempt << ": DeviceState took " << read_ms << " ms");
        CHECK(read_ms < kFastTargetMs);
        int exchanges = 0;
        for (std::size_t i = 0; i < before.size(); ++i) {
            exchanges += after[i] - before[i];
        }
        fewest_exchanges = std::min(fewest_exchanges, exchanges);
    }
    CHECK(fewest_exchanges == 0);

    for (const char* name : {"Altitude", "Azimuth", "Declination", "RightAscension", "SideOfPier", "Slewing"}) {
        INFO(name);
        CHECK(has_state(state, name));
    }
    CHECK(has_state(state, "TimeStamp"));
}

// Recon gaps (a) and (b). RED before the poller: get_side_of_pier() sends "p"
// and get_slewing() sends "L" on every call, with no cache in front of either.
TEST_CASE("SynScan poller - SideOfPier and Slewing are cache-first", "[synscan][telescope][poller]") {
    Rig rig;
    REQUIRE(rig.connect());
    auto& scope = *rig.driver;
    // Let the poller fill the caches.
    std::this_thread::sleep_for(milliseconds(1500));

    int fewest_pier = 1000;
    int fewest_slewing = 1000;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::this_thread::sleep_for(milliseconds(310));

        int before = rig.count('p');
        auto t0 = Clock::now();
        CHECK(scope.get_side_of_pier() == 1);
        CHECK(elapsed_ms(t0) < kFastTargetMs);
        fewest_pier = std::min(fewest_pier, rig.count('p') - before);

        before = rig.count('L');
        t0 = Clock::now();
        CHECK_FALSE(scope.get_slewing());
        CHECK(elapsed_ms(t0) < kFastTargetMs);
        fewest_slewing = std::min(fewest_slewing, rig.count('L') - before);
    }
    CHECK(fewest_pier == 0);
    CHECK(fewest_slewing == 0);
}

// T2. RED before the poller: a muted handset is never noticed, and the reads
// serve the last cached value for ever.
TEST_CASE("SynScan poller - a silent handset latches a link fault and recovers without a reconnect",
          "[synscan][telescope][poller]") {
    Rig rig;
    REQUIRE(rig.connect());
    auto& scope = *rig.driver;
    std::this_thread::sleep_for(milliseconds(1500));
    REQUIRE(rig.server.connection_count() == 1);

    rig.server.set_muted(true);
    // Three failed cycles. A cycle is at most four timed-out exchanges
    // (4 x 600 ms) plus the ~500 ms wait, so 12 s covers the slowest shape.
    const bool latched = wait_until(
        [&] {
            const auto thrown = thrown_by([&] { static_cast<void>(scope.get_right_ascension()); });
            return thrown.has_value() && thrown->code == alpacacore::AlpacaError::DriverException;
        },
        12000);
    REQUIRE(latched);

    // Faulted: every link-backed read refuses, at once, and says why.
    for (const auto& [name, read] : link_reads(scope)) {
        INFO(name);
        const auto t0 = Clock::now();
        const auto thrown = thrown_by(read);
        CHECK(elapsed_ms(t0) < kFastTargetMs);
        REQUIRE(thrown.has_value());
        CHECK(thrown->code == alpacacore::AlpacaError::DriverException);
        CHECK(thrown->message.find("SynScan communications compromised") != std::string::npos);
    }

    // Connected is the client's decision, and static metadata needs no link.
    CHECK(scope.get_connected());
    CHECK_FALSE(scope.get_name().empty());
    CHECK_FALSE(scope.get_description().empty());
    CHECK(scope.get_can_park());
    CHECK(scope.get_interface_version() == 4);

    // DeviceState degrades through the base class's per-getter catch.
    const auto state = scope.get_device_state();
    CHECK(has_state(state, "TimeStamp"));
    CHECK_FALSE(has_state(state, "RightAscension"));
    CHECK_FALSE(has_state(state, "Slewing"));
    CHECK_FALSE(has_state(state, "SideOfPier"));

    // Polling goes on at the normal cadence while faulted: that is what lets
    // the first good cycle clear the latch.
    const int polls_before = rig.count('L') + rig.count('e') + rig.count('z') + rig.count('p');
    std::this_thread::sleep_for(milliseconds(4000));
    CHECK(rig.count('L') + rig.count('e') + rig.count('z') + rig.count('p') > polls_before);

    rig.server.set_muted(false);
    const bool recovered = wait_until(
        [&] {
            for (const auto& [name, read] : link_reads(scope)) {
                if (thrown_by(read).has_value()) return false;
            }
            return true;
        },
        6000);
    CHECK(recovered);
    CHECK(scope.get_connected());
    CHECK(scope.get_side_of_pier() == 1);
    // Same TCP connection throughout: nothing reconnected.
    CHECK(rig.server.connection_count() == 1);
}

// T3. RED before the poller: there is no poll cycle to be in the middle of
// (the wait for one fails). Afterwards it holds the yield rule: AbortSlew
// called as a cycle begins reaches the handset after at most the exchange in
// flight, not after the 520 ms cycle.
TEST_CASE("SynScan poller - AbortSlew wins the link from a poll cycle in flight", "[synscan][telescope][poller]") {
    Rig rig;
    REQUIRE(rig.connect());
    auto& scope = *rig.driver;

    for (int round = 0; round < 3; ++round) {
        INFO("round " << round);
        // "L" opens a cycle; "e", "z" and "p" are still to come.
        REQUIRE(rig.wait_for_poller_exchange('L', 3000));
        rig.state->clear_receipts();
        const auto t0 = Clock::now();
        scope.abort_slew();
        const long call_ms = elapsed_ms(t0);
        const auto cancel_ms = rig.arrival_ms("M", t0, 2000);
        REQUIRE(cancel_ms.has_value());
        INFO("cancel reached the handset after " << *cancel_ms << " ms, call took " << call_ms << " ms");
        CHECK(*cancel_ms < kCommandLinkBudgetMs);
        CHECK(call_ms < 1000);  // ConformU's STANDARD target
    }
}

// The other command paths that talk to the handset. The design names the
// ASCOM commands; the survey added the site and clock writes. Same rule and
// the same budget for each.
TEST_CASE("SynScan poller - every command path wins the link from a poll cycle in flight",
          "[synscan][telescope][poller]") {
    Rig rig;
    REQUIRE(rig.connect());
    auto& scope = *rig.driver;

    struct Command {
        const char* name;
        std::string first_bytes;  // what reaches the handset first
        std::function<void()> call;
    };
    const std::vector<Command> commands = {
        {"set_tracking", "T", [&] { scope.set_tracking(true); }},
        {"set_utc_date", "H", [&] { scope.set_utc_date(std::chrono::system_clock::now()); }},
        {"set_site_latitude", "wW", [&] { scope.set_site_latitude(-36.85); }},
        {"set_site_longitude", "wW", [&] { scope.set_site_longitude(174.75); }},
        {"sync_to_coordinates", "sS", [&] { scope.sync_to_coordinates(5.0, 20.0); }},
        {"move_axis", "P", [&] { scope.move_axis(0, 1.0); }},
        {"move_axis stop", "P", [&] { scope.move_axis(0, 0.0); }},
        // The GOTO itself leaves from the slew task thread: it is a command
        // too, and yields the same way.
        {"slew_to_coordinates_async", "rR",
         [&] {
             rig.state->goto_active.store(true);
             scope.slew_to_coordinates_async(5.0, 20.0);
         }},
    };

    for (const auto& command : commands) {
        INFO(command.name);
        REQUIRE(rig.wait_for_poller_exchange('L', 3000));
        rig.state->clear_receipts();
        const auto t0 = Clock::now();
        REQUIRE_NOTHROW(command.call());
        const auto arrived_ms = rig.arrival_ms(command.first_bytes, t0, 2000);
        REQUIRE(arrived_ms.has_value());
        INFO("reached the handset after " << *arrived_ms << " ms");
        CHECK(*arrived_ms < kCommandLinkBudgetMs);
    }

    rig.state->goto_active.store(false);
    scope.abort_slew();
}

// Recon gap (e). A position exchange already on the wire when a command
// invalidates the caches carries a pre-command position. The poller must drop
// it (the generation moved) instead of publishing it as fresh. RED before the
// poller: there is no poller exchange to be in flight.
TEST_CASE("SynScan poller - a read in flight across an invalidation is never published",
          "[synscan][telescope][poller]") {
    Rig rig;
    REQUIRE(rig.connect());
    auto& scope = *rig.driver;

    for (int round = 0; round < 2; ++round) {
        INFO("round " << round);
        // The poller's "e" is on the wire for the next ~230 ms; its reply
        // carries this ordinal.
        REQUIRE(rig.wait_for_poller_exchange('e', 3000));
        const int in_flight = rig.state->ra_ordinal.load();
        REQUIRE(in_flight < 200);  // the ordinal is one byte

        // The slew initiator invalidates both position caches at once. The
        // handset reports the GOTO in progress, so no slew-end edge moves the
        // reads onto the target override.
        rig.state->goto_active.store(true);
        scope.slew_to_coordinates_async(5.0, 20.0);

        // Until the next cycle would overwrite it, every RA a client reads
        // must come from an exchange that began after the invalidation.
        const auto until = Clock::now() + milliseconds(900);
        int reads = 0;
        while (Clock::now() < until) {
            const int served = ra_ordinal_of(scope.get_right_ascension());
            INFO("read " << reads << " served ordinal " << served << ", in flight was " << in_flight);
            CHECK(served > in_flight);
            ++reads;
            std::this_thread::sleep_for(milliseconds(40));
        }
        CHECK(reads > 0);

        rig.state->goto_active.store(false);
        scope.abort_slew();
        std::this_thread::sleep_for(milliseconds(1200));
    }
}

// A GOTO across the meridian, a sync or an abort can leave the mount on the
// other side of the pier, and the poller reads "p" again only at the end of
// its next cycle. SideOfPier (and pulse_guide's DEC direction) must not serve
// the pier side read before the move in that window.
TEST_CASE("SynScan poller - SideOfPier is not served from before a sync or abort", "[synscan][telescope][poller]") {
    Rig rig;
    REQUIRE(rig.connect());
    auto& scope = *rig.driver;
    // Let the poller publish its own "p".
    std::this_thread::sleep_for(milliseconds(1500));
    REQUIRE(scope.get_side_of_pier() == 1);

    rig.state->pointing.store('W');  // pointing west: ASCOM pierEast (0)
    scope.sync_to_coordinates(5.0, 20.0);
    CHECK(scope.get_side_of_pier() == 0);

    rig.state->pointing.store('E');
    scope.abort_slew();
    CHECK(scope.get_side_of_pier() == 1);
}

// Lifecycle (design rule 6). RED before the poller on the two waits for a
// poller exchange.
TEST_CASE("SynScan poller - disconnect stops it promptly and a reconnect starts it again",
          "[synscan][telescope][poller]") {
    Rig rig;
    REQUIRE(rig.connect());
    auto& scope = *rig.driver;

    // Disconnect while a 230 ms position read is on the wire: the join waits
    // for that one exchange at most.
    REQUIRE(rig.wait_for_poller_exchange('e', 3000));
    const auto t0 = Clock::now();
    scope.set_connected(false);
    CHECK(elapsed_ms(t0) < 1000);
    CHECK_FALSE(scope.get_connected());

    // Stopped: nothing more reaches the handset.
    std::this_thread::sleep_for(milliseconds(400));
    const auto quiet_before = rig.link_counts();
    std::this_thread::sleep_for(milliseconds(1500));
    CHECK(rig.link_counts() == quiet_before);

    scope.set_connected(true);
    REQUIRE(scope.get_connected());
    CHECK(rig.server.connection_count() == 2);
    CHECK(rig.wait_for_poller_exchange('L', 3000));
}

// F3. A handset that answers L, e and z but not "p" is not a dead link: only
// the pier side is lost, and RA, Dec, Alt, Az and Slewing keep answering.
TEST_CASE("SynScan poller - a handset that does not answer p does not latch a link fault",
          "[synscan][telescope][poller]") {
    Rig rig;
    REQUIRE(rig.connect());
    auto& scope = *rig.driver;
    std::this_thread::sleep_for(milliseconds(1500));
    rig.state->p_silent.store(true);
    // Well past the three cycles that latch a fault.
    std::this_thread::sleep_for(milliseconds(14000));
    for (const auto& [name, read] : link_reads(scope)) {
        if (name == "SideOfPier") continue;  // asks the handset itself and gets no answer
        INFO(name);
        const auto thrown = thrown_by(read);
        CHECK_FALSE(thrown.has_value());
    }
}

// L1. A park, jog or forced slew the driver owns is Slewing whatever the link
// says: the overlays answer before the fault check.
TEST_CASE("SynScan poller - Slewing is true for a jog in flight on a faulted link", "[synscan][telescope][poller]") {
    Rig rig;
    REQUIRE(rig.connect());
    auto& scope = *rig.driver;
    std::this_thread::sleep_for(milliseconds(1500));
    rig.state->l_silent.store(true);
    REQUIRE(wait_until(
        [&] {
            const auto thrown = thrown_by([&] { static_cast<void>(scope.get_right_ascension()); });
            return thrown.has_value() && thrown->code == alpacacore::AlpacaError::DriverException;
        },
        15000));
    // Faulted and idle: Slewing refuses like the other link reads.
    CHECK(thrown_by([&] { static_cast<void>(scope.get_slewing()); }).has_value());

    scope.move_axis(0, 1.0);
    bool slewing = false;
    const auto thrown = thrown_by([&] { slewing = scope.get_slewing(); });
    CHECK_FALSE(thrown.has_value());
    CHECK(slewing);
    scope.move_axis(0, 0.0);
}

// F1. A sync disconnect racing a sync connect: both return, and the driver
// ends down with no poller left talking to the handset.
TEST_CASE("SynScan poller - a sync connect racing a sync disconnect both return", "[synscan][telescope][poller]") {
    for (int round = 0; round < 12; ++round) {
        INFO("round " << round);
        Rig rig;
        REQUIRE(rig.make());
        auto& scope = *rig.driver;
        auto connect = std::async(std::launch::async, [&] { scope.set_connected(true); });
        std::this_thread::sleep_for(milliseconds(round * 15));
        auto disconnect = std::async(std::launch::async, [&] { scope.set_connected(false); });
        REQUIRE(connect.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
        REQUIRE(disconnect.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
        connect.get();
        disconnect.get();
        scope.set_connected(false);
        CHECK_FALSE(scope.get_connected());
        std::this_thread::sleep_for(milliseconds(300));
        const auto quiet = rig.link_counts();
        std::this_thread::sleep_for(milliseconds(1200));
        CHECK(rig.link_counts() == quiet);
    }
}

#endif  // !_WIN32
