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
// Every other SkyWatcher loopback test judges the driver against its OWN
// reported coordinates, and the driver reports the same model it commands,
// so a wrong model is invisible to them and to ConformU alike. That is how a
// 90 deg RA-axis error shipped: the model read the RA axis angle as the hour
// angle, but with a counterweight-down home a pure dec rotation sweeps the
// HA = +/-6 h circle, not the meridian.
//
// These tests carry an INDEPENDENT oracle: a vector model of a German
// equatorial (RA axis along the visible pole, counterweight bar hanging down
// at home, OTA along the RA axis at home) that turns raw axis angles into the
// hour angle and declination the OTA physically points at. Its sign
// conventions are indi-eqmod's, the reference implementation for these motor
// boards: increasing RA counts move west north of the equator, and the dec
// branch/label relations are its EncodersToRADec(). The oracle is itself
// pinned against EQMOD's closed form below, so a drift in either shows up.

#ifndef _WIN32

#include <alpacacore/telescope_driver.h>
#include <alpacacore/vendor/skywatcher/skywatcher_telescope_driver.h>

#include <array>
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
constexpr double kSiderealHoursPerSecond = 24.0 / 86164.0905;

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

using Vec3 = std::array<double, 3>;

double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Rodrigues rotation of v about the unit axis k by angle (radians).
Vec3 rotate(const Vec3& v, const Vec3& k, double angle) {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const Vec3 kxv{k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0]};
    const double kd = dot(k, v);
    Vec3 out{};
    for (int i = 0; i < 3; ++i) {
        out[i] = v[i] * c + kxv[i] * s + k[i] * kd * (1.0 - c);
    }
    return out;
}

struct SkyPoint {
    double ha_hours;
    double dec_degrees;
};

// Where a German equatorial physically points for raw axis angles (a1, a2)
// measured from the counterweight-down, OTA-at-pole home.
//
// Frame: x = east, y = north, z = up. The RA axis runs along the VISIBLE
// pole (NCP north of the equator, SCP south of it); the counterweight bar is
// perpendicular to it, in the meridian plane, pointing down; the OTA is
// perpendicular to the bar and lies along the RA axis at home. A positive
// axis angle is a rotation by -angle about the toward-pole vector (the sense
// that makes increasing RA counts move WEST north of the equator, as EQMOD
// has it), and the same body-relative sense on the dec axis. The mount body
// is the same object in both hemispheres; only the pole it faces differs.
SkyPoint physical_pointing(double latitude_degrees, double a1_degrees, double a2_degrees) {
    const bool south = latitude_degrees < 0.0;
    const double lat = latitude_degrees * kPi / 180.0;
    const double a = std::abs(lat);
    const double ns = south ? -1.0 : 1.0;
    const Vec3 pole{0.0, ns * std::cos(a), std::sin(a)};       // visible pole
    const Vec3 bar_home{0.0, ns * std::sin(a), -std::cos(a)};  // counterweight bar, down
    const Vec3 bar = rotate(bar_home, pole, -a1_degrees * kPi / 180.0);
    const Vec3 ota = rotate(pole, bar, -a2_degrees * kPi / 180.0);
    // Sky coordinates are always measured from the NORTH celestial pole.
    const Vec3 ncp{0.0, std::cos(lat), std::sin(lat)};
    const Vec3 meridian_equator{0.0, -std::sin(lat), std::cos(lat)};  // HA 0, dec 0
    const Vec3 east{1.0, 0.0, 0.0};
    const double dec = std::asin(std::clamp(dot(ota, ncp), -1.0, 1.0)) * 180.0 / kPi;
    const double ha = std::atan2(-dot(ota, east), dot(ota, meridian_equator)) * 180.0 / kPi / 15.0;
    return {ha, dec};
}

// indi-eqmod's EncodersToRADec() re-expressed relative to its home position
// (DEStepHome = DEStepInit + steps/4): the closed form the driver implements.
SkyPoint eqmod_closed_form(double latitude_degrees, double a1_degrees, double a2_degrees) {
    const double sky_sign = latitude_degrees < 0.0 ? -1.0 : 1.0;
    double dec_mech = 0.0;
    double ha_mech = 0.0;
    if (a2_degrees >= 0.0) {
        dec_mech = 90.0 - a2_degrees;
        ha_mech = a1_degrees / 15.0 + 6.0;
    } else {
        dec_mech = 90.0 + a2_degrees;
        ha_mech = a1_degrees / 15.0 - 6.0;
    }
    return {wrap_ha(sky_sign * ha_mech), sky_sign * dec_mech};
}

struct LandedFrame {
    double a1;
    double a2;
    double lst;
    double reported_ra;
    double reported_dec;
    int side_of_pier;
};

LandedFrame land(alpacacore::TelescopeDriver& driver, FakeSkyWatcherMount& mount, double ra_hours, double dec_degrees) {
    driver.slew_to_coordinates_async(ra_hours, dec_degrees);
    REQUIRE(driver.get_slewing());
    REQUIRE(wait_until([&] { return !driver.get_slewing(); }, 90000));
    // One consistent snapshot: the sidereal time the driver itself uses, and
    // the raw axis angles the fake's motors are at, read back to back.
    LandedFrame f{};
    f.lst = driver.get_sidereal_time();
    f.a1 = mount.physical_degrees(1);
    f.a2 = mount.physical_degrees(2);
    f.reported_ra = driver.get_right_ascension();
    f.reported_dec = driver.get_declination();
    f.side_of_pier = driver.get_side_of_pier();
    return f;
}

// The tolerances cover the ~1 s between the snapshot reads (15 arcsec of
// sidereal motion) plus the goto's own landing deadband (~8 arcsec).
constexpr double kHaToleranceHours = 0.01;  // 36 s of time = 9 arcmin of RA
constexpr double kDecToleranceDegrees = 0.05;

void check_physical_landing(const LandedFrame& f, double latitude, double target_ra, double target_dec,
                            int expected_side) {
    const SkyPoint sky = physical_pointing(latitude, f.a1, f.a2);
    const double target_ha = wrap_ha(f.lst - target_ra);
    INFO("axes a1=" << f.a1 << " a2=" << f.a2 << " -> physical HA " << sky.ha_hours << " h, dec " << sky.dec_degrees
                    << "; target HA " << target_ha << " h, dec " << target_dec);
    // 1. The OTA physically points at the target (the oracle, not the driver).
    CHECK(std::abs(wrap_ha(sky.ha_hours - target_ha)) < kHaToleranceHours);
    CHECK(std::abs(sky.dec_degrees - target_dec) < kDecToleranceDegrees);
    // 2. The driver's own report agrees with the target (self-consistency,
    //    the only thing the older tests ever checked).
    CHECK(std::abs(wrap_ha(f.reported_ra - target_ra)) < kHaToleranceHours);
    CHECK(std::abs(f.reported_dec - target_dec) < kDecToleranceDegrees);
    // 3. The counterweight bar never goes above horizontal on a goto.
    CHECK(std::abs(f.a1) <= 90.0 + 0.5);
    // 4. The reported side is the ASCOM side for that hour angle.
    CHECK(f.side_of_pier == expected_side);
}

}  // namespace

TEST_CASE("SkyWatcher pointing - the physical oracle matches EQMOD's closed form in both hemispheres",
          "[skywatcher][telescope][pointing]") {
    // Guards the oracle itself: the vector geometry and the EQMOD-derived
    // formula the driver implements must agree everywhere, north and south,
    // on both dec-axis branches, or neither can be trusted.
    for (double latitude : {45.0, -35.0}) {
        for (double a1 = -170.0; a1 <= 170.0; a1 += 17.0) {
            for (double a2 : {-150.0, -71.0, -30.0, -5.0, 5.0, 30.0, 71.0, 150.0}) {
                const SkyPoint v = physical_pointing(latitude, a1, a2);
                const SkyPoint e = eqmod_closed_form(latitude, a1, a2);
                INFO("lat " << latitude << " a1 " << a1 << " a2 " << a2);
                REQUIRE(std::abs(wrap_ha(v.ha_hours - e.ha_hours)) < 1e-9);
                REQUIRE(std::abs(v.dec_degrees - e.dec_degrees) < 1e-9);
            }
        }
    }
    // The Wave 150i report that opened #432: the old model commanded RA axis
    // +62.0 deg, dec axis +71.0 deg for Arcturus at HA +4.12 h, dec +19.05.
    // Physically that is HA +10.13 h at dec 19 -- 20 deg below the horizon
    // from the reporter's latitude, which is where the tube ended up.
    const SkyPoint old_model = physical_pointing(45.0, 61.98, 70.95);
    CHECK(std::abs(old_model.ha_hours - 10.13) < 0.01);
    CHECK(std::abs(old_model.dec_degrees - 19.05) < 0.01);
}

TEST_CASE("SkyWatcher pointing - a goto west of the meridian lands on the sky, north (#432 regression)",
          "[skywatcher][telescope][pointing]") {
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i());
    REQUIRE(mount.ok());
    const double latitude = 45.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 11.0, 200.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    // The Arcturus geometry from the report: HA +4.12 h, dec +19.05.
    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst - 4.12 + 24.0, 24.0);
    const double target_dec = 19.05;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 0);

    const LandedFrame f = land(*driver, mount, target_ra, target_dec);
    check_physical_landing(f, latitude, target_ra, target_dec, 0);
    // The corrected axis solution: RA axis (4.12 - 6) * 15 = -28.2 deg, NOT
    // the +61.8 the old model produced; dec axis 90 - 19.05 on branch A.
    CHECK(f.a1 < -27.0);
    CHECK(f.a1 > -30.0);
    CHECK(std::abs(f.a2 - 70.95) < 0.2);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - a goto east of the meridian lands on the sky, north",
          "[skywatcher][telescope][pointing]") {
    FakeSkyWatcherMount mount(alpacacore::test::FakeMountProfile::wave_100i());
    REQUIRE(mount.ok());
    const double latitude = 45.0;
    auto driver = sw::create_skywatcher_telescope(0, endpoint(mount), latitude, 11.0, 200.0);
    driver->set_connected(true);
    driver->set_tracking(true);

    const double lst = driver->get_sidereal_time();
    const double target_ra = std::fmod(lst + 3.0, 24.0);  // HA -3 h
    const double target_dec = 40.0;
    REQUIRE(driver->get_destination_side_of_pier(target_ra, target_dec) == 1);

    const LandedFrame f = land(*driver, mount, target_ra, target_dec);
    check_physical_landing(f, latitude, target_ra, target_dec, 1);
    // Branch B: dec axis -(90 - 40) = -50, RA axis (-3 + 6) * 15 = +45.
    CHECK(f.a2 < 0.0);
    CHECK(std::abs(f.a1 - 45.0) < 1.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - a goto west of the meridian lands on the sky, south (#432, #261)",
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
    check_physical_landing(f, latitude, target_ra, target_dec, 0);
    // South of the equator the pierEast side is realised on the a2 < 0
    // branch (the mount faces the other pole): dec axis -(90 - 20) = -70,
    // RA axis (-3 + 6) * 15 = +45 in the mechanical frame. That answers the
    // #261 audit question -- the label and the branch part company below
    // the equator.
    CHECK(f.a2 < 0.0);
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
    check_physical_landing(f, latitude, target_ra, target_dec, 1);
    // Branch A (a2 >= 0) is pierWest here: dec axis 90 - 60 = +30, RA axis
    // (3 - 6) * 15 = -45.
    CHECK(f.a2 > 0.0);
    CHECK(std::abs(f.a1 + 45.0) < 1.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

TEST_CASE("SkyWatcher pointing - tracking holds the physical hour angle in both hemispheres (#250 reversed)",
          "[skywatcher][telescope][pointing][hemisphere]") {
    struct Site {
        alpacacore::test::FakeMountProfile profile;
        double latitude;
        double longitude;
        double expected_axis_sign;
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
        // Away from the pole, where HA is well defined: dec axis 45 deg.
        mount.jump_axis_degrees(2, 45.0);
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
        // The axis turns in the hemisphere's tracking sense...
        CHECK((a1_1 - a1_0) * site.expected_axis_sign > 0.0);
        // ...at a rate that keeps the PHYSICAL hour angle advancing with LST
        // (i.e. the OTA stays on the star), within the fake's ramp-up.
        const double physical_ha_advance = wrap_ha(physical_pointing(site.latitude, a1_1, 45.0).ha_hours -
                                                   physical_pointing(site.latitude, a1_0, 45.0).ha_hours);
        const double lst_advance = lst1 - lst0;
        CHECK(physical_ha_advance > 0.0);
        CHECK(std::abs(physical_ha_advance - lst_advance) < 0.3 * lst_advance + 0.5 / 3600.0);
        // ...and the reported RA stands still.
        CHECK(std::abs(wrap_ha(ra1 - ra0)) * 3600.0 < 2.0);

        driver->set_tracking(false);
        driver->set_connected(false);
    }
}

TEST_CASE("SkyWatcher pointing - an East guide pulse slows the axis in the tracking sense, south",
          "[skywatcher][telescope][pointing][eqm35][hemisphere]") {
    // Guide East = RA increasing = the OTA falling behind the sky, so the RA
    // axis must run SLOWER in the tracking direction, which below the equator
    // is the decreasing-count direction. A pulse that instead sped the axis
    // up (or reversed it) would push every East correction the wrong way.
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
    const double a1_after = mount.physical_degrees(1);
    const double ra_after = driver->get_right_ascension();

    const double moved = a1_after - a1_before;  // negative = tracking sense in the south
    INFO("axis1 moved " << moved * 3600.0 << " arcsec in " << elapsed << " s");
    CHECK(moved < 0.0);  // still turning in the tracking sense, never reversed
    // Slower than plain tracking over the window: 2 s at half rate is 1 s of
    // sidereal motion lost, well outside the sampling jitter.
    CHECK(std::abs(moved) < sidereal_deg_per_sec * (elapsed - 0.5));
    // And the driver reports what an autoguider expects: RA went UP.
    CHECK(wrap_ha(ra_after - ra_before) > 0.0);

    driver->set_tracking(false);
    driver->set_connected(false);
}

#endif  // !_WIN32
