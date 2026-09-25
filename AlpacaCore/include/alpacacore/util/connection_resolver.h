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

#include <alpacacore/util/logging.h>

#include <exception>
#include <functional>
#include <string>
#include <utility>

namespace alpacacore::util {

/**
 * Connect-time endpoint resolution for auto-detected devices.
 *
 * An auto-detect factory (`create_<vendor>_<device>_auto`, `_auto_network`,
 * `_by_index`) used to run its serial or network scan at construction, which
 * for a persisted device is server start-up. A device that is not there yet
 * at boot (a mount still joining the access point, a USB adapter powered
 * after the SBC) then threw out of the factory, the router logged "Failed to
 * load persisted device" and the entry sat as "(failed to load)" until the
 * service was restarted; no later connect could revive it.
 *
 * The factory now hands the driver a `ConnectionResolver<Info>` instead: a
 * callable that runs the scan and returns the endpoint, or throws the
 * operator-facing refusal ("No iOptron mount found on the local network ...").
 * The driver calls `connect_resolved()` from its connect path, so construction
 * never touches the bus and the scan runs when a client asks for the device.
 *
 * Policy (`connect_resolved`):
 * - No resolver: `try_connect(info)` once; the endpoint is fixed.
 * - Resolver, nothing resolved yet: resolve, then `try_connect` the result.
 * - Resolver, a previous connect resolved an endpoint: `try_connect` that
 *   endpoint first and re-resolve only if it fails. ConformU and NINA
 *   connect and disconnect many times per session; paying the full scan
 *   (several seconds for a serial probe ladder or a subnet sweep) on every
 *   connect would push a Platform 7 `Connect()` past its 5 s client budget,
 *   while a stale endpoint (re-enumerated USB port, new DHCP lease) still
 *   falls through to a fresh scan.
 *
 * `try_connect(const Info&)` must THROW on failure (the drivers' existing
 * `if (!protocol.connect(info)) throw AlpacaException(...)` shape, moved into
 * the lambda). An exception from the retry of a previously resolved endpoint
 * is swallowed and triggers the re-resolve; an exception from the resolver
 * or from the connect that follows it propagates unchanged, so the client
 * sees the scan's own message as the connect refusal (#358).
 *
 * The caller holds whatever lock its connect path already holds; this helper
 * takes none. `info` and `resolved` are the driver's own members so the
 * endpoint that answered survives across connects and is what the driver's
 * log lines and getters report.
 */
template <typename Info>
using ConnectionResolver = std::function<Info()>;

template <typename Info, typename TryConnect>
void connect_resolved(Info& info, bool& resolved, const ConnectionResolver<Info>& resolver, TryConnect&& try_connect) {
    if (!resolver) {
        try_connect(static_cast<const Info&>(info));
        return;
    }
    if (resolved) {
        try {
            try_connect(static_cast<const Info&>(info));
            return;
        } catch (const std::exception& e) {
            // The endpoint a previous connect found no longer answers: scan again.
            ALPACA_LOG_INFO("AutoDetect", "Previously resolved endpoint no longer answers (" + std::string(e.what()) +
                                              "); scanning again");
        }
    }
    Info fresh = resolver();
    info = std::move(fresh);
    resolved = true;
    try_connect(static_cast<const Info&>(info));
}

}  // namespace alpacacore::util
