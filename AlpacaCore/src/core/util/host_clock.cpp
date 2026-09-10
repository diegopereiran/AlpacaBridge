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

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

namespace alpacacore::util {

// "Mmm dd yyyy" (the __DATE__ shape) -> 00:00 UTC of that day. Days-from-civil
// (Howard Hinnant's algorithm), so this needs no timegm() and no locale, and
// it is compiled and unit-tested unconditionally -- build_time()'s fallback
// branch must not be first exercised in the field.
std::chrono::system_clock::time_point HostClock::day_from_date_string(const char* date) {
    static const char* kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if (date == nullptr || std::strlen(date) < 11) {
        return std::chrono::system_clock::time_point{};
    }
    const char* m = std::strstr(kMonths, std::string(date, 3).c_str());
    const int month = m != nullptr ? static_cast<int>((m - kMonths) / 3) + 1 : 1;
    const int parsed_day = std::atoi(date + 4);
    const int day = (parsed_day >= 1 && parsed_day <= 31) ? parsed_day : 1;
    const int year = std::atoi(date + 7);
    if (year <= 0) {
        return std::chrono::system_clock::time_point{};
    }
    // days_from_civil: era-based, exact for the whole proleptic Gregorian range.
    const int y = year - (month <= 2 ? 1 : 0);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = static_cast<unsigned>((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = static_cast<long long>(era) * 146097 + static_cast<long long>(doe) - 719468;
    return std::chrono::system_clock::time_point(std::chrono::seconds(days * 86400));
}

// Preferred: the epoch CMake captured at configure time (it honours
// SOURCE_DATE_EPOCH, so a reproducible .deb build stays reproducible and
// -Wdate-time never sees a __DATE__). Fallback for builds that bypass the
// CMake definition: this file's __DATE__. Both are floored to 00:00 UTC of
// that day so the two branches behave identically and an RTC a few seconds
// behind a just-configured build is not rejected. One translation unit, so
// every user sees the same value.
std::chrono::system_clock::time_point HostClock::build_time() {
    static const std::chrono::system_clock::time_point tp = [] {
#ifdef ALPACACORE_BUILD_EPOCH
        const long long epoch = static_cast<long long>(ALPACACORE_BUILD_EPOCH);
        return std::chrono::system_clock::time_point(std::chrono::seconds(epoch - (epoch % 86400)));
#else
        return day_from_date_string(__DATE__);
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
    // A reading is refreshed every second; "no RTC here" is re-probed far less
    // often, since it means re-opening up to 8 nonexistent sysfs paths and the
    // answer almost never changes after boot.
    const auto ttl = cached.has_value() ? std::chrono::seconds(1) : std::chrono::seconds(30);
    if (cached_at != std::chrono::steady_clock::time_point{} && now - cached_at < ttl) {
        return cached;
    }
    cached = read_host_rtc_time();
    cached_at = now;
    return cached;
}

std::optional<std::chrono::system_clock::time_point> HostClock::read_host_rtc_time() {
    // A found device is immutable for the process lifetime and is cached; a
    // miss is not (an early probe before the RTC registered must not stick).
    // Enumerated rather than guessed, so an hctosys device that is not rtc0
    // is still found and a miss costs one opendir instead of N failed opens.
    static std::string device;
    if (device.empty()) {
        if (DIR* dir = ::opendir("/sys/class/rtc")) {
            while (const dirent* entry = ::readdir(dir)) {
                const std::string name = entry->d_name;
                if (name.rfind("rtc", 0) != 0) {
                    continue;
                }
                const std::string dev = "/sys/class/rtc/" + name;
                std::ifstream f(dev + "/hctosys");
                int v = 0;
                if (f.is_open() && (f >> v) && v == 1) {
                    device = dev;
                    break;
                }
            }
            ::closedir(dir);
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
