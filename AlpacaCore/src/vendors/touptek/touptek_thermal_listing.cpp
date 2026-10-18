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

// Registers the camera lister behind GET /management/v1/thermalcameras for the
// ToupTek thermal switch. Compiled into alpacacore_touptek and called from
// register_builtin_factories(), so the router reaches the vendor SDK through
// the catalog and includes no vendor header for it.

#include <alpacacore/vendor/touptek/touptek_thermal_switch_driver.h>

#include "../../catalog/builtin_descriptors.h"

namespace alpacacore::catalog {

void register_touptek_thermal_camera_lister(DeviceCatalog& catalog) {
    catalog.add_thermal_camera_lister("touptek", [] { return vendor::touptek::list_touptek_thermal_cameras(); });
}

}  // namespace alpacacore::catalog
