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

#include <alpacacore/util/power_port_state_store.h>
#include <alpacacore/util/unique_id.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "catch2_compat.h"
#include "power_port_test_dir.h"

using alpacacore::util::plan_power_on;
using alpacacore::util::PowerOnPort;
using alpacacore::util::PowerOnState;
using alpacacore::util::PowerPortStateStore;

namespace {

// Every power-port switch saves what a client sets (decision 0021). This keeps
// the switch tests in this binary that do not make their own TempPowerPortDir
// from writing config/power_port_states.json into the working directory.
const alpacacore::test::TempPowerPortDir binary_sandbox;

const std::vector<PowerOnPort> kMixedPorts{
    {true, false},   // 0: boolean
    {true, true},    // 1: PWM
    {false, false},  // 2: read-only
    {true, true},    // 3: PWM, nothing saved
};

}  // namespace

TEST_CASE("Power port state - parse_power_on_state", "[power-port][unit]") {
    CHECK(alpacacore::util::parse_power_on_state("leave") == PowerOnState::Leave);
    CHECK(alpacacore::util::parse_power_on_state("restore") == PowerOnState::Restore);
    CHECK(alpacacore::util::parse_power_on_state("off") == PowerOnState::Off);
    CHECK_FALSE(alpacacore::util::parse_power_on_state("Off").has_value());
    CHECK_FALSE(alpacacore::util::parse_power_on_state("").has_value());
}

TEST_CASE("Power port state - leave plans nothing on any start", "[power-port][unit]") {
    const std::map<int, double> saved{{0, 100.0}, {1, 40.0}};
    for (const bool new_boot : {true, false}) {
        const auto plan = plan_power_on(PowerOnState::Leave, new_boot, kMixedPorts, saved);
        REQUIRE(plan.size() == kMixedPorts.size());
        for (const auto& value : plan) {
            CHECK_FALSE(value.has_value());
        }
    }
}

TEST_CASE("Power port state - off on a new boot turns every writable port off", "[power-port][unit]") {
    const auto plan = plan_power_on(PowerOnState::Off, true, kMixedPorts, {{0, 100.0}, {1, 40.0}});
    REQUIRE(plan.size() == 4);
    CHECK(plan[0] == std::optional<int>(0));
    CHECK(plan[1] == std::optional<int>(0));
    CHECK_FALSE(plan[2].has_value());  // read-only: never written
    CHECK(plan[3] == std::optional<int>(0));
}

TEST_CASE("Power port state - off within the same boot restores what was saved", "[power-port][unit]") {
    // A service restart (an update) must not cut power to gear in use.
    const auto plan = plan_power_on(PowerOnState::Off, false, kMixedPorts, {{0, 100.0}, {1, 40.0}});
    CHECK(plan[0] == std::optional<int>(1));
    CHECK(plan[1] == std::optional<int>(40));
    CHECK_FALSE(plan[2].has_value());
    CHECK_FALSE(plan[3].has_value());  // nothing saved: left as it is
}

TEST_CASE("Power port state - restore maps the saved percent onto each port's mode", "[power-port][unit]") {
    const std::vector<PowerOnPort> ports{{true, false}, {true, false}, {true, true}, {true, true}, {true, true}};
    const std::map<int, double> saved{{0, 40.0}, {1, 0.0}, {2, 100.0}, {3, 150.0}, {4, 39.6}};
    for (const bool new_boot : {true, false}) {
        const auto plan = plan_power_on(PowerOnState::Restore, new_boot, ports, saved);
        CHECK(plan[0] == std::optional<int>(1));  // a PWM duty on a now-boolean port is on
        CHECK(plan[1] == std::optional<int>(0));
        CHECK(plan[2] == std::optional<int>(100));  // a boolean "on" on a now-PWM port is full duty
        CHECK(plan[3] == std::optional<int>(100));  // clamped
        CHECK(plan[4] == std::optional<int>(40));   // rounded
    }
}

TEST_CASE("Power port state - reconcile_cached_value keeps a matching cache and follows the line otherwise",
          "[power-port][unit]") {
    using alpacacore::util::reconcile_cached_value;
    CHECK(reconcile_cached_value(1, 1, 1) == 1);
    CHECK(reconcile_cached_value(1, 0, 1) == 0);  // the boot left the port off: report off
    CHECK(reconcile_cached_value(0, 1, 1) == 1);
    CHECK(reconcile_cached_value(40, 1, 100) == 40);  // close() left a 40 % port high: resume 40 %
    CHECK(reconcile_cached_value(40, 0, 100) == 0);
    CHECK(reconcile_cached_value(0, 1, 100) == 100);
    CHECK(reconcile_cached_value(100, 0, 100) == 0);
}

TEST_CASE("Power port state store - saves per device and port", "[power-port][unit]") {
    alpacacore::test::TempPowerPortDir dir;
    CHECK(PowerPortStateStore::load("A").empty());
    REQUIRE(PowerPortStateStore::save("A", 0, 100.0));
    REQUIRE(PowerPortStateStore::save("A", 3, 40.0));
    REQUIRE(PowerPortStateStore::save("B", 0, 0.0));
    REQUIRE(PowerPortStateStore::save("A", 0, 0.0));
    const auto a = PowerPortStateStore::load("A");
    CHECK(a == std::map<int, double>{{0, 0.0}, {3, 40.0}});
    CHECK(PowerPortStateStore::load("B") == std::map<int, double>{{0, 0.0}});
    CHECK(PowerPortStateStore::load("C").empty());
}

TEST_CASE("Power port state store - corrupt file loads empty and failed save returns false", "[power-port][unit]") {
    alpacacore::test::TempPowerPortDir dir;
    std::filesystem::create_directories(dir.file().parent_path());
    { std::ofstream(dir.file(), std::ios::trunc) << "{\"A/0\": "; }
    CHECK(PowerPortStateStore::load("A").empty());
    REQUIRE(PowerPortStateStore::save("A", 1, 100.0));
    CHECK(PowerPortStateStore::load("A") == std::map<int, double>{{1, 100.0}});

    std::filesystem::remove_all(dir.file().parent_path());
    // A regular file where the state directory should be.
    { std::ofstream(dir.file().parent_path()) << "x"; }
    bool ok = true;
    CHECK_NOTHROW(ok = PowerPortStateStore::save("A", 1, 0.0));
    CHECK_FALSE(ok);
    CHECK_FALSE(PowerPortStateStore::save("A", -1, 0.0));
}

TEST_CASE("Power port state store - prune drops removed GUID devices and keeps the rest", "[power-port][unit]") {
    alpacacore::test::TempPowerPortDir dir;
    const std::string live = alpacacore::util::generate_unique_id();
    const std::string removed = alpacacore::util::generate_unique_id();
    REQUIRE(PowerPortStateStore::save(live, 0, 100.0));
    REQUIRE(PowerPortStateStore::save(removed, 0, 100.0));
    REQUIRE(PowerPortStateStore::save(removed, 1, 0.0));
    REQUIRE(PowerPortStateStore::save("ZWO_ASIAIR_0", 0, 100.0));
    CHECK(PowerPortStateStore::prune_unknown_unique_ids({live}) == 2);
    CHECK(PowerPortStateStore::load(live).size() == 1);
    CHECK(PowerPortStateStore::load(removed).empty());
    CHECK(PowerPortStateStore::load("ZWO_ASIAIR_0").size() == 1);
    CHECK(PowerPortStateStore::prune_unknown_unique_ids({live}) == 0);
}

TEST_CASE("Power port state store - begin_server_start tells a reboot from a service restart", "[power-port][unit]") {
    alpacacore::test::TempPowerPortDir dir;
    CHECK(PowerPortStateStore::begin_server_start());        // nothing recorded yet
    CHECK_FALSE(PowerPortStateStore::begin_server_start());  // restart, same boot
    CHECK_FALSE(PowerPortStateStore::begin_server_start());
    dir.set_boot_id("boot-2");
    CHECK(PowerPortStateStore::begin_server_start());  // rebooted
    CHECK_FALSE(PowerPortStateStore::begin_server_start());
}
