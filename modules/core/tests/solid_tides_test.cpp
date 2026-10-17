#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/hpop.hpp>
#include <sstream>
using namespace orbitforge::core;
namespace {
GravityInfo field(bool zero = false, int degree = 21, int order = 21) {
    return {"reference",    {},     {},   zero ? "zero_tide" : "tide_free", egm96_mu,
            egm96_radius_m, degree, order};
}
TideRaisingBody body(bool sun, double elapsed = 0, int geometry = 0) {
    const double radius = sun ? 1.496e11 : 384400000;
    const double rate = 2 * std::numbers::pi / ((sun ? 365.25 : 27.321661) * 86400);
    const double angle = (sun ? .3 : 1.2) + geometry + rate * elapsed;
    const double tilt = sun ? .409 : .5;
    return {{radius * std::cos(angle), radius * std::sin(angle) * std::cos(tilt),
             radius * std::sin(angle) * std::sin(tilt)},
            sun ? 1.3271244004127942e20 : 4.9028001184575496e12};
}
class Frames final : public IFrameProvider {
  public:
    PositionRotation gcrf_to_itrf(Epoch epoch, const EarthOrientationSample &) const override {
        return {epoch,
                OriginId::earth,
                FrameId::gcrf,
                FrameId::itrf,
                {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
                "test-identity",
                "test"};
    }
    StateTransform gcrf_to_itrf_state(Epoch epoch,
                                      const EarthOrientationState &eop) const override {
        return {gcrf_to_itrf(epoch, eop.sample), {}, "test-static"};
    }
};
class Eop final : public IEarthOrientationProvider {
    EopDataInfo info_{"test",
                      "",
                      "",
                      "",
                      "",
                      Epoch{-1000000000000000},
                      Epoch{1000000000000000},
                      {"", "", "", 0, 100000}};

  public:
    EarthOrientationState at(Epoch epoch) const override {
        return {{epoch, -31.7, .12 * std::numbers::pi / 648000, .25 * std::numbers::pi / 648000, 0,
                 0, "test"},
                {0, 0, 0, 0, 0}};
    }
    const EopDataInfo &data_info() const noexcept override { return info_; }
};
// Controlled clock shared with the independent Java reference, UTC = TAI - 32 s.
class Time final : public ITimeProvider {
    TimeDataInfo info_{"test", "", "", 0, 100000};

  public:
    explicit Time(std::string hash = {}) { info_.sha256 = std::move(hash); }
    Epoch from_utc(const UtcDateTime &u) const override {
        using namespace std::chrono;
        const auto days = sys_days{year{u.year} / month{static_cast<unsigned>(u.month)} /
                                   day{static_cast<unsigned>(u.day)}} -
                          sys_days{year{2000} / 1 / 1};
        const auto seconds = static_cast<std::int64_t>(days.count()) * 86400 + u.hour * 3600 +
                             u.minute * 60 + u.second - 43200 + 32;
        return Epoch{seconds * 1000000000LL + u.nanosecond};
    }
    UtcDateTime to_utc(Epoch e) const override {
        using namespace std::chrono;
        const auto ns = nanoseconds{e.tai_nanoseconds()} + seconds{43200 - 32};
        const auto days = floor<std::chrono::days>(ns);
        const year_month_day date{sys_days{year{2000} / 1 / 1} + days};
        const hh_mm_ss clock{ns - days};
        return {int(date.year()),
                int(unsigned(date.month())),
                int(unsigned(date.day())),
                int(clock.hours().count()),
                int(clock.minutes().count()),
                int(clock.seconds().count()),
                int(clock.subseconds().count())};
    }
    int tai_minus_utc_seconds(Epoch) const override { return 32; }
    JulianDate julian_date(Epoch e, TimeScale scale) const override {
        return {scale, 2451545,
                (e.seconds_since(Epoch{0}) + (scale == TimeScale::tt ? 32.184 : 0)) / 86400};
    }
    const TimeDataInfo &data_info() const noexcept override { return info_; }
};
class Ephemeris final : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{
        "test",        "", "test", Epoch{-90000000000000}, Epoch{90000000000000}, body(true).mu,
        body(false).mu};

  public:
    mutable std::vector<double> epochs;
    std::array<double, 3> position(ThirdBody id, Epoch epoch) const override {
        if (id != ThirdBody::sun && id != ThirdBody::moon)
            throw std::invalid_argument("Unexpected tide body");
        epochs.push_back(epoch.seconds_since(Epoch{0}));
        return body(id == ThirdBody::sun, epochs.back()).position_itrf_m;
    }
    void validate_interval(Epoch first, Epoch last,
                           std::span<const ThirdBody> bodies) const override {
        EXPECT_EQ(bodies.size(), 2U);
        EXPECT_EQ(bodies[0], ThirdBody::sun);
        EXPECT_EQ(bodies[1], ThirdBody::moon);
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw std::out_of_range("Tide ephemeris coverage");
    }
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
};
} // namespace
TEST(SolidTides, FullAccelerationMatchesIndependentOrekitAcrossTruncationsAndTideSystems) {
    std::ifstream file(ORBITFORGE_SOLID_TIDE_REFERENCE);
    ASSERT_TRUE(file.is_open());
    int rows = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] != 'A')
            continue;
        std::istringstream in(line);
        char tag;
        int zero, geometry, degree, order, truncate;
        std::array<double, 3> r{}, expected{};
        in >> tag >> zero >> geometry >> degree >> order >> truncate;
        for (auto &v : r)
            in >> v;
        for (auto &v : expected)
            in >> v;
        ASSERT_TRUE(in);
        auto info = field(zero != 0);
        const double c20 = -4.84165371736e-4 + (zero ? iers2003_permanent_tide_c20() : 0);
        GravityModel model(info, {{2, 0, c20, 0}});
        const std::array bodies{body(true, 0, geometry), body(false, 0, geometry)};
        const auto deltas = solid_tide_corrections(info, {SolidTideMode::full, truncate != 0},
                                                   degree, order, bodies);
        const auto actual = model.acceleration(r, degree, order, deltas);
        for (int axis = 0; axis < 3; ++axis)
            EXPECT_NEAR(actual[axis], expected[axis], 2e-14) << line;
        ++rows;
    }
    EXPECT_EQ(rows, 120);
}
TEST(SolidTides, PermanentTideMatchesAnalyticJ2AndDoesNotMutateTheBaseField) {
    auto info = field();
    GravityModel model(info, {});
    const std::array<double, 3> r{4e6, -3e6, 5e6};
    const auto unchanged = model.acceleration(r, 2, 2);
    const auto deltas = solid_tide_corrections(info, {SolidTideMode::permanent, true}, 2, 2);
    ASSERT_EQ(deltas.size(), 1U);
    EXPECT_NEAR(deltas[0].c, -4.20067548472e-9, 1e-24);
    const auto actual = model.acceleration(r, 2, 2, deltas);
    const double radius = std::hypot(r[0], r[1], r[2]);
    const double j2 = -std::sqrt(5.0) * deltas[0].c;
    const double factor = 1.5 * j2 * info.mu * info.radius_m * info.radius_m / std::pow(radius, 5);
    for (int axis = 0; axis < 3; ++axis) {
        const double analytic =
            factor * r[axis] * (5 * r[2] * r[2] / (radius * radius) - (axis == 2 ? 3 : 1));
        EXPECT_NEAR(actual[axis] - unchanged[axis], analytic, 2e-15);
    }
    EXPECT_EQ(model.acceleration(r, 2, 2), unchanged);
    const auto zero = field(true);
    EXPECT_TRUE(solid_tide_corrections(zero, {SolidTideMode::permanent, true}, 2, 2).empty());
    const auto removed = solid_tide_corrections(zero, {}, 2, 2);
    ASSERT_EQ(removed.size(), 1U);
    EXPECT_EQ(removed[0].c, -deltas[0].c);
    // A 0/0 base field contains no permanent C20 to subtract.
    EXPECT_TRUE(solid_tide_corrections(zero, {SolidTideMode::none, false}, 0, 0).empty());
    EXPECT_EQ(solid_tide_corrections(zero, {SolidTideMode::permanent, false}, 0, 0)[0].c,
              deltas[0].c);
}
TEST(SolidTides, UntruncatedCorrectionsExtendBeyondTheFileAndRemainRegularAtBodyPoles) {
    auto info = field(false, 2, 0);
    GravityModel low(info, {});
    GravityModel high(field(), {});
    std::array bodies{body(true), body(false)};
    bodies[1].position_itrf_m = {0, 0, 384400000};
    const auto polar = solid_tide_corrections(info, {SolidTideMode::full, false}, 2, 0, bodies);
    ASSERT_FALSE(polar.empty());
    EXPECT_TRUE(std::any_of(polar.begin(), polar.end(), [](auto v) { return v.degree == 4; }));
    for (const auto &r : {std::array<double, 3>{7e6, 0, 0}, std::array<double, 3>{0, 0, 7e6}}) {
        const auto a = low.acceleration(r, 0, 0, polar);
        const auto b = high.acceleration(r, 0, 0, polar);
        for (int i = 0; i < 3; ++i)
            EXPECT_EQ(a[i], b[i]);
    }
    bodies[1].position_itrf_m[0] = 1e-6;
    const auto near = solid_tide_corrections(info, {SolidTideMode::full, false}, 2, 0, bodies);
    ASSERT_EQ(near.size(), polar.size());
    for (std::size_t i = 0; i < polar.size(); ++i) {
        EXPECT_NEAR(polar[i].c, near[i].c, 1e-21);
        EXPECT_NEAR(polar[i].s, near[i].s, 1e-21);
    }
    const auto cut = solid_tide_corrections(info, {SolidTideMode::full, true}, 2, 0, bodies);
    ASSERT_EQ(cut.size(), 1U);
    EXPECT_EQ(cut[0].degree, 2);
    EXPECT_EQ(cut[0].order, 0);
}
TEST(SolidTides, HpopMatchesIndependentMovingBodyPropagationWithoutDirectThirdBodyGravity) {
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.solid_tides = {SolidTideMode::full, false};
    options.max_step_seconds = 30;
    options.relative_tolerance = 1e-13;
    options.position_tolerance_m = 1e-5;
    options.velocity_tolerance_mps = 1e-8;
    const auto ephemeris = std::make_shared<Ephemeris>();
    HpopEnvironment environment{std::make_shared<Frames>(), std::make_shared<Eop>(), {}, ephemeris};
    CartesianState initial{Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    EXPECT_FALSE(options.uses_third_bodies());
    EXPECT_TRUE(options.uses_ephemeris());
    const auto provider = make_hpop(initial, options, environment);
    std::ifstream file(ORBITFORGE_SOLID_TIDE_REFERENCE);
    ASSERT_TRUE(file.is_open());
    int rows = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] != 'T')
            continue;
        std::istringstream in(line);
        char tag;
        long long seconds;
        std::array<double, 3> r{}, v{};
        in >> tag >> seconds;
        for (auto &x : r)
            in >> x;
        for (auto &x : v)
            in >> x;
        ASSERT_TRUE(in);
        const auto actual = provider->state_at(Epoch{seconds * 1000000000LL});
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(actual.position_m[i], r[i], .003) << line;
            EXPECT_NEAR(actual.velocity_mps[i], v[i], 3e-6) << line;
        }
        ++rows;
    }
    EXPECT_EQ(rows, 4);
    EXPECT_GT(ephemeris->epochs.size(), 100U);
    EXPECT_LT(*std::min_element(ephemeris->epochs.begin(), ephemeris->epochs.end()), -6000);
    EXPECT_GT(*std::max_element(ephemeris->epochs.begin(), ephemeris->epochs.end()), 6000);
    EXPECT_THROW((void)provider->state_at(Epoch{91000000000000}), std::out_of_range);
    EXPECT_EQ(provider->state_at(Epoch{6000000000000}).position_m,
              make_hpop(initial, options, environment)->state_at(Epoch{6000000000000}).position_m);
    EXPECT_THROW((void)make_hpop(initial, options, {}), std::invalid_argument);
    auto missing = environment;
    missing.eop.reset();
    EXPECT_THROW((void)make_hpop(initial, options, missing), std::invalid_argument);
    options.solid_tides.truncate_to_gravity = true;
    EXPECT_FALSE(options.uses_ephemeris());
    EXPECT_FALSE(options.uses_eop_force());
    EXPECT_NO_THROW((void)make_hpop(initial, options, {}));
}
TEST(SolidTides, InvalidModelsBodiesAndAdditiveCoefficientsFailExplicitly) {
    auto info = field();
    auto bad = SolidTideOptions{};
    bad.mode = static_cast<SolidTideMode>(99);
    EXPECT_THROW((void)solid_tide_corrections(info, bad, 2, 2), std::invalid_argument);
    EXPECT_THROW((void)solid_tide_corrections(info, {SolidTideMode::full}, 2, 2),
                 std::invalid_argument);
    info.tide_system = "mean_tide";
    EXPECT_THROW((void)solid_tide_corrections(info, {}, 2, 2), std::invalid_argument);
    info = field();
    std::array bodies{body(true), body(false)};
    bodies[0].mu = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)solid_tide_corrections(info, {SolidTideMode::full}, 2, 2, bodies),
                 std::invalid_argument);
    bodies[0] = body(true);
    bodies[1].position_itrf_m = {0, 0, 0};
    EXPECT_THROW((void)solid_tide_corrections(info, {SolidTideMode::full}, 2, 2, bodies),
                 std::invalid_argument);
    GravityModel model(info, {});
    for (const auto &values : std::vector<std::vector<GravityCoefficient>>{
             {{1, 0, 0, 0}},
             {{gravity_max_degree + 1, 0, 0, 0}},
             {{2, 3, 0, 0}},
             {{2, 0, 0, 1}},
             {{2, 0, 0, 0}, {2, 0, 1, 0}},
             {{2, 0, std::numeric_limits<double>::infinity(), 0}}})
        EXPECT_THROW((void)model.acceleration({7e6, 0, 0}, 2, 2, values), std::invalid_argument);
}

TEST(SolidTides, TimeDependentCoefficientsMatchIndependentIers2003AcrossDatesAndPoleHistory) {
    std::ifstream file(ORBITFORGE_TIME_TIDE_REFERENCE);
    ASSERT_TRUE(file.is_open());
    const Time time;
    const auto info = field();
    const std::array bodies{body(true), body(false)};
    const auto primary =
        solid_tide_corrections(info, {SolidTideMode::full, false, false}, 0, 0, bodies);
    double maximum = 0;
    int count = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] != 'C')
            continue;
        std::istringstream in(line);
        char tag;
        std::int64_t ns;
        double ut1, x, y;
        std::array<double, 7> expected{};
        in >> tag >> ns >> ut1 >> x >> y;
        for (auto &v : expected)
            in >> v;
        ASSERT_TRUE(in);
        const EarthOrientationSample eop{Epoch{ns}, ut1, x, y, 0, 0, "test"};
        const auto full = solid_tide_corrections(info, {SolidTideMode::full, false, true}, 0, 0,
                                                 bodies, &eop, &time);
        ASSERT_EQ(full.size(), primary.size());
        const std::array values{full[0].c - primary[0].c, full[1].c - primary[1].c,
                                full[1].s - primary[1].s, full[2].c - primary[2].c,
                                full[2].s - primary[2].s};
        expected[1] += expected[5];
        expected[2] += expected[6];
        for (std::size_t i = 0; i < values.size(); ++i) {
            maximum = std::max(maximum, std::abs(values[i] - expected[i]));
            EXPECT_NEAR(values[i], expected[i], 2e-20) << line << " component " << i;
        }
        for (std::size_t i = 3; i < full.size(); ++i) {
            EXPECT_EQ(full[i].c, primary[i].c);
            EXPECT_EQ(full[i].s, primary[i].s);
        }
        ++count;
    }
    EXPECT_EQ(count, 39);
    std::cout << "Time-dependent tide maximum coefficient error: " << maximum << '\n';
}
TEST(SolidTides, TimeDependentAccelerationMatchesIndependentOrekitIncludingZonalTruncation) {
    std::ifstream file(ORBITFORGE_TIME_TIDE_REFERENCE);
    ASSERT_TRUE(file.is_open());
    const auto info = field();
    const Time time;
    const auto eop = Eop{}.at(Epoch{0}).sample;
    GravityModel model(info, {{2, 0, -4.84165371736e-4, 0}});
    const std::array bodies{body(true), body(false)};
    int count = 0;
    double maximum = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] != 'A')
            continue;
        std::istringstream in(line);
        char tag;
        int degree, order, truncate;
        std::array<double, 3> r{}, expected{};
        in >> tag >> degree >> order >> truncate;
        for (auto &v : r)
            in >> v;
        for (auto &v : expected)
            in >> v;
        ASSERT_TRUE(in);
        const auto delta = solid_tide_corrections(info, {SolidTideMode::full, truncate != 0, true},
                                                  degree, order, bodies, &eop, &time);
        const auto actual = model.acceleration(r, degree, order, delta);
        for (int i = 0; i < 3; ++i) {
            maximum = std::max(maximum, std::abs(actual[i] - expected[i]));
            EXPECT_NEAR(actual[i], expected[i], 2e-14) << line;
        }
        ++count;
    }
    EXPECT_EQ(count, 24);
    std::cout << "Time-dependent tide maximum acceleration error (m/s^2): " << maximum << '\n';
}
TEST(SolidTides, TimeDependentHpopMatchesIndependentForwardAndBackwardPropagation) {
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.solid_tides = {SolidTideMode::full, false, true};
    options.max_step_seconds = 30;
    options.relative_tolerance = 1e-13;
    options.position_tolerance_m = 1e-5;
    options.velocity_tolerance_mps = 1e-8;
    HpopEnvironment environment{std::make_shared<Frames>(),
                                std::make_shared<Eop>(),
                                {},
                                std::make_shared<Ephemeris>(),
                                std::make_shared<Time>()};
    const CartesianState initial{
        Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    const auto provider = make_hpop(initial, options, environment);
    std::ifstream file(ORBITFORGE_TIME_TIDE_REFERENCE);
    ASSERT_TRUE(file.is_open());
    int count = 0;
    double dr = 0, dv = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] != 'T')
            continue;
        std::istringstream in(line);
        char tag;
        std::int64_t seconds;
        std::array<double, 3> r{}, v{};
        in >> tag >> seconds;
        for (auto &x : r)
            in >> x;
        for (auto &x : v)
            in >> x;
        ASSERT_TRUE(in);
        const auto actual = provider->state_at(Epoch{seconds * 1000000000LL});
        for (int i = 0; i < 3; ++i) {
            dr = std::max(dr, std::abs(actual.position_m[i] - r[i]));
            dv = std::max(dv, std::abs(actual.velocity_mps[i] - v[i]));
            EXPECT_NEAR(actual.position_m[i], r[i], .003) << line;
            EXPECT_NEAR(actual.velocity_mps[i], v[i], 3e-6) << line;
        }
        ++count;
    }
    EXPECT_EQ(count, 4);
    std::cout << "Time-dependent tide maximum propagated component error: " << dr << " m; " << dv
              << " m/s\n";
    auto bad = environment;
    bad.time.reset();
    EXPECT_THROW((void)make_hpop(initial, options, bad), std::invalid_argument);
    bad.time = std::make_shared<Time>("different");
    EXPECT_THROW((void)make_hpop(initial, options, bad), std::invalid_argument);
    EXPECT_THROW((void)provider->state_at(Epoch{91000000000000LL}), std::out_of_range);
    EXPECT_EQ(
        provider->state_at(Epoch{6000000000000LL}).position_m,
        make_hpop(initial, options, environment)->state_at(Epoch{6000000000000LL}).position_m);
    options.solid_tides.truncate_to_gravity = true;
    EXPECT_FALSE(options.uses_utc_force());
    EXPECT_NO_THROW((void)make_hpop(initial, options, {}));
}
TEST(SolidTides, TimeDependentInputsAndInactiveDraftsAreExplicit) {
    const auto info = field();
    const Time time;
    const std::array bodies{body(true), body(false)};
    auto eop = Eop{}.at(Epoch{0}).sample;
    const SolidTideOptions options{SolidTideMode::full, false, true};
    EXPECT_THROW((void)solid_tide_corrections(info, options, 0, 0, bodies), std::invalid_argument);
    eop.ut1_minus_tai_seconds = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)solid_tide_corrections(info, options, 0, 0, bodies, &eop, &time),
                 std::invalid_argument);
    eop = Eop{}.at(time.from_utc({1899, 12, 31, 0, 0, 0, 0})).sample;
    EXPECT_THROW((void)solid_tide_corrections(info, options, 0, 0, bodies, &eop, &time),
                 std::out_of_range);
    for (auto mode : {SolidTideMode::none, SolidTideMode::permanent}) {
        const auto effective = effective_solid_tide_options({mode, false, true}, 2);
        EXPECT_FALSE(effective.include_time_dependent_terms);
        EXPECT_NO_THROW((void)solid_tide_corrections(info, {mode, false, true}, 2, 2));
    }
    EXPECT_EQ(effective_solid_tide_options({SolidTideMode::full, true, true}, 0),
              SolidTideOptions{});
}

TEST(SolidTides, CustomLoveNumbersMatchIndependentOrekitCoefficientsForBothTideSystems) {
    std::ifstream file(ORBITFORGE_LOVE_TIDE_REFERENCE);
    ASSERT_TRUE(file.is_open());
    const Time time;
    const Eop eop;
    double maximum = 0;
    int rows = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] != 'C')
            continue;
        std::istringstream in(line);
        char tag;
        double k20;
        int zero, secondary;
        long long seconds;
        in >> tag >> k20 >> zero >> secondary >> seconds;
        auto info = field(zero != 0);
        info.solid_tide_k20 = k20;
        const auto sample = eop.at(Epoch{seconds * 1000000000LL}).sample;
        const std::array bodies{body(true, static_cast<double>(seconds)),
                                body(false, static_cast<double>(seconds))};
        const auto actual = solid_tide_corrections(
            info, {SolidTideMode::full, false, secondary != 0}, 4, 3, bodies, &sample, &time);
        for (int n = 2; n <= 4; ++n)
            for (int m = 0; m <= (n == 4 ? 2 : n); ++m) {
                double c, s;
                in >> c >> s;
                ASSERT_TRUE(in);
                const auto found = std::ranges::find_if(
                    actual, [=](const auto &v) { return v.degree == n && v.order == m; });
                const double ac = found == actual.end() ? 0 : found->c;
                const double as = found == actual.end() ? 0 : found->s;
                maximum = std::max({maximum, std::abs(ac - c), std::abs(as - s)});
                EXPECT_NEAR(ac, c, 2e-20) << line;
                EXPECT_NEAR(as, s, 2e-20) << line;
            }
        ++rows;
    }
    EXPECT_EQ(rows, 48);
    std::cout << "Custom k20 maximum coefficient difference: " << maximum << '\n';
}
TEST(SolidTides, CustomLoveNumbersMatchIndependentOrekitAccelerationAndPropagation) {
    std::ifstream file(ORBITFORGE_LOVE_TIDE_REFERENCE);
    ASSERT_TRUE(file.is_open());
    const auto time = std::make_shared<Time>();
    const auto eop = std::make_shared<Eop>();
    const auto frames = std::make_shared<Frames>();
    double max_a = 0, max_r = 0, max_v = 0;
    int accelerations = 0, trajectories = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || (line[0] != 'A' && line[0] != 'T'))
            continue;
        std::istringstream in(line);
        char tag;
        double k20;
        in >> tag >> k20;
        auto info = field();
        info.solid_tide_k20 = k20;
        if (tag == 'A') {
            int zero, secondary, degree, order, truncate;
            std::array<double, 3> r{}, expected{};
            in >> zero >> secondary >> degree >> order >> truncate;
            for (auto &x : r)
                in >> x;
            for (auto &x : expected)
                in >> x;
            ASSERT_TRUE(in);
            info.tide_system = zero ? "zero_tide" : "tide_free";
            const GravityModel model(
                info,
                {{2, 0, -4.84165371736e-4 + (zero ? iers2003_permanent_tide_c20(k20) : 0), 0}});
            const std::array bodies{body(true), body(false)};
            const auto sample = eop->at(Epoch{0}).sample;
            const auto correction =
                solid_tide_corrections(info, {SolidTideMode::full, truncate != 0, secondary != 0},
                                       degree, order, bodies, &sample, time.get());
            const auto actual = model.acceleration(r, degree, order, correction);
            for (int i = 0; i < 3; ++i) {
                max_a = std::max(max_a, std::abs(actual[i] - expected[i]));
                EXPECT_NEAR(actual[i], expected[i], 2e-14) << line;
            }
            ++accelerations;
        } else {
            int secondary;
            long long seconds;
            std::array<double, 3> r{}, v{};
            in >> secondary >> seconds;
            for (auto &x : r)
                in >> x;
            for (auto &x : v)
                in >> x;
            ASSERT_TRUE(in);
            HpopOptions options;
            options.gravity_model = "custom";
            options.gravity_degree = options.gravity_order = 0;
            options.solid_tides = {SolidTideMode::full, false, secondary != 0};
            options.max_step_seconds = 30;
            options.relative_tolerance = 1e-13;
            options.position_tolerance_m = 1e-5;
            options.velocity_tolerance_mps = 1e-8;
            const auto model =
                std::make_shared<GravityModel>(info, std::vector<GravityCoefficient>{});
            HpopEnvironment environment{frames, eop, model, std::make_shared<Ephemeris>(), time};
            const CartesianState initial{
                Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
            const auto actual =
                make_hpop(initial, options, environment)->state_at(Epoch{seconds * 1000000000LL});
            for (int i = 0; i < 3; ++i) {
                max_r = std::max(max_r, std::abs(actual.position_m[i] - r[i]));
                max_v = std::max(max_v, std::abs(actual.velocity_mps[i] - v[i]));
                EXPECT_NEAR(actual.position_m[i], r[i], .003) << line;
                EXPECT_NEAR(actual.velocity_mps[i], v[i], 3e-6) << line;
            }
            ++trajectories;
        }
    }
    EXPECT_EQ(accelerations, 96);
    EXPECT_EQ(trajectories, 16);
    std::cout << "Custom k20 max acceleration/position/velocity differences: " << max_a << ' '
              << max_r << ' ' << max_v << '\n';
}
TEST(SolidTides, CustomLoveNumberControlsPermanentDeformationExactlyOnceAndRejectsNonfinite) {
    for (double k20 : {0., .3, .30190, .4, -.1}) {
        auto info = field();
        info.solid_tide_k20 = k20;
        const double expected = 4.4228e-8 * -0.31460 * k20;
        EXPECT_EQ(iers2003_permanent_tide_c20(k20), expected);
        const auto tide = solid_tide_corrections(info, {SolidTideMode::permanent}, 2, 0);
        if (k20 == 0) {
            EXPECT_TRUE(tide.empty());
        } else {
            ASSERT_EQ(tide.size(), 1U);
            EXPECT_EQ(tide.front().c, expected);
        }
        info.tide_system = "zero_tide";
        EXPECT_TRUE(solid_tide_corrections(info, {SolidTideMode::permanent}, 2, 0).empty());
        const auto removed = solid_tide_corrections(info, {}, 2, 0);
        if (k20 == 0) {
            EXPECT_TRUE(removed.empty());
        } else {
            ASSERT_EQ(removed.size(), 1U);
            EXPECT_EQ(removed.front().c, -expected);
        }
    }
    for (double k20 :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity()}) {
        auto info = field();
        info.solid_tide_k20 = k20;
        EXPECT_THROW((void)iers2003_permanent_tide_c20(k20), std::invalid_argument);
        EXPECT_THROW((void)solid_tide_corrections(info, {}, 0, 0), std::invalid_argument);
        EXPECT_THROW((void)GravityModel(info, {}), std::invalid_argument);
    }
}
