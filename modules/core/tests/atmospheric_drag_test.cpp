#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/atmospheric_drag.hpp>
#include <orbitforge/core/propagation/hpop.hpp>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <sstream>
#include <vector>

using namespace orbitforge::core;
namespace {
constexpr double spin = 7.292115e-5;
class SyntheticTime final : public ITimeProvider {
    TimeDataInfo info_{"synthetic-utc-is-tai", std::string(64, 'a'), "", 0, 100000};

  public:
    Epoch from_utc(const UtcDateTime &u) const override {
        const auto date = std::chrono::sys_days{std::chrono::year{u.year} /
                                                std::chrono::month{static_cast<unsigned>(u.month)} /
                                                std::chrono::day{static_cast<unsigned>(u.day)}};
        const auto first = std::chrono::sys_days{std::chrono::year{2000} / 1 / 1};
        return Epoch{std::chrono::duration_cast<std::chrono::nanoseconds>(date - first).count() +
                     ((u.hour * 3600LL + u.minute * 60 + u.second) - 43200) * 1000000000 +
                     u.nanosecond};
    }
    UtcDateTime to_utc(Epoch e) const override {
        const auto total = std::chrono::nanoseconds{e.tai_nanoseconds() + 43200000000000};
        const auto days = std::chrono::floor<std::chrono::days>(total);
        const std::chrono::year_month_day date{
            std::chrono::sys_days{std::chrono::year{2000} / 1 / 1} + days};
        const std::chrono::hh_mm_ss hms{total - days};
        return {static_cast<int>(date.year()),
                static_cast<int>(static_cast<unsigned>(date.month())),
                static_cast<int>(static_cast<unsigned>(date.day())),
                static_cast<int>(hms.hours().count()),
                static_cast<int>(hms.minutes().count()),
                static_cast<int>(hms.seconds().count()),
                static_cast<int>(hms.subseconds().count())};
    }
    int tai_minus_utc_seconds(Epoch) const override { return 0; }
    JulianDate julian_date(Epoch e, TimeScale scale) const override {
        return {scale, 2451545, e.seconds_since(Epoch{0}) / 86400};
    }
    const TimeDataInfo &data_info() const noexcept override { return info_; }
};
class RotatingFrames final : public IFrameProvider {
  public:
    PositionRotation gcrf_to_itrf(Epoch epoch, const EarthOrientationSample &) const override {
        const double angle = spin * epoch.seconds_since(Epoch{0}), c = std::cos(angle),
                     s = std::sin(angle);
        return {epoch,
                OriginId::earth,
                FrameId::gcrf,
                FrameId::itrf,
                {{{c, s, 0}, {-s, c, 0}, {0, 0, 1}}},
                "synthetic-spin",
                "test"};
    }
    StateTransform gcrf_to_itrf_state(Epoch epoch,
                                      const EarthOrientationState &eop) const override {
        const double angle = spin * epoch.seconds_since(Epoch{0}), c = std::cos(angle),
                     s = std::sin(angle);
        return {gcrf_to_itrf(epoch, eop.sample),
                {{{-spin * s, spin * c, 0}, {-spin * c, -spin * s, 0}, {0, 0, 0}}},
                "analytic-spin-rate"};
    }
};
class SyntheticEop final : public IEarthOrientationProvider {
    EopDataInfo info_{"test",
                      std::string(64, 'b'),
                      "",
                      "",
                      "",
                      Epoch{-172800000000000},
                      Epoch{172800000000000},
                      SyntheticTime{}.data_info()};

  public:
    EarthOrientationState at(Epoch e) const override {
        return {{e, 0, 0, 0, 0, 0, "test"}, {0, 0, 0, 0, 0}};
    }
    const EopDataInfo &data_info() const noexcept override { return info_; }
};
class SyntheticSun final : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{"test",
                            std::string(64, 'c'),
                            "test",
                            Epoch{-172800000000000},
                            Epoch{172800000000000},
                            1.327e20,
                            0};

  public:
    std::array<double, 3> position(ThirdBody body, Epoch e) const override {
        if (body != ThirdBody::sun)
            throw std::invalid_argument("Synthetic Sun only");
        const double phase =
            .03 + e.seconds_since(Epoch{0}) * 2 * std::numbers::pi / (365.25 * 86400);
        constexpr double au = 149597870700;
        return {au * std::cos(phase), au * std::sin(phase) * std::cos(.409),
                au * std::sin(phase) * std::sin(.409)};
    }
    void validate_interval(Epoch first, Epoch last,
                           std::span<const ThirdBody> bodies) const override {
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw std::out_of_range("Synthetic Sun coverage");
        for (auto body : bodies)
            if (body != ThirdBody::sun)
                throw std::invalid_argument("Synthetic Sun only");
    }
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
};
HpopEnvironment environment() {
    return {std::make_shared<RotatingFrames>(),
            std::make_shared<SyntheticEop>(),
            {},
            std::make_shared<SyntheticSun>(),
            std::make_shared<SyntheticTime>()};
}
} // namespace
TEST(AtmosphericDrag, KpConvertsToApRatherThanUsingKpAsAp) {
    EXPECT_EQ(kp_to_ap(0), 0);
    EXPECT_EQ(kp_to_ap(3), 15);
    EXPECT_EQ(kp_to_ap(9), 400);
    EXPECT_NEAR(kp_to_ap(1.5), 5.5, 1e-14);
    EXPECT_THROW((void)kp_to_ap(-1), std::invalid_argument);
    EXPECT_THROW((void)kp_to_ap(10), std::invalid_argument);
}
TEST(AtmosphericDrag, InvalidDatesEnumsNonfiniteInputsAndModelDomainsAreRejected) {
    AtmosphereInput input;
    input.geodetic.height_m = 400000;
    input.utc = {2001, 2, 29, 12, 0, 0, 0};
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::nrlmsise00, input),
                 std::invalid_argument);
    input.utc = {2000, 1, 1, 12, 0, 60, 0};
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input),
                 std::invalid_argument);
    input.utc = {2000, 1, 1, 12, 0, 0, 0};
    EXPECT_THROW((void)evaluate_atmosphere(static_cast<AtmosphereModel>(99), input),
                 std::invalid_argument);
    input.f107 = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::nrlmsise00, input),
                 std::invalid_argument);
    input.f107 = 150;
    input.geodetic.height_m = 1000001;
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::nrlmsise00, input), PropagationError);
    input.geodetic.height_m = 89999;
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input),
                 PropagationError);
    input.geodetic.height_m = 2500001;
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input),
                 PropagationError);
    EXPECT_THROW((void)drag_geodetic_position({0, 0, 0}), PropagationError);
    EXPECT_THROW((void)drag_geodetic_position({0, std::numeric_limits<double>::infinity(), 0}),
                 std::invalid_argument);
}
TEST(AtmosphericDrag, DensityMatchesIndependentAuthorNrlmsiseGtd7dIncludingLowAltitudeBoundaries) {
    std::ifstream source(ORBITFORGE_NRLMSISE_REFERENCE);
    ASSERT_TRUE(source);
    std::string line;
    std::size_t count = 0;
    double max_relative = 0;
    while (std::getline(source, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row(line);
        AtmosphereInput input;
        double seconds, expected_density, temperature, exospheric;
        ASSERT_TRUE(row >> input.utc.year >> input.utc.month >> input.utc.day >> seconds >>
                    input.geodetic.height_m >> input.geodetic.latitude_rad >>
                    input.geodetic.longitude_rad >> input.solar_hour_angle_rad >>
                    input.sun_declination_rad >> input.f107 >> input.f107_average >> input.kp >>
                    expected_density >> temperature >> exospheric);
        if (seconds == 86400) {
            input.utc.hour = 23;
            input.utc.minute = 59;
            input.utc.second = 60;
        } else {
            input.utc.hour = static_cast<int>(seconds) / 3600;
            input.utc.minute = static_cast<int>(seconds) % 3600 / 60;
            input.utc.second = static_cast<int>(seconds) % 60;
        }
        const auto actual = evaluate_atmosphere(AtmosphereModel::nrlmsise00, input);
        const double relative = std::abs(actual.density_kg_m3 / expected_density - 1);
        max_relative = std::max(max_relative, relative);
        EXPECT_NEAR(actual.density_kg_m3, expected_density, expected_density * 2e-10);
        EXPECT_NEAR(actual.temperature_k, temperature, 1e-7);
        EXPECT_NEAR(actual.exospheric_temperature_k, exospheric, 1e-7);
        ++count;
    }
    EXPECT_EQ(count, 396U);
    std::cout << "NRLMSISE author reference: max relative density error " << max_relative << '\n';
}
TEST(AtmosphericDrag, JrNoonMidnightEquinoxAndPolarLimitsAreFiniteAndConcurrent) {
    AtmosphereInput input;
    input.geodetic = {.5, .6, 400000};
    const auto expected = evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input);
    EXPECT_GT(expected.density_kg_m3, 0);
    for (double latitude : {-std::numbers::pi / 2, 0., std::numbers::pi / 2})
        for (double hour : {-std::numbers::pi, 0., std::numbers::pi}) {
            input.geodetic.latitude_rad = latitude;
            input.solar_hour_angle_rad = hour;
            EXPECT_GT(evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input).density_kg_m3,
                      0);
        }
    input.geodetic.latitude_rad = .6;
    input.solar_hour_angle_rad = 0;
    const auto run = [input] {
        return evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input).density_kg_m3;
    };
    auto a = std::async(std::launch::async, run), b = std::async(std::launch::async, run);
    EXPECT_EQ(a.get(), expected.density_kg_m3);
    EXPECT_EQ(b.get(), expected.density_kg_m3);
}
TEST(AtmosphericDrag, JrDensityMatchesGmatAndHighPrecisionOriginalEquationsAcrossRegimesAndSeasonalDates) {
    std::ifstream source(ORBITFORGE_JR_REFERENCE);
    ASSERT_TRUE(source);
    std::string line;
    std::size_t count = 0;
    double maximum = 0;
    while (std::getline(source, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row(line);
        AtmosphereInput input;
        double seconds, density;
        ASSERT_TRUE(row >> input.utc.year >> input.utc.month >> input.utc.day >> seconds >>
                    input.geodetic.height_m >> input.geodetic.latitude_rad >>
                    input.geodetic.longitude_rad >> input.solar_hour_angle_rad >>
                    input.sun_declination_rad >> input.f107 >> input.f107_average >> input.kp >>
                    density);
        if (seconds == 86400) {
            input.utc.hour = 23;
            input.utc.minute = 59;
            input.utc.second = 60;
        } else {
            input.utc.hour = static_cast<int>(seconds) / 3600;
            input.utc.minute = static_cast<int>(seconds) % 3600 / 60;
            input.utc.second = static_cast<int>(seconds) % 60;
        }
        const auto actual = evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input);
        EXPECT_NEAR(actual.density_kg_m3, density, density * 2e-10);
        maximum = std::max(maximum, std::abs(actual.density_kg_m3 / density - 1));
        ++count;
    }
    EXPECT_EQ(count, 576U);
    std::cout << "JR GMAT/high-precision reference: max relative density error " << maximum << '\n';
}
TEST(AtmosphericDrag, RelativeVelocityDirectionAreaAndLowAltitudeBlendActuallyAffectTheForce) {
    DragOptions options;
    options.enabled = true;
    const std::array<double, 3> r{6378137 + 400000, 0, 0}, v{0, 7200, 0}, sun{1e11, 0, 2e10};
    const auto original = evaluate_drag(r, v, sun, {2000, 1, 1, 12, 0, 0, 0}, options);
    EXPECT_GT(original.density_kg_m3, 0);
    EXPECT_EQ(original.acceleration_itrf_mps2[0], 0);
    EXPECT_LT(original.acceleration_itrf_mps2[1], 0);
    EXPECT_DOUBLE_EQ(original.acceleration_itrf_mps2[1],
                     -.5 * original.density_kg_m3 * 2.2 * .02 * 7200 * 7200);
    EXPECT_EQ(
        evaluate_drag(r, {0, 0, 0}, sun, {2000, 1, 1, 12, 0, 0, 0}, options).acceleration_itrf_mps2,
        (std::array{0., 0., 0.}));
    options.area_mass_ratio_m2_per_kg *= 2;
    EXPECT_EQ(
        evaluate_drag(r, v, sun, {2000, 1, 1, 12, 0, 0, 0}, options).acceleration_itrf_mps2[1],
        2 * original.acceleration_itrf_mps2[1]);
    options.low_altitude_model = LowAltitudeDensityModel::nrlmsise00;
    double previous = 0;
    for (double altitude : {80000., 89999., 90000., 90001., 119999., 120000., 120001.}) {
        const auto value =
            evaluate_drag({6378137 + altitude, 0, 0}, v, sun, {2000, 1, 1, 12, 0, 0, 0}, options);
        EXPECT_GT(value.density_kg_m3, 0);
        if (previous > 0 &&
            (altitude == 90000 || altitude == 90001 || altitude == 120000 || altitude == 120001)) {
            EXPECT_LT(std::abs(value.density_kg_m3 / previous - 1), .001);
        }
        previous = value.density_kg_m3;
    }
}
TEST(AtmosphericDrag, ApproximateAltitudeUsesRadialIntersectionAndKeepsGeodeticCoordinates) {
    // Independent 60-digit decimal ray/ellipsoid intersections, not the production formula.
    // Solve (t*x/a)^2+(t*y/a)^2+(t*z/b)^2=1, then measure |r-t*r|.
    const std::array<std::pair<std::array<double, 3>, double>, 3> cases{{
        {{4000000, 3000000, 4500000}, 358271.6089933367},
        {{-4800000, 4000000, -3500000}, 788693.7962020884},
        {{1000, -1000, 6800000}, 443247.83189334586},
    }};
    const std::array v{0., 7200., 0.}, sun{1e11, 0., 2e10};
    const UtcDateTime utc{2000, 1, 1, 12, 0, 0, 0};
    for (const auto model : {AtmosphereModel::jacchia_roberts, AtmosphereModel::nrlmsise00}) {
        for (const auto &[r, expected_height] : cases) {
            SCOPED_TRACE(expected_height);
            DragOptions options;
            options.enabled = true;
            options.atmosphere = model;
            const auto exact = evaluate_drag(r, v, sun, utc, options);
            options.use_approximate_altitude = true;
            const auto approximate = evaluate_drag(r, v, sun, utc, options);
            EXPECT_NEAR(approximate.density_altitude_m, expected_height, 3e-9);
            EXPECT_EQ(exact.density_altitude_m, exact.geodetic.height_m);
            EXPECT_EQ(approximate.geodetic.longitude_rad, exact.geodetic.longitude_rad);
            EXPECT_EQ(approximate.geodetic.latitude_rad, exact.geodetic.latitude_rad);
            EXPECT_EQ(approximate.geodetic.height_m, exact.geodetic.height_m);
            AtmosphereInput input;
            input.utc = utc;
            input.geodetic = exact.geodetic;
            input.geodetic.height_m = expected_height;
            input.solar_hour_angle_rad = std::atan2(r[1], r[0]);
            input.sun_declination_rad = std::atan2(2e10, 1e11);
            const auto density = evaluate_atmosphere(model, input).density_kg_m3;
            EXPECT_NEAR(approximate.density_kg_m3 / density, 1, 1e-12);
            EXPECT_DOUBLE_EQ(approximate.acceleration_itrf_mps2[1],
                             -.5 * approximate.density_kg_m3 * 2.2 * .02 * 7200 * 7200);
            if (std::abs(r[0]) > 1e6) {
                EXPECT_GT(approximate.density_altitude_m, exact.density_altitude_m);
                EXPECT_NE(approximate.density_kg_m3, exact.density_kg_m3);
            }
        }
    }
}
TEST(AtmosphericDrag, ApproximateAltitudeEquatorPolesDomainsAndLowAltitudeSelection) {
    constexpr double a = 6378137, b = a * (1 - 1 / 298.257223563);
    const std::array v{0., 7200., 0.}, sun{1e11, 0., 2e10};
    const UtcDateTime utc{2000, 1, 1, 12, 0, 0, 0};
    DragOptions options;
    options.enabled = options.use_approximate_altitude = true;
    options.atmosphere = AtmosphereModel::nrlmsise00;
    for (const double height : {0., 90000., 400000., 1000000.}) {
        for (const auto r : {std::array{a + height, 0., 0.}, std::array{0., -a - height, 0.},
                             std::array{0., 0., b + height}, std::array{0., 0., -b - height}}) {
            const auto result = evaluate_drag(r, v, sun, utc, options);
            EXPECT_DOUBLE_EQ(result.density_altitude_m, height);
            EXPECT_NEAR(result.geodetic.height_m, height, 1e-8);
            EXPECT_GT(result.density_kg_m3, 0);
        }
    }
    EXPECT_THROW((void)evaluate_drag({a - 1, 0, 0}, v, sun, utc, options), PropagationError);
    EXPECT_THROW((void)evaluate_drag({a + 1000001, 0, 0}, v, sun, utc, options), PropagationError);
    EXPECT_THROW((void)evaluate_drag({0, 0, 0}, v, sun, utc, options), PropagationError);
    EXPECT_THROW(
        (void)evaluate_drag({0, std::numeric_limits<double>::infinity(), 0}, v, sun, utc, options),
        std::invalid_argument);
    options.atmosphere = AtmosphereModel::jacchia_roberts;
    options.low_altitude_model = LowAltitudeDensityModel::nrlmsise00;
    options.blending_range_m = 0;
    // Exact geodetic height is below 90 km, radial height just above: model selection
    // must use the same height as the density kernel. Values are independent decimals.
    const std::array r{4566084.003699506, 0., 4566084.003699506};
    const auto radial = evaluate_drag(r, v, sun, utc, options);
    EXPECT_LT(radial.geodetic.height_m, 90000);
    EXPECT_NEAR(radial.density_altitude_m, 90000.2, 3e-9);
    AtmosphereInput input;
    input.utc = utc;
    input.geodetic = radial.geodetic;
    input.geodetic.height_m = radial.density_altitude_m;
    input.sun_declination_rad = std::atan2(2e10, 1e11);
    EXPECT_EQ(radial.density_kg_m3,
              evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input).density_kg_m3);
    options.use_approximate_altitude = false;
    const auto exact = evaluate_drag(r, v, sun, utc, options);
    input.geodetic.height_m = exact.geodetic.height_m;
    EXPECT_EQ(exact.density_kg_m3,
              evaluate_atmosphere(AtmosphereModel::nrlmsise00, input).density_kg_m3);
    options.use_approximate_altitude = true;
    options.blending_range_m = 100;
    const auto blend =
        evaluate_drag({4566119.217617209, 0., 4566119.217617209}, v, sun, utc, options);
    EXPECT_NEAR(blend.density_altitude_m, 90050, 3e-9);
    input.geodetic = blend.geodetic;
    input.geodetic.height_m = blend.density_altitude_m;
    const double lower = evaluate_atmosphere(AtmosphereModel::nrlmsise00, input).density_kg_m3;
    const double upper = evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input).density_kg_m3;
    // The decimal midpoint is rounded to binary64 Cartesian coordinates. Propagate
    // the 3 nm height bound through max |dw/dh| = 1.5 / blending_range.
    const double expected = .5 * (lower + upper);
    const double bound = expected * 1e-12 + std::abs(upper - lower) * 1.5 * 3e-9 / 100;
    EXPECT_NEAR(blend.density_kg_m3, expected, bound);
}
TEST(AtmosphericDrag, CoRotatingWgs84NrlmsisePropagationMatchesIndependentOrekit) {
    for (bool vop : {false, true})
        for (auto method : {HpopIntegrator::rkf78, HpopIntegrator::bulirsch_stoer}) {
            SCOPED_TRACE(vop);

            std::ifstream source(ORBITFORGE_DRAG_REFERENCE);
            ASSERT_TRUE(source);
            std::string line;
            std::size_t count = 0;
            double max_position = 0, max_velocity = 0;
            const auto env = environment();
            while (std::getline(source, line)) {
                if (line.empty() || line[0] == '#')
                    continue;
                SCOPED_TRACE(line);
                std::istringstream row(line);
                int mode;
                double seconds;
                CartesianState initial{Epoch{0}, OriginId::earth, FrameId::gcrf, {}, {}},
                    expected = initial;
                std::array<double, 3> acceleration;
                ASSERT_TRUE(row >> mode >> seconds);
                for (auto &v : initial.position_m)
                    ASSERT_TRUE(row >> v);
                for (auto &v : initial.velocity_mps)
                    ASSERT_TRUE(row >> v);
                for (auto &v : expected.position_m)
                    ASSERT_TRUE(row >> v);
                for (auto &v : expected.velocity_mps)
                    ASSERT_TRUE(row >> v);
                for (auto &v : acceleration)
                    ASSERT_TRUE(row >> v);
                expected.epoch = Epoch{static_cast<std::int64_t>(seconds * 1e9)};
                HpopOptions options;
                options.vop = vop;
                options.integrator = method;
                options.gravity_degree = options.gravity_order = 0;
                options.max_step_seconds = 2.5;
                options.min_step_seconds = 1e-6;
                options.relative_tolerance = 1e-14;
                options.position_tolerance_m = 1e-8;
                options.velocity_tolerance_mps = 1e-11;
                options.drag.enabled = true;
                options.drag.atmosphere = AtmosphereModel::nrlmsise00;
                if (mode == 1) {
                    options.drag.f107 = 70;
                    options.drag.f107_average = 80;
                    options.drag.kp = 0;
                }
                if (mode == 2) {
                    options.drag.f107 = 250;
                    options.drag.f107_average = 220;
                    options.drag.kp = 8.5;
                    options.drag.cd = 2.7;
                    options.drag.area_mass_ratio_m2_per_kg = .035;
                }
                const auto transform =
                    env.frames->gcrf_to_itrf_state(initial.epoch, env.eop->at(initial.epoch));
                const auto fixed = transform.apply(initial);
                const auto force =
                    evaluate_drag(fixed.position_m, fixed.velocity_mps,
                                  transform.rotation.apply_position(
                                      env.ephemeris->position(ThirdBody::sun, initial.epoch)),
                                  env.time->to_utc(initial.epoch), options.drag);
                const auto inertial_force =
                    transform.rotation.inverse_position(force.acceleration_itrf_mps2);
                const auto actual = make_hpop(initial, options, env)->state_at(expected.epoch);
                for (std::size_t i = 0; i < 3; ++i) {
                    EXPECT_NEAR(inertial_force[i], acceleration[i], 1e-17);
                    EXPECT_NEAR(actual.position_m[i], expected.position_m[i], .0005);
                    EXPECT_NEAR(actual.velocity_mps[i], expected.velocity_mps[i], 5e-7);
                    max_position = std::max(
                        max_position, std::abs(actual.position_m[i] - expected.position_m[i]));
                    max_velocity = std::max(
                        max_velocity, std::abs(actual.velocity_mps[i] - expected.velocity_mps[i]));
                }
                ++count;
            }
            EXPECT_EQ(count, 24U);
            std::cout << "Drag Orekit reference: max position component " << max_position
                      << " m; velocity " << max_velocity << " m/s\n";
        }
}
TEST(AtmosphericDrag, SharedMassAndIndependentAreasMatchOrekitCombinedForcesAndPropagation) {
    std::ifstream source(ORBITFORGE_MASS_REFERENCE);
    ASSERT_TRUE(source);
    const auto env = environment();
    std::string line;
    std::size_t count = 0;
    double max_acceleration = 0, max_position = 0, max_velocity = 0;
    while (std::getline(source, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row(line);
        double mass, drag_area, srp_area, seconds;
        CartesianState initial{Epoch{0}, OriginId::earth, FrameId::gcrf, {}, {}},
            expected = initial;
        std::array<double, 3> drag_acceleration, srp_acceleration;
        ASSERT_TRUE(row >> mass >> drag_area >> srp_area >> seconds);
        for (auto *vector : {&initial.position_m, &initial.velocity_mps, &expected.position_m,
                             &expected.velocity_mps, &drag_acceleration, &srp_acceleration}) {
            for (auto &value : *vector)
                ASSERT_TRUE(row >> value);
        }
        expected.epoch = Epoch{static_cast<std::int64_t>(seconds * 1e9)};
        HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.max_step_seconds = 2.5;
        options.min_step_seconds = 1e-6;
        options.relative_tolerance = 1e-14;
        options.position_tolerance_m = 1e-8;
        options.velocity_tolerance_mps = 1e-11;
        options.satellite_mass_kg = mass;
        options.drag.enabled = options.srp.enabled = true;
        options.drag.atmosphere = AtmosphereModel::nrlmsise00;
        options.drag.area_mass_ratio_m2_per_kg = drag_area / mass;
        options.srp.area_mass_ratio_m2_per_kg = srp_area / mass;
        options.srp.cr = 1.7;
        options.srp.sun_position_method = SrpSunPositionMethod::geometric;
        options.srp.moon_shadow = false;
        EXPECT_DOUBLE_EQ(options.drag_area_m2(), drag_area);
        EXPECT_DOUBLE_EQ(options.srp_area_m2(), srp_area);
        const auto transform =
            env.frames->gcrf_to_itrf_state(initial.epoch, env.eop->at(initial.epoch));
        const auto fixed = transform.apply(initial);
        const auto sun = env.ephemeris->position(ThirdBody::sun, initial.epoch);
        const auto drag = evaluate_drag(fixed.position_m, fixed.velocity_mps,
                                        transform.rotation.apply_position(sun),
                                        env.time->to_utc(initial.epoch), options.drag);
        const auto da = transform.rotation.inverse_position(drag.acceleration_itrf_mps2);
        const auto sa = evaluate_srp(initial.position_m, sun, options.srp).acceleration_mps2;
        const auto actual = make_hpop(initial, options, env)->state_at(expected.epoch);
        // STK spherical semantics: a different shared mass must leave both explicit ratios intact.
        options.satellite_mass_kg = 9876;
        const auto changed = make_hpop(initial, options, env)->state_at(expected.epoch);
        EXPECT_EQ(changed.position_m, actual.position_m);
        EXPECT_EQ(changed.velocity_mps, actual.velocity_mps);
        for (std::size_t i = 0; i < 3; ++i) {
            EXPECT_NEAR(da[i], drag_acceleration[i], 1e-17);
            EXPECT_NEAR(sa[i], srp_acceleration[i], 1e-17);
            EXPECT_NEAR(actual.position_m[i], expected.position_m[i], .0005);
            EXPECT_NEAR(actual.velocity_mps[i], expected.velocity_mps[i], 5e-7);
            max_acceleration = std::max({max_acceleration, std::abs(da[i] - drag_acceleration[i]),
                                         std::abs(sa[i] - srp_acceleration[i])});
            max_position =
                std::max(max_position, std::abs(actual.position_m[i] - expected.position_m[i]));
            max_velocity =
                std::max(max_velocity, std::abs(actual.velocity_mps[i] - expected.velocity_mps[i]));
        }
        ++count;
    }
    EXPECT_EQ(count, 28U);
    std::cout << "Mass Orekit reference: max acceleration " << max_acceleration
              << " m/s^2; position " << max_position << " m; velocity " << max_velocity << " m/s\n";
}
TEST(AtmosphericDrag, RequiresUtcEopSunAndDeclaredApparentCoverageAndCancellationIsReusable) {
    const CartesianState initial{
        Epoch{0}, OriginId::earth, FrameId::gcrf, {6778137, 0, 0}, {0, 6300, 4300}};
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.drag.enabled = true;
    auto env = environment();
    auto missing = env;
    missing.time.reset();
    EXPECT_THROW((void)make_hpop(initial, options, missing), std::invalid_argument);
    missing = env;
    missing.frames.reset();
    EXPECT_THROW((void)make_hpop(initial, options, missing), std::invalid_argument);
    missing = env;
    missing.eop.reset();
    EXPECT_THROW((void)make_hpop(initial, options, missing), std::invalid_argument);
    missing = env;
    missing.ephemeris.reset();
    EXPECT_THROW((void)make_hpop(initial, options, missing), std::invalid_argument);
    options.drag.use_apparent_sun_position = true;
    EXPECT_THROW((void)make_hpop(initial, options, env), std::invalid_argument);
    options.drag.use_apparent_sun_position = false;
    const auto provider = make_hpop(initial, options, env),
               fresh = make_hpop(initial, options, env);
    unsigned calls = 0;
    const Epoch target{6000000000000};
    EXPECT_THROW((void)provider->state_at(target,
                                          [&] {
                                              if (++calls == 40)
                                                  throw std::runtime_error("cancel-drag");
                                          }),
                 std::runtime_error);
    EXPECT_EQ(provider->state_at(target).position_m, fresh->state_at(target).position_m);
    EXPECT_THROW((void)provider->state_at(Epoch{172801000000000}), std::out_of_range);
}

TEST(AtmosphericDrag, NativeDailyAndHistoricalApMatchIndependentAuthorGtd7d) {
    std::ifstream source(ORBITFORGE_NRLMSISE_AP_REFERENCE);
    ASSERT_TRUE(source);
    std::string line;
    std::size_t count = 0;
    double max_relative = 0;
    while (std::getline(source, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row(line);
        AtmosphereInput input;
        double seconds, density, temperature, exospheric;
        int history;
        std::array<double, 7> ap{};
        ASSERT_TRUE(row >> input.utc.year >> input.utc.month >> input.utc.day >> seconds >>
                    input.geodetic.height_m >> input.geodetic.latitude_rad >>
                    input.geodetic.longitude_rad >> input.solar_hour_angle_rad >> input.f107 >>
                    input.f107_average >> history);
        for (auto &value : ap)
            ASSERT_TRUE(row >> value);
        ASSERT_TRUE(row >> density >> temperature >> exospheric);
        if (seconds == 86400) {
            input.utc.hour = 23;
            input.utc.minute = 59;
            input.utc.second = 60;
        } else {
            input.utc.hour = static_cast<int>(seconds) / 3600;
            input.utc.minute = static_cast<int>(seconds) % 3600 / 60;
            input.utc.second = static_cast<int>(seconds) % 60;
        }
        // Inactive Kp must not be converted, even as a default argument expression.
        input.kp = std::numeric_limits<double>::quiet_NaN();
        if (history)
            input.ap_history = ap;
        else
            input.daily_ap = ap[0];
        const auto actual = evaluate_atmosphere(AtmosphereModel::nrlmsise00, input);
        max_relative = std::max(max_relative, std::abs(actual.density_kg_m3 / density - 1));
        EXPECT_NEAR(actual.density_kg_m3, density, density * 2e-10);
        EXPECT_NEAR(actual.temperature_k, temperature, 1e-7);
        EXPECT_NEAR(actual.exospheric_temperature_k, exospheric, 1e-7);
        ++count;
    }
    EXPECT_EQ(count, 1056u);
    std::cout << "NRLMSISE native/history Ap max relative density error: " << max_relative << '\n';
}

TEST(AtmosphericDrag, ExplicitApModesAreDistinctValidatedAndLocalToEachCall) {
    AtmosphereInput input;
    input.geodetic = {.4, .6, 400000};
    const auto model = AtmosphereModel::nrlmsise00;
    const auto manual = evaluate_atmosphere(model, input);
    input.daily_ap = 15;
    const auto daily = evaluate_atmosphere(model, input);
    EXPECT_EQ(daily.density_kg_m3, manual.density_kg_m3);
    input.ap_history = std::array{15., 15., 15., 15., 15., 15., 15.};
    EXPECT_THROW((void)evaluate_atmosphere(model, input), std::invalid_argument);
    input.daily_ap.reset();
    const auto steady = evaluate_atmosphere(model, input);
    EXPECT_GT(std::abs(steady.density_kg_m3 / daily.density_kg_m3 - 1), 1e-4);
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input),
                 std::invalid_argument);
    auto storm = input;
    storm.ap_history = std::array{40., 400., 100., 50., 20., 7.125, 3.75};
    const auto storm_density = evaluate_atmosphere(model, storm).density_kg_m3;
    EXPECT_NE(storm_density, steady.density_kg_m3);
    std::vector<std::future<double>> calls;
    for (int i = 0; i < 16; ++i)
        calls.push_back(std::async(std::launch::async, [&, i] {
            return evaluate_atmosphere(model, i % 2 ? storm : input).density_kg_m3;
        }));
    for (std::size_t i = 0; i < calls.size(); ++i)
        EXPECT_EQ(calls[i].get(), i % 2 ? storm_density : steady.density_kg_m3);
    for (const double bad : {-1., 400.01, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        for (std::size_t i = 0; i < 7; ++i) {
            auto invalid = input;
            (*invalid.ap_history)[i] = bad;
            EXPECT_THROW((void)evaluate_atmosphere(model, invalid), std::invalid_argument);
        }
        auto invalid = input;
        invalid.ap_history.reset();
        invalid.daily_ap = bad;
        EXPECT_THROW((void)evaluate_atmosphere(model, invalid), std::invalid_argument);
    }
    input.ap_history.reset();
    input.daily_ap = 0;
    EXPECT_THROW((void)evaluate_atmosphere(AtmosphereModel::jacchia_roberts, input),
                 std::invalid_argument);
    input.daily_ap.reset();
    EXPECT_EQ(evaluate_atmosphere(model, input).density_kg_m3, manual.density_kg_m3);
}
