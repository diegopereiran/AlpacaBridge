// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// AlpacaCore is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
// for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with AlpacaCore. If not, see <https://www.gnu.org/licenses/>.

// Serial-transport tests for the Sky-Watcher protocol wrapper, over a
// pty-backed fake motor controller (fake_skywatcher_serial_board.h). These
// drive exchange_serial() itself: the late-reply settle after a timeout, the
// mis-paired-reply shape check and resend, and the ":i" readback's failure
// classification. The UDP loopback tests in test_skywatcher_async.cpp cannot
// reach any of this.

#ifndef _WIN32

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/skywatcher/skywatcher_protocol_wrapper.h>

#include <chrono>
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

#endif  // _WIN32
