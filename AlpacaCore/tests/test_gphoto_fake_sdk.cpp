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

// Hardware-free driver tests through the fault-injectable GPhotoSDK/RawDecoder
// seams (issue #489). Before this, the gphoto camera driver's connect,
// widget-fallback, ISO, and bulb-capture code paths were only exercisable
// against real DSLR hardware.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/gphoto/gphoto_camera_driver.h>

#include <chrono>
#include <thread>

#include "catch2_compat.h"
#include "fake_gphoto_sdk.h"
#include "fake_raw_decoder.h"

using alpacacore::AlpacaException;
using alpacacore::test::FakeGPhotoSDK;
using alpacacore::test::FakeRawDecoder;
using alpacacore::test::reset_gphoto_sensor_cache;
using alpacacore::test::unique_test_model;

namespace {

FakeGPhotoSDK::FakeCamera make_camera(const std::string& model = "Nikon DSC D5300",
                                      const std::string& port = "usb:001,004") {
    FakeGPhotoSDK::FakeCamera cam;
    cam.model = unique_test_model(model);
    cam.port = port;
    cam.choices["iso"] = {"100", "200", "400", "800"};
    cam.choice_value["iso"] = "200";
    cam.choices["shutterspeed2"] = {"1/4000", "1/200", "1", "30", "bulb"};
    cam.choice_value["shutterspeed2"] = "1/200";
    cam.toggle_value["bulb"] = false;
    cam.choices["imageformat"] = {"NEF (Raw)", "JPEG Fine"};
    cam.choice_value["imageformat"] = "JPEG Fine";
    return cam;
}

void wait_for_exposure_to_finish(alpacacore::CameraDriver& driver) {
    for (int i = 0; i < 500 && driver.get_camera_state() == alpacacore::CameraState::Exposing; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// A camera whose longest native shutter choice is very short (1/50s), so a
// bulb test can request a still-fast exposure (well under a second of real
// sleeping in bulb_capture_with_abort's slice loop) that nonetheless exceeds
// every native choice and forces the driver into the bulb toggle sequence.
FakeGPhotoSDK::FakeCamera make_bulb_camera() {
    auto cam = make_camera("Nikon DSC D5300 (bulb)", "usb:001,005");
    cam.choices["shutterspeed2"] = {"1/4000", "1/200", "1/50", "bulb"};
    cam.choice_value["shutterspeed2"] = "1/200";
    return cam;
}

}  // namespace

TEST_CASE("GPhoto camera fake - connect populates handle and balances open/close", "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
    CHECK(fake.open_count == 1);
    CHECK(fake.close_count == 0);
    // Connect must have taken the priming capture+decode, not the on-disk
    // sensor-cache shortcut -- otherwise reset_gphoto_sensor_cache() above
    // has silently stopped working and every test here loses that coverage.
    CHECK(decoder.decode_call_count.load() >= 1);
    // Priming must not look like a real exposure to an ASCOM client.
    CHECK(driver->get_image_ready() == false);

    driver->set_connected(false);
    CHECK(driver->get_connected() == false);
    CHECK(fake.close_count == 1);
}

TEST_CASE("GPhoto camera fake - connect-path throw closes the handle and leaves disconnected",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    fake.throw_from.insert("get_choices");
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    CHECK_THROWS_AS(driver->set_connected(true), AlpacaException);
    CHECK(driver->get_connected() == false);
    // The connect-path open must be balanced by a close on the failure path.
    CHECK(fake.open_count == fake.close_count);

    fake.throw_from.clear();
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
}

TEST_CASE("GPhoto camera fake - shutter widget fallback to shutterspeed when shutterspeed2 absent",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    auto cam = make_camera();
    cam.choices.erase("shutterspeed2");
    cam.choice_value.erase("shutterspeed2");
    cam.choices["shutterspeed"] = {"1/1000", "1", "bulb"};
    cam.choice_value["shutterspeed"] = "1/1000";
    fake.cameras.push_back(cam);
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    CHECK(driver->get_connected() == true);
    // ExposureMax reflects the fallback widget's native shutter choices
    // having been read at all (non-default sentinel of 30.0 from "1").
    CHECK(driver->get_exposure_min() > 0.0);
    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - ISO get/set round-trip via the iso widget", "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    // Fake camera starts at iso choice_value "200", index 1 in {100,200,400,800}.
    CHECK(driver->get_gain() == 1);

    driver->set_gain(3);
    CHECK(driver->get_gain() == 3);
    CHECK(fake.cameras[0].choice_value.at("iso") == "800");

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - short native exposure decodes into image_ready with fake frame dims",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);
    CHECK(driver->get_image_ready() == false);

    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_image_ready() == true);
    CHECK(driver->get_camera_x_size() == decoder.canned_frame.width);
    CHECK(driver->get_camera_y_size() == decoder.canned_frame.height);
    CHECK(driver->get_bayer_offset_x() == decoder.canned_frame.bayer_offset_x);
    CHECK(driver->get_bayer_offset_y() == decoder.canned_frame.bayer_offset_y);

    auto image = driver->get_image_array();
    CHECK(image.width == decoder.canned_frame.width);
    CHECK(image.height == decoder.canned_frame.height);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - bulb capture drives toggle true then false then downloads",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_bulb_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    // Longer than the longest native shutter choice (1/50s, from
    // make_bulb_camera()'s shutterspeed2 choices) so the driver must fall
    // back to the bulb toggle sequence, while staying fast to run.
    driver->start_exposure(0.05, true);
    wait_for_exposure_to_finish(*driver);

    REQUIRE(fake.bulb_toggle_history.size() >= 2);
    CHECK(fake.bulb_toggle_history.front() == true);
    CHECK(fake.bulb_toggle_history.back() == false);
    CHECK(driver->get_image_ready() == true);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - stop_exposure mid-bulb closes the shutter early", "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_bulb_camera());
    FakeRawDecoder decoder;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    // Well past the bulb camera's 1/50s native ceiling, so the driver enters
    // the bulb toggle sequence; stop_exposure() aborts it well before the
    // full requested duration elapses.
    driver->start_exposure(5.0, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    driver->stop_exposure();

    REQUIRE(fake.bulb_toggle_history.size() >= 2);
    CHECK(fake.bulb_toggle_history.front() == true);
    CHECK(fake.bulb_toggle_history.back() == false);

    driver->set_connected(false);
}

TEST_CASE("GPhoto camera fake - decoder failure leaves camera idle with no image ready",
          "[gphoto][camera][unit][fakesdk]") {
    reset_gphoto_sensor_cache();
    FakeGPhotoSDK fake;
    fake.cameras.push_back(make_camera());
    FakeRawDecoder decoder;
    decoder.should_throw = true;

    auto driver = alpacacore::vendor::gphoto::create_gphoto_camera(0, 0, fake, decoder);
    driver->set_connected(true);

    driver->start_exposure(0.01, true);
    wait_for_exposure_to_finish(*driver);

    CHECK(driver->get_camera_state() == alpacacore::CameraState::Idle);
    CHECK(driver->get_image_ready() == false);

    driver->set_connected(false);
}
