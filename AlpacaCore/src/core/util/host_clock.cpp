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
#include <mutex>
#include <string>

namespace alpacacore::util {

// Preferred: the epoch CMake captured at configure time (it honours
// SOURCE_DATE_EPOCH, so a reproducible .deb build stays reproducible and
// -Wdate-time never sees a __DATE__). Fallback for builds that bypass the
// CMake definition: __DATE__ ("Mmm dd yyyy"). Both are floored to 00:00 UTC of
// that day so the two branches behave identically and an RTC a few seconds
// behind a just-configured build is not rejected. One translation unit, so
// every user sees the same value.
std::chrono::system_clock::time_point HostClock::build_time() {
    static const std::chrono::system_clock::time_point tp = [] {
#ifdef ALPACACORE_BUILD_EPOCH
        const long long epoch = static_cast<long long>(ALPACACORE_BUILD_EPOCH);
        return std::chrono::system_clock::time_point(std::chrono::seconds(epoch - (epoch % 86400)));
#else
        static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const char* d = __DATE__;
        const char* m = std::strstr(months, std::string(d, 3).c_str());
        std::tm tm{};
        tm.tm_mon = m ? static_cast<int>((m - months) / 3) : 0;
        tm.tm_mday = std::atoi(d + 4) > 0 ? std::atoi(d + 4) : 1;
        tm.tm_year = std::atoi(d + 7) - 1900;
        return std::chrono::system_clock::from_time_t(timegm(&tm));
#endif
    }();
    return tp;
}

// Which RTC (if any) the kernel loaded system time from cannot change after
// boot, so the device is found once. Its reading is memoised for one second:
// on an I2C RTC every read is a bus transaction, and this runs inside HTTP
// handlers and the device-connect path.
std::optional<std::chrono::system_clock::time_point> HostClock::host_rtc_time() {
    static std::mutex cache_mutex;
    static std::optional<std::chrono::system_clock::time_point> cached;
    static std::chrono::steady_clock::time_point cached_at{};
    std::lock_guard<std::mutex> lock(cache_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (cached_at != std::chrono::steady_clock::time_point{} && now - cached_at < std::chrono::seconds(1)) {
        return cached;
    }
    cached = read_host_rtc_time();
    cached_at = now;
    return cached;
}

std::optional<std::chrono::system_clock::time_point> HostClock::read_host_rtc_time() {
    // A found device is immutable for the process lifetime and is cached; a
    // miss is not (an early probe before the RTC registered must not stick).
    static std::string device;
    if (device.empty()) {
        for (int i = 0; i < 8; ++i) {
            const std::string dev = "/sys/class/rtc/rtc" + std::to_string(i);
            std::ifstream f(dev + "/hctosys");
            int v = 0;
            if (f.is_open() && (f >> v) && v == 1) {
                device = dev;
                break;
            }
        }
    }
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
