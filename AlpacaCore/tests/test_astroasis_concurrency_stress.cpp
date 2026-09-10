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
// wrapper, so this connects against the deliberately-nonexistent
// "/dev/hidraw-alpacabridge-absent" (not the unit tests' "/dev/hidraw0",
// which can exist on a dev box and would risk hid_open_path matching an
// unrelated HID device -- hid_open_path does not check VID:PID). Every
// connect fails fast at that open, which still storms the AsyncConnectable
// machinery and the failure-path cleanup; this test never exercises the
// real connect path, regardless of what hardware is attached.

#include <alpacacore/focuser_driver.h>
#include <alpacacore/vendor/astroasis/astroasis_focuser_driver.h>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;

TEST_CASE("Astroasis focuser - concurrent connect/disconnect/operate stress", "[astroasis][focuser][stress]") {
    auto driver = alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw-alpacabridge-absent");

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

    // Still alive and coherent after the storm. Unlike the SVBONY/ZWO
    // fail-fast cases, Connected can only be false here: the sentinel path
    // never resolves to a real HID node, so no connect in the storm ever
    // succeeds.
    CHECK(driver->get_connected() == false);
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("Astroasis focuser - destruction races an in-flight connect", "[astroasis][focuser][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::astroasis::create_astroasis_focuser(0, "/dev/hidraw-alpacabridge-absent"); });
}
