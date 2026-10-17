#include <cmath>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/hpop.hpp>
#include <orbitforge/core/propagation/mission.hpp>

using namespace orbitforge::core;
using namespace std::chrono_literals;
namespace {
class LinearCoast final : public IStateProvider {
    CartesianState initial_;

  public:
    explicit LinearCoast(CartesianState initial) : initial_(initial) {}
    CartesianState state_at(Epoch epoch) const override {
        auto result = initial_;
        result.epoch = epoch;
        for (std::size_t i = 0; i < 3; ++i)
            result.position_m[i] += initial_.velocity_mps[i] * epoch.seconds_since(initial_.epoch);
        return result;
    }
};
CartesianState initial() {
    return {Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 7500, 0}};
}
CoastFactory linear() {
    return [](CartesianState state, Epoch, const std::function<void()> &) {
        return std::make_shared<LinearCoast>(state);
    };
}
} // namespace
TEST(Mission, SequentialImpulsesAreRightContinuousAndQueriesIndependent) {
    MissionPlan plan{MissionCoastModel::two_body,
                     {MissionSegment::coast(10s),
                      MissionSegment::impulse({10, 20, 30}, ManeuverFrame::gcrf, "First"),
                      MissionSegment::impulse({1, 2, 3}, ManeuverFrame::gcrf, "Second"),
                      MissionSegment::coast(20s)}};
    const auto provider = make_mission(initial(), plan, linear());
    const auto last = provider->state_at(Epoch{30'000'000'000});
    EXPECT_EQ(last.position_m, (std::array{7e6 + 220, 225000.0 + 440, 660.0}));
    const auto at = provider->state_at(Epoch{10'000'000'000});
    EXPECT_EQ(at.position_m, (std::array{7e6, 75000.0, 0.0}));
    EXPECT_EQ(at.velocity_mps, (std::array{11.0, 7522.0, 33.0}));
    EXPECT_EQ(provider->state_at(Epoch{9'999'999'999}).velocity_mps, initial().velocity_mps);
    EXPECT_EQ(provider->state_at(Epoch{0}).position_m, initial().position_m);
    EXPECT_EQ(provider->discontinuities(), (std::vector{Epoch{10'000'000'000}}));
    const auto events = provider->maneuvers(Epoch{0}, last.epoch);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].segment_index, 1U);
    EXPECT_EQ(events[1].before.velocity_mps, events[0].after.velocity_mps);
    EXPECT_NEAR(events[1].cumulative_delta_v_mps, std::sqrt(1400.0) + std::sqrt(14.0), 1e-12);
    auto query = std::async(std::launch::async, [&] { return provider->state_at(last.epoch); });
    EXPECT_EQ(query.get().position_m, last.position_m);
    EXPECT_THROW((void)provider->state_at(Epoch{-1}), PropagationError);
    EXPECT_THROW((void)provider->state_at(Epoch{30'000'000'001}), PropagationError);
}
TEST(Mission, LocalAxesFollowStkVncAndLvlhForNonCircularState) {
    auto state = initial();
    state.velocity_mps = {3000, 4000, 0};
    const auto vnc = apply_impulse(state, {10, 20, 30}, ManeuverFrame::vnc);
    // V=(.6,.8,0), N=(0,0,1), C=(.8,-.6,0).
    EXPECT_NEAR(vnc.velocity_mps[0] - 3000, 30, 1e-12);
    EXPECT_NEAR(vnc.velocity_mps[1] - 4000, -10, 1e-12);
    EXPECT_DOUBLE_EQ(vnc.velocity_mps[2], 20);
    const auto lvlh = apply_impulse(state, {10, 20, 30}, ManeuverFrame::lvlh);
    EXPECT_EQ(lvlh.velocity_mps, (std::array{3010.0, 4020.0, 30.0}));
    EXPECT_EQ(lvlh.position_m, state.position_m);
    state.velocity_mps = {1, 0, 0};
    EXPECT_THROW((void)apply_impulse(state, {1, 0, 0}, ManeuverFrame::vnc), PropagationError);
    EXPECT_NO_THROW((void)apply_impulse(state, {0, 1, 0}, ManeuverFrame::gcrf));
}
TEST(Mission, BoundaryBurnsAndCancellationRetainConsistentCache) {
    MissionPlan plan{MissionCoastModel::two_body,
                     {MissionSegment::impulse({100, 0, 0}), MissionSegment::coast(10s),
                      MissionSegment::impulse({200, 0, 0})}};
    const auto provider = make_mission(initial(), plan, linear());
    EXPECT_THROW(
        (void)provider->state_at(Epoch{10'000'000'000}, [] { throw std::runtime_error("cancel"); }),
        std::runtime_error);
    int calls = 0;
    EXPECT_THROW((void)provider->state_at(Epoch{10'000'000'000},
                                          [&] {
                                              if (++calls == 5)
                                                  throw std::runtime_error("cancel mid-sequence");
                                          }),
                 std::runtime_error);
    const auto fresh = make_mission(initial(), plan, linear());
    const auto final = provider->state_at(Epoch{10'000'000'000});
    EXPECT_EQ(final.velocity_mps, fresh->state_at(final.epoch).velocity_mps);
    EXPECT_EQ(provider->maneuvers(Epoch{0}, final.epoch).size(), 2U);
    EXPECT_DOUBLE_EQ(provider->state_at(Epoch{0}).velocity_mps[1], 7600);
    EXPECT_DOUBLE_EQ(provider->maneuvers(final.epoch, final.epoch)[0].cumulative_delta_v_mps, 300);
}
TEST(Mission, ValidatesDurationsLimitsOverflowAndFiniteBurns) {
    MissionPlan plan;
    EXPECT_THROW((void)validate_mission(Epoch{0}, plan), std::invalid_argument);
    plan.segments = {MissionSegment::coast(0s)};
    EXPECT_THROW((void)validate_mission(Epoch{0}, plan), std::invalid_argument);
    plan.segments = {MissionSegment::coast(1s)};
    EXPECT_THROW((void)validate_mission(Epoch{std::numeric_limits<std::int64_t>::max()}, plan),
                 std::invalid_argument);
    EXPECT_NO_THROW((void)validate_mission(Epoch{std::numeric_limits<std::int64_t>::min()}, plan));
    plan.segments.push_back(
        MissionSegment::impulse({std::numeric_limits<double>::infinity(), 0, 0}));
    EXPECT_THROW((void)validate_mission(Epoch{0}, plan), std::invalid_argument);
    plan.segments = {MissionSegment::coast(std::chrono::hours{367 * 24})};
    EXPECT_THROW((void)validate_mission(Epoch{0}, plan), std::invalid_argument);
    plan.segments.assign(257, MissionSegment::coast(1s));
    EXPECT_THROW((void)validate_mission(Epoch{0}, plan), std::invalid_argument);
}
TEST(Mission, HpopPointMassHohmannTransferMatchesAnalyticApoapsisAndCircularization) {
    for (bool vop : {false, true})
        for (bool regularized : {false, true}) {
            SCOPED_TRACE(vop);

            constexpr double r1 = 7e6, r2 = 8e6;
            const double a = (r1 + r2) / 2;
            const double transfer_time = std::numbers::pi * std::sqrt(a * a * a / egm96_mu);
            const auto transfer = std::chrono::nanoseconds{std::llround(transfer_time * 1e9)};
            const double dv1 = std::sqrt(egm96_mu * (2 / r1 - 1 / a)) - std::sqrt(egm96_mu / r1);
            const double dv2 = std::sqrt(egm96_mu / r2) - std::sqrt(egm96_mu * (2 / r2 - 1 / a));
            auto start = initial();
            start.velocity_mps = {0, std::sqrt(egm96_mu / r1), 0};
            HpopOptions options;
            options.gravity_degree = options.gravity_order = 0;
            MissionPlan plan{MissionCoastModel::hpop,
                             {MissionSegment::impulse({dv1, 0, 0}), MissionSegment::coast(transfer),
                              MissionSegment::impulse({dv2, 0, 0}), MissionSegment::coast(3600s)}};
            for (auto integrator : {HpopIntegrator::rkf78, HpopIntegrator::bulirsch_stoer,
                                    HpopIntegrator::gauss_jackson}) {
                options.integrator = integrator;
                options.vop = vop;
                options.time_regularization = regularized;
                options.steps_per_orbit = 480;
                const auto provider = make_mission(
                    start, plan,
                    [options](CartesianState state, Epoch, const std::function<void()> &) {
                        return make_hpop(state, options);
                    });
                const auto apo = provider->state_at(Epoch{transfer.count()});
                EXPECT_NEAR(apo.position_m[0], -r2, 0.02);
                EXPECT_NEAR(apo.position_m[1], 0, 0.02);
                EXPECT_NEAR(apo.velocity_mps[0], 0, 2e-5);
                EXPECT_NEAR(apo.velocity_mps[1], -std::sqrt(egm96_mu / r2), 2e-5);
                const auto later = provider->state_at(Epoch{transfer.count() + 3600'000'000'000LL});
                EXPECT_NEAR(
                    std::hypot(later.position_m[0], later.position_m[1], later.position_m[2]), r2,
                    0.05);
                EXPECT_NEAR(
                    provider->maneuvers(start.epoch, later.epoch).back().cumulative_delta_v_mps,
                    dv1 + dv2, 1e-9);
            }
        }
}
