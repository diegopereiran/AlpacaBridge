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

// Connect/disconnect/operate concurrency stress for the WeeWX
// ObservingConditions driver (issue #101). No fake seam exists for its curl
// HTTP fetch, so this points at http://127.0.0.1:1/ -- port 1 is privileged
// and never listening, so every connect fails fast at curl's connect() with
// no network round trip, and it can never reach a real WeeWX instance the
// way a plausible-looking host or port could. That still storms the
// AsyncConnectable machinery and the connect-failure unwind. It does NOT
// reach the poll thread: set_connected(true) throws out of fetch_snapshot()
// before start_polling() is called, so poll_running_ stays false for the
// whole run. The connected surface needs a fake HTTP seam this driver does
// not have.

#include <alpacacore/observingconditions_driver.h>
#include <alpacacore/vendor/weewx/weewx_observingconditions_driver.h>

#include <chrono>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;

namespace {

// Port 1 never listens; the short timeout keeps each failed connect cheap so
// the storm gets many full lifecycle cycles inside its window.
alpacacore::vendor::weewx::WeeWxHttpConfig unreachable_config() {
    alpacacore::vendor::weewx::WeeWxHttpConfig config;
    config.url = "http://127.0.0.1:1/";
    config.timeout = std::chrono::milliseconds(100);
    return config;
}

}  // namespace

TEST_CASE("WeeWX observing conditions - concurrent connect/disconnect/operate stress",
          "[weewx][observingconditions][stress]") {
    auto driver = alpacacore::vendor::weewx::create_weewx_observingconditions(0, unreachable_config());

    alpacacore::test::run_lifecycle_stress(*driver, [](AlpacaDriver& d) {
        auto& oc = static_cast<alpacacore::ObservingConditionsDriver&>(d);
        // The sensor getters throw NotConnected on this unreachable URL
        // (AveragePeriod answers without the device, DeviceState swallows
        // internally), and the harness only swallows the exception from the
        // callback as a whole -- so without per-call handling the first
        // throw would skip every call below it and they would never be
        // exercised at all. std::exception rather than AlpacaException:
        // anything else escaping curl teardown would unwind just the same.
        auto call = [](auto&& fn) {
            try {
                fn();
            } catch (const std::exception&) {
            }
        };
        call([&] { static_cast<void>(oc.get_temperature()); });
        call([&] { static_cast<void>(oc.get_humidity()); });
        call([&] { static_cast<void>(oc.get_dew_point()); });
        call([&] { static_cast<void>(oc.get_pressure()); });
        call([&] { static_cast<void>(oc.get_wind_speed()); });
        call([&] { static_cast<void>(oc.get_sky_quality()); });
        call([&] { static_cast<void>(oc.get_sky_temperature()); });
        call([&] { static_cast<void>(oc.get_average_period()); });
        call([&] { oc.set_average_period(0.0); });
        call([&] { static_cast<void>(oc.get_time_since_last_update("temperature")); });
        call([&] { static_cast<void>(oc.get_sensor_description("temperature")); });
        call([&] { static_cast<void>(oc.get_device_state()); });
        call([&] { oc.refresh(); });
    });

    // Connected can only be false here: the sentinel URL can never resolve to
    // a listening server, so no connect in the storm ever succeeds.
    CHECK(driver->get_connected() == false);
    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
}

TEST_CASE("WeeWX observing conditions - destruction races an in-flight connect",
          "[weewx][observingconditions][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::weewx::create_weewx_observingconditions(0, unreachable_config()); });
}
