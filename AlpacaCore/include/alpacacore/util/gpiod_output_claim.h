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

// Include only from a translation unit that links libgpiod (>= 2.0): the GPIO
// power-port wrappers (ZWO ASIAIR Pro / Plus CM4, ToupTek StellaVita, iOptron
// iMate). AlpacaCore itself does not link it.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/serial_io.h>
#include <gpiod.h>

#include <cerrno>
#include <string>
#include <vector>

namespace alpacacore::util {

// Lines claimed by claim_outputs_at_current_level(). The caller owns both
// handles and releases the request before closing the chip.
struct GpiodOutputClaim {
    gpiod_chip* chip = nullptr;
    gpiod_line_request* request = nullptr;
    std::vector<int> levels;  // 0 or 1 per offset, in the order given
};

// Claims @p offsets on @p chip_path as outputs, each held at the level it has
// now (issue #300). The lines are first requested as-is, so the request itself
// changes nothing; their levels are read; then the request is reconfigured to
// outputs driven to exactly those levels. A line the boot left high stays high
// and one left low stays low: claiming never powers a port on or off. Throws
// AlpacaException (NotConnected when the chip or lines cannot be had,
// DriverException otherwise); nothing stays claimed then.
inline GpiodOutputClaim claim_outputs_at_current_level(const std::string& chip_path,
                                                       const std::vector<unsigned int>& offsets, const char* consumer) {
    GpiodOutputClaim claim;
    claim.chip = ::gpiod_chip_open(chip_path.c_str());
    if (claim.chip == nullptr) {
        const int err = errno;
        throw ::alpacacore::AlpacaException("Failed to open GPIO chip '" + chip_path + "': " + errno_string(err),
                                            ::alpacacore::AlpacaError::NotConnected);
    }

    gpiod_line_settings* settings = nullptr;
    gpiod_line_config* line_cfg = nullptr;
    gpiod_request_config* req_cfg = nullptr;
    const auto fail = [](const char* call, int code) {
        const int err = errno;
        throw ::alpacacore::AlpacaException(std::string(call) + " failed: " + errno_string(err), code);
    };
    // Builds a line config holding @p direction for every offset.
    const auto build_line_config = [&](gpiod_line_direction direction) {
        if (::gpiod_line_settings_set_direction(settings, direction) != 0) {
            fail("gpiod_line_settings_set_direction", ::alpacacore::AlpacaError::DriverException);
        }
        if (line_cfg != nullptr) {
            ::gpiod_line_config_free(line_cfg);
        }
        line_cfg = ::gpiod_line_config_new();
        if (line_cfg == nullptr) {
            throw ::alpacacore::AlpacaException("gpiod_line_config_new failed",
                                                ::alpacacore::AlpacaError::DriverException);
        }
        if (::gpiod_line_config_add_line_settings(line_cfg, offsets.data(), offsets.size(), settings) != 0) {
            fail("gpiod_line_config_add_line_settings", ::alpacacore::AlpacaError::DriverException);
        }
    };

    try {
        settings = ::gpiod_line_settings_new();
        if (settings == nullptr) {
            throw ::alpacacore::AlpacaException("gpiod_line_settings_new failed",
                                                ::alpacacore::AlpacaError::DriverException);
        }
        build_line_config(GPIOD_LINE_DIRECTION_AS_IS);

        req_cfg = ::gpiod_request_config_new();
        if (req_cfg == nullptr) {
            throw ::alpacacore::AlpacaException("gpiod_request_config_new failed",
                                                ::alpacacore::AlpacaError::DriverException);
        }
        ::gpiod_request_config_set_consumer(req_cfg, consumer);

        claim.request = ::gpiod_chip_request_lines(claim.chip, req_cfg, line_cfg);
        if (claim.request == nullptr) {
            fail("gpiod_chip_request_lines", ::alpacacore::AlpacaError::NotConnected);
        }

        std::vector<gpiod_line_value> values(offsets.size(), GPIOD_LINE_VALUE_INACTIVE);
        if (::gpiod_line_request_get_values(claim.request, values.data()) != 0) {
            fail("gpiod_line_request_get_values", ::alpacacore::AlpacaError::DriverException);
        }
        claim.levels.reserve(values.size());
        for (const gpiod_line_value v : values) {
            if (v == GPIOD_LINE_VALUE_ERROR) {
                throw ::alpacacore::AlpacaException("gpiod_line_request_get_values returned an error level",
                                                    ::alpacacore::AlpacaError::DriverException);
            }
            claim.levels.push_back(v == GPIOD_LINE_VALUE_ACTIVE ? 1 : 0);
        }

        build_line_config(GPIOD_LINE_DIRECTION_OUTPUT);
        if (::gpiod_line_config_set_output_values(line_cfg, values.data(), values.size()) != 0) {
            fail("gpiod_line_config_set_output_values", ::alpacacore::AlpacaError::DriverException);
        }
        if (::gpiod_line_request_reconfigure_lines(claim.request, line_cfg) != 0) {
            fail("gpiod_line_request_reconfigure_lines", ::alpacacore::AlpacaError::DriverException);
        }
    } catch (...) {
        if (req_cfg) ::gpiod_request_config_free(req_cfg);
        if (line_cfg) ::gpiod_line_config_free(line_cfg);
        if (settings) ::gpiod_line_settings_free(settings);
        if (claim.request) ::gpiod_line_request_release(claim.request);
        ::gpiod_chip_close(claim.chip);
        throw;
    }

    ::gpiod_request_config_free(req_cfg);
    ::gpiod_line_config_free(line_cfg);
    ::gpiod_line_settings_free(settings);
    return claim;
}

}  // namespace alpacacore::util
