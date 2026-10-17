#include <algorithm>
#include <cmath>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <map>
#include <orbitforge/core/propagation/circular_two_body.hpp>
#include <orbitforge/core/propagation/tabulated_orbit.hpp>
#include <sstream>
using namespace orbitforge::core;
namespace {
constexpr std::int64_t base = 8'000'000'000'000'000'123;
constexpr double mu = 3.986004415e14;
double distance(const std::array<double, 3> &a, const std::array<double, 3> &b) {
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}
} // namespace
TEST(VopInterpolation, AllOrdersConicsAndIndependentMuMatchOrekitHipparchus) {
    std::ifstream input(ORBITFORGE_VOP_INTERPOLATION_REFERENCE);
    ASSERT_TRUE(input);
    std::string line;
    OrbitEphemerisArc nodes;
    OrbitInterpolationOptions options{OrbitInterpolationMethod::vop, 7, mu};
    std::shared_ptr<const IStateProvider> provider, plain;
    int kind = 0, step = 0;
    std::size_t count = 0, cases = 0, queries = 0;
    std::map<int, std::array<double, 3>> errors;
    double max_r = 0, max_v = 0;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row(line);
        char tag;
        row >> tag;
        if (tag == 'C') {
            row >> kind >> options.order >> step >> options.vop_mu >> count;
            ASSERT_TRUE(row);
            nodes.clear();
            provider.reset();
            plain.reset();
            ++cases;
        } else {
            std::int64_t ns;
            row >> ns;
            CartesianState expected{Epoch{base + ns}, OriginId::earth, FrameId::gcrf, {}, {}};
            for (auto &v : expected.position_m)
                row >> v;
            for (auto &v : expected.velocity_mps)
                row >> v;
            ASSERT_TRUE(row);
            if (tag == 'N') {
                nodes.push_back(expected);
                continue;
            }
            ASSERT_EQ(tag, 'Q');
            ASSERT_EQ(nodes.size(), count);
            CartesianState truth = expected;
            for (auto &v : truth.position_m)
                row >> v;
            for (auto &v : truth.velocity_mps)
                row >> v;
            ASSERT_TRUE(row);
            if (!provider)
                provider = make_tabulated_orbit({nodes}, options);
            SCOPED_TRACE("kind=" + std::to_string(kind) + " order=" +
                         std::to_string(options.order) + " query=" + std::to_string(ns));
            const auto actual = provider->state_at(expected.epoch);
            const double dr = distance(actual.position_m, expected.position_m);
            const double dv = distance(actual.velocity_mps, expected.velocity_mps);
            max_r = std::max(max_r, dr);
            max_v = std::max(max_v, dv);
            EXPECT_LT(dr, 2e-4);
            EXPECT_LT(dv, 2e-7);
            EXPECT_EQ(actual.epoch, expected.epoch);
            EXPECT_EQ(actual.frame, FrameId::gcrf);
            if (kind < 3) {
                EXPECT_LT(distance(actual.position_m, truth.position_m), 2e-4);
                EXPECT_LT(distance(actual.velocity_mps, truth.velocity_mps), 2e-7);
            }
            if (kind == 5) {
                if (!plain)
                    plain = make_tabulated_orbit({nodes}, {OrbitInterpolationMethod::lagrange, 7});
                auto &e = errors[step];
                e[0] = std::max(e[0], distance(actual.position_m, truth.position_m));
                e[1] = std::max(e[1], distance(actual.velocity_mps, truth.velocity_mps));
                e[2] = std::max(
                    e[2], distance(plain->state_at(expected.epoch).position_m, truth.position_m));
            }
            ++queries;
        }
    }
    EXPECT_EQ(cases, 153U);
    EXPECT_EQ(queries, 1059U);
    ASSERT_EQ(errors.size(), 3U);
    for (std::size_t axis = 0; axis < 2; ++axis) {
        EXPECT_GT(errors[600][axis], 64 * errors[300][axis]);
        EXPECT_GT(errors[300][axis], 64 * errors[150][axis]);
    }
    EXPECT_LT(errors[150][0], .002);
    EXPECT_LT(errors[150][1], 2e-6);
    // VOP is not guaranteed a fixed speedup or accuracy ratio over Cartesian interpolation.
    // The independently checked J2 error and step-halving bounds above are the acceptance gates.
    RecordProperty("j2_lagrange_position_error_step_300", errors[300][2]);
    RecordProperty("max_reference_position_error_m", max_r);
    RecordProperty("max_reference_velocity_error_mps", max_v);
    for (const auto &[h, e] : errors) {
        RecordProperty("j2_position_error_step_" + std::to_string(h), e[0]);
        RecordProperty("j2_velocity_error_step_" + std::to_string(h), e[1]);
    }
}
TEST(VopInterpolation, InertialFramesZeroOrderGapsManeuversAndCancellation) {
    const CircularTwoBody source{Length{7e6}, GravitationalParameter{mu}, Epoch{base}};
    OrbitEphemerisArc before, after, later;
    for (int t : {0, 600, 1200})
        before.push_back(source.state_at(Epoch{base + t * 1'000'000'000LL}));
    after.push_back(before.back());
    after.push_back(source.state_at(Epoch{base + 1800'000'000'000LL}));
    for (auto &s : after)
        s.velocity_mps[2] += 5;
    later.push_back(source.state_at(Epoch{base + 2400'000'000'000LL}));
    later.push_back(source.state_at(Epoch{base + 3000'000'000'000LL}));
    for (auto frame : {FrameId::gcrf, FrameId::spice_j2000}) {
        for (auto *arc : {&before, &after, &later})
            for (auto &s : *arc)
                s.frame = frame;
        for (int order : {0, 1}) {
            const auto p = make_tabulated_orbit({before, after, later},
                                                {OrbitInterpolationMethod::vop, order, mu});
            for (const auto &node : before)
                if (node.epoch != before.back().epoch) {
                    EXPECT_EQ(p->state_at(node.epoch).position_m, node.position_m);
                }
            EXPECT_EQ(p->state_at(after.front().epoch).velocity_mps, after.front().velocity_mps);
            EXPECT_THROW((void)p->state_at(Epoch{base - 1}), PropagationError);
            EXPECT_THROW((void)p->state_at(Epoch{base + 2100'000'000'000LL}), PropagationError);
            const Epoch query{base + 599'123'456'789LL};
            EXPECT_LT(distance(p->state_at(query).position_m, source.state_at(query).position_m),
                      1e-7);
            EXPECT_EQ(p->state_at(query).frame, frame);
            int calls = 0;
            EXPECT_THROW((void)p->state_at(query,
                                           [&] {
                                               if (++calls == 4)
                                                   throw std::runtime_error("cancel transport");
                                           }),
                         std::runtime_error);
            EXPECT_EQ(calls, 4);
            auto concurrent = std::async(std::launch::async, [&] { return p->state_at(query); });
            EXPECT_EQ(concurrent.get().position_m, p->state_at(query).position_m);
            EXPECT_EQ(p->discontinuities().size(), order == 0 ? 5U : 3U);
        }
    }
    const OrbitInterpolationOptions vop{OrbitInterpolationMethod::vop, 1, mu};
    for (auto frame : {FrameId::itrf, FrameId::teme}) {
        auto nodes = before;
        for (auto &s : nodes)
            s.frame = frame;
        EXPECT_THROW((void)make_tabulated_orbit({nodes}, vop), std::invalid_argument);
    }
    for (double bad : {0., -mu, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()}) {
        auto options = vop;
        options.vop_mu = bad;
        EXPECT_THROW((void)make_tabulated_orbit({before}, options), std::invalid_argument);
        options.method = OrbitInterpolationMethod::hermite;
        options.order = 3;
        EXPECT_THROW((void)make_tabulated_orbit({before}, options), std::invalid_argument);
    }
    auto radial = before;
    radial[0].velocity_mps = {100, 0, 0};
    EXPECT_THROW((void)make_tabulated_orbit({radial}, vop), std::invalid_argument);
    EXPECT_THROW((void)make_tabulated_orbit(
                     {before}, vop, [] { throw std::runtime_error("cancel construction"); }),
                 std::runtime_error);
}
