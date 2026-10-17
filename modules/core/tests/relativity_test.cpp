#include <algorithm>
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
constexpr double sun_mu = 1.3271244004127942e20;
GeometricBodyState sun(double t) {
    const double radius = 1.496e11, rate = 2 * std::numbers::pi / (365.25 * 86400),
                 a = .3 + rate * t;
    return {{radius * std::cos(a), radius * std::sin(a) * std::cos(.409),
             radius * std::sin(a) * std::sin(.409)},
            {-radius * rate * std::sin(a), radius * rate * std::cos(a) * std::cos(.409),
             radius * rate * std::cos(a) * std::sin(.409)}};
}
class Frames final : public IFrameProvider {
  public:
    PositionRotation gcrf_to_itrf(Epoch epoch, const EarthOrientationSample &) const override {
        return {epoch,
                OriginId::earth,
                FrameId::gcrf,
                FrameId::itrf,
                {{{1, 0, 0}, {0, std::cos(.2), std::sin(.2)}, {0, -std::sin(.2), std::cos(.2)}}},
                "test-tilted",
                "test"};
    }
    StateTransform gcrf_to_itrf_state(Epoch e, const EarthOrientationState &o) const override {
        return {gcrf_to_itrf(e, o.sample), {}, "test-static"};
    }
};
class Eop final : public IEarthOrientationProvider {
    EopDataInfo info_{"test",
                      "",
                      "",
                      "",
                      "",
                      Epoch{-90000000000000LL},
                      Epoch{90000000000000LL},
                      {"", "", "", 0, 100000}};

  public:
    EarthOrientationState at(Epoch e) const override {
        return {{e, 0, 0, 0, 0, 0, "test"}, {0, 0, 0, 0, 0}};
    }
    const EopDataInfo &data_info() const noexcept override { return info_; }
};
class Ephemeris : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{"test", "", "test", Epoch{-91000000000000LL}, Epoch{91000000000000LL},
                            sun_mu, 0};

  public:
    bool state_supported = true;
    std::array<double, 3> position(ThirdBody b, Epoch e) const override {
        EXPECT_EQ(b, ThirdBody::sun);
        return sun(e.seconds_since(Epoch{0})).position_m;
    }
    GeometricBodyState geometric_state(ThirdBody b, Epoch e) const override {
        if (!state_supported)
            return IThirdBodyEphemeris::geometric_state(b, e);
        EXPECT_EQ(b, ThirdBody::sun);
        return sun(e.seconds_since(Epoch{0}));
    }
    void validate_interval(Epoch first, Epoch last,
                           std::span<const ThirdBody> bodies) const override {
        EXPECT_EQ(bodies.size(), 1U);
        EXPECT_EQ(bodies[0], ThirdBody::sun);
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw std::out_of_range("Test ephemeris coverage");
    }
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
};
HpopOptions options(bool enabled = true) {
    HpopOptions o;
    o.gravity_degree = o.gravity_order = 0;
    o.include_relativistic_accelerations = enabled;
    o.max_step_seconds = 30;
    o.relative_tolerance = 1e-13;
    o.position_tolerance_m = 1e-5;
    o.velocity_tolerance_mps = 1e-8;
    return o;
}
const CartesianState initial{
    Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
HpopEnvironment environment() {
    return {std::make_shared<Frames>(), std::make_shared<Eop>(), {}, std::make_shared<Ephemeris>()};
}
} // namespace
TEST(Relativity, EachTermMatchesIndependentOrekitAtInclinedPolarAndGeostationaryStates) {
    std::ifstream file(ORBITFORGE_RELATIVITY_REFERENCE);
    ASSERT_TRUE(file.is_open());
    int count = 0;
    std::array<double, 4> maximum{};
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] != 'A')
            continue;
        std::istringstream in(line);
        char tag;
        in >> tag;
        std::array<std::array<double, 3>, 9> values{};
        for (auto &v : values)
            for (auto &x : v)
                in >> x;
        ASSERT_TRUE(in);
        const auto actual = evaluate_relativity(values[0], values[1], egm96_mu, values[2],
                                                values[3], sun_mu, values[4]);
        const std::array terms{actual.schwarzschild_mps2, actual.lense_thirring_mps2,
                               actual.de_sitter_mps2, actual.acceleration_mps2};
        for (std::size_t term = 0; term < 4; ++term)
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const double expected = values[term + 5][axis];
                maximum[term] = std::max(maximum[term], std::abs(terms[term][axis] - expected));
                // Double roundoff at ~1e-8 (Schwarzschild) and ~1e-10 m/s^2 (other terms).
                EXPECT_NEAR(terms[term][axis], expected, term == 0 || term == 3 ? 2e-22 : 2e-24)
                    << line;
            }
        ++count;
    }
    EXPECT_EQ(count, 45);
    std::cout << "Relativity maximum component errors (S, LT, DS, total) m/s^2:";
    for (auto v : maximum)
        std::cout << ' ' << v;
    std::cout << '\n';
}
TEST(Relativity, ForwardBackwardPropagationMatchesOrekitAndIsIndependentOfThirdBodySwitch) {
    const auto on = make_hpop(initial, options(), environment()),
               off = make_hpop(initial, options(false));
    std::ifstream file(ORBITFORGE_RELATIVITY_REFERENCE);
    ASSERT_TRUE(file.is_open());
    int count = 0;
    double dr = 0, dv = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] != 'T')
            continue;
        std::istringstream in(line);
        char tag;
        int enabled;
        std::int64_t elapsed;
        std::array<double, 6> expected{};
        in >> tag >> enabled >> elapsed;
        for (auto &x : expected)
            in >> x;
        ASSERT_TRUE(in);
        const auto state = (enabled ? on : off)->state_at(Epoch{elapsed * 1000000000LL});
        for (std::size_t i = 0; i < 3; ++i) {
            dr = std::max(dr, std::abs(state.position_m[i] - expected[i]));
            dv = std::max(dv, std::abs(state.velocity_mps[i] - expected[i + 3]));
            EXPECT_NEAR(state.position_m[i], expected[i], 1e-4) << line;
            EXPECT_NEAR(state.velocity_mps[i], expected[i + 3], 1e-7) << line;
        }
        ++count;
    }
    EXPECT_EQ(count, 8);
    std::cout << "Relativity trajectory maximum component errors: " << dr << " m; " << dv
              << " m/s\n";
    const Epoch at{6000000000000LL};
    EXPECT_EQ(on->state_at(at).position_m,
              make_hpop(initial, options(), environment())->state_at(at).position_m);
    EXPECT_NE(on->state_at(at).position_m, off->state_at(at).position_m);
    auto different = options();
    different.third_body(ThirdBody::sun).source = ThirdBodyGravitySource::user_specified;
    different.third_body(ThirdBody::sun).user_mu =
        1e18; // Disabled direct force cannot alter de Sitter GM.
    EXPECT_EQ(on->state_at(at).position_m,
              make_hpop(initial, different, environment())->state_at(at).position_m);
}
TEST(Relativity, ExplicitProvidersCoverageAndNoImplicitVelocityFallback) {
    const auto o = options();
    EXPECT_TRUE(o.uses_eop_force());
    EXPECT_TRUE(o.uses_ephemeris());
    EXPECT_FALSE(o.uses_utc_force());
    EXPECT_FALSE(o.uses_third_bodies());
    EXPECT_EQ(o.required_ephemeris_bodies(), std::vector{ThirdBody::sun});
    auto e = environment();
    for (int missing = 0; missing < 3; ++missing) {
        auto invalid = e;
        if (missing == 0)
            invalid.eop.reset();
        if (missing == 1)
            invalid.frames.reset();
        if (missing == 2)
            invalid.ephemeris.reset();
        EXPECT_THROW((void)make_hpop(initial, o, invalid), std::invalid_argument);
    }
    const auto prop = make_hpop(initial, o, e);
    EXPECT_THROW((void)prop->state_at(Epoch{90000000000001LL}), EopCoverageError);
    EXPECT_THROW((void)prop->state_at(Epoch{-91000000000001LL}), std::out_of_range);
    auto unsupported = std::make_shared<Ephemeris>();
    unsupported->state_supported = false;
    e.ephemeris = unsupported;
    EXPECT_THROW((void)make_hpop(initial, o, e), std::invalid_argument);
    EXPECT_NO_THROW((void)make_hpop(initial, options(false), {}));
}
TEST(Relativity, InvalidInputsAndZeroVelocityHaveExplicitSemantics) {
    const auto solar = sun(0);
    const std::array<double, 3> pole{0, 0, 1}, zero{};
    const auto at_rest = evaluate_relativity(initial.position_m, zero, egm96_mu, solar.position_m,
                                             solar.velocity_mps, sun_mu, pole);
    EXPECT_EQ(at_rest.lense_thirring_mps2, zero);
    EXPECT_EQ(at_rest.de_sitter_mps2, zero);
    EXPECT_GT(at_rest.schwarzschild_mps2[0], 0);
    for (int input = 0; input < 7; ++input) {
        auto r = initial.position_m, v = initial.velocity_mps, s = solar.position_m,
             sv = solar.velocity_mps, k = pole;
        double mu = egm96_mu, smu = sun_mu;
        if (input == 0)
            r = {};
        if (input == 1)
            s = {};
        if (input == 2)
            v = {299792458, 0, 0};
        if (input == 3)
            sv[0] = std::numeric_limits<double>::quiet_NaN();
        if (input == 4)
            k = {0, 0, 2};
        if (input == 5)
            mu = -1;
        if (input == 6)
            smu = std::numeric_limits<double>::infinity();
        EXPECT_THROW((void)evaluate_relativity(r, v, mu, s, sv, smu, k), std::invalid_argument);
    }
}
