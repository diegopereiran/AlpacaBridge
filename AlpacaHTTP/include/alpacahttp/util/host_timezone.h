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

#pragma once

#include <string>

namespace alpacahttp {
namespace util {

// open-astro#354: the IANA name of the host's configured time zone
// ("America/Denver"), or "" when it cannot be determined.
//
// The web UI header clock renders in this zone so it agrees with the
// server's own log lines, which are written through localtime_r(). The
// three sources are consulted in the order the C library itself uses:
//
//   1. the TZ environment variable (a leading ':' is stripped, as tzset()
//      does; a POSIX rule string such as "EST5EDT" is NOT an IANA name and
//      is rejected, since a browser's Intl cannot resolve it),
//   2. /etc/timezone (Debian and derivatives; the file the images ship),
//   3. the /etc/localtime symlink target, taken after its "zoneinfo/"
//      component (systemd's timedatectl maintains only this one).
//
// Only a value that looks like an IANA name is returned: Area/City segments
// of letters, digits, '_', '-' and '+', at least one '/', and not "localtime"
// or "posixrules". Anything else is "" so the UI takes its browser-zone
// fallback rather than handing Intl a string it will throw on.
//
// Cheap enough to call per description GET (a getenv and at most one small
// read or readlink), and deliberately NOT cached: timedatectl set-timezone
// changes the answer at runtime and the header should follow it on the next
// refresh, as the log lines do.
std::string host_time_zone();

// Test seam: the same resolution against an explicit TZ value (nullptr for
// unset) and an alternative /etc directory.
std::string host_time_zone(const char* tz_env, const std::string& etc_dir);

// True when `name` has the IANA Area/City shape described above.
bool looks_like_iana_zone(const std::string& name);

}  // namespace util
}  // namespace alpacahttp
