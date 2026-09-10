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

#include <alpacacore/util/host_clock.h>
#include <dirent.h>

#include <fstream>
#include <string>

namespace alpacacore::util {

namespace {

// The device the kernel set system time from at boot is the one whose hctosys
// attribute reads 1 (the kernel gates that on the read having succeeded).
// Nothing else counts: a present-but-unread RTC, a kernel without
// CONFIG_RTC_HCTOSYS, or a userspace `hwclock --hctosys` all read as no RTC.
bool probe_boot_rtc() {
    DIR* dir = ::opendir("/sys/class/rtc");
    if (dir == nullptr) {
        return false;
    }
    std::string device;
    while (const dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.rfind("rtc", 0) != 0) {
            continue;
        }
        std::ifstream hctosys("/sys/class/rtc/" + name + "/hctosys");
        int used = 0;
        if (hctosys.is_open() && (hctosys >> used) && used == 1) {
            device = "/sys/class/rtc/" + name;
            break;
        }
    }
    ::closedir(dir);
    if (device.empty()) {
        return false;
    }
    std::ifstream since_epoch(device + "/since_epoch");
    long long epoch = 0;
    if (!since_epoch.is_open() || !(since_epoch >> epoch)) {
        return false;
    }
    return epoch > HostClock::kMinPlausibleEpoch;
}

}  // namespace

bool HostClock::host_booted_from_rtc() {
    static std::mutex mutex;
    static bool found = false;
    static std::chrono::steady_clock::time_point last_miss{};
    std::lock_guard<std::mutex> lock(mutex);
    if (found) {
        return true;  // an RTC the kernel booted from does not go away
    }
    // A miss is not sticky (the RTC may register after the first request), but
    // re-scanning sysfs on every /management/v1/description poll is wasteful.
    const auto now = std::chrono::steady_clock::now();
    if (last_miss != std::chrono::steady_clock::time_point{} && now - last_miss < std::chrono::seconds(30)) {
        return false;
    }
    found = probe_boot_rtc();
    if (!found) {
        last_miss = now;
    }
    return found;
}

}  // namespace alpacacore::util
