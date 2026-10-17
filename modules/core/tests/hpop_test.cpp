#include <atomic>
#include <cmath>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <iomanip>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/hpop.hpp>
#include <sstream>

using namespace orbitforge::core;
namespace {
CartesianState circular() {
    return {
        Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, std::sqrt(egm96_mu / 7e6), 0}};
}
HpopOptions point_mass() {
    HpopOptions o;
    o.gravity_degree = o.gravity_order = 0;
    return o;
}
class FixedFrames final : public IFrameProvider {
  public:
    mutable std::atomic<std::size_t> evaluations{0};
    PositionRotation gcrf_to_itrf(Epoch epoch, const EarthOrientationSample &) const override {
        ++evaluations;
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
class FixedEop final : public IEarthOrientationProvider {
    EopDataInfo info_{"test",
                      "",
                      "",
                      "",
                      "",
                      Epoch{-1000000000000000000},
                      Epoch{1000000000000000000},
                      {"", "", "", 0, 100000}};

  public:
    EarthOrientationState at(Epoch epoch) const override {
        return {{epoch, -32, 0, 0, 0, 0, "test"}, {0, 0, 0, 0, 0}};
    }
    const EopDataInfo &data_info() const noexcept override { return info_; }
};
double position_error(const CartesianState &a, const CartesianState &b) {
    return std::hypot(a.position_m[0] - b.position_m[0], a.position_m[1] - b.position_m[1],
                      a.position_m[2] - b.position_m[2]);
}
} // namespace
TEST(Hpop, SatelliteMassValidatesCompatibleAreasEvenForDisabledForceDrafts) {
    auto options = point_mass();
    EXPECT_EQ(options.satellite_mass_kg, 1000);
    EXPECT_EQ(options.drag_area_m2(), 20);
    options.satellite_mass_kg = 1250;
    options.srp.area_mass_ratio_m2_per_kg = .03;
    EXPECT_EQ(options.drag_area_m2(), 25);
    EXPECT_EQ(options.srp_area_m2(), 37.5);
    for (double mass :
         {0., -1., std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::denorm_min()}) {
        options.satellite_mass_kg = mass;
        EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
        EXPECT_THROW((void)make_hpop(circular(), options), std::invalid_argument);
    }
    options.satellite_mass_kg = std::numeric_limits<double>::max();
    options.srp.area_mass_ratio_m2_per_kg = 2;
    EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
    options.srp.area_mass_ratio_m2_per_kg = 0;
    EXPECT_NO_THROW(validate_hpop_options(options));
    EXPECT_EQ(options.srp_area_m2(), 0);
    options.drag.area_mass_ratio_m2_per_kg = 0;
    options.satellite_mass_kg = std::numeric_limits<double>::denorm_min();
    EXPECT_NO_THROW(validate_hpop_options(options));
    EXPECT_EQ(options.drag_area_m2(), 0);
}
TEST(Hpop, PointMassMatchesCircularAnalyticSolutionForwardBackwardAndNearEpochLimits) {
    const double rate = std::sqrt(egm96_mu / std::pow(7e6, 3));
    for (auto integrator : {HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                            HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson})
        for (auto origin : {0LL, 8'000'000'000'000'000'000LL}) {
            auto initial = circular();
            initial.epoch = Epoch{origin};
            auto options = point_mass();
            options.integrator = integrator;
            const auto provider = make_hpop(initial, options);
            for (auto seconds : {86400LL, -86400LL, 1701LL, 0LL, -5000LL}) {
                const auto state = provider->state_at(Epoch{origin + seconds * 1000000000LL});
                const double angle = rate * static_cast<double>(seconds);
                EXPECT_NEAR(state.position_m[0], 7e6 * std::cos(angle), 0.06);
                EXPECT_NEAR(state.position_m[1], 7e6 * std::sin(angle), 0.06);
                EXPECT_NEAR(state.velocity_mps[0], -7e6 * rate * std::sin(angle), 6e-5);
                EXPECT_NEAR(state.velocity_mps[1], 7e6 * rate * std::cos(angle), 6e-5);
            }
            EXPECT_EQ(provider->state_at(initial.epoch).position_m, initial.position_m);
            const auto tiny = provider->state_at(Epoch{origin + 1});
            EXPECT_NEAR(tiny.position_m[1], initial.velocity_mps[1] * 1e-9, 1e-16);
        }
}
TEST(Hpop, Rk4ConvergesAtFourthOrderAndSamplingDoesNotChangeTheSolution) {
    const auto initial = circular();
    auto options = point_mass();
    const auto reference = make_hpop(initial, options)->state_at(Epoch{6000000000000});
    options.integrator = HpopIntegrator::rk4;
    options.fixed_step_seconds = 20;
    const auto coarse = make_hpop(initial, options)->state_at(reference.epoch);
    options.fixed_step_seconds = 10;
    const auto provider = make_hpop(initial, options);
    const auto fine = provider->state_at(reference.epoch);
    EXPECT_GT(position_error(coarse, reference) / position_error(fine, reference), 14);
    EXPECT_LT(position_error(fine, reference), 0.1);
    const auto fresh = make_hpop(initial, options);
    for (auto ns : {777000000000LL, -199000000000LL, 1000000000LL})
        (void)fresh->state_at(Epoch{ns});
    EXPECT_EQ(fresh->state_at(reference.epoch).position_m, fine.position_m);
    auto job =
        std::async(std::launch::async, [&] { return provider->state_at(Epoch{-8123456789012}); });
    EXPECT_EQ(provider->state_at(reference.epoch).position_m, fine.position_m);
    EXPECT_EQ(job.get().position_m, fresh->state_at(Epoch{-8123456789012}).position_m);
}
TEST(Hpop, CancellationDoesNotPublishIncompleteCheckpointsOrPoisonLaterQueries) {
    auto provider = make_hpop(circular(), point_mass());
    int calls = 0;
    const auto cancel = [&] {
        if (++calls == 20)
            throw std::runtime_error("cancel");
    };
    EXPECT_THROW((void)provider->state_at(Epoch{9000000000000}, cancel), std::runtime_error);
    EXPECT_EQ(calls, 20);
    EXPECT_EQ(provider->state_at(Epoch{9000000000000}).position_m,
              make_hpop(circular(), point_mass())->state_at(Epoch{9000000000000}).position_m);
}
TEST(Hpop, ValidatesOptionsResourcesFramesAltitudeToleranceAndWorkBounds) {
    auto initial = circular();
    auto options = point_mass();
    EXPECT_THROW((void)make_hpop(initial), std::invalid_argument);
    initial.frame = FrameId::itrf;
    EXPECT_THROW((void)make_hpop(initial, options), std::invalid_argument);
    initial = circular();
    initial.position_m[0] = egm96_radius_m;
    EXPECT_THROW((void)make_hpop(initial, options), PropagationError);
    initial = circular();
    initial.velocity_mps[0] = std::numeric_limits<double>::infinity();
    EXPECT_THROW((void)make_hpop(initial, options), PropagationError);
    for (int degree : {-1, 1, 21}) {
        options.gravity_degree = degree;
        EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
    }
    options = point_mass();
    options.min_step_seconds = options.max_step_seconds = 1800;
    options.position_tolerance_m = 1e-10;
    options.velocity_tolerance_mps = 1e-13;
    options.relative_tolerance = 1e-14;
    initial = circular();
    EXPECT_THROW((void)make_hpop(initial, options)->state_at(Epoch{300000000000}),
                 PropagationError);
    EXPECT_THROW((void)make_hpop(circular(), point_mass())->state_at(Epoch{31622401000000000}),
                 PropagationError);
    options = point_mass();
    options.integrator = HpopIntegrator::rk4;
    options.fixed_step_seconds = 1e-6;
    EXPECT_THROW((void)make_hpop(circular(), options)->state_at(Epoch{1000000000}),
                 PropagationError);
}
TEST(EarthGravity, DegreeTwoZonalMatchesClosedFormIncludingThePoles) {
    const double j2 = std::sqrt(5.0) * 0.484165371736e-3;
    for (const std::array<double, 3> r :
         {std::array{7e6, 0., 0.}, {0., 0., 7e6}, {4e6, 5e6, 3e6}}) {
        const double radius = std::hypot(r[0], r[1], r[2]), z2 = r[2] * r[2] / (radius * radius);
        const double k =
            1.5 * j2 * egm96_mu * egm96_radius_m * egm96_radius_m / std::pow(radius, 5);
        const auto actual = egm96_acceleration(r, 2, 0);
        for (std::size_t i = 0; i < 3; ++i)
            EXPECT_NEAR(actual[i],
                        -egm96_mu * r[i] / std::pow(radius, 3) +
                            k * r[i] * (5 * z2 - (i == 2 ? 3 : 1)),
                        5e-14);
    }
}
TEST(Hpop, MatchesIndependentOrekitHolmesFeatherstoneAndDp853Reference) {
    std::ifstream file{ORBITFORGE_HPOP_REFERENCE};
    ASSERT_TRUE(file.good());
    const HpopEnvironment environment{std::make_shared<FixedFrames>(),
                                      std::make_shared<FixedEop>()};
    std::string line;
    int count = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row{line};
        HpopOptions options;
        double elapsed;
        auto initial = circular();
        std::array<double, 3> expected_r{}, expected_v{}, expected_a{};
        row >> options.gravity_degree >> options.gravity_order >> elapsed;
        for (auto &v : initial.position_m)
            row >> v;
        for (auto &v : initial.velocity_mps)
            row >> v;
        for (auto &v : expected_r)
            row >> v;
        for (auto &v : expected_v)
            row >> v;
        for (auto &v : expected_a)
            row >> v;
        ASSERT_FALSE(row.fail()) << line;
        SCOPED_TRACE(line);
        const auto acceleration =
            egm96_acceleration(initial.position_m, options.gravity_degree, options.gravity_order);
        for (auto integrator : {HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                                HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
            options.integrator = integrator;
            const auto actual = make_hpop(initial, options, environment)
                                    ->state_at(Epoch{static_cast<std::int64_t>(elapsed * 1e9)});
            for (std::size_t i = 0; i < 3; ++i) {
                EXPECT_NEAR(acceleration[i], expected_a[i], 5e-13);
                EXPECT_NEAR(actual.position_m[i], expected_r[i], 0.15);
                EXPECT_NEAR(actual.velocity_mps[i], expected_v[i], 0.00015);
            }
        }
        ++count;
    }
    EXPECT_EQ(count, 60);
}

namespace {
class MovingMoon final : public IThirdBodyEphemeris {
    ThirdBodyDataInfo info_{"synthetic-moving-Moon",
                            "",
                            "analytic-v1",
                            Epoch{-86400'000'000'000},
                            Epoch{86400'000'000'000},
                            1.32712440018e20,
                            4.902800066e12};

  public:
    std::array<double, 3> position(ThirdBody body, Epoch at) const override {
        if (body != ThirdBody::moon || at.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            at.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw std::out_of_range("Moon stage outside certified interval");
        const double angle = static_cast<double>(at.tai_nanoseconds()) * 1e-9 * 2 *
                                 std::numbers::pi / (27.321661 * 86400) +
                             .4;
        return {384400000 * std::cos(angle), 384400000 * std::sin(angle) * std::cos(.3),
                384400000 * std::sin(angle) * std::sin(.3)};
    }
    void validate_interval(Epoch first, Epoch last, std::span<const ThirdBody>) const override {
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds() ||
            first.tai_nanoseconds() > last.tai_nanoseconds())
            throw std::out_of_range("Moon coverage");
    }
    const ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
};
CartesianState eccentric_analytic(double seconds) {
    constexpr double e = .7, perigee = 7e6;
    const double a = perigee / (1 - e), rate = std::sqrt(egm96_mu / (a * a * a));
    const double mean = rate * seconds;
    double anomaly = mean;
    for (int i = 0; i < 30; ++i)
        anomaly -= (anomaly - e * std::sin(anomaly) - mean) / (1 - e * std::cos(anomaly));
    const double c = std::cos(anomaly), sn = std::sin(anomaly), beta = std::sqrt(1 - e * e);
    return {Epoch{static_cast<std::int64_t>(seconds * 1e9)},
            OriginId::earth,
            FrameId::gcrf,
            {a * (c - e), a * beta * sn, 0},
            {-a * rate * sn / (1 - e * c), a * rate * beta * c / (1 - e * c), 0}};
}
} // namespace
TEST(Hpop, Rkv89MatchesIndependentScipyDop853ForEccentricJ2AndTimeVaryingForces) {
    std::ifstream file{ORBITFORGE_RKV89_REFERENCE};
    ASSERT_TRUE(file.good());
    HpopEnvironment environment{std::make_shared<FixedFrames>(), std::make_shared<FixedEop>()};
    environment.ephemeris = std::make_shared<MovingMoon>();
    std::string line;
    int count = 0;
    double max_position = 0, max_velocity = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row{line};
        int kind;
        double seconds;
        auto initial = circular(), expected = initial;
        row >> kind >> seconds;
        for (auto &v : initial.position_m)
            row >> v;
        for (auto &v : initial.velocity_mps)
            row >> v;
        for (auto &v : expected.position_m)
            row >> v;
        for (auto &v : expected.velocity_mps)
            row >> v;
        ASSERT_FALSE(row.fail());
        expected.epoch = Epoch{std::llround(seconds * 1e9)};
        auto options = point_mass();
        options.integrator = HpopIntegrator::rkv89;
        options.gravity_degree = kind ? 2 : 0;
        options.relative_tolerance = 1e-14;
        options.position_tolerance_m = 1e-8;
        options.velocity_tolerance_mps = 1e-11;
        options.max_step_seconds = 300;
        options.third_bodies[static_cast<std::size_t>(ThirdBody::moon)].enabled = kind == 2;
        const auto actual = make_hpop(initial, options, environment)->state_at(expected.epoch);
        const double er = position_error(actual, expected);
        const double ev = std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                     actual.velocity_mps[1] - expected.velocity_mps[1],
                                     actual.velocity_mps[2] - expected.velocity_mps[2]);
        max_position = std::max(max_position, er);
        max_velocity = std::max(max_velocity, ev);
        EXPECT_LT(er, .005);
        EXPECT_LT(ev, 5e-6);
        ++count;
    }
    EXPECT_EQ(count, 60);
    std::ostringstream metrics_r, metrics_v;
    metrics_r << std::setprecision(17) << max_position;
    metrics_v << std::setprecision(17) << max_velocity;
    RecordProperty("max_position_error_m", metrics_r.str());
    RecordProperty("max_velocity_error_mps", metrics_v.str());
}
TEST(Hpop, FixedStepMethodsMatchIndependentHipparchusAndConverge) {
    for (auto integrator : {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                            HpopIntegrator::bulirsch_stoer}) {
        SCOPED_TRACE(static_cast<int>(integrator));
        std::ifstream file{ORBITFORGE_BS_REFERENCE};
        ASSERT_TRUE(file.good());
        HpopEnvironment environment{std::make_shared<FixedFrames>(), std::make_shared<FixedEop>()};
        environment.ephemeris = std::make_shared<MovingMoon>();
        std::string line;
        int count = 0;
        double max_position = 0, max_velocity = 0, max_refinement = 0;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            SCOPED_TRACE(line);
            std::istringstream row{line};
            int kind;
            double seconds;
            auto initial = circular(), expected = initial;
            row >> kind >> seconds;
            for (auto &v : initial.position_m)
                row >> v;
            for (auto &v : initial.velocity_mps)
                row >> v;
            for (auto &v : expected.position_m)
                row >> v;
            for (auto &v : expected.velocity_mps)
                row >> v;
            ASSERT_FALSE(row.fail());
            expected.epoch = Epoch{std::llround(seconds * 1e9)};
            auto options = point_mass();
            options.integrator = integrator;
            options.step_control = HpopStepControl::fixed;
            options.fixed_step_seconds = integrator == HpopIntegrator::rk4 ? 2 : 30;
            options.gravity_degree = kind ? 2 : 0;
            options.relative_tolerance = 1e-14;
            options.position_tolerance_m = 1e-8;
            options.velocity_tolerance_mps = 1e-11;
            options.max_step_seconds = 300;
            options.third_bodies[static_cast<std::size_t>(ThirdBody::moon)].enabled = kind == 2;
            const auto actual = make_hpop(initial, options, environment)->state_at(expected.epoch);
            options.fixed_step_seconds *= .5;
            const auto refined = make_hpop(initial, options, environment)->state_at(expected.epoch);
            const double refinement = position_error(actual, refined);
            max_refinement = std::max(max_refinement, refinement);
            EXPECT_LT(refinement, .001);
            EXPECT_LT(position_error(refined, expected), .005);
            EXPECT_LT(std::hypot(refined.velocity_mps[0] - expected.velocity_mps[0],
                                 refined.velocity_mps[1] - expected.velocity_mps[1],
                                 refined.velocity_mps[2] - expected.velocity_mps[2]),
                      5e-6);
            const double er = position_error(actual, expected);
            const double ev = std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                         actual.velocity_mps[1] - expected.velocity_mps[1],
                                         actual.velocity_mps[2] - expected.velocity_mps[2]);
            max_position = std::max(max_position, er);
            max_velocity = std::max(max_velocity, ev);
            EXPECT_LT(er, .005);
            EXPECT_LT(ev, 5e-6);
            ++count;
        }
        EXPECT_EQ(count, 60);
        std::ostringstream metrics_r, metrics_v;
        metrics_r << std::setprecision(17) << max_position;
        metrics_v << std::setprecision(17) << max_velocity;
        RecordProperty(std::to_string(static_cast<int>(integrator)) + "_position_error_m",
                       metrics_r.str());
        RecordProperty(std::to_string(static_cast<int>(integrator)) + "_velocity_error_mps",
                       metrics_v.str());
        RecordProperty(std::to_string(static_cast<int>(integrator)) + "_refinement_m",
                       max_refinement);
    }
}
TEST(Hpop, FixedStepsIgnoreToleranceDraftsAndPreserveTheEpochGrid) {
    for (auto integrator : {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                            HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
        auto options = point_mass();
        options.integrator = integrator;
        options.step_control = HpopStepControl::fixed;
        options.fixed_step_seconds = 7.1;
        const auto a = make_hpop(circular(), options);
        options.relative_tolerance = 1e-14;
        options.position_tolerance_m = options.velocity_tolerance_mps = 1e-200;
        options.min_step_seconds = options.max_step_seconds = 1e-6;
        const auto b = make_hpop(circular(), options);
        for (auto t : {1999'000'000'000LL, -2077'000'000'000LL, 3LL, 333'700'000'000LL}) {
            const auto expected = a->state_at(Epoch{t}), actual = b->state_at(Epoch{t});
            EXPECT_EQ(actual.position_m, expected.position_m);
            EXPECT_EQ(actual.velocity_mps, expected.velocity_mps);
            EXPECT_EQ(make_hpop(circular(), options)->state_at(Epoch{t}).position_m,
                      actual.position_m);
        }
    }
    // Three 700 s steps must not become seven 300 s cache cells.
    auto initial = circular();
    initial.position_m = {42164000, 0, 0};
    initial.velocity_mps = {0, std::sqrt(egm96_mu / 42164000), 0};
    auto options = point_mass();
    options.gravity_degree = 2;
    options.step_control = HpopStepControl::fixed;
    options.fixed_step_seconds = 700;
    const auto frames = std::make_shared<FixedFrames>();
    const auto provider = make_hpop(initial, options, {frames, std::make_shared<FixedEop>()});
    frames->evaluations = 0;
    (void)provider->state_at(Epoch{2100'000'000'000});
    EXPECT_EQ(frames->evaluations, 3U * (13 + 1));
    options.step_control = static_cast<HpopStepControl>(2);
    EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
}
TEST(Hpop, LongAdaptiveStepsMatchIndependentHipparchusAndConverge) {
    for (auto integrator :
         {HpopIntegrator::rkf78, HpopIntegrator::rkv89, HpopIntegrator::bulirsch_stoer}) {
        double max_r = 0, max_v = 0, max_refinement = 0;
        for (const auto *path : {ORBITFORGE_BS_REFERENCE, ORBITFORGE_LONG_STEP_REFERENCE}) {
            std::ifstream file{path};
            ASSERT_TRUE(file.good());
            HpopEnvironment environment{std::make_shared<FixedFrames>(),
                                        std::make_shared<FixedEop>()};
            environment.ephemeris = std::make_shared<MovingMoon>();
            std::string line;
            int count = 0;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#')
                    continue;
                SCOPED_TRACE(line);
                SCOPED_TRACE(static_cast<int>(integrator));
                std::istringstream row{line};
                int kind;
                double seconds;
                auto initial = circular(), expected = initial;
                row >> kind >> seconds;
                for (auto &v : initial.position_m)
                    row >> v;
                for (auto &v : initial.velocity_mps)
                    row >> v;
                for (auto &v : expected.position_m)
                    row >> v;
                for (auto &v : expected.velocity_mps)
                    row >> v;
                ASSERT_FALSE(row.fail());
                expected.epoch = Epoch{std::llround(seconds * 1e9)};
                auto options = point_mass();
                options.integrator = integrator;
                options.gravity_degree = kind ? 2 : 0;
                options.relative_tolerance = 1e-14;
                options.position_tolerance_m = 1e-8;
                options.velocity_tolerance_mps = 1e-11;
                options.third_body(ThirdBody::moon).enabled = kind == 2;
                CartesianState previous = expected;
                for (double ceiling : {86400., 43200.}) {
                    options.max_step_seconds = ceiling;
                    const auto actual =
                        make_hpop(initial, options, environment)->state_at(expected.epoch);
                    const double er = position_error(actual, expected);
                    const double ev = std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                                 actual.velocity_mps[1] - expected.velocity_mps[1],
                                                 actual.velocity_mps[2] - expected.velocity_mps[2]);
                    EXPECT_LT(er, .005);
                    EXPECT_LT(ev, 5e-6);
                    max_r = std::max(max_r, er);
                    max_v = std::max(max_v, ev);
                    if (ceiling == 43200) {
                        const double refinement = position_error(actual, previous);
                        EXPECT_LT(refinement, .001);
                        max_refinement = std::max(max_refinement, refinement);
                    }
                    previous = actual;
                }
                ++count;
            }
            EXPECT_EQ(count, std::string_view(path) == ORBITFORGE_BS_REFERENCE ? 60 : 52);
        }
        const auto metric = [&](const char *name, double value) {
            std::ostringstream text;
            text << std::setprecision(17) << value;
            RecordProperty(std::to_string(static_cast<int>(integrator)) + name, text.str());
        };
        metric("_position_m", max_r);
        metric("_velocity_mps", max_v);
        metric("_refinement_m", max_refinement);
    }
}
TEST(Hpop, FullDayFixedStepsMatchIndependentLongPeriodReference) {
    for (auto integrator : {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                            HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
        std::ifstream file{ORBITFORGE_LONG_STEP_REFERENCE};
        ASSERT_TRUE(file.good());
        std::string line;
        int count = 0;
        double max_r = 0, max_v = 0;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            std::istringstream row{line};
            int kind;
            double seconds;
            auto initial = circular(), expected = initial;
            row >> kind >> seconds;
            for (auto &v : initial.position_m)
                row >> v;
            for (auto &v : initial.velocity_mps)
                row >> v;
            for (auto &v : expected.position_m)
                row >> v;
            for (auto &v : expected.velocity_mps)
                row >> v;
            ASSERT_FALSE(row.fail());
            // This remote mathematical two-body orbit checks the range, not a
            // physically valid geocentric prediction beyond Earth's Hill sphere.
            if (kind || initial.position_m[0] != 1e10)
                continue;
            SCOPED_TRACE(line);
            SCOPED_TRACE(static_cast<int>(integrator));
            expected.epoch = Epoch{std::llround(seconds * 1e9)};
            auto options = point_mass();
            options.integrator = integrator;
            options.step_control = HpopStepControl::fixed;
            for (double step : {86400., 43200.}) {
                options.fixed_step_seconds = step;
                const auto actual = make_hpop(initial, options)->state_at(expected.epoch);
                const double er = position_error(actual, expected);
                const double ev = std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                             actual.velocity_mps[1] - expected.velocity_mps[1],
                                             actual.velocity_mps[2] - expected.velocity_mps[2]);
                EXPECT_LT(er, .005);
                EXPECT_LT(ev, 5e-8);
                max_r = std::max(max_r, er);
                max_v = std::max(max_v, ev);
            }
            ++count;
        }
        EXPECT_EQ(count, 4);
        std::ostringstream metric_r, metric_v;
        metric_r << std::setprecision(17) << max_r;
        metric_v << std::setprecision(17) << max_v;
        RecordProperty(std::to_string(static_cast<int>(integrator)) + "_position_m",
                       metric_r.str());
        RecordProperty(std::to_string(static_cast<int>(integrator)) + "_velocity_mps",
                       metric_v.str());
    }
}
TEST(Hpop, AdaptiveCacheAllowsFullDayStepsAndPreservesCancellationAndQueryOrder) {
    auto initial = circular();
    initial.position_m = {1e10, 0, 0};
    initial.velocity_mps = {0, std::sqrt(egm96_mu / 1e10), 0};
    for (auto integrator :
         {HpopIntegrator::rkf78, HpopIntegrator::rkv89, HpopIntegrator::bulirsch_stoer}) {
        auto options = point_mass();
        options.gravity_degree = 2;
        options.integrator = integrator;
        options.min_step_seconds = options.max_step_seconds = 86400;
        const auto frames = std::make_shared<FixedFrames>();
        const HpopEnvironment environment{frames, std::make_shared<FixedEop>()};
        const auto provider = make_hpop(initial, options, environment);
        frames->evaluations = 0;
        const auto first = provider->state_at(Epoch{86400'000'000'000});
        // One complete accepted day step; no hidden 300 s or one-hour limit.
        const auto evaluations = frames->evaluations.load();
        if (integrator == HpopIntegrator::rkf78) {
            EXPECT_EQ(evaluations, 14U);
        }
        if (integrator == HpopIntegrator::rkv89) {
            EXPECT_EQ(evaluations, 17U);
        }
        EXPECT_LT(evaluations, 100U);
        int calls = 0;
        EXPECT_THROW((void)provider->state_at(Epoch{4 * 86400'000'000'000LL},
                                              [&] {
                                                  if (++calls == 3)
                                                      throw std::runtime_error("cancel long step");
                                              }),
                     std::runtime_error);
        EXPECT_EQ(provider->state_at(first.epoch).position_m, first.position_m);
        auto job = std::async(std::launch::async,
                              [&] { return provider->state_at(Epoch{-3 * 86400'000'000'000LL}); });
        for (auto t : {3 * 86400'000'000'000LL, 86400'000'000'001LL, -86400'000'000'001LL,
                       86400'000'000'000LL - 1, 5LL}) {
            const auto actual = provider->state_at(Epoch{t});
            const auto fresh = make_hpop(initial, options, environment)->state_at(Epoch{t});
            EXPECT_EQ(actual.position_m, fresh.position_m);
            EXPECT_EQ(actual.velocity_mps, fresh.velocity_mps);
        }
        EXPECT_EQ(job.get().position_m, make_hpop(initial, options, environment)
                                            ->state_at(Epoch{-3 * 86400'000'000'000LL})
                                            .position_m);
    }
    auto options = point_mass();
    options.min_step_seconds = options.max_step_seconds = options.fixed_step_seconds = 86400;
    EXPECT_NO_THROW(validate_hpop_options(options));
    for (auto member : {&HpopOptions::min_step_seconds, &HpopOptions::max_step_seconds,
                        &HpopOptions::fixed_step_seconds}) {
        const double saved = options.*member;
        for (double bad : {86400.000001, 0., std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
            options.*member = bad;
            EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
        }
        options.*member = saved;
    }
}
TEST(Hpop, Rkf78MatchesIndependentHipparchusForEccentricJ2AndTimeVaryingForces) {
    std::ifstream file{ORBITFORGE_BS_REFERENCE};
    ASSERT_TRUE(file.good());
    HpopEnvironment environment{std::make_shared<FixedFrames>(), std::make_shared<FixedEop>()};
    environment.ephemeris = std::make_shared<MovingMoon>();
    std::string line;
    int count = 0;
    double max_position = 0, max_velocity = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row{line};
        int kind;
        double seconds;
        auto initial = circular(), expected = initial;
        row >> kind >> seconds;
        for (auto &v : initial.position_m)
            row >> v;
        for (auto &v : initial.velocity_mps)
            row >> v;
        for (auto &v : expected.position_m)
            row >> v;
        for (auto &v : expected.velocity_mps)
            row >> v;
        ASSERT_FALSE(row.fail());
        expected.epoch = Epoch{std::llround(seconds * 1e9)};
        auto options = point_mass();
        options.integrator = HpopIntegrator::rkf78;
        options.gravity_degree = kind ? 2 : 0;
        options.relative_tolerance = 1e-14;
        options.position_tolerance_m = 1e-8;
        options.velocity_tolerance_mps = 1e-11;
        options.max_step_seconds = 300;
        options.third_bodies[static_cast<std::size_t>(ThirdBody::moon)].enabled = kind == 2;
        const auto actual = make_hpop(initial, options, environment)->state_at(expected.epoch);
        const double er = position_error(actual, expected);
        const double ev = std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                     actual.velocity_mps[1] - expected.velocity_mps[1],
                                     actual.velocity_mps[2] - expected.velocity_mps[2]);
        max_position = std::max(max_position, er);
        max_velocity = std::max(max_velocity, ev);
        EXPECT_LT(er, .005);
        EXPECT_LT(ev, 5e-6);
        ++count;
    }
    EXPECT_EQ(count, 60);
    std::ostringstream metrics_r, metrics_v;
    metrics_r << std::setprecision(17) << max_position;
    metrics_v << std::setprecision(17) << max_velocity;
    RecordProperty("max_position_error_m", metrics_r.str());
    RecordProperty("max_velocity_error_mps", metrics_v.str());
}
TEST(Hpop, BulirschStoerMatchesIndependentHipparchusForEccentricJ2AndTimeVaryingForces) {
    std::ifstream file{ORBITFORGE_BS_REFERENCE};
    ASSERT_TRUE(file.good());
    HpopEnvironment environment{std::make_shared<FixedFrames>(), std::make_shared<FixedEop>()};
    environment.ephemeris = std::make_shared<MovingMoon>();
    std::string line;
    int count = 0;
    double max_position = 0, max_velocity = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row{line};
        int kind;
        double seconds;
        auto initial = circular(), expected = initial;
        row >> kind >> seconds;
        for (auto &v : initial.position_m)
            row >> v;
        for (auto &v : initial.velocity_mps)
            row >> v;
        for (auto &v : expected.position_m)
            row >> v;
        for (auto &v : expected.velocity_mps)
            row >> v;
        ASSERT_FALSE(row.fail());
        expected.epoch = Epoch{std::llround(seconds * 1e9)};
        auto options = point_mass();
        options.integrator = HpopIntegrator::bulirsch_stoer;
        options.gravity_degree = kind ? 2 : 0;
        options.relative_tolerance = 1e-14;
        options.position_tolerance_m = 1e-8;
        options.velocity_tolerance_mps = 1e-11;
        options.max_step_seconds = 300;
        options.third_bodies[static_cast<std::size_t>(ThirdBody::moon)].enabled = kind == 2;
        const auto actual = make_hpop(initial, options, environment)->state_at(expected.epoch);
        const double er = position_error(actual, expected);
        const double ev = std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                     actual.velocity_mps[1] - expected.velocity_mps[1],
                                     actual.velocity_mps[2] - expected.velocity_mps[2]);
        max_position = std::max(max_position, er);
        max_velocity = std::max(max_velocity, ev);
        EXPECT_LT(er, .005);
        EXPECT_LT(ev, 5e-6);
        ++count;
    }
    EXPECT_EQ(count, 60);
    std::ostringstream metrics_r, metrics_v;
    metrics_r << std::setprecision(17) << max_position;
    metrics_v << std::setprecision(17) << max_velocity;
    RecordProperty("max_position_error_m", metrics_r.str());
    RecordProperty("max_velocity_error_mps", metrics_v.str());
}
TEST(Hpop, Rkf78AdvancesTheSeventhOrderMemberAndControlsRejectedSteps) {
    const auto initial = eccentric_analytic(0), expected = eccentric_analytic(600);
    auto options = point_mass();
    options.integrator = HpopIntegrator::rkf78;
    // Loose tolerances force equal steps for the order test, with no error rejection.
    options.relative_tolerance = 1e-3;
    options.position_tolerance_m = 1;
    options.velocity_tolerance_mps = .001;
    options.min_step_seconds = options.max_step_seconds = 150;
    const auto coarse = make_hpop(initial, options)->state_at(expected.epoch);
    options.min_step_seconds = options.max_step_seconds = 75;
    const auto fine = make_hpop(initial, options)->state_at(expected.epoch);
    const double ratio = position_error(coarse, expected) / position_error(fine, expected);
    EXPECT_GT(ratio, 115); // Seventh order predicts 2^7; eighth-order advancement is excluded.
    EXPECT_LT(ratio, 150);
    EXPECT_LT(position_error(fine, expected), 1e-5);
    RecordProperty("coarse_position_error_m", std::to_string(position_error(coarse, expected)));
    RecordProperty("fine_position_error_m", std::to_string(position_error(fine, expected)));
    RecordProperty("step_halving_error_ratio", std::to_string(ratio));
    options.relative_tolerance = 1e-14;
    options.position_tolerance_m = 1e-10;
    options.velocity_tolerance_mps = 1e-13;
    options.min_step_seconds = options.max_step_seconds = 300;
    EXPECT_THROW((void)make_hpop(initial, options)->state_at(Epoch{300'000'000'000}),
                 PropagationError);
    options.min_step_seconds = .001;
    const auto adaptive = make_hpop(initial, options)->state_at(expected.epoch);
    EXPECT_LT(position_error(adaptive, expected), 1e-4);
}
TEST(Hpop, Rkv89AdvancesTheEighthOrderMemberAndControlsRejectedSteps) {
    const auto initial = eccentric_analytic(0), expected = eccentric_analytic(6000);
    auto options = point_mass();
    options.integrator = HpopIntegrator::rkv89;
    // Loose tolerances force equal steps for the order test, with no error rejection.
    options.relative_tolerance = 1e-3;
    options.position_tolerance_m = 1;
    options.velocity_tolerance_mps = .001;
    options.min_step_seconds = options.max_step_seconds = 300;
    const auto coarse = make_hpop(initial, options)->state_at(expected.epoch);
    options.min_step_seconds = options.max_step_seconds = 150;
    const auto fine = make_hpop(initial, options)->state_at(expected.epoch);
    const double ratio = position_error(coarse, expected) / position_error(fine, expected);
    EXPECT_GT(ratio, 220); // Eighth order predicts 2^8; ninth-order advancement is excluded.
    EXPECT_LT(ratio, 330);
    EXPECT_LT(position_error(fine, expected), .001);
    RecordProperty("step_halving_error_ratio", std::to_string(ratio));
    options.relative_tolerance = 1e-14;
    options.position_tolerance_m = 1e-10;
    options.velocity_tolerance_mps = 1e-13;
    options.min_step_seconds = options.max_step_seconds = 300;
    EXPECT_THROW((void)make_hpop(initial, options)->state_at(Epoch{300'000'000'000}),
                 PropagationError);
    options.min_step_seconds = .001;
    const auto adaptive = make_hpop(initial, options)->state_at(expected.epoch);
    EXPECT_LT(position_error(adaptive, expected), 1e-4);
}
TEST(Hpop, BothStepModesPreserveCancellationAndQueryOrder) {
    for (bool vop : {false, true}) {
        SCOPED_TRACE(vop);

        for (auto control : {HpopStepControl::relative_error, HpopStepControl::fixed}) {
            for (auto integrator :
                 {HpopIntegrator::rkf78, HpopIntegrator::rkv89, HpopIntegrator::bulirsch_stoer,
                  HpopIntegrator::gauss_jackson}) {
                auto options = point_mass();
                options.integrator = integrator;
                options.vop = vop;
                options.step_control = control;
                const auto provider = make_hpop(circular(), options),
                           fresh = make_hpop(circular(), options);
                int calls = 0;
                EXPECT_THROW((void)provider->state_at(Epoch{9000'000'000'000},
                                                      [&] {
                                                          if (++calls == 20)
                                                              throw std::runtime_error("cancel");
                                                      }),
                             std::runtime_error);
                EXPECT_EQ(calls, 20);
                if (integrator == HpopIntegrator::gauss_jackson) {
                    (void)provider->state_at(Epoch{1200'000'000'000});
                    calls = 0;
                    EXPECT_THROW(
                        (void)provider->state_at(Epoch{9000'000'000'000},
                                                 [&] {
                                                     if (++calls == 5)
                                                         throw std::runtime_error(
                                                             "cancel with established history");
                                                 }),
                        std::runtime_error);
                    EXPECT_EQ(calls, 5);
                }
                for (auto ns : {777'000'000'000LL, -199'000'000'000LL, 1LL})
                    (void)provider->state_at(Epoch{ns});
                auto job = std::async(std::launch::async,
                                      [&] { return provider->state_at(Epoch{-8123'456'789'012}); });
                EXPECT_EQ(provider->state_at(Epoch{9000'000'000'000}).position_m,
                          fresh->state_at(Epoch{9000'000'000'000}).position_m);
                const auto actual = job.get(), expected = fresh->state_at(Epoch{-8123'456'789'012});
                EXPECT_EQ(actual.position_m, expected.position_m);
                EXPECT_EQ(actual.velocity_mps, expected.velocity_mps);
            }
        }
    }
}

TEST(Hpop, BulirschStoerAdaptsOrderAndRejectsUnresolvedMinimumSteps) {
    const auto initial = eccentric_analytic(0), expected = eccentric_analytic(6000);
    auto options = point_mass();
    options.integrator = HpopIntegrator::bulirsch_stoer;
    options.min_step_seconds = options.max_step_seconds = 300;
    options.relative_tolerance = 1e-6;
    options.position_tolerance_m = .1;
    options.velocity_tolerance_mps = 1e-4;
    int loose_work = 0, tight_work = 0;
    const auto loose = make_hpop(initial, options)->state_at(expected.epoch, [&] { ++loose_work; });
    options.relative_tolerance = 1e-14;
    options.position_tolerance_m = 1e-8;
    options.velocity_tolerance_mps = 1e-11;
    const auto tight = make_hpop(initial, options)->state_at(expected.epoch, [&] { ++tight_work; });
    EXPECT_LT(position_error(tight, expected), .001);
    EXPECT_LT(position_error(tight, expected), position_error(loose, expected) * .01);
    EXPECT_GT(tight_work, loose_work);
    options.min_step_seconds = options.max_step_seconds = 60;
    const auto small_steps = make_hpop(initial, options)->state_at(expected.epoch);
    EXPECT_LT(position_error(small_steps, tight), .001);
    // Fast hyperbolic departure needs a shorter step than the allowed 300 seconds.
    auto fast = initial;
    fast.velocity_mps[1] = 120000;
    options.min_step_seconds = options.max_step_seconds = 300;
    EXPECT_THROW((void)make_hpop(fast, options)->state_at(Epoch{300'000'000'000}),
                 PropagationError);
    options.min_step_seconds = .001;
    EXPECT_NO_THROW((void)make_hpop(fast, options)->state_at(Epoch{300'000'000'000}));
    RecordProperty("loose_work", loose_work);
    RecordProperty("tight_work", tight_work);
}

TEST(Hpop, GaussJacksonMatchesIndependentHipparchusForBothCorrections) {
    for (auto correction : {HpopCorrection::full, HpopCorrection::pseudo}) {
        std::ifstream file{ORBITFORGE_BS_REFERENCE};
        ASSERT_TRUE(file.good());
        HpopEnvironment environment{std::make_shared<FixedFrames>(), std::make_shared<FixedEop>()};
        environment.ephemeris = std::make_shared<MovingMoon>();
        std::string line;
        int count = 0;
        double max_position = 0, max_velocity = 0, max_refined = 0, max_change = 0;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            SCOPED_TRACE(line);
            std::istringstream row{line};
            int kind;
            double seconds;
            auto initial = circular(), expected = initial;
            row >> kind >> seconds;
            for (auto &v : initial.position_m)
                row >> v;
            for (auto &v : initial.velocity_mps)
                row >> v;
            for (auto &v : expected.position_m)
                row >> v;
            for (auto &v : expected.velocity_mps)
                row >> v;
            ASSERT_FALSE(row.fail());
            expected.epoch = Epoch{std::llround(seconds * 1e9)};
            auto options = point_mass();
            options.integrator = HpopIntegrator::gauss_jackson;
            options.correction = correction;
            options.fixed_step_seconds = 30;
            options.gravity_degree = kind ? 2 : 0;
            options.relative_tolerance = 1e-14;
            options.position_tolerance_m = 1e-8;
            options.velocity_tolerance_mps = 1e-11;
            options.max_step_seconds = 300;
            options.third_bodies[static_cast<std::size_t>(ThirdBody::moon)].enabled = kind == 2;
            const auto actual = make_hpop(initial, options, environment)->state_at(expected.epoch);
            const double er = position_error(actual, expected);
            const double ev = std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                         actual.velocity_mps[1] - expected.velocity_mps[1],
                                         actual.velocity_mps[2] - expected.velocity_mps[2]);
            max_position = std::max(max_position, er);
            max_velocity = std::max(max_velocity, ev);
            EXPECT_LT(er, .005);
            EXPECT_LT(ev, 5e-6);
            options.fixed_step_seconds = 15;
            const auto refined = make_hpop(initial, options, environment)->state_at(expected.epoch);
            max_refined = std::max(max_refined, position_error(refined, expected));
            max_change = std::max(max_change, position_error(refined, actual));
            EXPECT_LT(position_error(refined, expected), .005);
            EXPECT_LT(position_error(refined, actual), .001);
            ++count;
        }
        EXPECT_EQ(count, 60);
        std::ostringstream metrics_r, metrics_v;
        metrics_r << std::setprecision(17) << max_position;
        metrics_v << std::setprecision(17) << max_velocity;
        RecordProperty(correction == HpopCorrection::full ? "full_refined_error_m"
                                                          : "pseudo_refined_error_m",
                       std::to_string(max_refined));
        RecordProperty(correction == HpopCorrection::full ? "full_step_halving_change_m"
                                                          : "pseudo_step_halving_change_m",
                       std::to_string(max_change));
        RecordProperty(correction == HpopCorrection::full ? "full_position_error_m"
                                                          : "pseudo_position_error_m",
                       metrics_r.str());
        RecordProperty(correction == HpopCorrection::full ? "full_velocity_error_mps"
                                                          : "pseudo_velocity_error_mps",
                       metrics_v.str());
    }
}

TEST(Hpop, GaussJacksonPreservesGridHistoryAndConvergesAtTwelfthOrder) {
    auto options = point_mass();
    options.integrator = HpopIntegrator::gauss_jackson;
    const auto initial = circular();
    const double seconds = 60000;
    const double angle = std::sqrt(egm96_mu / std::pow(7e6, 3)) * seconds;
    auto expected = initial;
    expected.epoch = Epoch{static_cast<std::int64_t>(seconds * 1e9)};
    expected.position_m = {7e6 * std::cos(angle), 7e6 * std::sin(angle), 0};
    options.fixed_step_seconds = 200;
    const auto coarse = make_hpop(initial, options)->state_at(expected.epoch);
    options.fixed_step_seconds = 160;
    const auto fine = make_hpop(initial, options)->state_at(expected.epoch);
    const double coarse_error = position_error(coarse, expected);
    const double fine_error = position_error(fine, expected);
    RecordProperty("coarse_position_error_m", std::to_string(coarse_error));
    RecordProperty("fine_position_error_m", std::to_string(fine_error));
    const double observed_order = std::log(coarse_error / fine_error) / std::log(1.25);
    RecordProperty("observed_order", std::to_string(observed_order));
    EXPECT_LT(fine_error, .001);
    // These two steps stay above the floating point error floor.
    EXPECT_GT(observed_order, 11);
    EXPECT_LT(observed_order, 14);
    // Neither query order nor fractional checkpoint spacing can change saved history.
    for (double step : {7., 60., 10.3}) {
        options.fixed_step_seconds = step;
        const auto used = make_hpop(initial, options), fresh = make_hpop(initial, options);
        for (auto ns : {899'000'000'001LL, 333'333'333'333LL, -310'000'000'001LL})
            (void)used->state_at(Epoch{ns});
        const auto a = used->state_at(Epoch{8100'123'456'789});
        const auto b = fresh->state_at(a.epoch);
        EXPECT_EQ(a.position_m, b.position_m);
        EXPECT_EQ(a.velocity_mps, b.velocity_mps);
    }
    options.correction = static_cast<HpopCorrection>(123);
    EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
}

TEST(Hpop, GaussJacksonPseudoCorrectionPreservesTrajectoryAndReducesForceEvaluations) {
    std::array states{circular(), circular()};
    std::array<std::size_t, 2> evaluations{};
    for (std::size_t mode = 0; mode < 2; ++mode) {
        auto frames = std::make_shared<FixedFrames>();
        HpopEnvironment environment{frames, std::make_shared<FixedEop>()};
        auto options = point_mass();
        options.gravity_degree = 2;
        options.integrator = HpopIntegrator::gauss_jackson;
        options.correction = mode == 0 ? HpopCorrection::full : HpopCorrection::pseudo;
        options.fixed_step_seconds = 30;
        const auto provider = make_hpop(circular(), options, environment);
        (void)provider->state_at(Epoch{1200'000'000'000});
        frames->evaluations = 0;
        states[mode] = provider->state_at(Epoch{1800'000'000'000});
        evaluations[mode] = frames->evaluations.load();
    }
    EXPECT_LT(evaluations[1], evaluations[0]);
    // Across different propagated histories either mode can need another EC
    // iteration near the convergence threshold. The exact one-call saving is
    // checked on cloned histories by GaussJacksonKernel, not inferred from totals.
    EXPECT_LT(position_error(states[0], states[1]), 1e-5);
    RecordProperty("full_force_evaluations", std::to_string(evaluations[0]));
    RecordProperty("pseudo_force_evaluations", std::to_string(evaluations[1]));
}

TEST(HpopEphemeris, AcceptedAndReportedEndpointsMatchIndependentHipparchus) {
    for (const auto integrator : {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                                  HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
        for (const auto reporting : {0LL, 120'123'456'789LL}) {
            std::ifstream file{ORBITFORGE_BS_REFERENCE};
            ASSERT_TRUE(file.good());
            HpopEnvironment environment{std::make_shared<FixedFrames>(),
                                        std::make_shared<FixedEop>()};
            environment.ephemeris = std::make_shared<MovingMoon>();
            std::string line;
            int count = 0;
            double max_r = 0, max_v = 0;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#')
                    continue;
                SCOPED_TRACE(line);
                SCOPED_TRACE(static_cast<int>(integrator));
                SCOPED_TRACE(reporting);
                std::istringstream row{line};
                int kind;
                double seconds;
                CartesianState initial{Epoch{0}, OriginId::earth, FrameId::gcrf, {}, {}},
                    expected = initial;
                row >> kind >> seconds;
                for (auto &v : initial.position_m)
                    row >> v;
                for (auto &v : initial.velocity_mps)
                    row >> v;
                for (auto &v : expected.position_m)
                    row >> v;
                for (auto &v : expected.velocity_mps)
                    row >> v;
                ASSERT_TRUE(row);
                expected.epoch = Epoch{std::llround(seconds * 1e9)};
                auto options = point_mass();
                options.integrator = integrator;
                options.fixed_step_seconds = integrator == HpopIntegrator::rk4 ? 2 : 30;
                options.gravity_degree = kind ? 2 : 0;
                options.relative_tolerance = 1e-14;
                options.position_tolerance_m = 1e-8;
                options.velocity_tolerance_mps = 1e-11;
                options.max_step_seconds = 60;
                options.third_body(ThirdBody::moon).enabled = kind == 2;
                const auto arcs = propagate_hpop_ephemeris(initial, expected.epoch, options,
                                                           environment, reporting);
                ASSERT_EQ(arcs.size(), 1U);
                ASSERT_GE(arcs.front().size(), 2U);
                const auto &actual = seconds > 0 ? arcs.back().back() : arcs.front().front();
                EXPECT_EQ(actual.epoch, expected.epoch);
                const auto er = position_error(actual, expected);
                const auto ev = std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                           actual.velocity_mps[1] - expected.velocity_mps[1],
                                           actual.velocity_mps[2] - expected.velocity_mps[2]);
                EXPECT_LT(er, .005);
                EXPECT_LT(ev, 5e-6);
                max_r = std::max(max_r, er);
                max_v = std::max(max_v, ev);
                for (std::size_t i = 1; i < arcs[0].size(); ++i)
                    EXPECT_GT(arcs[0][i].epoch.tai_nanoseconds(),
                              arcs[0][i - 1].epoch.tai_nanoseconds());
                ++count;
            }
            EXPECT_EQ(count, 60);
            const auto key =
                std::to_string(static_cast<int>(integrator)) + "_" + std::to_string(reporting);
            RecordProperty(key + "_r_m", max_r);
            RecordProperty(key + "_v_mps", max_v);
        }
    }
}
TEST(HpopEphemeris, FixedGridOutputAndExactNanosecondEndpointsWorkInBothDirections) {
    for (bool vop : {false, true}) {
        SCOPED_TRACE(vop);

        for (const auto integrator :
             {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
              HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
            auto o = point_mass();
            o.integrator = integrator;
            o.vop = vop;
            o.step_control = HpopStepControl::fixed;
            o.fixed_step_seconds = 10;
            for (auto start : {-8'000'000'000'000'000'000LL, 8'000'000'000'000'000'000LL}) {
                for (auto direction : {-1LL, 1LL}) {
                    auto initial = circular();
                    initial.epoch = Epoch{start};
                    const Epoch end{start + direction * 123'123'456'789LL};
                    const auto accepted = propagate_hpop_ephemeris(initial, end, o);
                    ASSERT_EQ(accepted.size(), 1U);
                    EXPECT_EQ(accepted[0].size(), 14U);
                    const auto reported =
                        propagate_hpop_ephemeris(initial, end, o, {}, 7'100'000'000LL);
                    ASSERT_EQ(reported.size(), 1U);
                    ASSERT_EQ(reported[0].size(), 19U);
                    for (std::size_t i = 0; i < reported[0].size(); ++i) {
                        const auto n = direction > 0 ? i : reported[0].size() - 1 - i;
                        const auto offset = std::min(static_cast<std::int64_t>(n) * 7'100'000'000LL,
                                                     123'123'456'789LL);
                        EXPECT_EQ(reported[0][i].epoch, Epoch{start + direction * offset});
                    }
                }
            }
        }
        auto o = point_mass();
        o.integrator = HpopIntegrator::rkf78;
        const auto tiny = propagate_hpop_ephemeris(circular(), Epoch{31}, o, {}, 1);
        ASSERT_EQ(tiny[0].size(), 32U);
        for (std::size_t i = 0; i < tiny[0].size(); ++i)
            EXPECT_EQ(tiny[0][i].epoch, Epoch{static_cast<std::int64_t>(i)});
    }
}
TEST(HpopEphemeris, RejectsInvalidCoverageAndCancellationWithoutPartialPublication) {
    auto o = point_mass();
    for (auto sign : {-1LL, 1LL}) {
        const Epoch outside{sign * (366LL * 86400'000'000'000LL + 1)};
        EXPECT_THROW((void)propagate_hpop_ephemeris(circular(), outside, o), PropagationError);
        EXPECT_THROW((void)make_hpop(circular(), o)->state_at(outside), PropagationError);
    }
    auto extreme = circular();
    extreme.epoch = Epoch{std::numeric_limits<std::int64_t>::min()};
    EXPECT_THROW(
        (void)propagate_hpop_ephemeris(extreme, Epoch{std::numeric_limits<std::int64_t>::max()}, o),
        PropagationError);
    EXPECT_THROW((void)propagate_hpop_ephemeris(circular(), Epoch{1}, o, {}, -1),
                 std::invalid_argument);
    EXPECT_THROW((void)propagate_hpop_ephemeris(circular(), Epoch{1}, o, {}, 86400'000'000'001LL),
                 std::invalid_argument);
    EXPECT_THROW((void)propagate_hpop_ephemeris(circular(), Epoch{367LL * 86400'000'000'000LL}, o),
                 PropagationError);
    int calls = 0;
    EXPECT_THROW((void)propagate_hpop_ephemeris(circular(), Epoch{600'000'000'000LL}, o, {}, 0,
                                                [&] {
                                                    if (++calls == 8)
                                                        throw std::runtime_error("cancel nodes");
                                                }),
                 std::runtime_error);
    const auto retry = propagate_hpop_ephemeris(circular(), Epoch{600'000'000'000LL}, o);
    const auto fresh = propagate_hpop_ephemeris(circular(), Epoch{600'000'000'000LL}, o);
    EXPECT_EQ(retry.back().back().position_m, fresh.back().back().position_m);
    const auto instant = propagate_hpop_ephemeris(circular(), Epoch{0}, o);
    ASSERT_EQ(instant.size(), 1U);
    ASSERT_EQ(instant[0].size(), 1U);
    EXPECT_EQ(instant[0][0].position_m, circular().position_m);
    auto initial = circular();
    initial.position_m = {1e10, 0, 0};
    initial.velocity_mps = {0, std::sqrt(egm96_mu / 1e10), 0};
    o.step_control = HpopStepControl::fixed;
    o.fixed_step_seconds = 86400;
    EXPECT_THROW((void)propagate_hpop_ephemeris(initial, Epoch{256LL * 86400'000'000'000LL + 1}, o,
                                                {}, 86400'000'000'000LL),
                 PropagationError);
}

TEST(HpopVop, MatchesIndependentHipparchusWithPerturbationsAndRegularization) {
    for (auto integrator : {HpopIntegrator::rkf78, HpopIntegrator::bulirsch_stoer})
        for (bool regularized : {false, true})
            for (bool fixed : {false, true}) {
                SCOPED_TRACE(static_cast<int>(integrator));
                SCOPED_TRACE(regularized);
                SCOPED_TRACE(fixed);
                std::ifstream file{ORBITFORGE_BS_REFERENCE};
                ASSERT_TRUE(file);
                HpopEnvironment environment{std::make_shared<FixedFrames>(),
                                            std::make_shared<FixedEop>()};
                environment.ephemeris = std::make_shared<MovingMoon>();
                std::string line;
                int count = 0;
                double max_r = 0, max_v = 0;
                while (std::getline(file, line)) {
                    if (line.empty() || line[0] == '#')
                        continue;
                    SCOPED_TRACE(line);
                    std::istringstream row{line};
                    int kind;
                    double seconds;
                    auto initial = circular(), expected = initial;
                    row >> kind >> seconds;
                    for (auto &v : initial.position_m)
                        row >> v;
                    for (auto &v : initial.velocity_mps)
                        row >> v;
                    for (auto &v : expected.position_m)
                        row >> v;
                    for (auto &v : expected.velocity_mps)
                        row >> v;
                    ASSERT_TRUE(row);
                    expected.epoch = Epoch{std::llround(seconds * 1e9)};
                    auto options = point_mass();
                    options.integrator = integrator;
                    options.vop = true;
                    options.time_regularization = regularized;
                    options.steps_per_orbit = 240;
                    options.step_control =
                        fixed ? HpopStepControl::fixed : HpopStepControl::relative_error;
                    options.fixed_step_seconds = 30;
                    options.gravity_degree = kind ? 2 : 0;
                    options.relative_tolerance = 1e-14;
                    options.position_tolerance_m = 1e-8;
                    options.velocity_tolerance_mps = 1e-11;
                    options.max_step_seconds = 300;
                    options.third_body(ThirdBody::moon).enabled = kind == 2;
                    const auto arcs =
                        propagate_hpop_ephemeris(initial, expected.epoch, options, environment);
                    ASSERT_EQ(arcs.size(), 1U);
                    const auto &actual = seconds > 0 ? arcs.front().back() : arcs.front().front();
                    EXPECT_EQ(actual.epoch, expected.epoch);
                    const double er = position_error(actual, expected);
                    const double ev = std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                                 actual.velocity_mps[1] - expected.velocity_mps[1],
                                                 actual.velocity_mps[2] - expected.velocity_mps[2]);
                    EXPECT_LT(er, .005);
                    EXPECT_LT(ev, 5e-6);
                    max_r = std::max(max_r, er);
                    max_v = std::max(max_v, ev);
                    ++count;
                }
                EXPECT_EQ(count, 60);
                const auto key = std::to_string(static_cast<int>(integrator)) + "_" +
                                 std::to_string(regularized) + "_" + std::to_string(fixed);
                RecordProperty(key + "_position_m", max_r);
                RecordProperty(key + "_velocity_mps", max_v);
            }
}
TEST(HpopVop, OneLargeStepMatchesOrekitAcrossAllConics) {
    for (auto integrator : {HpopIntegrator::rkf78, HpopIntegrator::bulirsch_stoer}) {
        std::ifstream input(ORBITFORGE_UNIVERSAL_KEPLER_REFERENCE);
        ASSERT_TRUE(input);
        std::string line;
        int count = 0;
        while (std::getline(input, line)) {
            if (line.empty() || line.front() == '#')
                continue;
            SCOPED_TRACE(line);
            int kind;
            double mu, seconds;
            auto initial = circular(), expected = initial;
            std::istringstream row(line);
            row >> kind >> mu >> seconds;
            for (auto &v : initial.position_m)
                row >> v;
            for (auto &v : initial.velocity_mps)
                row >> v;
            for (auto &v : expected.position_m)
                row >> v;
            for (auto &v : expected.velocity_mps)
                row >> v;
            ASSERT_TRUE(row);
            expected.epoch = Epoch{std::llround(seconds * 1e9)};
            auto options = point_mass();
            options.integrator = integrator;
            options.vop = true;
            options.gravity_mu = mu;
            options.step_control = HpopStepControl::fixed;
            options.fixed_step_seconds = 86400;
            const auto arcs = propagate_hpop_ephemeris(initial, expected.epoch, options);
            ASSERT_EQ(arcs.size(), 1U);
            ASSERT_EQ(arcs.front().size(), 2U);
            const auto &actual = seconds > 0 ? arcs.front().back() : arcs.front().front();
            EXPECT_LT(position_error(actual, expected), 5e-4);
            for (int i = 0; i < 3; ++i)
                EXPECT_NEAR(actual.velocity_mps[i], expected.velocity_mps[i], 5e-7);
            ++count;
        }
        EXPECT_EQ(count, 80);
    }
}

TEST(HpopVop, RejectsRadialGeometryOnlyWhenTheFormulationIsActive) {
    for (auto method : {HpopIntegrator::rkf78, HpopIntegrator::bulirsch_stoer}) {
        auto options = point_mass();
        options.integrator = method;
        options.vop = true;
        auto radial = circular();
        radial.velocity_mps = {1000, 0, 0};
        EXPECT_THROW((void)make_hpop(radial, options), std::invalid_argument);
        options.vop = false;
        EXPECT_NO_THROW((void)make_hpop(radial, options));
        options.vop = true;
        options.integrator = HpopIntegrator::rk4;
        EXPECT_NO_THROW((void)make_hpop(radial, options));
    }
}
