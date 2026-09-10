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

// Connect/disconnect/operate concurrency stress for the Astroasis Oasis
// Focuser (issue #101). No fake seam exists for the hidapi-backed protocol
// wrapper, so on a hardware-free host every connect fails fast at USB HID
// enumeration -- which still storms the AsyncConnectable machinery and the
// failure-path cleanup. With a focuser attached the same test exercises the
// full connect path.

#include <alpacacore/focuser_driver.h>
#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;

TEST_CASE("Astroasis focuser - concurrent connect/disconnect/operate stress", "[astroasis][focuser][stress]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0");

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& focuser = static_cast<alpacacore::FocuserDriver&>(d);
        static_cast<void>(focuser.get_is_moving());
        static_cast<void>(focuser.get_position());
        static_cast<void>(focuser.get_max_step());
        static_cast<void>(focuser.get_max_increment());
        static_cast<void>(focuser.get_temperature());
        focuser.move(1234);
        focuser.halt();
    });

    // Still alive and coherent after the storm (Connected reflects whether a
    // physical focuser is attached; both outcomes are valid here).
    static_cast<void>(driver->get_connected());
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("Astroasis focuser - destruction races an in-flight connect", "[astroasis][focuser][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw0"); });
}
