#include <cmath>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <orbitforge/core/propagation/circular_two_body.hpp>
#include <orbitforge/core/propagation/tabulated_orbit.hpp>
#include <sstream>

using namespace orbitforge::core;
namespace {
constexpr std::int64_t base = 8'000'000'000'000'000'000LL;
CartesianState polynomial(std::int64_t ns, double offset = 0) {
    const double t = static_cast<double>(ns) / 1e9;
    return {Epoch{base + ns},
            OriginId::earth,
            FrameId::gcrf,
            {1e6 + 3 * t - 2 * t * t + t * t * t + offset, 2e6 + t * t, -3e6 + 5 * t},
            {3 - 4 * t + 3 * t * t, 2 * t, 5}};
}
OrbitEphemerisArc arc(std::initializer_list<std::int64_t> seconds, double offset = 0) {
    OrbitEphemerisArc result;
    for (auto t : seconds)
        result.push_back(polynomial(t * 1'000'000'000, offset));
    return result;
}
} // namespace
TEST(OrbitInterpolation, AllOrdersMatchIndependentHipparchusReference) {
    std::ifstream input(ORBITFORGE_INTERPOLATION_REFERENCE);
    ASSERT_TRUE(input);
    std::string line;
    OrbitEphemerisArc nodes;
    OrbitInterpolationOptions options;
    std::shared_ptr<const IStateProvider> provider;
    std::size_t count = 0, cases = 0, queries = 0;
    double max_r = 0, max_v = 0;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#')
            continue;
        std::istringstream row(line);
        char tag;
        row >> tag;
        if (tag == 'C') {
            int kind, method;
            row >> kind >> method >> options.order >> count;
            ASSERT_TRUE(row);
            options.method =
                method ? OrbitInterpolationMethod::hermite : OrbitInterpolationMethod::lagrange;
            nodes.clear();
            provider.reset();
            ++cases;
        } else {
            std::int64_t ns;
            row >> ns;
            CartesianState state{Epoch{base + ns}, OriginId::earth, FrameId::gcrf, {}, {}};
            for (auto &v : state.position_m)
                row >> v;
            for (auto &v : state.velocity_mps)
                row >> v;
            ASSERT_TRUE(row);
            if (tag == 'N') {
                nodes.push_back(state);
            } else {
                ASSERT_EQ(tag, 'Q');
                ASSERT_EQ(nodes.size(), count);
                if (!provider)
                    provider = make_tabulated_orbit({nodes}, options);
                SCOPED_TRACE(line + " order " + std::to_string(options.order));
                const auto result = provider->state_at(state.epoch);
                for (std::size_t i = 0; i < 3; ++i) {
                    max_r = std::max(max_r, std::abs(result.position_m[i] - state.position_m[i]));
                    max_v =
                        std::max(max_v, std::abs(result.velocity_mps[i] - state.velocity_mps[i]));
                    EXPECT_NEAR(result.position_m[i], state.position_m[i], 2e-5);
                    EXPECT_NEAR(result.velocity_mps[i], state.velocity_mps[i], 2e-7);
                }
                ++queries;
            }
        }
    }
    EXPECT_EQ(cases, 118U);
    EXPECT_EQ(queries, 826U);
    RecordProperty("max_position_error_m", max_r);
    RecordProperty("max_velocity_error_mps", max_v);
}
TEST(OrbitInterpolation, HermiteReproducesCubicAtLargeEpochIncludingOneNanosecond) {
    const auto nodes = arc({-3, 0, 2, 7});
    for (int order : {3, 5, 7}) {
        const auto provider =
            make_tabulated_orbit({nodes}, {OrbitInterpolationMethod::hermite, order});
        for (auto ns : {-2'000'000'001LL, -1LL, 0LL, 1LL, 2'000'000'001LL, 6'123'456'789LL}) {
            const auto expected = polynomial(ns), actual = provider->state_at(expected.epoch);
            EXPECT_EQ(actual.epoch, expected.epoch);
            for (std::size_t i = 0; i < 3; ++i) {
                EXPECT_NEAR(actual.position_m[i], expected.position_m[i], 2e-9);
                EXPECT_NEAR(actual.velocity_mps[i], expected.velocity_mps[i], 2e-9);
            }
        }
    }
}
TEST(OrbitInterpolation, KeepsExactNodesAndInterpolatesLagrangeVelocityIndependently) {
    auto nodes = arc({0, 2, 4, 7});
    for (auto &node : nodes)
        node.velocity_mps = {100, 200, 300};
    for (const auto method :
         {OrbitInterpolationMethod::lagrange, OrbitInterpolationMethod::hermite}) {
        const auto provider = make_tabulated_orbit({nodes}, {method, 3});
        for (const auto &node : nodes) {
            EXPECT_EQ(provider->state_at(node.epoch).position_m, node.position_m);
            EXPECT_EQ(provider->state_at(node.epoch).velocity_mps, node.velocity_mps);
        }
    }
    const auto provider = make_tabulated_orbit({nodes}, {OrbitInterpolationMethod::lagrange, 3});
    const auto result = provider->state_at(Epoch{base + 3'000'000'000});
    EXPECT_NEAR(result.position_m[0], polynomial(3'000'000'000).position_m[0], 1e-9);
    EXPECT_EQ(result.velocity_mps, nodes[0].velocity_mps);
}
TEST(OrbitInterpolation, WindowsUseNearestContiguousNodesWithEarlierTie) {
    auto nodes = arc({0, 2, 4, 6, 8});
    nodes[0].position_m[0] += 500;
    nodes[4].velocity_mps[1] += 200;
    for (const auto method :
         {OrbitInterpolationMethod::lagrange, OrbitInterpolationMethod::hermite}) {
        // Lagrange 1 and Hermite 3 each select two nodes; 3 s ties 2 and 4 s.
        const int order = method == OrbitInterpolationMethod::lagrange ? 1 : 3;
        const auto all = make_tabulated_orbit({nodes}, {method, order});
        const auto middle = make_tabulated_orbit({{nodes[1], nodes[2]}}, {method, order});
        const auto time = Epoch{base + 3'000'000'000};
        EXPECT_EQ(all->state_at(time).position_m, middle->state_at(time).position_m);
        EXPECT_EQ(all->state_at(time).velocity_mps, middle->state_at(time).velocity_mps);
    }
    // Three-node tie at 3 s chooses 0,2,4 rather than 2,4,6.
    const auto all = make_tabulated_orbit({nodes}, {OrbitInterpolationMethod::lagrange, 2});
    const auto earlier = make_tabulated_orbit({{nodes[0], nodes[1], nodes[2]}},
                                              {OrbitInterpolationMethod::lagrange, 2});
    EXPECT_EQ(all->state_at(Epoch{base + 3'000'000'000}).position_m,
              earlier->state_at(Epoch{base + 3'000'000'000}).position_m);
}
TEST(OrbitInterpolation, ArcsAreOwnedRightContinuousAndNeverBridgeGapsOrManeuvers) {
    auto before = arc({0, 2}), after = arc({2, 4}, 1000), later = arc({6, 8}, -500);
    const auto provider =
        make_tabulated_orbit({before, after, later}, {OrbitInterpolationMethod::hermite, 3});
    before[0].position_m[0] = -1;
    EXPECT_EQ(provider->state_at(Epoch{base}).position_m, polynomial(0).position_m);
    for (auto ns :
         {1'999'999'999LL, 2'000'000'000LL, 2'000'000'001LL, 4'000'000'000LL, 6'000'000'000LL}) {
        const auto expected = polynomial(ns, ns < 2'000'000'000    ? 0
                                             : ns <= 4'000'000'000 ? 1000
                                                                   : -500);
        EXPECT_NEAR(provider->state_at(expected.epoch).position_m[0], expected.position_m[0], 2e-9);
    }
    EXPECT_EQ(provider->discontinuities(),
              (std::vector<Epoch>{Epoch{base + 2'000'000'000}, Epoch{base + 4'000'000'000},
                                  Epoch{base + 6'000'000'000}}));
    for (auto ns : {-1LL, 4'000'000'001LL, 5'000'000'000LL, 5'999'999'999LL, 8'000'000'001LL}) {
        EXPECT_THROW((void)provider->state_at(Epoch{base + ns}), PropagationError);
    }
}
TEST(OrbitInterpolation, ZeroOrderHoldsLeftAndReportsEveryJump) {
    auto nodes = arc({0, 2, 4});
    const auto provider = make_tabulated_orbit({nodes}, {OrbitInterpolationMethod::lagrange, 0});
    EXPECT_EQ(provider->state_at(Epoch{base + 1'999'999'999}).position_m, nodes[0].position_m);
    EXPECT_EQ(provider->state_at(nodes[1].epoch).position_m, nodes[1].position_m);
    EXPECT_EQ(provider->discontinuities(), (std::vector<Epoch>{nodes[1].epoch, nodes[2].epoch}));
    const auto single = make_tabulated_orbit({{nodes[0]}}, {OrbitInterpolationMethod::lagrange, 0});
    EXPECT_EQ(single->state_at(nodes[0].epoch).position_m, nodes[0].position_m);
    EXPECT_THROW((void)single->state_at(Epoch{base + 1}), PropagationError);
}
TEST(OrbitInterpolation, RejectsInvalidInputsWithoutReducingOrder) {
    const auto good = arc({0, 1, 2, 3});
    for (int order : {-1, 30, 2, 4}) {
        EXPECT_THROW((void)make_tabulated_orbit({good}, {OrbitInterpolationMethod::hermite, order}),
                     std::invalid_argument);
    }
    EXPECT_THROW((void)make_tabulated_orbit({good}, {static_cast<OrbitInterpolationMethod>(9), 3}),
                 std::invalid_argument);
    EXPECT_THROW((void)make_tabulated_orbit({good}, {OrbitInterpolationMethod::lagrange, 4}),
                 std::invalid_argument);
    EXPECT_THROW((void)make_tabulated_orbit({good}, {OrbitInterpolationMethod::hermite, 9}),
                 std::invalid_argument);
    EXPECT_THROW((void)make_tabulated_orbit({}), std::invalid_argument);
    EXPECT_THROW((void)make_tabulated_orbit({{}}), std::invalid_argument);
    EXPECT_THROW((void)make_tabulated_orbit({good, arc({2, 3, 4, 5})}), std::invalid_argument);
    EXPECT_THROW((void)make_tabulated_orbit({good, {}}), std::invalid_argument);
    for (int bad = 0; bad < 7; ++bad) {
        auto nodes = good;
        if (bad == 0)
            nodes[1].epoch = nodes[0].epoch;
        if (bad == 1)
            std::swap(nodes[1], nodes[2]);
        if (bad == 2)
            nodes[1].position_m[0] = std::numeric_limits<double>::quiet_NaN();
        if (bad == 3)
            nodes[1].velocity_mps[0] = std::numeric_limits<double>::infinity();
        if (bad == 4)
            nodes[1].frame = FrameId::itrf;
        if (bad == 5)
            for (auto &n : nodes)
                n.frame = static_cast<FrameId>(99);
        if (bad == 6)
            for (auto &n : nodes)
                n.origin = static_cast<OriginId>(99);
        EXPECT_THROW((void)make_tabulated_orbit({nodes}), std::invalid_argument);
    }
    // Distinct int64 times can collapse in a huge floating window; never silently alias nodes.
    auto nodes = arc({0, 1, 2});
    nodes[0].epoch = Epoch{0};
    nodes[1].epoch = Epoch{1};
    nodes[2].epoch = Epoch{9'000'000'000'000'000'000};
    const auto provider = make_tabulated_orbit({nodes}, {OrbitInterpolationMethod::lagrange, 2});
    EXPECT_THROW((void)provider->state_at(Epoch{4'500'000'000'000'000'000}), PropagationError);
}
TEST(OrbitInterpolation, CancellationAndConcurrentQueriesCannotChangeResults) {
    const auto nodes = arc({-3, 0, 2, 7});
    EXPECT_THROW(
        (void)make_tabulated_orbit({nodes}, {}, [] { throw std::runtime_error("cancel prepare"); }),
        std::runtime_error);
    const auto provider = make_tabulated_orbit({nodes});
    const Epoch time{base + 1'234'567'890};
    const auto expected = provider->state_at(time);
    int calls = 0;
    EXPECT_THROW((void)provider->state_at(time,
                                          [&] {
                                              if (++calls == 2)
                                                  throw std::runtime_error("cancel result");
                                          }),
                 std::runtime_error);
    auto other =
        std::async(std::launch::async, [&] { return provider->state_at(Epoch{base - 1}); });
    EXPECT_EQ(provider->state_at(time).position_m, expected.position_m);
    EXPECT_EQ(other.get().position_m, provider->state_at(Epoch{base - 1}).position_m);
}
TEST(OrbitInterpolation, CircularOrbitErrorConvergesWithStepAndOrder) {
    const CircularTwoBody source{Length{7e6}, GravitationalParameter{3.986004415e14}, Epoch{0}};
    for (auto method : {OrbitInterpolationMethod::lagrange, OrbitInterpolationMethod::hermite}) {
        double coarse = 0, fine = 0, low = 0;
        for (int h : {120, 60}) {
            OrbitEphemerisArc nodes;
            for (int t = -1200; t <= 1200; t += h)
                nodes.push_back(source.state_at(Epoch{t * 1'000'000'000LL}));
            for (int order : {3, 7}) {
                const auto provider = make_tabulated_orbit({nodes}, {method, order});
                double error = 0;
                for (auto ns : {-37'123'456'789LL, 17'123'456'789LL, 91'123'456'789LL}) {
                    const auto expected = source.state_at(Epoch{ns}),
                               actual = provider->state_at(Epoch{ns});
                    error =
                        std::max(error, std::hypot(actual.position_m[0] - expected.position_m[0],
                                                   actual.position_m[1] - expected.position_m[1],
                                                   actual.position_m[2] - expected.position_m[2]));
                }
                if (order == 7 && h == 120)
                    coarse = error;
                if (order == 7 && h == 60)
                    fine = error;
                if (order == 3 && h == 120)
                    low = error;
            }
        }
        EXPECT_LT(fine, 1e-4);
        EXPECT_LT(fine, coarse / 64);
        EXPECT_LT(coarse, low / 1000);
        RecordProperty(method == OrbitInterpolationMethod::lagrange ? "lagrange_fine_m"
                                                                    : "hermite_fine_m",
                       fine);
    }
}

TEST(OrbitInterpolation, UnevenSamplingKeepsBothBracketingNodes) {
    auto nodes = arc({0, 1, 100, 101});
    nodes[0].velocity_mps[0] = 1e6;
    for (auto method : {OrbitInterpolationMethod::lagrange, OrbitInterpolationMethod::hermite}) {
        const int order = method == OrbitInterpolationMethod::lagrange ? 1 : 3;
        const auto actual = make_tabulated_orbit({nodes}, {method, order});
        const auto bracket = make_tabulated_orbit({{nodes[1], nodes[2]}}, {method, order});
        for (auto ns : {2'000'000'000LL, 99'000'000'000LL}) {
            EXPECT_EQ(actual->state_at(Epoch{base + ns}).position_m,
                      bracket->state_at(Epoch{base + ns}).position_m);
            EXPECT_EQ(actual->state_at(Epoch{base + ns}).velocity_mps,
                      bracket->state_at(Epoch{base + ns}).velocity_mps);
        }
    }
}
TEST(OrbitInterpolation, ImpulsiveVelocityUsesSeparatePolynomialsOnEitherSide) {
    auto before = arc({0, 2}), after = arc({2, 4});
    for (auto &node : after) {
        node.position_m[0] += 100 * node.epoch.seconds_since(after.front().epoch);
        node.velocity_mps[0] += 100;
    }
    const auto provider =
        make_tabulated_orbit({before, after}, {OrbitInterpolationMethod::hermite, 3});
    EXPECT_EQ(provider->state_at(after.front().epoch).position_m, before.back().position_m);
    EXPECT_EQ(provider->state_at(after.front().epoch).velocity_mps, after.front().velocity_mps);
    for (auto ns : {2'000'000'000LL - 1, 2'000'000'000LL + 1}) {
        auto expected = polynomial(ns);
        if (ns > 2'000'000'000LL) {
            expected.position_m[0] += 100 * static_cast<double>(ns - 2'000'000'000LL) / 1e9;
            expected.velocity_mps[0] += 100;
        }
        const auto actual = provider->state_at(expected.epoch);
        EXPECT_NEAR(actual.position_m[0], expected.position_m[0], 2e-9);
        EXPECT_NEAR(actual.velocity_mps[0], expected.velocity_mps[0], 2e-9);
    }
}
