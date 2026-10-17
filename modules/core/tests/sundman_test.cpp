#include "../src/propagation/integrators/gauss_jackson.hpp"
#include "../src/propagation/integrators/one_step.hpp"
#include "../src/propagation/integrators/sundman.hpp"
#include "../src/propagation/integrators/sundman_propagation.hpp"
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {
using namespace orbitforge::core;
using namespace orbitforge::core::detail;
using Cartesian = SundmanTransform::Cartesian;
using First = SundmanTransform::FirstOrder;
using Second = SundmanTransform::SecondOrder;
constexpr double mu = 3.986004415e14;
Cartesian initial(double e) {
    const double r = 7e6, v = std::sqrt(mu * (1 + e) / r), angle = .4, inclination = .6;
    return {r * std::cos(angle),
            r * std::sin(angle) * std::cos(inclination),
            r * std::sin(angle) * std::sin(inclination),
            -v * std::sin(angle),
            v * std::cos(angle) * std::cos(inclination),
            v * std::cos(angle) * std::sin(inclination)};
}
std::array<double, 3> force(int kind, double t, const Cartesian &s) {
    const double r = std::hypot(s[0], s[1], s[2]), k = -mu / (r * r * r);
    std::array<double, 3> a{k * s[0], k * s[1], k * s[2]};
    if (kind == 1) {
        const double j2 = std::sqrt(5.) * .484165371736e-3, radius = 6378136.3;
        const double z2 = s[2] * s[2] / (r * r);
        const double factor = 1.5 * j2 * mu * radius * radius / std::pow(r, 5);
        for (std::size_t i = 0; i < 3; ++i)
            a[i] += factor * s[i] * (5 * z2 - (i == 2 ? 3 : 1)) +
                    1e-7 * std::sin(t / 1900 + .7 * i) - 1e-12 * s[i + 3];
    }
    return a;
}
double error(const Cartesian &a, const Cartesian &b, std::size_t start) {
    return std::hypot(a[start] - b[start], a[start + 1] - b[start + 1],
                      a[start + 2] - b[start + 2]);
}
struct Reference {
    int kind;
    double eccentricity, exponent, end, time;
    Cartesian initial{}, final{};
};
std::vector<Reference> references() {
    std::ifstream file{ORBITFORGE_SUNDMAN_REFERENCE};
    if (!file.good())
        throw std::runtime_error("Missing independent Sundman reference");
    std::vector<Reference> result;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] != 'T')
            continue;
        std::istringstream row{line.substr(1)};
        Reference r{};
        row >> r.kind >> r.eccentricity >> r.exponent >> r.end;
        for (auto &v : r.initial)
            row >> v;
        for (auto &v : r.final)
            row >> v;
        row >> r.time;
        if (row.fail())
            throw std::runtime_error("Invalid independent Sundman reference");
        result.push_back(r);
    }
    return result;
}
struct Result {
    Cartesian state{};
    double time = 0;
    std::size_t multisteps = 0, steps = 0;
};
// Fixed q endpoints are used here deliberately. Physical-time event fitting and
// resource coverage are a separate production adapter, not approximated by this test.
Result integrate(const Reference &ref, int method, int per_orbit, bool pseudo = false,
                 std::size_t exact_steps = 0) {
    const SundmanTransform transform{ref.initial, mu, ref.exponent};
    const auto acceleration = [&](double t, const Cartesian &s) { return force(ref.kind, t, s); };
    const auto derivative = [&](double, const First &s) {
        return transform.derivative(s, acceleration);
    };
    const auto second_acceleration = [&](const Second &s) {
        return transform.acceleration(s, acceleration);
    };
    const auto second_derivative = [&](double, const Second &s) {
        const auto a = second_acceleration(s);
        return Second{s[3], s[4], s[5], a[0], a[1], a[2], a[3]};
    };
    CompensatedState<7> accumulated{transform.first_order(ref.initial, 0)};
    auto &value = accumulated.value;
    GaussJacksonSystem12<3, 1> history;
    history.state = transform.second_order(ref.initial, 0);
    ErrorControl<7> fixed;
    fixed.fixed = true;
    ErrorControl<7> startup;
    startup.relative = 1e-14;
    startup.absolute = {1e-9, 1e-9, 1e-9, 1e-8, 1e-8, 1e-8, 1e-11};
    const double ceiling = transform.period() / per_orbit;
    const auto count = exact_steps
                           ? exact_steps
                           : static_cast<std::size_t>(std::ceil(std::abs(ref.end) / ceiling));
    // Exactly uniform q spacing exercises GJ's complete history, including final
    // endpoint. It does not accidentally use the starter for every output query.
    const double h = ref.end / count;
    const auto converged = [](const Second &a, const Second &b) {
        const double r = std::max(1., std::hypot(b[0], b[1], b[2]));
        const double u = std::max(1., std::hypot(b[3], b[4], b[5]));
        constexpr double epsilon = 8 * std::numeric_limits<double>::epsilon();
        for (std::size_t i = 0; i < 3; ++i)
            if (std::abs(a[i] - b[i]) > epsilon * r || std::abs(a[i + 3] - b[i + 3]) > epsilon * u)
                return false;
        return std::abs(a[6] - b[6]) <= epsilon * std::max(1., std::abs(b[6]));
    };
    std::size_t multisteps = 0;
    if (method == 4)
        history.seed(history.state, second_acceleration(history.state), h);
    for (std::size_t j = 0; j < count; ++j) {
        const double q = j * h;
        switch (method) {
        case 0:
            accumulated.advance(rk4_trial(q, value, h, derivative).increment);
            break;
        case 1:
            accumulated.advance(rkf78(q, value, h, derivative, fixed).increment);
            break;
        case 2:
            accumulated.advance(rkv89(q, value, h, derivative, fixed).increment);
            break;
        case 3:
            accumulated.advance(bulirsch_stoer(q, value, h, derivative, fixed).increment);
            break;
        case 4:
            if (history.count < history.history.size()) {
                double at = 0, step = std::abs(h);
                auto next = history.state;
                std::size_t attempts = 0;
                while (at != h) {
                    if (++attempts > 10000)
                        throw std::runtime_error("Sundman GJ test starter did not converge");
                    const double magnitude = std::min(step, std::abs(h - at));
                    const double delta = std::copysign(magnitude, h);
                    startup.max_step = magnitude;
                    const auto trial =
                        bulirsch_stoer(q + at, next, delta, second_derivative, startup, {}, true);
                    if (trial.error <= 1) {
                        next = trial.value;
                        at = magnitude == std::abs(h - at) ? h : at + delta;
                    }
                    step = magnitude *
                           (trial.error == 0
                                ? 4
                                : std::clamp(.9 * std::pow(trial.error, trial.exponent), .1, 4.));
                }
                history.seed(next, second_acceleration(next), h);
            } else {
                history.advance(
                    h, !pseudo, second_acceleration,
                    [&](const Second &before, const Second &after, const auto &a) {
                        return transform.pseudo_correction(before, after, a);
                    },
                    converged);
                ++multisteps;
            }
            break;
        default:
            throw std::logic_error("Invalid Sundman test method");
        }
    }
    if (method == 4)
        return {transform.cartesian_second(history.state), history.state[6], multisteps, count};
    return {transform.cartesian_first(value), value[6], 0, count};
}
class SundmanMethods : public ::testing::TestWithParam<int> {};
TEST_P(SundmanMethods, FixedRegularizedStepsMatchIndependentHighPrecisionAndConverge) {
    const int selection = GetParam(), method = std::min(selection, 4);
    const bool pseudo = selection == 5;
    double max_r = 0, max_v = 0, max_t = 0;
    double halving_r = 0, halving_v = 0, halving_t = 0;
    const auto cases = references();
    ASSERT_EQ(cases.size(), 54);
    for (const auto &ref : cases) {
        SCOPED_TRACE(::testing::Message() << "kind=" << ref.kind << " e=" << ref.eccentricity
                                          << " n=" << ref.exponent << " q=" << ref.end);
        // A time-dependent force adds its own sampling scale near apoapsis.
        // Pure gravity uses the established 4096/8192 GJ convergence pair;
        // the oscillatory force needs 16384/32768. Increasing fixed steps beyond
        // the truncation-dominated range eventually amplifies roundoff in the clock.
        const int steps = method == 0 ? 262144
                                      : (method == 4 ? (ref.kind == 0 ? 4096 : 16384)
                                                     : (method == 3 ? 2048 : 4096));
        const auto coarse = integrate(ref, method, steps, pseudo);
        // Subdivide precisely the same q interval. Independently rounding two
        // nearly integral revolution counts can otherwise add a grid cell.
        const auto fine = integrate(ref, method, steps * 2, pseudo, 2 * coarse.steps);
        for (const auto &result : {coarse, fine}) {
            const double r = error(result.state, ref.final, 0);
            const double v = error(result.state, ref.final, 3);
            const double t = std::abs(result.time - ref.time);
            EXPECT_LT(r, .005);
            EXPECT_LT(v, 5e-6);
            EXPECT_LT(t, 5e-7);
            max_r = std::max(max_r, r);
            max_v = std::max(max_v, v);
            max_t = std::max(max_t, t);
            if (method == 4) {
                EXPECT_GT(result.multisteps, 100);
            }
        }
        halving_r = std::max(halving_r, error(coarse.state, fine.state, 0));
        halving_v = std::max(halving_v, error(coarse.state, fine.state, 3));
        halving_t = std::max(halving_t, std::abs(coarse.time - fine.time));
        EXPECT_LT(error(coarse.state, fine.state, 0), .001);
        EXPECT_LT(error(coarse.state, fine.state, 3), 1e-6);
        // Half the absolute clock budget; unlike spatial state, this integral
        // is especially sensitive to the initial energy of a long-period ellipse.
        EXPECT_LT(std::abs(coarse.time - fine.time), 2.5e-7);
    }
    for (const auto &[name, v] : std::initializer_list<std::pair<const char *, double>>{
             {"max_position_m", max_r},
             {"max_velocity_mps", max_v},
             {"max_clock_seconds", max_t},
             {"halving_position_m", halving_r},
             {"halving_velocity_mps", halving_v},
             {"halving_clock_seconds", halving_t}}) {
        std::ostringstream text;
        text << std::setprecision(17) << v;
        RecordProperty(name, text.str());
    }
}
INSTANTIATE_TEST_SUITE_P(All, SundmanMethods, ::testing::Range(0, 6));

TEST(GaussJacksonKernel, FirstOrderClockRetainsSubUlpStarterResidual) {
    // y'=constant has an exact linear solution. A quarter-ulp initial offset
    // must survive the starter/multistep handoff, including halfway rounding.
    using Clock = GaussJacksonSystem12<0, 1>;
    constexpr double base = 1e6;
    const double ulp = std::nextafter(base, std::numeric_limits<double>::infinity()) - base;
    const Clock::Acceleration rate{.5 * ulp};
    for (double direction : {-1., 1.}) {
        Clock history;
        for (int j = 0; j < 13; ++j) {
            const double offset = (.25 + direction * .5 * j) * ulp;
            const double rounded = base + offset;
            history.seed({rounded}, rate, direction, {(rounded - base) - offset});
        }
        for (int j = 13; j <= 140; ++j) {
            history.advance(
                direction, true, [&](const auto &) { return rate; },
                [](const auto &, const auto &, const auto &a) { return a; },
                [](const auto &a, const auto &b) { return a == b; });
            EXPECT_EQ(history.state[0], base + (.25 + direction * .5 * j) * ulp);
        }
    }
}

TEST(GaussJacksonKernel, PseudoSkipsExactlyOnePostConvergenceForceCallOnTheSameHistory) {
    for (double h : {-30., 30.}) {
        GaussJackson12 history;
        const double angular_rate = std::sqrt(mu / std::pow(7e6, 3));
        for (int i = 0; i < 13; ++i) {
            const double t = i * h, angle = angular_rate * t, speed = angular_rate * 7e6;
            const Cartesian s{7e6 * std::cos(angle),    7e6 * std::sin(angle),   0,
                              -speed * std::sin(angle), speed * std::cos(angle), 0};
            history.seed(s, force(1, t, s), h);
        }
        auto full = history, pseudo = history;
        std::size_t full_calls = 0, pseudo_calls = 0;
        full.advance(h, mu, true, [&](const Cartesian &s) {
            ++full_calls;
            return force(1, 13 * h, s);
        });
        pseudo.advance(h, mu, false, [&](const Cartesian &s) {
            ++pseudo_calls;
            return force(1, 13 * h, s);
        });
        EXPECT_GT(pseudo_calls, 0);
        EXPECT_EQ(full_calls, pseudo_calls + 1);
        EXPECT_EQ(full.state, pseudo.state);
    }
}

TEST(Sundman, ExponentsOneAndTwoFollowEccentricAndTrueAnomaly) {
    constexpr double pi = std::numbers::pi;
    for (double e : {0., .7, .98}) {
        const double a = 7e6 / (1 - e), time_scale = a / std::sqrt(mu / a);
        for (double n : {1., 2.}) {
            const SundmanTransform transform{initial(e), mu, n};
            for (double turns : {-.375, .4375, 1.125}) {
                const double wrapped = 2 * pi * (turns - std::floor(turns));
                const double eccentric =
                    n == 1 ? wrapped
                           : 2 * std::atan2(std::sqrt(1 - e) * std::sin(wrapped / 2),
                                            std::sqrt(1 + e) * std::cos(wrapped / 2));
                const double r = a * (1 - e * std::cos(eccentric));
                const double speed = std::sqrt(mu * a) / r;
                const std::array<double, 2> p{a * (std::cos(eccentric) - e),
                                              a * std::sqrt(1 - e * e) * std::sin(eccentric)};
                const std::array<double, 2> v{-speed * std::sin(eccentric),
                                              speed * std::sqrt(1 - e * e) * std::cos(eccentric)};
                const auto rotate = [](const std::array<double, 2> &value) {
                    const double x = value[0] * std::cos(.4) - value[1] * std::sin(.4);
                    const double y = value[0] * std::sin(.4) + value[1] * std::cos(.4);
                    return std::array<double, 3>{x, y * std::cos(.6), y * std::sin(.6)};
                };
                const auto position = rotate(p), velocity = rotate(v);
                const Cartesian expected{position[0], position[1], position[2],
                                         velocity[0], velocity[1], velocity[2]};
                const double time =
                    time_scale * (eccentric + 2 * pi * std::floor(turns) - e * std::sin(eccentric));
                Reference ref{0, e, n, transform.period() * turns, time, initial(e), expected};
                for (int method : {3, 4}) {
                    SCOPED_TRACE(::testing::Message() << "method=" << method << " e=" << e
                                                      << " n=" << n << " turns=" << turns);
                    Result result;
                    ASSERT_NO_THROW(result = integrate(ref, method, method == 4 ? 4096 : 2048));
                    EXPECT_LT(error(result.state, expected, 0), .005);
                    EXPECT_LT(error(result.state, expected, 3), 5e-6);
                    EXPECT_LT(std::abs(result.time - time), 5e-7);
                }
            }
        }
    }
}
TEST(Sundman, StepsPerOrbitNormalizationMatchesIndependentLegendreGauss) {
    std::ifstream file{ORBITFORGE_SUNDMAN_REFERENCE};
    ASSERT_TRUE(file.good());
    std::string line;
    std::size_t count = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] != 'P')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row{line.substr(1)};
        double e, n, period;
        row >> e >> n >> period;
        ASSERT_FALSE(row.fail());
        const SundmanTransform transform{initial(e), mu, n};
        // Converting a rounded Cartesian orbit into e amplifies its roundoff by
        // O(1/(1-e)) in this integral. The e=.9999 cases explicitly include that
        // conditioning; normal elliptic cases retain the tighter requirement.
        EXPECT_NEAR(transform.period() / period, 1, e > .99 ? 2e-10 : 2e-12);
        EXPECT_NEAR(transform.semimajor_axis() / (7e6 / (1 - e)), 1, 5e-12);
        EXPECT_NEAR(transform.eccentricity(), e, 1e-14);
        ++count;
    }
    EXPECT_EQ(count, 36);
}
TEST(Sundman, FirstAndSecondOrderCoordinatesPreservePhysicalTimeAndVelocity) {
    for (double e : {0., .7, .98}) {
        for (double n : {.1, 1., 1.5, 2., 10.}) {
            const auto s = initial(e);
            const SundmanTransform transform{s, mu, n};
            for (double t : {-86400., 0., 12345.6789}) {
                const auto first = transform.first_order(s, t);
                const auto second = transform.second_order(s, t);
                EXPECT_EQ(first[6], t);
                EXPECT_EQ(second[6], t);
                EXPECT_EQ(transform.cartesian_first(first), s);
                const auto recovered = transform.cartesian_second(second);
                EXPECT_LT(error(recovered, s, 0), 1e-12);
                EXPECT_LT(error(recovered, s, 3), 2e-12);
                const auto rhs =
                    transform.derivative(first, [&](double at, const Cartesian &value) {
                        EXPECT_EQ(at, t);
                        EXPECT_EQ(value, s);
                        return force(1, at, value);
                    });
                EXPECT_DOUBLE_EQ(rhs[6], transform.clock_rate(s));
            }
        }
    }
}
TEST(Sundman, PseudoCorrectionRetainsPhysicalPerturbationAndUpdatesClockFactors) {
    const auto s = initial(.7);
    const SundmanTransform transform{s, mu, 1.5};
    const auto before = transform.second_order(s, 100);
    auto changed = s;
    changed[0] *= 1.01;
    changed[4] *= .98;
    const auto after = transform.second_order(changed, 120);
    const auto previous =
        transform.acceleration(before, [](double t, const Cartesian &v) { return force(1, t, v); });
    const auto corrected = transform.pseudo_correction(before, after, previous);
    const auto old_full = force(1, before[6], s), old_central = force(0, before[6], s);
    const auto expected = transform.acceleration(after, [&](double at, const Cartesian &v) {
        auto a = force(0, at, v);
        for (std::size_t i = 0; i < 3; ++i)
            a[i] += old_full[i] - old_central[i];
        return a;
    });
    for (std::size_t i = 0; i < corrected.size(); ++i)
        EXPECT_NEAR(corrected[i], expected[i], 1e-13 * std::max(1., std::abs(expected[i])));
    const auto full =
        transform.acceleration(after, [](double t, const Cartesian &v) { return force(1, t, v); });
    EXPECT_GT(std::abs(full[0] - corrected[0]), 1);
}
TEST(Sundman, RejectsInvalidClockAndNonEllipticNormalization) {
    const auto s = initial(.7);
    for (double n : {0., -1., 10.1, std::numeric_limits<double>::quiet_NaN()})
        EXPECT_THROW((void)SundmanTransform(s, mu, n), std::invalid_argument);
    EXPECT_THROW((void)SundmanTransform(s, 0, 1.5), std::invalid_argument);
    auto escape = s;
    for (std::size_t i = 3; i < 6; ++i)
        escape[i] *= 2;
    EXPECT_THROW((void)SundmanTransform(escape, mu, 1.5), std::invalid_argument);
    EXPECT_THROW((void)SundmanTransform(Cartesian{}, mu, 1.5), std::invalid_argument);
    EXPECT_THROW((void)SundmanTransform(Cartesian{7e6, 0, 0, 100, 0, 0}, mu, 1.5),
                 std::invalid_argument);
    const SundmanTransform transform{s, mu, 1.5};
    auto second = transform.second_order(s, 0);
    second[6] = std::numeric_limits<double>::infinity();
    EXPECT_THROW((void)transform.cartesian_second(second), PropagationError);
    second = {};
    EXPECT_THROW((void)transform.cartesian_second(second), PropagationError);
    EXPECT_THROW((void)transform.clock_rate(Cartesian{}), PropagationError);
}
TEST(Sundman, SharedExtrapolationChecksCancellationWithoutPublishingAPartialState) {
    const SundmanTransform transform{initial(.7), mu, 1.5};
    const auto first = transform.first_order(initial(.7), 0);
    const auto saved = first;
    int calls = 0;
    const auto derivative = [&](double, const First &v) {
        return transform.derivative(v, [](double t, const Cartesian &s) { return force(1, t, s); });
    };
    ErrorControl<7> fixed;
    fixed.fixed = true;
    EXPECT_THROW((void)bulirsch_stoer(0, first, .01, derivative, fixed,
                                      [&] {
                                          if (++calls == 10)
                                              throw std::runtime_error("cancel");
                                      }),
                 std::runtime_error);
    EXPECT_EQ(calls, 10);
    EXPECT_EQ(first, saved);
}

class SundmanBoundaries : public ::testing::TestWithParam<int> {};
TEST_P(SundmanBoundaries, PhysicalEndpointsMatchIndependentReferenceWithoutOutOfRangeForces) {
    const int selection = GetParam();
    const bool adaptive = selection >= 6;
    const int method = adaptive ? selection - 5 : std::min(selection, 4);
    constexpr std::array methods{HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                                 HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson};
    double max_r = 0, max_v = 0, change_r = 0, change_v = 0;
    std::size_t boundary_count = 0, multisteps = 0;
    for (const auto &ref : references()) {
        SCOPED_TRACE(::testing::Message() << "kind=" << ref.kind << " e=" << ref.eccentricity
                                          << " n=" << ref.exponent << " t=" << ref.time);
        HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.integrator = methods[method];
        options.correction = selection == 5 ? HpopCorrection::pseudo : HpopCorrection::full;
        options.step_control = adaptive ? HpopStepControl::relative_error : HpopStepControl::fixed;
        options.relative_tolerance = 1e-14;
        options.position_tolerance_m = 1e-8;
        options.velocity_tolerance_mps = 1e-11;
        options.max_step_seconds = 300;
        // The oscillatory force has a physical-time bandwidth near apoapsis.
        // Fixed-q boundary bridges need to resolve it as well as the orbit.
        const std::size_t steps = method == 0 ? 131072 : (ref.kind == 0 ? 4096 : 32768);
        const auto run = [&](bool fine) {
            auto settings = options;
            if (fine)
                settings.max_step_seconds *= .5;
            SundmanPropagation propagation(ref.initial, mu, ref.exponent, steps * (fine ? 2 : 1),
                                           settings);
            auto result = propagation.initial_state();
            // Three independent query gates keep each validation query within the
            // production 200000-attempt budget, including the fine RK4 mesh.
            for (int gate = 1; gate <= 3; ++gate) {
                std::size_t attempts = 0;
                const double target = gate == 3 ? ref.time : ref.time * (gate / 3.);
                result = propagation.propagate(
                    result, target,
                    [&](double time, const Cartesian &s) {
                        if (time < std::min(0., target) || time > std::max(0., target))
                            throw std::runtime_error(
                                "Force evaluated beyond the exact physical interval");
                        return force(ref.kind, time, s);
                    },
                    attempts);
                EXPECT_EQ(result.time(), target);
            }
            EXPECT_EQ(result.time(), ref.time);
            EXPECT_LT(error(result.cartesian(), ref.final, 0), .005);
            EXPECT_LT(error(result.cartesian(), ref.final, 3), 5e-6);
            max_r = std::max(max_r, error(result.cartesian(), ref.final, 0));
            max_v = std::max(max_v, error(result.cartesian(), ref.final, 3));
            boundary_count += result.boundary_steps;
            multisteps += result.multisteps;
            if (method == 4) {
                EXPECT_GT(result.multisteps, 100);
            }
            return result.cartesian();
        };
        Cartesian coarse{}, fine{};
        ASSERT_NO_THROW(coarse = run(false));
        ASSERT_NO_THROW(fine = run(true));
        change_r = std::max(change_r, error(coarse, fine, 0));
        change_v = std::max(change_v, error(coarse, fine, 3));
        EXPECT_LT(error(coarse, fine, 0), .001);
        EXPECT_LT(error(coarse, fine, 3), 1e-6);
    }
    EXPECT_GT(boundary_count, 0);
    RecordProperty("boundary_steps", std::to_string(boundary_count));
    RecordProperty("gj_multisteps", std::to_string(multisteps));
    for (const auto &[name, v] : std::initializer_list<std::pair<const char *, double>>{
             {"max_position_m", max_r},
             {"max_velocity_mps", max_v},
             {"halving_position_m", change_r},
             {"halving_velocity_mps", change_v}}) {
        std::ostringstream text;
        text << std::setprecision(17) << v;
        RecordProperty(name, text.str());
    }
}
INSTANTIATE_TEST_SUITE_P(All, SundmanBoundaries, ::testing::Range(0, 9));

TEST(SundmanBounded, NanosecondEndpointsAndCancellationRetainTheInputHistory) {
    const auto s = initial(.7);
    for (auto method : {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                        HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
        HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.integrator = method;
        SundmanPropagation propagation(s, mu, 1.5, 4096, options);
        for (double end : {-1e-9, 1e-9}) {
            std::size_t attempts = 0, calls = 0;
            const auto source = propagation.initial_state();
            const auto result = propagation.propagate(
                source, end,
                [&](double t, const Cartesian &v) {
                    EXPECT_GE(t, std::min(0., end));
                    EXPECT_LE(t, std::max(0., end));
                    ++calls;
                    return force(0, t, v);
                },
                attempts);
            EXPECT_EQ(source.time(), 0);
            EXPECT_EQ(source.q, 0);
            EXPECT_EQ(result.time(), end);
            EXPECT_GT(calls, 0);
            for (std::size_t i = 0; i < 3; ++i)
                EXPECT_NEAR(result.cartesian()[i], s[i] + end * s[i + 3], 2e-9);
        }
        std::size_t attempts = 0;
        int checkpoints = 0;
        const auto source = propagation.initial_state();
        EXPECT_THROW((void)propagation.propagate(
                         source, 1000, [](double t, const Cartesian &v) { return force(0, t, v); },
                         attempts,
                         [&] {
                             if (++checkpoints == 20)
                                 throw std::runtime_error("cancel");
                         }),
                     std::runtime_error);
        EXPECT_EQ(checkpoints, 20);
        EXPECT_EQ(source.time(), 0);
        EXPECT_EQ(source.history.count, 0);
    }
}

TEST(SundmanBounded, ForceDiscontinuitiesUseOneSidedCoverageAndExactPhysicalGates) {
    const auto initial_state = initial(.7);
    constexpr std::array<double, 3> first_force{-.1, .02, -.03}, second_force{.03, -.06, .02};
    const auto analytic = [](const Cartesian &s, double dt, const std::array<double, 3> &a) {
        auto result = s;
        for (std::size_t i = 0; i < 3; ++i) {
            result[i] += dt * s[i + 3] + .5 * dt * dt * a[i];
            result[i + 3] += dt * a[i];
        }
        return result;
    };
    for (auto method : {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                        HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
        for (double direction : {-1., 1.}) {
            HpopOptions options;
            options.gravity_degree = options.gravity_order = 0;
            options.integrator = method;
            options.step_control = HpopStepControl::fixed;
            SundmanPropagation propagation(initial_state, mu, 1.5, 4096, options);
            std::size_t attempts = 0, nodes = 0;
            double previous = 0;
            const double jump = direction * 315.123456789, end = direction * 700.987654321;
            const auto observed = [&](double t, const Cartesian &) {
                EXPECT_GT(direction * (t - previous), 0);
                previous = t;
                ++nodes;
            };
            auto result = propagation.propagate(
                propagation.initial_state(), jump,
                [&](double time, const Cartesian &) {
                    EXPECT_GE(time, std::min(0., jump));
                    EXPECT_LE(time, std::max(0., jump));
                    return first_force;
                },
                attempts, {}, observed);
            EXPECT_EQ(result.time(), jump);
            auto expected = analytic(initial_state, jump, first_force);
            EXPECT_LT(error(result.cartesian(), expected, 0), 1e-5);
            EXPECT_LT(error(result.cartesian(), expected, 3), 1e-8);
            result.restart_force_segment();
            EXPECT_EQ(result.history.count, 0);
            result = propagation.propagate(
                result, end,
                [&](double time, const Cartesian &) {
                    EXPECT_GE(time, std::min(jump, end));
                    EXPECT_LE(time, std::max(jump, end));
                    return second_force;
                },
                attempts, {}, observed);
            expected = analytic(expected, end - jump, second_force);
            EXPECT_EQ(result.time(), end);
            EXPECT_EQ(previous, end);
            EXPECT_GT(nodes, 100);
            EXPECT_LT(error(result.cartesian(), expected, 0), 1e-5);
            EXPECT_LT(error(result.cartesian(), expected, 3), 1e-8);
            if (method == HpopIntegrator::gauss_jackson) {
                EXPECT_GT(result.multisteps, 100);
            }
        }
    }
}
TEST(SundmanBounded, AdaptivePhysicalLimitsTakePrecedenceOverTheNominalRegularizedScale) {
    for (auto method :
         {HpopIntegrator::rkf78, HpopIntegrator::rkv89, HpopIntegrator::bulirsch_stoer}) {
        for (double e : {0., .7}) {
            const auto source = initial(e);
            const double axis = 7e6 / (1 - e), omega = std::sqrt(mu / std::pow(axis, 3));
            for (double direction : {-1., 1.}) {
                for (bool equal_bounds : {false, true}) {
                    HpopOptions options;
                    options.gravity_degree = options.gravity_order = 0;
                    options.integrator = method;
                    options.relative_tolerance = 1e-14;
                    options.position_tolerance_m = 1e-8;
                    options.velocity_tolerance_mps = 1e-11;
                    options.min_step_seconds = 20;
                    options.max_step_seconds = equal_bounds ? 20 : 35;
                    // Cover a q scale below Min Step and one above Max Step.
                    // Explicit physical limits take precedence over that nominal scale.
                    SundmanPropagation propagation(source, mu, 1.5, equal_bounds ? 100000000 : 120,
                                                   options);
                    std::size_t attempts = 0, nodes = 0;
                    const double begin = direction * 100, end = direction * 223.123456789;
                    const auto acceleration = [&](double t, const Cartesian &v) {
                        EXPECT_GE(t, std::min(0., end));
                        EXPECT_LE(t, std::max(0., end));
                        return force(0, t, v);
                    };
                    auto result = propagation.propagate(propagation.initial_state(), begin,
                                                        acceleration, attempts);
                    double previous = begin;
                    result = propagation.propagate(
                        result, end, acceleration, attempts, {}, [&](double at, const Cartesian &) {
                            const double span = direction * (at - previous);
                            EXPECT_GT(span, 0);
                            if (at != end) {
                                EXPECT_GE(span, options.min_step_seconds - 1e-10);
                            }
                            EXPECT_LE(span, options.max_step_seconds + 1e-10);
                            previous = at;
                            ++nodes;
                        });
                    EXPECT_EQ(result.time(), end);
                    EXPECT_GT(nodes, 2);
                    // Independent Kepler equation and conic geometry, in the
                    // initial periapsis axes (p,q), rather than another ODE solver.
                    const double mean = omega * end, beta = std::sqrt(1 - e * e);
                    double eccentric = mean;
                    for (int iteration = 0; iteration < 20; ++iteration)
                        eccentric -= (eccentric - e * std::sin(eccentric) - mean) /
                                     (1 - e * std::cos(eccentric));
                    const double radius = axis * (1 - e * std::cos(eccentric));
                    const double speed = std::hypot(source[3], source[4], source[5]);
                    Cartesian expected{};
                    for (std::size_t i = 0; i < 3; ++i) {
                        const double p_axis = source[i] / 7e6, q_axis = source[i + 3] / speed;
                        expected[i] = axis * ((std::cos(eccentric) - e) * p_axis +
                                              beta * std::sin(eccentric) * q_axis);
                        expected[i + 3] =
                            std::sqrt(mu * axis) / radius *
                            (-std::sin(eccentric) * p_axis + beta * std::cos(eccentric) * q_axis);
                    }
                    EXPECT_LT(error(result.cartesian(), expected, 0), 1e-5);
                    EXPECT_LT(error(result.cartesian(), expected, 3), 1e-8);
                }
            }
        }
    }
}
TEST(SundmanBounded, AdaptiveMinimumStepRejectsUnresolvedOscillatoryForce) {
    for (bool vop : {false, true}) {
        SCOPED_TRACE(vop);

        for (auto method :
             {HpopIntegrator::rkf78, HpopIntegrator::rkv89, HpopIntegrator::bulirsch_stoer}) {
            for (double maximum : {20., 35.}) {
                HpopOptions options;
                options.gravity_degree = options.gravity_order = 0;
                options.integrator = method;
                options.vop = vop;
                options.relative_tolerance = 1e-14;
                options.position_tolerance_m = 1e-8;
                options.velocity_tolerance_mps = 1e-11;
                options.min_step_seconds = 20;
                options.max_step_seconds = maximum;
                SundmanPropagation propagation(initial(.7), mu, 1.5, 100000000, options);
                const auto source = propagation.initial_state();
                std::size_t attempts = 0;
                EXPECT_THROW((void)propagation.propagate(
                                 source, 1000,
                                 [](double t, const Cartesian &v) {
                                     auto a = force(0, t, v);
                                     a[0] += std::sin(100 * t);
                                     return a;
                                 },
                                 attempts),
                             PropagationError);
                EXPECT_EQ(source.time(), 0);
                EXPECT_EQ(source.q, 0);
                // A minimum-step error is diagnosed before the work limit; repeated
                // rejection of the same clamped q step must not exhaust that budget.
                EXPECT_LT(attempts, 100);
            }
        }
    }
}

TEST(SundmanBounded, RejectsInvalidStepCountsWorkExhaustionAndReversedHistory) {
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    const auto s = initial(.7);
    EXPECT_THROW((void)SundmanPropagation(s, mu, 1.5, 0, options), std::invalid_argument);
    SundmanPropagation propagation(s, mu, 1.5, 4096, options);
    const auto source = propagation.initial_state();
    std::size_t attempts = 200000, calls = 0;
    const auto acceleration = [&](double t, const Cartesian &v) {
        ++calls;
        return force(0, t, v);
    };
    EXPECT_THROW((void)propagation.propagate(source, 100, acceleration, attempts),
                 PropagationError);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(source.q, 0);
    attempts = 0;
    const auto advanced = propagation.propagate(source, 100, acceleration, attempts);
    EXPECT_THROW((void)propagation.propagate(advanced, 0, acceleration, attempts),
                 std::invalid_argument);
    EXPECT_EQ(advanced.time(), 100);
}

TEST(HpopRegularization, PublicStateQueriesMatchIndependentHipparchusAtLargeTaiEpochs) {
    constexpr std::array methods{HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                                 HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson};
    constexpr std::int64_t initial_ns =
        std::numeric_limits<std::int64_t>::max() - 40LL * 86400 * 1000000000;
    double max_r = 0, max_v = 0;
    for (int selection = 0; selection < 9; ++selection) {
        const bool adaptive = selection >= 6;
        const int method = adaptive ? selection - 5 : std::min(selection, 4);
        for (const auto &ref : references()) {
            if (ref.kind != 0)
                continue;
            SCOPED_TRACE(::testing::Message()
                         << "selection=" << selection << " e=" << ref.eccentricity
                         << " n=" << ref.exponent << " t=" << ref.time);
            HpopOptions options;
            options.gravity_degree = options.gravity_order = 0;
            options.integrator = methods[method];
            options.step_control =
                adaptive ? HpopStepControl::relative_error : HpopStepControl::fixed;
            options.correction = selection == 5 ? HpopCorrection::pseudo : HpopCorrection::full;
            options.time_regularization = true;
            options.regularization_exponent = ref.exponent;
            options.steps_per_orbit = method == 0 ? 131072 : 4096;
            options.relative_tolerance = 1e-14;
            options.position_tolerance_m = 1e-8;
            options.velocity_tolerance_mps = 1e-11;
            options.max_step_seconds = 150;
            const CartesianState source{Epoch{initial_ns},
                                        OriginId::earth,
                                        FrameId::gcrf,
                                        {ref.initial[0], ref.initial[1], ref.initial[2]},
                                        {ref.initial[3], ref.initial[4], ref.initial[5]}};
            const Epoch end{initial_ns + static_cast<std::int64_t>(std::llround(ref.time * 1e9))};
            const auto orbit = make_hpop(source, options);
            auto output = source;
            ASSERT_NO_THROW(output = orbit->state_at(end));
            EXPECT_EQ(output.epoch, end);
            const Cartesian result{output.position_m[0],   output.position_m[1],
                                   output.position_m[2],   output.velocity_mps[0],
                                   output.velocity_mps[1], output.velocity_mps[2]};
            max_r = std::max(max_r, error(result, ref.final, 0));
            max_v = std::max(max_v, error(result, ref.final, 3));
            EXPECT_LT(error(result, ref.final, 0), .005);
            EXPECT_LT(error(result, ref.final, 3), 5e-6);
        }
    }
    std::ostringstream position, velocity;
    position << std::setprecision(17) << max_r;
    velocity << std::setprecision(17) << max_v;
    RecordProperty("max_position_m", position.str());
    RecordProperty("max_velocity_mps", velocity.str());
}
TEST(HpopRegularization, ExactTaiReportingCancellationAndInactiveOrdinaryStepDrafts) {
    const auto vector = initial(.7);
    const CartesianState source{Epoch{8000000000000000000LL},
                                OriginId::earth,
                                FrameId::gcrf,
                                {vector[0], vector[1], vector[2]},
                                {vector[3], vector[4], vector[5]}};
    for (auto method : {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                        HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
        HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.integrator = method;
        options.time_regularization = true;
        options.steps_per_orbit = 4096;
        options.relative_tolerance = 1e-14;
        options.position_tolerance_m = 1e-8;
        options.velocity_tolerance_mps = 1e-11;
        for (std::int64_t sign : {-1, 1}) {
            constexpr std::int64_t span = 3600'000'000'017LL, report = 60'000'000'001LL;
            const Epoch end{source.epoch.tai_nanoseconds() + sign * span};
            const auto arcs = propagate_hpop_ephemeris(source, end, options, {}, report);
            ASSERT_EQ(arcs.size(), 1);
            EXPECT_EQ(arcs[0].front().epoch, sign > 0 ? source.epoch : end);
            EXPECT_EQ(arcs[0].back().epoch, sign > 0 ? end : source.epoch);
            EXPECT_EQ(arcs[0].size(), 61);
            for (std::size_t i = 0; i < arcs[0].size(); ++i) {
                const auto &node = arcs[0][sign > 0 ? i : arcs[0].size() - 1 - i];
                const auto offset = std::min(span, static_cast<std::int64_t>(i) * report);
                EXPECT_EQ(node.epoch.tai_nanoseconds(),
                          source.epoch.tai_nanoseconds() + sign * offset);
            }
            auto draft = options;
            draft.fixed_step_seconds = 37;
            const auto same = propagate_hpop_ephemeris(source, end, draft, {}, report);
            EXPECT_EQ(arcs[0].back().position_m, same[0].back().position_m);
            EXPECT_EQ(arcs[0].front().velocity_mps, same[0].front().velocity_mps);
            const auto orbit = make_hpop(source, options);
            int checks = 0;
            EXPECT_THROW((void)orbit->state_at(end,
                                               [&] {
                                                   if (++checks == 20)
                                                       throw std::runtime_error("cancel");
                                               }),
                         std::runtime_error);
            const auto after = orbit->state_at(end),
                       fresh = make_hpop(source, options)->state_at(end);
            EXPECT_EQ(after.position_m, fresh.position_m);
            EXPECT_EQ(after.velocity_mps, fresh.velocity_mps);
        }
    }
}
TEST(HpopRegularization, RejectsInvalidParametersAndNonperiodicActiveStates) {
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    for (double n : {0., -1., 10.01, std::numeric_limits<double>::infinity()}) {
        options.regularization_exponent = n;
        EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
    }
    options.regularization_exponent = 1.5;
    options.steps_per_orbit = 0;
    EXPECT_THROW(validate_hpop_options(options), std::invalid_argument);
    options.steps_per_orbit = 120;
    CartesianState escape{Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 15000, 0}};
    EXPECT_NO_THROW((void)make_hpop(escape, options));
    options.time_regularization = true;
    EXPECT_THROW((void)make_hpop(escape, options), std::invalid_argument);
}
} // namespace

TEST(SundmanBounded, ReportingGatesDoNotSeedArtificialClustersOfAdaptiveNodes) {
    for (auto method :
         {HpopIntegrator::rkf78, HpopIntegrator::rkv89, HpopIntegrator::bulirsch_stoer}) {
        HpopOptions options;
        options.integrator = method;
        const auto source = initial(0);
        SundmanPropagation propagation(source, mu, 1.5, 8192, options);
        auto state = propagation.initial_state();
        std::size_t attempts = 0, published = 0;
        for (double end : {.333333333, .666666666, 1.}) {
            state = propagation.propagate(
                state, end, [](double t, const Cartesian &s) { return force(0, t, s); }, attempts,
                {},
                [&](double t, const Cartesian &) {
                    ++published;
                    EXPECT_EQ(t, end);
                });
            EXPECT_EQ(state.time(), end);
            EXPECT_EQ(state.next_step, propagation.nominal_step());
        }
        EXPECT_EQ(published, 3U);
    }
}
