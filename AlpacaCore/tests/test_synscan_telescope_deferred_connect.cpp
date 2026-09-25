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

// SynScan telescope: auto-detect resolves at connect time, not at construction
// (issue #659). The persisted device is constructed at server start-up, when
// the hardware is often not there yet; the scan now runs inside the connect,
// its refusal is the connect error the client sees, and the resolved
// endpoint is reused on the next connect. Cases live in
// deferred_connect_cases.h; this file supplies the fake and the seam.

#ifndef _WIN32

#include <alpacacore/vendor/synscan/synscan_telescope_driver.h>

#include <memory>
#include <string>

#include "catch2_compat.h"
#include "deferred_connect_cases.h"
#include "fake_mount_server.h"

namespace {

using Info = alpacacore::vendor::synscan::ConnectionInfo;

using Fake = alpacacore::test::FakeMountServer;

// Enough of the hand controller for connect: the echo test, the firmware
// query and a parseable position pair.
std::string synscan_responder(const std::string& chunk) {
    if (chunk.empty()) return "0#";
    switch (chunk[0]) {
        case 'K':
            return std::string(1, chunk.size() > 1 ? chunk[1] : 'K') + "#";
        case 'V':
            return "042A00#";
        case 'e':
        case 'E':
        case 'z':
        case 'Z':
            return "12AB0500,20000500#";
        default:
            return "0#";
    }
}

Info endpoint(int port) {
    Info info;
    info.type = alpacacore::vendor::synscan::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 200;
    return info;
}

Info spawn(std::unique_ptr<Fake>& fake) {
    auto next = std::make_unique<Fake>(synscan_responder);
    REQUIRE(next->ok());
    fake = std::move(next);
    return endpoint(fake->port());
}

alpacacore::test::DeferredFactory<Info> make_driver() {
    return [](alpacacore::util::ConnectionResolver<Info> resolver) -> std::unique_ptr<alpacacore::AlpacaDriver> {
        return alpacacore::vendor::synscan::create_synscan_telescope_deferred(
            0, std::move(resolver), alpacacore::vendor::synscan::SynScanVersion::V4);
    };
}

}  // namespace

TEST_CASE("SynScan telescope auto-detect - a failed scan refuses the connect, not construction",
          "[synscan][telescope][unit]") {
    alpacacore::test::check_deferred_connect_refused<Info>(make_driver(), "nothing answered the auto-detect probe");
}

TEST_CASE("SynScan telescope auto-detect - resolves at connect and reuses the endpoint", "[synscan][telescope][unit]") {
    std::unique_ptr<Fake> fake;
    alpacacore::test::check_deferred_connect_reuses_endpoint<Info>(make_driver(), [&fake] { return spawn(fake); });
}

TEST_CASE("SynScan telescope auto-detect - re-scans when the resolved endpoint dies", "[synscan][telescope][unit]") {
    std::unique_ptr<Fake> fake;
    alpacacore::test::check_deferred_connect_re_resolves<Info>(make_driver(), [&fake] { return spawn(fake); });
}

#endif  // _WIN32
