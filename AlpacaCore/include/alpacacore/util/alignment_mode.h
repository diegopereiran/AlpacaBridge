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

#include <string_view>

namespace alpacacore::util {

// The three values the `alignmentMode` device-config key accepts (issue #860):
// the geometry of a mount whose handset does not report it. The one home of
// the value set, read by the router, the Celestron schema and the Celestron
// factory; anything else is dropped and reads as "auto".
inline constexpr std::string_view kAlignmentModeAuto = "auto";
inline constexpr std::string_view kAlignmentModeAltAz = "altaz";
inline constexpr std::string_view kAlignmentModeEquatorial = "equatorial";

constexpr bool is_known_alignment_mode(std::string_view mode) {
    return mode == kAlignmentModeAuto || mode == kAlignmentModeAltAz || mode == kAlignmentModeEquatorial;
}

}  // namespace alpacacore::util
