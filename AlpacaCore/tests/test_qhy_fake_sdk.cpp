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

// Contract tests for the QHY SDK fake itself (issue #321). The driver tests
// assert on this fake's ledger and fault injection, so the fake's own
// behaviour needs to hold up first -- a fake that miscounts opens would turn
// a real driver leak into a green test.

#include <alpacacore/util/error_handling.h>

#include <string>
#include <type_traits>

#include "catch2_compat.h"
#include "fake_qhy_sdk.h"
#include "locked_qhy_sdk.h"

using alpacacore::AlpacaException;
using alpacacore::test::FakeQHYSDK;
using alpacacore::test::LockedQHYSDK;
namespace AlpacaError = alpacacore::AlpacaError;
namespace control = alpacacore::vendor::qhy::control;

namespace {

FakeQHYSDK make_fake() {
    FakeQHYSDK fake;
    fake.cameras.push_back(FakeQHYSDK::default_camera("fake-qhy-0", "FakeQHY600"));
    return fake;
}

}  // namespace

TEST_CASE("FakeQHYSDK - opens are reference counted per camera id", "[qhy][fake][unit]") {
    // The wrapper shares ONE physical handle between the camera driver and the
    // paired CFW driver; the fake has to model that or the pairing tests lie.
    auto fake = make_fake();

    fake.open_camera("fake-qhy-0");
    CHECK(fake.physical_opens == 1);
    CHECK(fake.ref_count("fake-qhy-0") == 1);

    fake.open_camera("fake-qhy-0");
    CHECK(fake.physical_opens == 1);  // shared, not reopened
    CHECK(fake.ref_count("fake-qhy-0") == 2);

    fake.close_camera("fake-qhy-0");
    CHECK(fake.physical_closes == 0);  // still held by the other owner
    CHECK(fake.ref_count("fake-qhy-0") == 1);

    fake.close_camera("fake-qhy-0");
    CHECK(fake.physical_closes == 1);
    CHECK(fake.ref_count("fake-qhy-0") == 0);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("FakeQHYSDK - closing an unopened camera is counted, not thrown", "[qhy][fake][unit]") {
    // Driver teardown paths close best-effort and must not have their
    // exceptions swallowed into a false pass; the count is the assertion.
    auto fake = make_fake();
    fake.close_camera("fake-qhy-0");
    CHECK(fake.underflow_closes == 1);
    CHECK(fake.physical_closes == 0);
}

TEST_CASE("FakeQHYSDK - per-camera calls require an open handle", "[qhy][fake][unit]") {
    auto fake = make_fake();
    CHECK_THROWS_AS(fake.init_camera("fake-qhy-0"), AlpacaException);
    CHECK_THROWS_AS(fake.get_param("fake-qhy-0", control::GAIN), AlpacaException);
    CHECK_THROWS_AS(fake.get_cfw_position("fake-qhy-0"), AlpacaException);

    try {
        fake.get_param("fake-qhy-0", control::GAIN);
        FAIL("expected a throw");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::NotConnected);
    }
}

TEST_CASE("FakeQHYSDK - opening an unknown id throws NotConnected", "[qhy][fake][unit]") {
    auto fake = make_fake();
    try {
        fake.open_camera("no-such-camera");
        FAIL("expected a throw");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::NotConnected);
    }
    CHECK(fake.physical_opens == 0);
}

TEST_CASE("FakeQHYSDK - throw_from injects a failure into one named call", "[qhy][fake][unit]") {
    auto fake = make_fake();
    fake.throw_from.insert("init_camera");

    fake.open_camera("fake-qhy-0");
    try {
        fake.init_camera("fake-qhy-0");
        FAIL("expected a throw");
    } catch (const AlpacaException& ex) {
        CHECK(ex.error_code() == AlpacaError::DriverException);
    }
    // The call is still counted -- injection happens after the tally, so a
    // test can assert the driver reached the call it was meant to fail on.
    CHECK(fake.call_count("init_camera") == 1);
    CHECK(fake.call_count("open_camera") == 1);
}

TEST_CASE("FakeQHYSDK - an unavailable SDK resource fails the three entry points", "[qhy][fake][unit]") {
    // On real hardware this state is only reachable by segfaulting inside
    // libqhyccd's libusb hotplug init (issue #321), so it has never had a test.
    auto fake = make_fake();
    fake.sdk_resource_available = false;

    std::string model;
    CHECK_THROWS_AS(fake.enumerate_cameras(), AlpacaException);
    CHECK_THROWS_AS(fake.get_camera_model("fake-qhy-0", model), AlpacaException);
    CHECK_THROWS_AS(fake.open_camera("fake-qhy-0"), AlpacaException);
    CHECK(fake.physical_opens == 0);
}

TEST_CASE("FakeQHYSDK - the CFW position script drives transit then settles", "[qhy][fake][unit]") {
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");
    // -1 is GetQHYCCDCFWStatus's "still moving"; the last entry repeats so a
    // polling driver settles instead of running off the end of the script.
    fake.cfw_position_script = {-1, -1, 3};

    CHECK(fake.get_cfw_position("fake-qhy-0") == -1);
    CHECK(fake.get_cfw_position("fake-qhy-0") == -1);
    CHECK(fake.get_cfw_position("fake-qhy-0") == 3);
    CHECK(fake.get_cfw_position("fake-qhy-0") == 3);
    CHECK(fake.get_cfw_position("fake-qhy-0") == 3);
}

TEST_CASE("FakeQHYSDK - a move clears any pending script", "[qhy][fake][unit]") {
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");
    fake.cfw_position_script = {-1, -1, 0};
    fake.move_cfw("fake-qhy-0", 4);
    CHECK(fake.last_cfw_target == 4);
    CHECK(fake.get_cfw_position("fake-qhy-0") == 4);
}

TEST_CASE("FakeQHYSDK - chip info comes from the canned camera and keeps a set model", "[qhy][fake][unit]") {
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");

    alpacacore::vendor::qhy::QHYCameraInfo info{};
    REQUIRE(fake.get_chip_info("fake-qhy-0", info));
    CHECK(info.max_width == 64);
    CHECK(info.max_height == 48);
    CHECK(info.bpp == 16);
    CHECK(info.model == "FakeQHY600");

    // The camera driver pre-seeds info.model on reconnect and must not lose it.
    alpacacore::vendor::qhy::QHYCameraInfo preset{};
    preset.model = "Remembered";
    REQUIRE(fake.get_chip_info("fake-qhy-0", preset));
    CHECK(preset.model == "Remembered");
    CHECK(preset.max_width == 64);
}

TEST_CASE("FakeQHYSDK - frame length follows the configured ROI and bit depth", "[qhy][fake][unit]") {
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");

    fake.set_bits_mode("fake-qhy-0", 16);
    fake.set_resolution("fake-qhy-0", 0, 0, 64, 48);
    CHECK(fake.get_mem_length("fake-qhy-0") == 64U * 48U * 2U);

    fake.set_bits_mode("fake-qhy-0", 8);
    CHECK(fake.get_mem_length("fake-qhy-0") == 64U * 48U);
}

TEST_CASE("FakeQHYSDK - get_single_frame fills the buffer and returns immediately", "[qhy][fake][unit]") {
    // "Immediately" is the contract that keeps the driver's DETACHABLE
    // exposure worker from outliving the fake -- see rule 1 on FakeQHYSDK.
    auto fake = make_fake();
    fake.open_camera("fake-qhy-0");
    fake.set_resolution("fake-qhy-0", 0, 0, 4, 2);
    fake.set_bits_mode("fake-qhy-0", 16);

    std::vector<uint8_t> buf(4 * 2 * 2, 0xAB);
    uint32_t w = 0, h = 0, bpp = 0, ch = 0;
    CHECK(fake.get_single_frame("fake-qhy-0", buf.data(), w, h, bpp, ch));
    CHECK(w == 4);
    CHECK(h == 2);
    CHECK(bpp == 16);
    CHECK(ch == 1);
    CHECK(buf[0] == 0);
    CHECK(buf.back() == 0);

    fake.frame_ok = false;
    CHECK_FALSE(fake.get_single_frame("fake-qhy-0", buf.data(), w, h, bpp, ch));
}

TEST_CASE("FakeQHYSDK - default camera reports no cooler", "[qhy][fake][unit]") {
    // has_cooler starts the driver's telemetry thread, whose 1s poll makes
    // every disconnect block on the join. The cooled variant is opt-in.
    CHECK_FALSE(FakeQHYSDK::default_camera("id", "m").has_cooler);
    CHECK(FakeQHYSDK::default_cooled_camera("id", "m").has_cooler);
}

TEST_CASE("LockedQHYSDK - forwards to the inner SDK", "[qhy][fake][unit]") {
    auto fake = make_fake();
    LockedQHYSDK sdk(fake);

    sdk.open_camera("fake-qhy-0");
    sdk.init_camera("fake-qhy-0");
    CHECK(fake.physical_opens == 1);
    CHECK(fake.init_calls == 1);
    CHECK(sdk.get_sdk_version() == "fake-qhy-1.0");
    CHECK(sdk.enumerate_cameras().size() == 1);

    sdk.close_camera("fake-qhy-0");
    CHECK(fake.physical_closes == 1);
    CHECK(fake.underflow_closes == 0);
}

TEST_CASE("FakeQHYSDK - is movable so helpers can build one and return it", "[qhy][fake][unit]") {
    static_assert(std::is_move_constructible<FakeQHYSDK>::value,
                  "tests build a fake in a helper and return it by value");
    auto fake = make_fake();
    CHECK(fake.cameras.size() == 1);
}
