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

#include <alpacacore/util/power_port_state_store.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace alpacacore::test {

// Points the power port state store (states file, boot ID file and the kernel
// boot ID it reads) at a fresh temp directory for one test, and restores the
// previous paths and removes the directory on exit. The boot ID starts as
// "boot-1"; set_boot_id() simulates a reboot.
class TempPowerPortDir {
public:
    TempPowerPortDir() {
        static std::atomic<int> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("power_port_states_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(dir_);
        previous_path_ = util::PowerPortStateStore::set_default_path(file().string());
        previous_boot_path_ = util::PowerPortStateStore::set_boot_id_path((dir_ / "state" / "boot_id").string());
        previous_boot_id_ = util::PowerPortStateStore::set_current_boot_id_for_testing("boot-1");
    }
    ~TempPowerPortDir() {
        util::PowerPortStateStore::set_default_path(previous_path_);
        util::PowerPortStateStore::set_boot_id_path(previous_boot_path_);
        util::PowerPortStateStore::set_current_boot_id_for_testing(previous_boot_id_);
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    TempPowerPortDir(const TempPowerPortDir&) = delete;
    TempPowerPortDir& operator=(const TempPowerPortDir&) = delete;

    std::filesystem::path file() const { return dir_ / "state" / "power_port_states.json"; }
    void set_boot_id(const std::string& boot_id) {
        util::PowerPortStateStore::set_current_boot_id_for_testing(boot_id);
    }

private:
    std::filesystem::path dir_;
    std::string previous_path_;
    std::string previous_boot_path_;
    std::string previous_boot_id_;
};

}  // namespace alpacacore::test
