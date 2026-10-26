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

#include <map>
#include <string>

namespace alpacacore::util {

// A state file holding one flat JSON object of finite numbers, {"key": number, ...}.
// AlpacaCore carries no JSON library (decision 0013), so this reads and writes only
// that shape. Shared by the rotator Sync offset store (decision 0008) and the power
// port state store (decision 0021). Not thread-safe: each store serialises its own calls.

// Reads @p path. A missing file is an empty map; an unreadable or malformed one is
// an empty map and a WARNING under @p log_category naming @p what (e.g. "sync offsets").
std::map<std::string, double> read_flat_number_file(const std::string& path, const char* log_category,
                                                    const char* what);

// Writes @p values to @p path (parent directories created, temp file, then rename).
// Throws std::exception on failure; the previous file is then left as it was.
void write_flat_number_file(const std::string& path, const std::map<std::string, double>& values);

}  // namespace alpacacore::util
