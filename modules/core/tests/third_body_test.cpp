#include <algorithm>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <numbers>
#include <orbitforge/core/propagation/hpop.hpp>
#include <sstream>

using namespace orbitforge::core;
namespace {
class MovingBodies final : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{"synthetic",
                            std::string(64, 'a'),
                            "test-v1",
                            Epoch{-172800000000000},
                            Epoch{172800000000000},
                            1.3271244004127942e20,
                            4.9028001184575496e12};

  public:
    mutable std::size_t sun_calls = 0, moon_calls = 0;
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
    void validate_interval(Epoch first, Epoch last, std::span<const ThirdBody>) const override {
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw std::out_of_range("Synthetic coverage exceeded");
    }
    std::array<double, 3> position(ThirdBody body, Epoch epoch) const override {
        const bool sun = body == ThirdBody::sun;
        ++(sun ? sun_calls : moon_calls);
        const double radius = sun ? 1.496e11 : 384400000;
        const double rate = 2 * std::numbers::pi / ((sun ? 365.25 : 27.321661) * 86400);
        const double tilt = sun ? .409 : .5;
        const double angle = (sun ? .3 : 1.2) + rate * epoch.seconds_since(Epoch{0});
        return {radius * std::cos(angle), radius * std::sin(angle) * std::cos(tilt),
                radius * std::sin(angle) * std::sin(tilt)};
    }
};
} // namespace
TEST(ThirdBody, StableTidalLimitSymmetryAndInvalidInputs) {
    constexpr double mu = 1.3271244004127942e20, distance = 1.496e11;
    // At one millimetre from the origin the direct subtraction loses useful digits.
    const auto a = third_body_acceleration({.001, 0, 0}, {distance, 0, 0}, mu);
    EXPECT_NEAR(a[0], 2 * mu * .001 / std::pow(distance, 3), 1e-30);
    EXPECT_EQ(a[1], 0);
    EXPECT_EQ(third_body_acceleration({0, 0, 0}, {distance, 0, 0}, mu), (std::array{0., 0., 0.}));
    const auto normal = third_body_acceleration({7e6, 2e6, -3e6}, {3e8, -1e8, 2e8}, 4.9e12);
    const auto mirrored = third_body_acceleration({-7e6, -2e6, 3e6}, {-3e8, 1e8, -2e8}, 4.9e12);
    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(normal[i], -mirrored[i]);
    EXPECT_THROW((void)third_body_acceleration({1, 0, 0}, {0, 0, 0}, mu), std::invalid_argument);
    EXPECT_THROW((void)third_body_acceleration({1, 0, 0}, {1, 0, 0}, mu), std::invalid_argument);
    EXPECT_THROW((void)third_body_acceleration({1, 0, 0}, {2, 0, 0}, -1), std::invalid_argument);
    EXPECT_THROW((void)third_body_acceleration({NAN, 0, 0}, {2, 0, 0}, mu), std::invalid_argument);
}
TEST(ThirdBody, MovingSunAndMoonMatchIndependentOrekitIntegration) {
    std::ifstream source(ORBITFORGE_THIRD_BODY_REFERENCE);
    ASSERT_TRUE(source);
    std::string line;
    std::size_t cases = 0;
    double maximum_position = 0, maximum_velocity = 0, maximum_refinement = 0;
    while (std::getline(source, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row(line);
        int mode = 0;
        double seconds = 0;
        CartesianState initial{Epoch{0}, OriginId::earth, FrameId::gcrf, {}, {}},
            expected = initial;
        std::array<double, 3> acceleration{};
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
        HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.third_body(orbitforge::core::ThirdBody::sun).enabled = mode != 1;
        options.third_body(orbitforge::core::ThirdBody::moon).enabled = mode != 0;
        // Isolate the force comparison from the seventh-order long-arc truncation error.
        options.max_step_seconds = 30;
        options.relative_tolerance = 1e-13;
        options.position_tolerance_m = 1e-5;
        options.velocity_tolerance_mps = 1e-8;
        auto bodies = std::make_shared<MovingBodies>();
        const auto &info = bodies->data_info();
        if (mode == 3) {
            options.third_body(ThirdBody::sun).source = ThirdBodyGravitySource::user_specified;
            options.third_body(ThirdBody::moon).source = ThirdBodyGravitySource::user_specified;
            options.third_body(ThirdBody::sun).user_mu = info.sun_mu * 1.01;
            options.third_body(ThirdBody::moon).user_mu = info.moon_mu * .99;
        }
        std::array<double, 3> actual_force{};
        for (auto body : {ThirdBody::sun, ThirdBody::moon}) {
            if (body == ThirdBody::sun
                    ? !options.third_body(orbitforge::core::ThirdBody::sun).enabled
                    : !options.third_body(orbitforge::core::ThirdBody::moon).enabled)
                continue;
            const auto mu =
                body == ThirdBody::sun
                    ? options.third_body(ThirdBody::sun).user_mu.value_or(info.sun_mu)
                    : options.third_body(ThirdBody::moon).user_mu.value_or(info.moon_mu);
            const auto force = third_body_acceleration(initial.position_m,
                                                       bodies->position(body, initial.epoch), mu);
            for (int i = 0; i < 3; ++i)
                actual_force[i] += force[i];
        }
        for (int i = 0; i < 3; ++i)
            EXPECT_NEAR(actual_force[i], acceleration[i], 3e-18);
        const auto provider = make_hpop(initial, options, {nullptr, nullptr, nullptr, bodies});
        expected.epoch = Epoch{static_cast<std::int64_t>(seconds * 1e9)};
        const auto result = provider->state_at(expected.epoch);
        options.max_step_seconds = 15;
        const auto refined = make_hpop(initial, options, {nullptr, nullptr, nullptr, bodies})
                                 ->state_at(expected.epoch);
        SCOPED_TRACE(line);
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(result.position_m[i], expected.position_m[i], .002);
            EXPECT_NEAR(result.velocity_mps[i], expected.velocity_mps[i], 2e-6);
            EXPECT_NEAR(refined.position_m[i], expected.position_m[i], .002);
            EXPECT_NEAR(refined.velocity_mps[i], expected.velocity_mps[i], 2e-6);
            EXPECT_NEAR(result.position_m[i], refined.position_m[i], 1e-4);
            maximum_position =
                std::max(maximum_position,
                         std::max(std::abs(result.position_m[i] - expected.position_m[i]),
                                  std::abs(refined.position_m[i] - expected.position_m[i])));
            maximum_velocity =
                std::max(maximum_velocity,
                         std::max(std::abs(result.velocity_mps[i] - expected.velocity_mps[i]),
                                  std::abs(refined.velocity_mps[i] - expected.velocity_mps[i])));
            maximum_refinement = std::max(maximum_refinement,
                                          std::abs(result.position_m[i] - refined.position_m[i]));
        }
        EXPECT_EQ(provider->state_at(expected.epoch).position_m, result.position_m);
        EXPECT_EQ(bodies->sun_calls > 1,
                  options.third_body(orbitforge::core::ThirdBody::sun).enabled);
        EXPECT_EQ(bodies->moon_calls > 1,
                  options.third_body(orbitforge::core::ThirdBody::moon).enabled);
        ++cases;
    }
    EXPECT_EQ(cases, 32U);
    RecordProperty("maximum_position_error_m", maximum_position);
    RecordProperty("maximum_velocity_error_mps", maximum_velocity);
    RecordProperty("maximum_step_refinement_m", maximum_refinement);
}
TEST(ThirdBody, ResourceCoverageAndCancellationAreEnforcedBeforePublishingResults) {
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.third_body(orbitforge::core::ThirdBody::sun).enabled = true;
    const CartesianState initial{
        Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    EXPECT_THROW((void)make_hpop(initial, options), std::invalid_argument);
    auto bodies = std::make_shared<MovingBodies>();
    const auto provider = make_hpop(initial, options, {nullptr, nullptr, nullptr, bodies});
    const auto calls = bodies->sun_calls;
    EXPECT_THROW((void)provider->state_at(Epoch{172800000000001}), std::out_of_range);
    EXPECT_EQ(calls, bodies->sun_calls);
    int checks = 0;
    EXPECT_THROW((void)provider->state_at(Epoch{600000000000},
                                          [&] {
                                              if (++checks == 5)
                                                  throw std::runtime_error("cancel");
                                          }),
                 std::runtime_error);
    EXPECT_EQ(provider->state_at(Epoch{600000000000}).position_m,
              make_hpop(initial, options, {nullptr, nullptr, nullptr, bodies})
                  ->state_at(Epoch{600000000000})
                  .position_m);
    options.third_body(ThirdBody::moon).user_mu = 0;
    EXPECT_THROW((void)validate_hpop_options(options), std::invalid_argument);
}
namespace {
class PlanetBodies final : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{"synthetic",
                            std::string(64, 'b'),
                            "planets-test",
                            Epoch{-1000000000},
                            Epoch{1000000000},
                            1e15,
                            2e15,
                            {3e15, 4e15, 5e15, 6e15, 7e15, 8e15, 9e15, 1e16}};

  public:
    mutable std::array<unsigned, third_body_count> calls{};
    mutable std::vector<ThirdBody> validated;
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
    void validate_interval(Epoch, Epoch, std::span<const ThirdBody> bodies) const override {
        validated.assign(bodies.begin(), bodies.end());
    }
    std::array<double, 3> position(ThirdBody body, Epoch) const override {
        ++calls.at(static_cast<std::size_t>(body));
        return {1e10 * (1 + static_cast<int>(body)), 2e9, -3e8};
    }
};
} // namespace
TEST(ThirdBody, EveryBodyAndSourceActuallyChangesTheIntegratedForce) {
    const CartesianState initial{
        Epoch{0}, OriginId::earth, FrameId::gcrf, {42164000, 1200000, -3400000}, {0, 0, 0}};
    for (auto body : third_body_list) {
        for (auto source : {ThirdBodyGravitySource::jpl_de, ThirdBodyGravitySource::cb_file,
                            ThirdBodyGravitySource::user_specified}) {
            SCOPED_TRACE(std::string(third_body_name(body)) + ":" + std::to_string(int(source)));
            auto ephemeris = std::make_shared<PlanetBodies>();
            HpopOptions options;
            options.gravity_degree = options.gravity_order = 0;
            options.integrator = HpopIntegrator::rk4;
            options.fixed_step_seconds = .0001;
            auto &settings = options.third_body(body);
            settings.enabled = true;
            settings.source = source;
            settings.user_mu = ephemeris->data_info().mu(body) * 1.2;
            settings.cb =
                CbGravity{ephemeris->data_info().mu(body) * 1.3,
                          std::string(third_body_name(body)) + ".cb", std::string(64, 'a'), body};
            const auto rb = ephemeris->position(body, Epoch{0});
            // Independent direct equation in extended precision; also checks the indirect term.
            long double distance2 = 0, separation2 = 0, radius2 = 0;
            for (int axis = 0; axis < 3; ++axis) {
                distance2 += (long double)rb[axis] * rb[axis];
                const auto delta = (long double)rb[axis] - initial.position_m[axis];
                separation2 += delta * delta;
                radius2 += (long double)initial.position_m[axis] * initial.position_m[axis];
            }
            const auto mu = source == ThirdBodyGravitySource::jpl_de
                                ? ephemeris->data_info().mu(body)
                            : source == ThirdBodyGravitySource::cb_file ? settings.cb->mu
                                                                        : *settings.user_mu;
            const auto provider =
                make_hpop(initial, options, {nullptr, nullptr, nullptr, ephemeris});
            const auto state = provider->state_at(Epoch{100000});
            for (int axis = 0; axis < 3; ++axis) {
                const auto expected =
                    mu * (((long double)rb[axis] - initial.position_m[axis]) /
                              (separation2 * std::sqrt(separation2)) -
                          rb[axis] / (distance2 * std::sqrt(distance2))) -
                    egm96_mu * initial.position_m[axis] / (radius2 * std::sqrt(radius2));
                EXPECT_NEAR(state.velocity_mps[axis] / .0001, double(expected), 2e-14);
            }
            EXPECT_EQ(ephemeris->validated, std::vector{body});
            for (auto other : third_body_list)
                EXPECT_EQ(ephemeris->calls[static_cast<std::size_t>(other)] > 1, other == body);
        }
    }
}
TEST(ThirdBody, MissingMismatchedOrInvalidSourcesAreRejected) {
    HpopOptions options;
    auto &jupiter = options.third_body(ThirdBody::jupiter);
    jupiter.enabled = true;
    jupiter.source = ThirdBodyGravitySource::cb_file;
    EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
    jupiter.cb = CbGravity{1e17, "Sun.cb", std::string(64, 'a')};
    EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
    jupiter.cb.reset();
    jupiter.source = ThirdBodyGravitySource::user_specified;
    EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
    jupiter.user_mu = INFINITY;
    EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
    jupiter.user_mu = 1e17;
    EXPECT_NO_THROW(validate_hpop_options(options));
    jupiter.source = static_cast<ThirdBodyGravitySource>(3);
    EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
}
