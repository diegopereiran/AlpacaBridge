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

// Connect/disconnect/operate concurrency stress for the iOptron
// telescope (audit follow-up, 3.0.1). The audited detached-thread fixes on
// this driver (async slew + pulse-guide threads racing set_connected(false)
// and destruction) only run on a CONNECTED driver, so these tests connect
// through a FakeMountServer: the protocol wrapper treats a successful TCP
// connect as mount-connected and every post-connect query tolerates failure,
// so a canned "0#" responder is enough to spawn the driver's worker threads
// on a hardware-free host. Operations racing a disconnect are EXPECTED to
// throw; what must never happen is a crash, hang, or TSan report.

#ifndef _WIN32

#include <alpacacore/filterwheel_driver.h>
#include <alpacacore/focuser_driver.h>
#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_filterwheel_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_ieaf_focuser_driver.h>
#include <alpacacore/vendor/ioptron/ioptron_protocol_wrapper.h>
#include <alpacacore/vendor/ioptron/ioptron_telescope_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_mount_server.h"

using alpacacore::AlpacaDriver;

namespace {

alpacacore::vendor::ioptron::ConnectionInfo ioptron_endpoint(int port) {
    alpacacore::vendor::ioptron::ConnectionInfo info;
    info.type = alpacacore::vendor::ioptron::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.tcp_port = port;
    info.response_timeout_ms = 50;  // dumb canned replies leave some reads to time out — keep that cheap
    return info;
}

void telescope_operate(AlpacaDriver& d) {
    auto& scope = static_cast<alpacacore::TelescopeDriver&>(d);
    static_cast<void>(scope.get_tracking());
    static_cast<void>(scope.get_right_ascension());
    static_cast<void>(scope.get_declination());
    static_cast<void>(scope.get_slewing());
    // The newly-fixed thread paths: async slew and pulse guiding issued
    // while the lifecycle threads disconnect underneath them.
    scope.slew_to_coordinates_async(5.0, 20.0);
    scope.pulse_guide(0, 50);
    scope.abort_slew();
}

}  // namespace

TEST_CASE("iOptron telescope - concurrent connect/disconnect/slew/pulse stress", "[ioptron][telescope][stress]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, ioptron_endpoint(server.port()));

    alpacacore::test::run_lifecycle_stress(*driver, telescope_operate);

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("iOptron telescope - destruction races an in-flight connect", "[ioptron][telescope][stress]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());
    const int port = server.port();
    alpacacore::test::run_destruction_during_connect_stress(
        [port]() { return alpacacore::vendor::ioptron::create_ioptron_telescope(0, ioptron_endpoint(port)); },
        // 25 iterations, not the default 100: with the dumb canned replies a
        // fake-connected mount's connect sequence rides several read
        // timeouts, and each destruction joins the in-flight connect.
        25);
}

TEST_CASE("iOptron telescope - destruction mid-operation (slew/pulse threads live)", "[ioptron][telescope][stress]") {
    alpacacore::test::FakeMountServer server;
    REQUIRE(server.ok());

    for (int i = 0; i < 10; ++i) {
        auto driver = alpacacore::vendor::ioptron::create_ioptron_telescope(0, ioptron_endpoint(server.port()));
        static_cast<void>(alpacacore::test::settle_connected(*driver, true, std::chrono::seconds(5)));
        try {
            driver->slew_to_coordinates_async(5.0, 20.0);
        } catch (const std::exception&) {
        }
        try {
            driver->pulse_guide(0, 300);
        } catch (const std::exception&) {
        }
        if ((i % 2) != 0) {
            // Half the time a disconnect is also in flight at destruction —
            // the destructor must join every worker thread, every time.
            driver->disconnect();
        }
        driver.reset();
    }
}

// iEFW filter wheel and iEAF focuser stress (#271): these are separate
// serial devices from the mount above (each on its own Prolific USB-serial
// bridge), and neither has a fake seam. On a hardware-free host every
// connect fails fast at open()/handshake time, which still storms the
// AsyncConnectable machinery and the failure-path cleanup, same shape as the
// ZWO EFW/camera cases. With hardware attached the same tests exercise the
// full connect path.
//
// The filter wheel uses create_iefw_filterwheel_by_index: its :DeviceInfo#
// port scan runs inside the async connect task, so construction is trivial
// and the fail-fast happens on connect() as expected. The focuser's
// create_ieaf_focuser_by_index is NOT the same shape -- it scans for a port
// SYNCHRONOUSLY at construction and throws immediately if none is found
// (see ioptron_ieaf_focuser_driver.cpp), so it cannot be used here; this
// uses the explicit-port create_ieaf_focuser("/dev/ttyUSB0") instead, the
// same convention test_ioptron_ieaf_focuser.cpp's unit tests already use,
// which defers to connect() like every other case in this file.
TEST_CASE("iEFW filter wheel - concurrent connect/disconnect/operate stress", "[ioptron][filterwheel][stress]") {
    auto driver = alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(0, 0);

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& wheel = static_cast<alpacacore::FilterWheelDriver&>(d);
        static_cast<void>(wheel.get_position());
        wheel.set_position(1);
        static_cast<void>(wheel.get_names());
        static_cast<void>(wheel.get_focus_offsets());
    });

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("iEFW filter wheel - destruction races an in-flight connect", "[ioptron][filterwheel][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::ioptron::create_iefw_filterwheel_by_index(0, 0); });
}

TEST_CASE("iEAF focuser - concurrent connect/disconnect/operate stress", "[ioptron][focuser][stress]") {
    auto driver = alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0");

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
        static_cast<void>(focuser.get_position());
        static_cast<void>(focuser.get_temperature());
        focuser.move(100);
        focuser.halt();
    });

    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("iEAF focuser - destruction races an in-flight connect", "[ioptron][focuser][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::ioptron::create_ieaf_focuser(0, "/dev/ttyUSB0"); });
}

#endif  // !_WIN32
