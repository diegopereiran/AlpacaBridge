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

// Sky-truth checks for the Sky-Watcher direct driver's pointing model
// (open-astro#432).
//
// WHY THIS FILE EXISTS. Every other SkyWatcher loopback test, and ConformU
// too, judges the driver against its own reported coordinates. The driver
// computes those from the same motor counts it commanded, so it always
// agrees with itself and a wrong pointing model is invisible. That is how a
// six-hour error shipped and passed conformance on three different boards.
//
// WHAT IS AND IS NOT AN EXTERNAL ANCHOR HERE. The four hardware rows in the
// first test case are: an EQM-35 Pro at latitude -37.2 was driven to known
// axis positions on 2026-09-12 with the shipped (wrong) 3.5.1 build and the
// tube's real direction was read off the mount by hand (three rows), and a
// fourth, northern row comes from the Wave 150i report that opened the
// issue. Those four, and the alt/az cross-check against what was observed,
// are the only checks in this file that the driver cannot satisfy by
// agreeing with itself.
//
// `sky_from_axes()` below is a transcription of the driver's own formula, so
// the goto cases downstream of it pin the goto path against the model rather
// than against the sky. They are still worth having -- they catch a goto that
// stops commanding what the model says -- but they are not independent
// evidence for the model. #458 tracks the vector oracle that would be.
//
// If you change the pointing model, this file is what has to justify it, and
// a new hardware row is what has to extend it. Do not "verify" a change here
// against the driver's own readback.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <numbers>
#include <thread>

#include "catch2_compat.h"
#include "fake_skywatcher_mount.h"

namespace sw = alpacacore::vendor::skywatcher;
using alpacacore::test::FakeSkyWatcherMount;

namespace {

constexpr double kPi = std::numbers::pi;

sw::ConnectionInfo endpoint(const FakeSkyWatcherMount& mount) {
    sw::ConnectionInfo info;
    info.type = sw::ConnectionType::Network;
    info.host = "127.0.0.1";
    info.udp_port = mount.port();
    info.response_timeout_ms = 250;
    return info;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

double wrap_ha(double hours) {
    double h = std::fmod(hours, 24.0);
    if (h < -12.0) {
        h += 24.0;
    }
    if (h >= 12.0) {
        h -= 24.0;
    }
    return h;
}

struct SkyPoint {
    double ha_hours;
    double dec_degrees;
};

struct AltAz {
    double altitude_degrees;
    double azimuth_degrees;
};

// THE REFERENCE. Where a Sky-Watcher German equatorial physically points for
// raw axis angles measured from the counterweight-down, tube-at-the-pole home.
//
//   HA  = s * (a1 / 15) + (a2 >= 0 ? +6 h : -6 h)
//   dec = s * (90 - |a2|)                      s = +1 north, -1 south
//
// The 6 h term is the home position: with the counterweight hanging straight
// down the dec axis lies in the meridian plane, so rotating the dec axis
// alone sweeps the tube along the HA = +/-6 h circle and the meridian is
// reached only with the counterweight bar horizontal. Its SIGN follows which
// side of the dec axis the tube is on, and does NOT flip with hemisphere.
// The a1 term does flip, because the mount faces the opposite pole.
SkyPoint sky_from_axes(double latitude_degrees, double a1_degrees, double a2_degrees) {
    const double s = latitude_degrees < 0.0 ? -1.0 : 1.0;
    const double ha = s * (a1_degrees / 15.0) + (a2_degrees >= 0.0 ? 6.0 : -6.0);
    return {wrap_ha(ha), s * (90.0 - std::abs(a2_degrees))};
}

AltAz horizon_from_sky(const SkyPoint& p, double latitude_degrees) {
    const double h = p.ha_hours * 15.0 * kPi / 180.0;
    const double d = p.dec_degrees * kPi / 180.0;
    const double l = latitude_degrees * kPi / 180.0;
    const double sin_alt = std::clamp(std::sin(d) * std::sin(l) + std::cos(d) * std::cos(l) * std::cos(h), -1.0, 1.0);
    const double alt = std::asin(sin_alt);
    const double cos_az =
        std::clamp((std::sin(d) - std::sin(alt) * std::sin(l)) / (std::cos(alt) * std::cos(l)), -1.0, 1.0);
    double az = std::acos(cos_az) * 180.0 / kPi;
    if (std::sin(h) > 0.0) {
        az = 360.0 - az;
    }
    return {alt * 180.0 / kPi, az};
}

struct LandedFrame {
    double a1;
    double a2;
    double lst;
    double reported_ra;
    double reported_dec;
    int side_of_pier;
};

LandedFrame land(alpacacore::TelescopeDriver& driver, FakeSkyWatcherMount& mount, double ra_hours, double dec) {
    driver.slew_to_coordinates_async(ra_hours, dec);
    REQUIRE(driver.get_slewing());
    REQUIRE(wait_until([&] { return !driver.get_slewing(); }, 90000));
    LandedFrame f{};
    f.lst = driver.get_sidereal_time();
    f.a1 = mount.physical_degrees(1);
    f.a2 = mount.physical_degrees(2);
    f.reported_ra = driver.get_right_ascension();
    f.reported_dec = driver.get_declination();
    f.side_of_pier = driver.get_side_of_pier();
    return f;
}

// Covers the ~1 s of sidereal motion between the snapshot reads plus the
// goto's own landing deadband (~8 arcsec).
constexpr double kHaToleranceHours = 0.01;
constexpr double kDecToleranceDegrees = 0.05;

void check_landing(const LandedFrame& f, double latitude, double target_ra, double target_dec, int expected_side) {
    const SkyPoint sky = sky_from_axes(latitude, f.a1, f.a2);
    const double target_ha = wrap_ha(f.lst - target_ra);
    INFO("axes a1=" << f.a1 << " a2=" << f.a2 << " -> real HA " << sky.ha_hours << " h, dec " << sky.dec_degrees
                    << "; target HA " << target_ha << " h, dec " << target_dec);
    // 1. The tube physically points at the target, judged by the reference.
    CHECK(std::abs(wrap_ha(sky.ha_hours - target_ha)) < kHaToleranceHours);
    CHECK(std::abs(sky.dec_degrees - target_dec) < kDecToleranceDegrees);
    // 2. The driver's own report agrees, which is all the older tests checked.
    CHECK(std::abs(wrap_ha(f.reported_ra - target_ra)) < kHaToleranceHours);
    CHECK(std::abs(f.reported_dec - target_dec) < kDecToleranceDegrees);
    // 3. The counterweight bar never rises above horizontal on a goto.
    CHECK(std::abs(f.a1) <= 90.0 + 0.5);
    // 4. The reported pier side is the ASCOM side for that hour angle.
    CHECK(f.side_of_pier == expected_side);
}

}  // namespace

TEST_CASE("SkyWatcher pointing - the model reproduces the positions measured on hardware (#432)",
          "[skywatcher][telescope][pointing]") {
    // Rows 1-3: EQM-35 Pro, latitude -37.2 (rounded), 2026-09-12, shipped
    // 3.5.1 build, tube direction read off the mount by hand after each
    // goto. Row 4: the Wave 150i report that opened #432, latitude +45.45.
    struct Row {
        const char* what;
        double latitude;
        double a1;
        double a2;
        double expect_ha;
        double expect_dec;
        double expect_alt;
        double expect_az;  // negative = not read off the mount for this row
        const char* observed;
    };
    const Row rows[] = {
        {"counterweight down, dec axis square", -37.2, 1.6, -90.0, -6.11, 0.0, -1.3, 91.0, "level, pointing east"},
        {"RA axis 60 deg, dec axis square", -37.2, 60.0, -90.0, -10.00, 0.0, -43.6, -1.0, "down about 45 deg"},
        {"RA axis 45 deg, dec axis 70 deg", -37.2, 45.1, -70.0, -9.01, -20.0, -18.9, 136.0, "down, azimuth about 136"},
        {"Wave 150i, the #432 report", 45.45, 61.98, 70.95, 10.13, 19.05, -20.7, -1.0, "down about 20 deg"},
    };
    for (const Row& r : rows) {
        const SkyPoint sky = sky_from_axes(r.latitude, r.a1, r.a2);
        const AltAz horizon = horizon_from_sky(sky, r.latitude);
        INFO(r.what << ": observed " << r.observed);
        CHECK(std::abs(wrap_ha(sky.ha_hours - r.expect_ha)) < 0.02);
        CHECK(std::abs(sky.dec_degrees - r.expect_dec) < 0.1);
        CHECK(std::abs(horizon.altitude_degrees - r.expect_alt) < 0.5);
        if (r.expect_az >= 0.0) {
            // A second independent quantity per row where the azimuth was
            // read off the mount as well as the altitude.
            CHECK(std::abs(horizon.azimuth_degrees - r.expect_az) < 1.0);
        }
    }

    // The first row is the one that needs no instrument, and it is what
    // refutes the shipped model on its own: counterweight straight down and
    // the dec axis at 90 deg puts the tube perpendicular to both the polar
    // axis and the counterweight bar, which share one vertical plane, so the
    // tube MUST be level. Level and square to the meridian is six hours of
    // hour angle away from it. The shipped model called that position
    // HA -11.9 h, which is 53 degrees below the horizon.
    const AltAz perpendicular = horizon_from_sky(sky_from_axes(-37.2, 0.0, -90.0), -37.2);
    CHECK(std::abs(perpendicular.altitude_degrees) < 0.5);
}

TEST_CASE("SkyWatcher pointing - a goto west of the meridian lands on the sky, south (#432)",
          "[skywatcher][telescope][pointing][eqm35][hemisphere]") {
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    const double latitude = -35.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 150.0, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 3.0 + 24.0, 24.0);  // HA +3 h
    const double target_dec = -20.0;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 0);

    const LandedFrame f = land(*driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 0);
    // HA >= 0 takes the a2 >= 0 branch in both hemispheres; south of the
    // equator the RA axis then runs the other way: a1 = -(3 - 6) * 15 = +45.
    CHECK(f.a2 > 0.0);
    CHECK(std::abs(f.a1 - 45.0) < 1.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - a goto east of the meridian lands on the sky, south",
          "[skywatcher][telescope][pointing][eqm35][hemisphere]") {
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    const double latitude = -35.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 150.0, 80.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst + 3.0, 24.0);  // HA -3 h
    const double target_dec = -60.0;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 1);

    const LandedFrame f = land(*driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 1);
    CHECK(f.a2 < 0.0);
    CHECK(std::abs(f.a1 + 45.0) < 1.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - the Wave 150i goto from the #432 report lands on the sky, north",
          "[skywatcher][telescope][pointing]") {
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i());
    REQUIRE(mount.ok());
    const double latitude = 45.45;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 11.0, 200.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 4.12 + 24.0, 24.0);  // HA +4.12 h, the Arcturus geometry
    const double target_dec = 19.05;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 0);

    const LandedFrame f = land(*driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 0);
    // The shipped build sent a1 = +62 here and the tube ended 20 deg below
    // the horizon. The correct axis angle is (4.12 - 6) * 15 = -28.2.
    CHECK(std::abs(f.a1 + 28.2) < 1.0);
    CHECK(std::abs(f.a2 - 70.95) < 0.2);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - a goto east of the meridian lands on the sky, north",
          "[skywatcher][telescope][pointing]") {
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i());
    REQUIRE(mount.ok());
    const double latitude = 45.45;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 11.0, 200.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst + 3.0, 24.0);  // HA -3 h
    const double target_dec = 40.0;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 1);

    const LandedFrame f = land(*driver, mount, target_ra, target_dec);
    check_landing(f, latitude, target_ra, target_dec, 1);
    CHECK(f.a2 < 0.0);
    CHECK(std::abs(f.a1 - 45.0) < 1.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - tracking holds the physical hour angle in both hemispheres",
          "[skywatcher][telescope][pointing][hemisphere]") {
    struct Site {
        alpacacore::test::FakeMountProfile profile;
        double latitude;
        double longitude;
        double expected_axis_sign;  // which way the counts must run to follow the sky
    };
    const Site sites[] = {
        {alpacacore::test::FakeMountProfile::wave_100i(), 45.0, 11.0, +1.0},
        {alpacacore::test::FakeMountProfile::eqm35_pro(), -35.0, 150.0, -1.0},
    };
    for (const Site& site : sites) {
        FakeSkyWatcherMount mount(site.profile);
        REQUIRE(mount.ok());
        auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), site.latitude, site.longitude, 100.0);
        driver->set_connected(true);
        mount.jump_axis_degrees(2, 45.0);  // off the pole, where hour angle means something
        driver->set_tracking(true);
        REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));

        const double lst0 = driver->get_sidereal_time();
        const double a1_0 = mount.physical_degrees(1);
        const double ra0 = driver->get_right_ascension();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const double lst1 = driver->get_sidereal_time();
        const double a1_1 = mount.physical_degrees(1);
        const double ra1 = driver->get_right_ascension();

        INFO("latitude " << site.latitude << ": axis1 " << a1_0 << " -> " << a1_1 << " deg over "
                         << (lst1 - lst0) * 3600.0 << " sidereal seconds");
        // Counts up north of the equator, down south of it.
        CHECK((a1_1 - a1_0) * site.expected_axis_sign > 0.0);
        // And at the rate that keeps the tube on the star: the physical hour
        // angle advances with sidereal time.
        const double physical_advance = wrap_ha(sky_from_axes(site.latitude, a1_1, 45.0).ha_hours -
                                                sky_from_axes(site.latitude, a1_0, 45.0).ha_hours);
        const double lst_advance = lst1 - lst0;
        CHECK(physical_advance > 0.0);
        CHECK(std::abs(physical_advance - lst_advance) < 0.3 * lst_advance + 0.5 / 3600.0);
        // ...which is the same thing as the reported RA standing still.
        CHECK(std::abs(wrap_ha(ra1 - ra0)) * 3600.0 < 2.0);

        driver->set_tracking(false);
        driver->set_connected(false);
    }
}

TEST_CASE("SkyWatcher pointing - an East guide pulse with Tracking off runs the southern way",
          "[skywatcher][telescope][pointing][eqm35][hemisphere]") {
    // Round-2 review finding: pulse_guide()'s not-tracking RA branch is the
    // SECOND call site of ra_axis_sign_locked() and had no case of its own.
    // Every other pulse test connects at latitude +39.7392, where the sign is
    // +1 and the factor is a no-op, and the southern East-pulse case below
    // enables tracking, so it takes the ra_rate_adjust branch instead. Delete
    // `ra_axis_sign_locked() *` from that line and the whole suite stayed
    // green while an autoguider pulsing a parked-rate mount below the equator
    // pushed the star the wrong way.
    //
    // Sky sense, south: guide East = RA increasing. dec = -(90 - |a2|) and
    // HA = -(a1/15) + 6 on this branch, so RA = LST - HA rises when a1 rises:
    // the axis must run in the INCREASING-count direction, the opposite of
    // the same pulse in the north.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), -35.0, 150.0, 80.0);
    driver->set_connected(true);
    mount.jump_axis_degrees(2, 45.0);
    REQUIRE_FALSE(driver->get_tracking());

    const double a1_before = mount.physical_degrees(1);
    driver->pulse_guide(2, 1500);  // East, 1.5 s, no tracking to fold it into
    REQUIRE(driver->get_is_pulse_guiding());
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 8000));
    const double moved = mount.physical_degrees(1) - a1_before;

    INFO("south, Tracking off, East pulse: axis 1 moved " << moved << " deg");
    CHECK(moved > 0.0);

    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - a southern East guide pulse stays on the in-place rate change",
          "[skywatcher][telescope][pointing][eqm35][hemisphere]") {
    // Round-2 review finding, the other unguarded sign: the in-place-versus-
    // stop-and-restart guard is `axis_sign * ra_pulse_rate <= kMinInPlace...`.
    // Drop the axis_sign factor and every southern pulse rate is negative, so
    // the guard is always true and EVERY guide correction takes the full
    // stop-and-restart path -- the mount stops and re-accelerates the RA axis
    // on every cycle of a guiding session. Magnitude and direction both still
    // come out right on that path, which is why the direction case below
    // passes either way; only the stop count can see it.
    //
    // This is the southern twin of the northern assertion in
    // test_skywatcher_async.cpp ("a kick, never a stop/restart").
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), -35.0, 150.0, 80.0);
    driver->set_connected(true);
    mount.jump_axis_degrees(2, 45.0);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // past the ramp
    const int stops_before = mount.stop_count(1);

    driver->pulse_guide(2, 1200);  // East, at the 0.5x default guide rate
    REQUIRE(driver->get_is_pulse_guiding());
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 8000));

    INFO("south, tracking, East pulse: RA stops " << stops_before << " -> " << mount.stop_count(1));
    CHECK(mount.stop_count(1) == stops_before);
    CHECK(mount.axis_running(1));

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - an East guide pulse slows the axis in the tracking sense, south",
          "[skywatcher][telescope][pointing][eqm35][hemisphere]") {
    // Guide East means the tube falls behind the sky, so the RA axis must run
    // SLOWER in whichever direction tracking uses -- below the equator that is
    // the decreasing-count direction. An East correction that sped the axis up
    // or reversed it would push the star the wrong way on every guide cycle.
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::eqm35_pro());
    REQUIRE(mount.ok());
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), -35.0, 150.0, 80.0);
    driver->set_connected(true);
    mount.jump_axis_degrees(2, 45.0);
    driver->set_tracking(true);
    REQUIRE(wait_until([&] { return mount.axis_running(1); }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // past the ramp

    const double sidereal_deg_per_sec = 360.0 / 86164.0905;
    const double a1_before = mount.physical_degrees(1);
    const double ra_before = driver->get_right_ascension();
    const auto t0 = std::chrono::steady_clock::now();
    driver->pulse_guide(2, 2000);  // East, 2 s at the 0.5x default guide rate
    REQUIRE(driver->get_is_pulse_guiding());
    REQUIRE(wait_until([&] { return !driver->get_is_pulse_guiding(); }, 10000));
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double moved = mount.physical_degrees(1) - a1_before;
    const double ra_after = driver->get_right_ascension();

    INFO("axis1 moved " << moved * 3600.0 << " arcsec in " << elapsed << " s");
    CHECK(moved < 0.0);  // still turning the tracking way, never reversed
    CHECK(std::abs(moved) < sidereal_deg_per_sec * (elapsed - 0.5));
    CHECK(wrap_ha(ra_after - ra_before) > 0.0);  // an autoguider sees RA rise

    driver->set_tracking(false);
    driver->set_connected(false);
}

#endif  // !_WIN32

TEST_CASE("SkyWatcher pointing - the home term keeps its branch at the exact pole (#459)",
          "[skywatcher][telescope][unit]") {
    // ra_dec_to_axis_degrees_locked() computes the dec-axis angle as
    // branch * (90 - dec_mech); at the visible pole that is +0.0 or -0.0
    // depending on the hour angle's sign, and the readback derives the 6 h
    // home term from that angle. A `>= 0.0` test collapsed -0.0 onto the
    // positive branch (IEEE 754: -0.0 >= 0.0 is true), so a slew to
    // dec = +90 with a negative hour angle read back 12 h out in RA. Only
    // the reported coordinate was wrong (RA is degenerate at the pole), but
    // a conformance client comparing readback to target sees the 12 h.
    using alpacacore::vendor::skywatcher::detail::home_hour_angle_offset;
    CHECK(home_hour_angle_offset(45.0) == 6.0);
    CHECK(home_hour_angle_offset(-45.0) == -6.0);
    CHECK(home_hour_angle_offset(0.0) == 6.0);
    // The literal shape the driver produces on the negative branch at the pole.
    const double branch = -1.0;
    const double a2_at_pole = branch * (90.0 - 90.0);
    REQUIRE(std::signbit(a2_at_pole));  // the fixture really is -0.0
    CHECK(home_hour_angle_offset(a2_at_pole) == -6.0);
    CHECK(home_hour_angle_offset(-0.0) == -6.0);
}
