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

}  // namespace alpacacore::util
