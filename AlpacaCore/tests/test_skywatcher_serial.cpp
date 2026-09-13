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

// Serial-transport tests for the Sky-Watcher protocol wrapper, over a
// pty-backed fake motor controller (fake_skywatcher_serial_board.h). These
// drive exchange_serial() itself: the late-reply settle after a timeout, the
// mis-paired-reply shape check and resend, and the ":i" readback's failure
// classification. The UDP loopback tests in test_skywatcher_async.cpp cannot
// reach any of this.

#ifndef _WIN32

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/vendor/skywatcher/skywatcher_protocol_wrapper.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>

#include "catch2_compat.h"
#include "fake_skywatcher_serial_board.h"

using alpacacore::test::FakeSkyWatcherSerialBoard;
namespace sw = alpacacore::vendor::skywatcher;

namespace {

struct SerialLink {
    FakeSkyWatcherSerialBoard board;
    sw::SkyWatcherProtocolWrapper& proto = sw::SkyWatcherProtocolWrapper::instance();

    explicit SerialLink(int timeout_ms = 300) {
        sw::ConnectionInfo info;
        info.type = sw::ConnectionType::Serial;
        info.port_path = board.slave_path();
        info.baud_rate = 9600;
        info.response_timeout_ms = timeout_ms;
        REQUIRE(proto.connect(info));
        REQUIRE_FALSE(proto.get_motor_board_version().empty());  // ":e1" answered
    }
    ~SerialLink() { proto.disconnect(); }
};

}  // namespace

TEST_CASE("SkyWatcher serial - a reply that arrives after the timeout is not read as the next command's answer",
          "[skywatcher][serial]") {
    // A plain tcflush before the next write only discards bytes that have
    // ALREADY arrived. If the previous reply is still in flight it lands
    // after the flush and is consumed as this command's reply: here a ":j1"
    // answered with the STALE counts. exchange_serial marks the link dirty
    // after a timeout and settles (waits for the line to go quiet) before the
    // next write instead.
    SerialLink link(300);
    link.board.set_counts(1, 0x800000);
    // Lands 100 ms after the wrapper gives up, inside the 200 ms settle
    // window. A reply later than that window is only caught by the shape
    // check, which cannot help when it is the SAME command's shape.
    link.board.delay_next_reply(400);
    REQUIRE_THROWS_AS(link.proto.inquire_position(1), alpacacore::AlpacaException);

    link.board.set_counts(1, 0x812345);
    // The late "=…800000" reply is now on the line (or about to be). The next
    // inquiry must return the NEW counts, i.e. its own reply.
    REQUIRE(link.proto.inquire_position(1) == 0x812345);
}

TEST_CASE("SkyWatcher serial - a mis-paired OK reply is rejected and the command resent once", "[skywatcher][serial]") {
    // An OK reply whose data length does not match the command is a reply to
    // something else. Before the shape check, ":j1" answered with "=00" would
    // have been decoded as counts 0 (a 3-hex-char nothing) and reported as
    // success.
    SerialLink link(300);
    link.board.set_counts(1, 0x8000FF);
    link.board.mispair_next();
    REQUIRE(link.proto.inquire_position(1) == 0x8000FF);
    REQUIRE(link.board.count_frames('j') == 2);  // rejected once, resent once
}

TEST_CASE("SkyWatcher serial - giving up on a second mis-pair still settles the line", "[skywatcher][serial]") {
    // send_command settles the link before its one resend. It must also
    // settle before it gives up on a second mis-pair: the stale frame behind
    // the mis-paired reply is still in flight, and a caller that catches the
    // exception and carries on (the driver's dispatch threads do) would have
    // its NEXT command answered by it. When that straggler has the same
    // shape as the next reply the shape check cannot help, so only the
    // settle window catches it (PR #245 review).
    SerialLink link(300);
    link.board.set_counts(1, 0x800000);
    // Two mis-paired ":j1" replies, then a stale ":j" reply (the old counts)
    // landing 50 ms after the second one -- inside the settle window.
    link.board.mispair_next(2, "=800000", 50);
    REQUIRE_THROWS_AS(link.proto.inquire_position(1), alpacacore::AlpacaException);
    REQUIRE(link.board.count_frames('j') == 2);  // one resend, then gave up

    link.board.set_counts(1, 0x812345);
    // The next inquiry must get its OWN reply, not the straggler.
    REQUIRE(link.proto.inquire_position(1) == 0x812345);
}

TEST_CASE("SkyWatcher serial - a transient failure of the ':i' readback does not disable the diagnostic",
          "[skywatcher][serial]") {
    // Only an explicit "!0" (Unknown command) means the board has no ":i". A
    // timeout or a mis-pair on the readback itself is a transport blip, and a
    // busy link is exactly when the diagnostic matters, so it must stay on.
    SerialLink link(300);

    // 1. The ":i" reply times out: the write is done, the readback is skipped,
    //    nothing throws, and the NEXT write still reads back.
    link.board.delay_next_reply(400, 'i');  // 100 ms past the timeout, inside the settle window
    REQUIRE_NOTHROW(link.proto.set_step_period(1, 5000));
    REQUIRE(link.board.count_frames('I') == 1);
    REQUIRE(link.board.count_frames('i') == 1);
    REQUIRE_NOTHROW(link.proto.set_step_period(1, 5004));
    REQUIRE(link.board.count_frames('I') == 2);
    REQUIRE(link.board.count_frames('i') == 2);  // still enabled

    // 2. A refusal for another reason ("!2" Motor not stopped) is skipped, not
    //    treated as unsupported.
    link.board.reject_next_readback("2");
    REQUIRE_NOTHROW(link.proto.set_step_period(1, 5008));
    REQUIRE_NOTHROW(link.proto.set_step_period(1, 5012));
    REQUIRE(link.board.count_frames('i') == 4);  // still enabled

    // 3. "!0" Unknown command: off until the next connect.
    link.board.set_no_readback(true);
    REQUIRE_NOTHROW(link.proto.set_step_period(1, 5016));
    REQUIRE(link.board.count_frames('i') == 5);
    REQUIRE_NOTHROW(link.proto.set_step_period(1, 5020));
    REQUIRE(link.board.count_frames('i') == 5);  // no further ":i"
}

TEST_CASE("SkyWatcher serial - connect claims the port in the cross-vendor registry and releases it",
          "[skywatcher][serial][registry]") {
    // Issue #230: EQDIR cables share their USB-serial chips with other
    // vendors' hardware, so the registry is what keeps one driver's connect
    // off a port another driver holds. connect_serial() must refuse a held
    // port, claim a free one before opening, and release it on disconnect
    // and on every failure path.
    FakeSkyWatcherSerialBoard board;
    auto& proto = sw::SkyWatcherProtocolWrapper::instance();
    const std::string key = std::filesystem::canonical(board.slave_path()).string();
    REQUIRE_FALSE(alpacacore::util::is_serial_port_in_use(key));

    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Serial;
    info.port_path = board.slave_path();
    info.baud_rate = 9600;
    info.response_timeout_ms = 300;

    // Held by "another vendor": refused, nothing on the wire, still held.
    alpacacore::util::mark_serial_port_open(key);
    REQUIRE_FALSE(proto.connect(info));
    REQUIRE(board.frames().empty());
    REQUIRE(alpacacore::util::is_serial_port_in_use(key));
    alpacacore::util::mark_serial_port_closed(key);

    // Free: claimed for the life of the connection, released on disconnect.
    REQUIRE(proto.connect(info));
    REQUIRE(alpacacore::util::is_serial_port_in_use(key));
    REQUIRE_FALSE(proto.get_motor_board_version().empty());
    proto.disconnect();
    REQUIRE_FALSE(alpacacore::util::is_serial_port_in_use(key));

    // A failed open must not leave a stale claim behind.
    info.port_path = "/dev/alpacacore-no-such-port";
    REQUIRE_FALSE(proto.connect(info));
    REQUIRE_FALSE(alpacacore::util::is_serial_port_in_use(info.port_path));
}

TEST_CASE("SkyWatcher serial - the dual-baud probe finds a Synta EQ board at 115200 and carries the baud",
          "[skywatcher][serial][probe]") {
    // Issue #230: a Synta EQ board over its own USB port speaks 115200 and
    // never answered the 9600-only scan. probe_skywatcher_port_any_baud()
    // tries 9600 first (a Wave answers there) and then 115200.
    FakeSkyWatcherSerialBoard board;
    sw::MotorBoardInfo info;
    int baud = 0;

    SECTION("a Wave answers the 9600 attempt") {
        REQUIRE(sw::probe_skywatcher_port_any_baud(board.slave_path(), info, baud));
        CHECK(baud == 9600);
        CHECK(info.firmware_version == "3.58");
        CHECK(board.count_frames('e') == 1);  // no second attempt
    }

    SECTION("an EQM-35 board only decodes at 115200") {
        board.set_version_reply("032732");  // MC 3.39, mount code 0x32
        board.answer_only_at_baud(115200);
        REQUIRE(sw::probe_skywatcher_port_any_baud(board.slave_path(), info, baud));
        CHECK(baud == 115200);
        CHECK(info.firmware_version == "3.39");
        CHECK(info.mount_code == 0x32);
        CHECK(info.model_name == "EQM-35 Pro");
    }

    SECTION("a port another device holds is not probed at all") {
        // The re-check after open() (issue #230) and the echo guard's own
        // registry look: nothing is written to a held port at either rate.
        const std::string key = std::filesystem::canonical(board.slave_path()).string();
        alpacacore::util::mark_serial_port_open(key);
        REQUIRE_FALSE(sw::probe_skywatcher_port_any_baud(key, info, baud));
        REQUIRE(board.frames().empty());
        alpacacore::util::mark_serial_port_closed(key);
    }
}

// ── open-astro#445: Connected follows the serial link ──────────────────────

namespace {

sw::ConnectionInfo serial_info(const std::string& path) {
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Serial;
    info.port_path = path;
    info.baud_rate = 9600;
    info.response_timeout_ms = 300;
    return info;
}

std::unique_ptr<alpacacore::TelescopeDriver> serial_driver(const std::string& path) {
    return sw::create_skywatcher_telescope(0, serial_info(path), -37.0, 175.0, 50.0);
}

void require_not_connected_error(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const alpacacore::AlpacaException& e) {
        CHECK(e.error_code() == alpacacore::AlpacaError::NotConnected);
        return;
    }
    FAIL("expected AlpacaException(NotConnected)");
}

// A by-id style symlink to a fake board, repointable to a "replugged" one.
struct PortLink {
    std::filesystem::path path;
    explicit PortLink(const std::string& target) {
        path = std::filesystem::temp_directory_path() /
               ("alpacacore-sw445-" + std::to_string(::getpid()) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_symlink(target, path);
    }
    void repoint(const std::string& target) const {
        std::filesystem::remove(path);
        std::filesystem::create_symlink(target, path);
    }
    ~PortLink() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

}  // namespace

TEST_CASE("SkyWatcher serial - Connected drops when the adapter is pulled, with no client I/O",
          "[skywatcher][serial][connected]") {
    // The #445 rig trace: `connected` read True on every sample for 80 s with
    // the device node gone. Nothing is asked of the mount between the unplug
    // and the read, so the answer must come from the link, not from an error.
    FakeSkyWatcherSerialBoard board;
    const std::string key = std::filesystem::canonical(board.slave_path()).string();
    auto driver = serial_driver(board.slave_path());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    board.sever_link();
    CHECK_FALSE(driver->get_connected());
    // The stale descriptor is what kept the kernel from handing the returning
    // adapter its old name (/dev/ttyUSB0 -> ttyUSB1); noticing the loss
    // releases it, and the port's registry claim with it.
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(key));
    require_not_connected_error([&] { driver->get_right_ascension(); });

    // An explicit disconnect of the dead link still completes cleanly.
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("SkyWatcher serial - an operation that hits a dead link fails NotConnected and drops Connected",
          "[skywatcher][serial][connected]") {
    // The other half of the trace: "Serial write failed: Input/output error"
    // from every read, as a generic driver error, with Connected still True.
    FakeSkyWatcherSerialBoard board;
    auto& proto = sw::SkyWatcherProtocolWrapper::instance();
    REQUIRE(proto.connect(serial_info(board.slave_path())));
    REQUIRE_FALSE(proto.get_motor_board_version().empty());

    board.sever_link();
    require_not_connected_error([&] { proto.inquire_position(sw::kAxisRa); });
    CHECK_FALSE(proto.is_connected());
    proto.disconnect();
}

TEST_CASE("SkyWatcher serial - Connected=true on a stale link reconnects instead of short-circuiting",
          "[skywatcher][serial][connected]") {
    // The #445 recovery path: the adapter comes back (here under the same
    // by-id style symlink, repointed as udev does) and the client re-sends
    // Connected=true without a disconnect first. That used to return success
    // with no driver activity at all.
    auto board = std::make_unique<FakeSkyWatcherSerialBoard>();
    PortLink link(board->slave_path());
    auto driver = serial_driver(link.path.string());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());

    // A redundant connect on a LIVE link is still a no-op: nothing on the wire.
    const std::size_t frames_before = board->frames().size();
    driver->set_connected(true);
    CHECK(board->frames().size() == frames_before);

    board->sever_link();
    FakeSkyWatcherSerialBoard replugged;
    link.repoint(replugged.slave_path());
    REQUIRE(replugged.frames().empty());

    driver->set_connected(true);
    CHECK(driver->get_connected());
    CHECK(replugged.count_frames('e') >= 1);  // the connect sequence really ran
    CHECK_NOTHROW(driver->get_right_ascension());
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher serial - a connect whose board never answers leaves Connected false",
          "[skywatcher][serial][connected]") {
    // open() and tcsetattr() succeeding proves only that a port exists. The
    // ":e" identity query is allowed to fail, but the axis-parameter reads
    // after it are not; when they throw, the connect must not stay latched.
    FakeSkyWatcherSerialBoard board;
    board.answer_only_at_baud(115200);  // the driver speaks 9600: nothing decodes
    const std::string key = std::filesystem::canonical(board.slave_path()).string();
    auto driver = serial_driver(board.slave_path());

    CHECK_THROWS(driver->set_connected(true));
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(alpacacore::util::is_serial_port_in_use(key));
}

#endif  // _WIN32
