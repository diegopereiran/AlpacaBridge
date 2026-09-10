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

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

namespace alpacacore::util {

namespace {

// The device the kernel set system time from at boot is the one whose hctosys
// attribute reads 1 (the kernel gates that on the read having succeeded).
// Nothing else counts: a present-but-unread RTC, a kernel without
// CONFIG_RTC_HCTOSYS, or a userspace `hwclock --hctosys` all read as no RTC.
// Distinguishes "no device the kernel booted from" (which may still appear:
// the RTC can register after our first probe) from "found one, but its time is
// not usable" -- a dead RTC free-running from its own wrong value, or one whose
// since_epoch will not read. Neither of the latter changes while we run, so
// only the first is worth re-probing.
enum class Probe : std::uint8_t { NoDevice, Implausible, Ok };

Probe probe_boot_rtc() {
    const std::unique_ptr<DIR, int (*)(DIR*)> dir(::opendir("/sys/class/rtc"), &::closedir);
    if (dir == nullptr) {
        return Probe::NoDevice;
    }
    std::string device;
    while (const dirent* entry = ::readdir(dir.get())) {
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
    if (device.empty()) {
        return Probe::NoDevice;
    }
    std::ifstream since_epoch(device + "/since_epoch");
    long long epoch = 0;
    if (!since_epoch.is_open() || !(since_epoch >> epoch)) {
        // The hctosys device is here but will not tell us its time; that is as
        // unchangeable as an implausible reading, so do not re-probe it (each
        // attempt is a bus transaction on an I2C RTC).
        return Probe::Implausible;
    }
    return epoch > HostClock::kMinPlausibleEpoch ? Probe::Ok : Probe::Implausible;
}

}  // namespace

bool HostClock::host_booted_from_rtc() {
    static std::mutex mutex;
    static bool settled = false;  // Ok or Implausible: neither can change after boot
    static bool result = false;
    static std::chrono::steady_clock::time_point last_probe{};
    std::lock_guard<std::mutex> lock(mutex);
    if (settled) {
        return result;
    }
    // Only "no device" is re-probed, and not on every /management/v1/description poll.
    const auto now = std::chrono::steady_clock::now();
    if (last_probe != std::chrono::steady_clock::time_point{} && now - last_probe < std::chrono::seconds(30)) {
        return false;
    }
    last_probe = now;
    const Probe probe = probe_boot_rtc();
    if (probe != Probe::NoDevice) {
        settled = true;
        result = probe == Probe::Ok;
    }
    return result;
}

}  // namespace alpacacore::util
