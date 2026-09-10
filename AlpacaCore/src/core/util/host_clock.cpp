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

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>

namespace alpacacore::util {

// __DATE__ is "Mmm dd yyyy". Captured here, in one translation unit, so every
// user of HostClock::build_time() sees the same value (a header-inline
// definition would expand per TU and per compile date: an ODR violation, and a
// stale floor under incremental builds).
std::chrono::system_clock::time_point HostClock::build_time() {
    static const std::chrono::system_clock::time_point tp = [] {
        static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const char* d = __DATE__;
        const char* m = std::strstr(months, std::string(d, 3).c_str());
        std::tm tm{};
        tm.tm_mon = m ? static_cast<int>((m - months) / 3) : 0;
        tm.tm_mday = 1;
        tm.tm_year = std::atoi(d + 7) - 1900;
        return std::chrono::system_clock::from_time_t(timegm(&tm));
    }();
    return tp;
}

// Which RTC (if any) the kernel loaded system time from cannot change after
// boot, so the device is found once; its time is read on every call.
std::optional<std::chrono::system_clock::time_point> HostClock::host_rtc_time() {
    static const std::string device = [] {
        for (int i = 0; i < 8; ++i) {
            const std::string dev = "/sys/class/rtc/rtc" + std::to_string(i);
            std::ifstream f(dev + "/hctosys");
            int v = 0;
            if (f.is_open() && (f >> v) && v == 1) {
                return dev;
            }
        }
        return std::string();
    }();
    if (device.empty()) {
        return std::nullopt;
    }
    std::ifstream f(device + "/since_epoch");
    long long epoch = 0;
    if (!f.is_open() || !(f >> epoch) || epoch <= 0) {
        return std::nullopt;
    }
    return std::chrono::system_clock::time_point(std::chrono::seconds(epoch));
}

}  // namespace alpacacore::util
