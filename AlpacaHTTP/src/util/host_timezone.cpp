// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#include <alpacahttp/util/host_timezone.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace alpacahttp {
namespace util {

namespace {

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// The part of a zoneinfo path after its last "zoneinfo/" component, or ""
// when there is none: "/usr/share/zoneinfo/America/Denver" -> "America/Denver",
// "../usr/share/zoneinfo/Etc/UTC" -> "Etc/UTC" (Debian ships the relative
// form), "/etc/localtime" -> "".
std::string zone_from_zoneinfo_path(const std::string& path) {
    static const std::string kMarker = "zoneinfo/";
    const auto pos = path.rfind(kMarker);
    if (pos == std::string::npos) {
        return "";
    }
    return path.substr(pos + kMarker.size());
}

}  // namespace

bool looks_like_iana_zone(const std::string& name) {
    if (name.empty() || name.size() > 64 || name == "localtime" || name == "posixrules") {
        return false;
    }
    bool slash = false;
    bool segment_started = false;
    for (const char c : name) {
        if (c == '/') {
            if (!segment_started) {
                return false;  // leading '/' or "//"
            }
            slash = true;
            segment_started = false;
            continue;
        }
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '+';
        if (!ok) {
            return false;
        }
        segment_started = true;
    }
    // "Etc/UTC" and "America/Argentina/Buenos_Aires" pass; a bare "UTC" or a
    // POSIX rule like "EST5EDT" does not (no '/'), nor does a trailing '/'.
    return slash && segment_started;
}

std::string host_time_zone(const char* tz_env, const std::string& etc_dir) {
    if (tz_env != nullptr) {
        std::string tz = tz_env;
        if (!tz.empty() && tz.front() == ':') {
            tz.erase(0, 1);
        }
        // TZ may also carry an absolute zoneinfo path (":/usr/share/zoneinfo/X").
        if (!tz.empty() && tz.front() == '/') {
            tz = zone_from_zoneinfo_path(tz);
        }
        if (looks_like_iana_zone(tz)) {
            return tz;
        }
        // A set-but-unusable TZ still governs localtime_r(), so do not let a
        // stale /etc/timezone contradict it; report "unknown" instead.
        return "";
    }

    {
        std::ifstream in(etc_dir + "/timezone");
        std::string line;
        if (in && std::getline(in, line)) {
            line = trim(line);
            if (looks_like_iana_zone(line)) {
                return line;
            }
        }
    }

    std::error_code ec;
    const auto target = std::filesystem::read_symlink(etc_dir + "/localtime", ec);
    if (!ec) {
        const std::string zone = zone_from_zoneinfo_path(target.string());
        if (looks_like_iana_zone(zone)) {
            return zone;
        }
    }
    return "";
}

std::string host_time_zone() { return host_time_zone(std::getenv("TZ"), "/etc"); }

}  // namespace util
}  // namespace alpacahttp
