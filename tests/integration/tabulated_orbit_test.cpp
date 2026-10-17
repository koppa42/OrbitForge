#include <cmath>
#include <gtest/gtest.h>
#include <orbitforge/application/scenario.hpp>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/hpop.hpp>
using namespace orbitforge;
TEST(TabulatedOrbit, HpopSamplesUseSharedSessionAndScenarioWithoutChangingSource) {
    core::HpopOptions hpop;
    hpop.gravity_degree = hpop.gravity_order = 0;
    const core::CartesianState initial{core::Epoch{0},
                                       core::OriginId::earth,
                                       core::FrameId::gcrf,
                                       {7e6, 0, 0},
                                       {0, std::sqrt(core::egm96_mu / 7e6), 0}};
    const auto source = runtime::make_hpop_session(initial, hpop, {}, "original", 12);
    std::vector<core::Epoch> times;
    for (int t = -600; t <= 600; t += 60)
        times.emplace_back(t * 1'000'000'000LL);
    const auto samples = source.sample(times);
    for (auto method : {OrbitInterpolationMethod::lagrange, OrbitInterpolationMethod::hermite}) {
        auto session =
            make_tabulated_orbit_session({samples.states()}, {method, 7}, "interpolated", 13);
        const auto scenario =
            application::Scenario{"sampled", times.front(), times.back()}.with_object(
                make_tabulated_orbit_session({samples.states()}, {method, 7}, "scenario"));
        const std::array query{core::Epoch{-582'123'456'789}, core::Epoch{17'123'456'789},
                               core::Epoch{582'123'456'789}};
        const auto result = session.sample(query), direct = source.sample(query);
        EXPECT_EQ(result.provenance().object_id, "interpolated");
        EXPECT_EQ(result.provenance().input_revision, 13U);
        EXPECT_NE(result.provenance().algorithm.find("order=7"), std::string::npos);
        EXPECT_NE(result.provenance().algorithm.find(method == OrbitInterpolationMethod::hermite
                                                         ? "hermite-posvel"
                                                         : "lagrange-independent"),
                  std::string::npos);
        for (std::size_t n = 0; n < query.size(); ++n)
            for (std::size_t axis = 0; axis < 3; ++axis) {
                EXPECT_NEAR(result.states()[n].position_m[axis],
                            direct.states()[n].position_m[axis], 1e-3);
                EXPECT_NEAR(result.states()[n].velocity_mps[axis],
                            direct.states()[n].velocity_mps[axis], 1e-5);
            }
        EXPECT_EQ(scenario.sample(query).front().states()[1].position_m,
                  result.states()[1].position_m);
        EXPECT_THROW((void)session.sample(query, [] { throw std::runtime_error("cancel"); }),
                     std::runtime_error);
        EXPECT_EQ(session.sample(query).states()[1].position_m, result.states()[1].position_m);
    }
    EXPECT_EQ(source.provenance().object_id, "original");
    EXPECT_EQ(source.sample(times).states().back().position_m, samples.states().back().position_m);
}
