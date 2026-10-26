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

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace alpacacore::util {

// What a GPIO power-port switch does to its ports when the server starts
// (decision 0021, issue #300). The device config's `powerOnState` field.
enum class PowerOnState : std::uint8_t {
    Leave,    // "leave": never touch a port at start; it keeps what the boot gave it
    Restore,  // "restore": put back the value a client last set
    Off,      // "off": every writable port off on the first start after a boot
};

// The config spellings, in the order the catalog lists them.
inline constexpr const char* kPowerOnStateNames[] = {"leave", "restore", "off"};

// "leave", "restore" or "off"; anything else is nullopt.
std::optional<PowerOnState> parse_power_on_state(std::string_view name);

// One port as the start plan sees it.
struct PowerOnPort {
    bool writable = true;  // false: a read-only port, never written
    bool pwm = false;      // true: values 0..100; false: 0 or 1
};

// The value each port is set to at a server start, nullopt = leave it as it is.
// Leave plans nothing. Off on a new boot plans 0 for every writable port.
// Restore, and Off on a start within the same boot (a service restart), plan
// the saved percent of each writable port that has one: a PWM port takes it
// rounded and clamped to 0..100, a boolean port 1 when it is above 0.
std::vector<std::optional<int>> plan_power_on(PowerOnState state, bool new_boot, const std::vector<PowerOnPort>& ports,
                                              const std::map<int, double>& saved_percent);

// The cached value a port keeps when its lines are claimed, given the level
// just read (issue #300). A cached value whose steady level matches the line
// is kept, so a reconnect resumes a PWM duty that close() left at its steady
// level; otherwise the line wins: high reads as full on (@p max_value), low as
// off. The constructor's "on" default therefore reads back the true level on
// the first claim.
inline int reconcile_cached_value(int cached, int level, int max_value) {
    const int steady = cached > 0 ? 1 : 0;
    if (steady == level) {
        return cached;
    }
    return level != 0 ? max_value : 0;
}

// Persists the last value a client set on each GPIO power port, so a server
// start can put it back (decision 0021). One flat JSON object in a state file,
// key "<UniqueID>/<port index>", value the port's level in percent (a boolean
// port saves 0 or 100), so a port switched between boolean and PWM in the
// config keeps a meaning. Every call is mutex-guarded and never throws.
class PowerPortStateStore {
public:
    // Default files, relative to the working directory.
    static std::string default_path();          // config/power_port_states.json
    static std::string default_boot_id_path();  // config/power_port_boot_id

    // Test seams: replace the process-wide paths, or the boot ID the kernel
    // reports. An empty string restores the default. Each returns the
    // previous override (empty if none).
    static std::string set_default_path(const std::string& path);
    static std::string set_boot_id_path(const std::string& path);
    static std::string set_current_boot_id_for_testing(const std::string& boot_id);

    // Saved percent of each port of @p unique_id; empty when none or unreadable.
    static std::map<int, double> load(const std::string& unique_id);

    // Writes @p percent for port @p port of @p unique_id, keeping other keys.
    // Returns false (and logs a WARNING) when it could not be stored.
    static bool save(const std::string& unique_id, int port, double percent);

    // Removes the entries of every GUID-shaped UniqueID (util::is_unique_id)
    // that is not in @p live_unique_ids: the ports of removed devices. Returns
    // how many entries went.
    static std::size_t prune_unknown_unique_ids(const std::set<std::string>& live_unique_ids);

    // Called once per server start, before any power-on plan runs. Returns true
    // when this is the first start since the machine booted: the kernel boot ID
    // (/proc/sys/kernel/random/boot_id) differs from the one the last start
    // recorded, or none was recorded. Records the current one. An unreadable
    // boot ID counts as the same boot, so a start that cannot tell never
    // applies Off over a running session.
    static bool begin_server_start();
};

}  // namespace alpacacore::util
