#include <cmath>
#include <future>
#include <gtest/gtest.h>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/report.hpp>
#include <orbitforge/runtime/two_body.hpp>

using namespace orbitforge;
namespace {
class BoundedMoon final : public core::IThirdBodyEphemeris {
    core::ThirdBodyDataInfo info_;

  public:
    mutable std::size_t calls = 0;
    bool queries_disabled = false;
    BoundedMoon(core::Epoch first, core::Epoch last) {
        info_.model = "bounded-test-moon";
        info_.content_sha256 = std::string(64, 'a');
        info_.interpretation_version = "test-v1";
        info_.first = first;
        info_.last = last;
        info_.moon_mu = 4.902800066e12;
    }
    std::array<double, 3> position(core::ThirdBody, core::Epoch epoch) const override {
        if (queries_disabled || epoch.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            epoch.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw core::PropagationError("Force query outside requested coast");
        ++calls;
        return {1e12, 2e11, 1e11};
    }
    void validate_interval(core::Epoch first, core::Epoch last,
                           std::span<const core::ThirdBody>) const override {
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw core::PropagationError("Requested coverage outside coast");
    }
    const core::ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
};
} // namespace
TEST(HpopEphemerisSession, BothInterpolatorsMatchCspiceForCircularAndEccentricCoasts) {
    for (bool regularized : {false, true}) {
        SCOPED_TRACE(regularized);
        if (!runtime::two_body_available()) {
            GTEST_SKIP();
        }
        double max_r = 0, max_v = 0;
        for (double e : {0., .72}) {
            const double radius = 7e6, speed = std::sqrt(core::egm96_mu * (1 + e) / radius);
            const core::CartesianState initial{core::Epoch{0},
                                               core::OriginId::earth,
                                               core::FrameId::gcrf,
                                               {radius, 0, 0},
                                               {0, speed * std::cos(.4), speed * std::sin(.4)}};
            const auto reference = runtime::make_two_body_session(
                initial, core::GravitationalParameter{core::egm96_mu}, "cspice");
            for (auto integrator :
                 {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                  HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
                for (auto sign : {-1LL, 1LL}) {
                    core::HpopOptions options;
                    options.time_regularization = regularized;
                    options.steps_per_orbit = 8192;
                    options.gravity_degree = options.gravity_order = 0;
                    options.integrator = integrator;
                    options.fixed_step_seconds = integrator == HpopIntegrator::rk4 ? 2 : 10;
                    options.max_step_seconds = 30;
                    options.relative_tolerance = 1e-14;
                    options.position_tolerance_m = 1e-8;
                    options.velocity_tolerance_mps = 1e-11;
                    for (auto method :
                         {OrbitInterpolationMethod::lagrange, OrbitInterpolationMethod::hermite,
                          OrbitInterpolationMethod::vop}) {
                        for (auto report : {0LL, 30'000'000'000LL}) {
                            SCOPED_TRACE(static_cast<int>(integrator));
                            SCOPED_TRACE(e);
                            SCOPED_TRACE(sign);
                            SCOPED_TRACE(report);
                            const auto session = make_hpop_ephemeris_session(
                                initial, core::Epoch{sign * 6000'000'000'000LL}, options, {},
                                {method, 7}, report, "interpolated", 5);
                            for (auto t : {1LL, 123'123'456'789LL, 1822'123'456'789LL,
                                           5990'000'000'001LL, 6000'000'000'000LL}) {
                                const std::array epochs{core::Epoch{sign * t}};
                                const auto actual = session.sample(epochs).states()[0],
                                           expected = reference.sample(epochs).states()[0];
                                const double er =
                                    std::hypot(actual.position_m[0] - expected.position_m[0],
                                               actual.position_m[1] - expected.position_m[1],
                                               actual.position_m[2] - expected.position_m[2]);
                                const double ev =
                                    std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                               actual.velocity_mps[1] - expected.velocity_mps[1],
                                               actual.velocity_mps[2] - expected.velocity_mps[2]);
                                EXPECT_LT(er, .005);
                                EXPECT_LT(ev, 5e-6);
                                max_r = std::max(max_r, er);
                                max_v = std::max(max_v, ev);
                            }
                        }
                    }
                }
            }
        }
        RecordProperty("max_position_error_m", max_r);
        RecordProperty("max_velocity_error_mps", max_v);
    }
}
TEST(HpopEphemerisSession, ImmutableBoundedQueriesHaveExplicitSourceAndCancellation) {
    for (bool regularized : {false, true}) {
        SCOPED_TRACE(regularized);
        core::HpopOptions options;
        options.time_regularization = regularized;
        options.steps_per_orbit = 8192;
        options.gravity_degree = options.gravity_order = 0;
        const core::CartesianState initial{core::Epoch{0},
                                           core::OriginId::earth,
                                           core::FrameId::gcrf,
                                           {7e6, 0, 0},
                                           {0, std::sqrt(core::egm96_mu / 7e6), 0}};
        const auto session = make_hpop_ephemeris_session(
            initial, core::Epoch{600'000'000'000LL}, options, {},
            {OrbitInterpolationMethod::hermite, 7}, 30'000'000'000LL, "ephemeris", 9);
        EXPECT_EQ(session.provenance().object_id, "ephemeris");
        EXPECT_EQ(session.provenance().input_revision, 9U);
        EXPECT_NE(session.provenance().algorithm.find("report-ns=30000000000"), std::string::npos);
        EXPECT_NE(session.provenance().algorithm.find("method=hermite;interpolation-order=7"),
                  std::string::npos);
        const std::array epochs{core::Epoch{123'000'000'001LL}, core::Epoch{0},
                                core::Epoch{600'000'000'000LL}};
        const auto before = session.sample(epochs);
        auto query = std::async(std::launch::async, [&] { return session.sample(epochs); });
        EXPECT_THROW((void)session.sample(epochs, [] { throw std::runtime_error("cancel query"); }),
                     std::runtime_error);
        EXPECT_EQ(query.get().states()[0].position_m, before.states()[0].position_m);
        EXPECT_EQ(session.sample(epochs).states()[0].position_m, before.states()[0].position_m);
        for (auto invalid : {-1LL, 600'000'000'001LL}) {
            EXPECT_THROW((void)session.sample(std::array{core::Epoch{invalid}}),
                         core::PropagationError);
        }
        EXPECT_THROW((void)make_hpop_ephemeris_session(initial, core::Epoch{1'000'000'000LL},
                                                       options, {},
                                                       {OrbitInterpolationMethod::hermite, 7}),
                     std::invalid_argument);
        EXPECT_THROW((void)make_hpop_ephemeris_session(
                         initial, core::Epoch{600'000'000'000LL}, options, {},
                         {OrbitInterpolationMethod::hermite, 7}, 0, "cancel", 1,
                         [] { throw std::runtime_error("cancel creation"); }),
                     std::runtime_error);
    }
}

TEST(HpopEphemerisSession, StagesStayInTheExactCoastAndQueriesNeedNoForceProvider) {
    for (bool regularized : {false, true}) {
        SCOPED_TRACE(regularized);
        for (auto integrator : {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                                HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
            for (auto sign : {-1LL, 1LL}) {
                SCOPED_TRACE(static_cast<int>(integrator));
                SCOPED_TRACE(sign);
                core::CartesianState initial{core::Epoch{8'000'000'000'000'000'000LL},
                                             core::OriginId::earth,
                                             core::FrameId::gcrf,
                                             {1e10, 0, 0},
                                             {0, std::sqrt(core::egm96_mu / 1e10), 0}};
                const core::Epoch end{initial.epoch.tai_nanoseconds() +
                                      sign * (256LL * 86400'000'000'000LL + 3)};
                auto moon = std::make_shared<BoundedMoon>((sign > 0 ? initial.epoch : end),
                                                          (sign > 0 ? end : initial.epoch));
                core::HpopEnvironment environment;
                environment.ephemeris = moon;
                core::HpopOptions options;
                options.time_regularization = regularized;
                options.steps_per_orbit = 8192;
                options.gravity_degree = options.gravity_order = 0;
                options.third_body(core::ThirdBody::moon).enabled = true;
                options.integrator = integrator;
                options.step_control = core::HpopStepControl::fixed;
                options.fixed_step_seconds = 86400;
                for (auto report : {0LL, 86400'000'000'000LL}) {
                    SCOPED_TRACE(report);
                    const auto arcs =
                        propagate_hpop_ephemeris(initial, end, options, environment, report);
                    EXPECT_EQ(arcs.front().front().epoch, (sign > 0 ? initial.epoch : end));
                    EXPECT_EQ(arcs.back().back().epoch, (sign > 0 ? end : initial.epoch));
                }
                // A shorter coast has enough well-spaced points for order seven.
                const core::Epoch short_end{initial.epoch.tai_nanoseconds() +
                                            sign * 600'000'000'003LL};
                options.fixed_step_seconds = 10;
                const auto session = make_hpop_ephemeris_session(
                    initial, short_end, options, environment,
                    {OrbitInterpolationMethod::hermite, 7}, 30'000'000'000LL);
                const auto calls = moon->calls;
                EXPECT_GT(calls, 0U);
                moon->queries_disabled = true;
                const auto result = session.sample(std::array{
                    core::Epoch{initial.epoch.tai_nanoseconds() + sign * 173'123'456'789LL}});
                EXPECT_TRUE(std::isfinite(result.states()[0].position_m[0]));
                EXPECT_EQ(moon->calls, calls);
            }
        }
    }
}

TEST(HpopProjectInterpolation, ShortForwardBackwardAndSplitIntervalsMatchCspice) {
    for (bool regularized : {false, true}) {
        SCOPED_TRACE(regularized);
        if (!runtime::two_body_available()) {
            GTEST_SKIP();
        }
        double max_r = 0, max_v = 0;
        for (auto method : {OrbitInterpolationMethod::lagrange, OrbitInterpolationMethod::hermite,
                            OrbitInterpolationMethod::vop}) {
            for (auto integrator :
                 {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                  HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
                for (auto bounds : {std::pair{-45LL, -30LL},
                                    {-1LL, 1LL},
                                    {30LL, 45LL},
                                    {0LL, 1LL},
                                    {-1LL, 0LL}}) {
                    SCOPED_TRACE(static_cast<int>(method));
                    SCOPED_TRACE(static_cast<int>(integrator));
                    SCOPED_TRACE(bounds.first);
                    SCOPED_TRACE(bounds.second);
                    application::OrbitInput input;
                    input.hpop.time_regularization = regularized;
                    input.hpop.steps_per_orbit = 8192;
                    input.propagator = OrbitPropagator::hpop;
                    input.initial = {Epoch{8'000'000'000'000'000'000LL},
                                     core::OriginId::earth,
                                     FrameId::gcrf,
                                     {7e6, 0, 0},
                                     {0, std::sqrt(core::egm96_mu / 7e6), 0}};
                    input.first =
                        Epoch{input.initial.epoch.tai_nanoseconds() + bounds.first * 1'000'000'000};
                    input.last = Epoch{input.initial.epoch.tai_nanoseconds() +
                                       bounds.second * 1'000'000'000};
                    input.hpop.gravity_degree = input.hpop.gravity_order = 0;
                    input.hpop.integrator = integrator;
                    input.hpop.fixed_step_seconds = 2;
                    input.hpop.interpolation = {method, 7};
                    const auto session = runtime::make_orbit_session(input);
                    EXPECT_NE(session.provenance().algorithm.find("bounded-accepted-support-v1"),
                              std::string::npos);
                    const auto truth = runtime::make_two_body_session(
                        input.initial, core::GravitationalParameter{core::egm96_mu}, "cspice");
                    const auto a = input.first.tai_nanoseconds(), b = input.last.tai_nanoseconds();
                    for (auto ns : {a, a + 1, a + (b - a) / 2, b - 1, b}) {
                        const std::array epochs{Epoch{ns}};
                        const auto actual = session.sample(epochs).states()[0],
                                   expected = truth.sample(epochs).states()[0];
                        const auto r = std::hypot(actual.position_m[0] - expected.position_m[0],
                                                  actual.position_m[1] - expected.position_m[1],
                                                  actual.position_m[2] - expected.position_m[2]);
                        const auto v =
                            std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                       actual.velocity_mps[1] - expected.velocity_mps[1],
                                       actual.velocity_mps[2] - expected.velocity_mps[2]);
                        EXPECT_LT(r, .001);
                        EXPECT_LT(v, 1e-6);
                        max_r = std::max(max_r, r);
                        max_v = std::max(max_v, v);
                    }
                    EXPECT_THROW((void)session.sample(std::array{Epoch{a - 1}}),
                                 core::PropagationError);
                    EXPECT_THROW((void)session.sample(std::array{Epoch{b + 1}}),
                                 core::PropagationError);
                }
            }
        }
        RecordProperty("max_position_error_m", max_r);
        RecordProperty("max_velocity_error_mps", max_v);
    }
}
TEST(HpopProjectInterpolation, MissionBurnsAndStoppedPrefixMatchIndependentCspiceCoasts) {
    for (bool regularized : {false, true}) {
        SCOPED_TRACE(regularized);
        if (!runtime::two_body_available()) {
            GTEST_SKIP();
        }
        using namespace std::chrono_literals;
        const auto burn = 137'123'456'789ns, coast = 8'123'456'789ns;
        const Epoch stop{(burn + coast).count()};
        const core::CartesianState initial{Epoch{0},
                                           core::OriginId::earth,
                                           FrameId::gcrf,
                                           {7e6, 0, 0},
                                           {0, std::sqrt(core::egm96_mu / 7e6), 0}};
        const std::array da{1., 2., 3.}, db{4., -5., 6.}, dc{-.1, .2, -.3}, dd{7., 8., 9.};
        auto after_initial = initial;
        for (std::size_t axis = 0; axis < 3; ++axis)
            after_initial.velocity_mps[axis] += da[axis];
        const auto first_truth = runtime::make_two_body_session(
            after_initial, core::GravitationalParameter{core::egm96_mu}, "cspice");
        auto after_middle = first_truth.sample(std::array{Epoch{burn.count()}}).states()[0];
        for (std::size_t axis = 0; axis < 3; ++axis)
            after_middle.velocity_mps[axis] += db[axis] + dc[axis];
        const auto second_truth = runtime::make_two_body_session(
            after_middle, core::GravitationalParameter{core::egm96_mu}, "cspice");
        auto after_final = second_truth.sample(std::array{stop}).states()[0];
        for (std::size_t axis = 0; axis < 3; ++axis)
            after_final.velocity_mps[axis] += dd[axis];
        double max_r = 0, max_v = 0;
        for (auto method : {OrbitInterpolationMethod::lagrange, OrbitInterpolationMethod::hermite,
                            OrbitInterpolationMethod::vop}) {
            for (auto integrator :
                 {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                  HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
                SCOPED_TRACE(static_cast<int>(method));
                SCOPED_TRACE(static_cast<int>(integrator));
                application::OrbitInput input;
                input.hpop.time_regularization = regularized;
                input.hpop.steps_per_orbit = 8192;
                input.initial = initial;
                input.first = initial.epoch;
                input.last = stop;
                input.propagator = OrbitPropagator::mission;
                input.mission = {
                    MissionCoastModel::hpop,
                    {MissionSegment::impulse(da, ManeuverFrame::gcrf), MissionSegment::coast(burn),
                     MissionSegment::impulse(db, ManeuverFrame::gcrf),
                     MissionSegment::impulse(dc, ManeuverFrame::gcrf), MissionSegment::coast(coast),
                     MissionSegment::impulse(dd, ManeuverFrame::gcrf),
                     MissionSegment::coast(500s)}};
                input.hpop.gravity_degree = input.hpop.gravity_order = 0;
                input.hpop.integrator = integrator;
                input.hpop.fixed_step_seconds = 2;
                input.hpop.interpolation = {method, 7};
                const auto session = runtime::make_orbit_session(input);
                EXPECT_EQ(session.discontinuities(),
                          (std::vector{Epoch{0}, Epoch{burn.count()}, stop}));
                for (auto ns : std::array<std::int64_t, 7>{
                         0, 1, burn.count() - 1, burn.count(), burn.count() + 1,
                         stop.tai_nanoseconds() - 1, stop.tai_nanoseconds()}) {
                    const std::array epochs{Epoch{ns}};
                    const auto actual = session.sample(epochs).states()[0];
                    const auto expected = ns == stop.tai_nanoseconds()
                                              ? after_final
                                              : (ns < burn.count() ? first_truth : second_truth)
                                                    .sample(epochs)
                                                    .states()[0];
                    const auto r = std::hypot(actual.position_m[0] - expected.position_m[0],
                                              actual.position_m[1] - expected.position_m[1],
                                              actual.position_m[2] - expected.position_m[2]);
                    const auto v = std::hypot(actual.velocity_mps[0] - expected.velocity_mps[0],
                                              actual.velocity_mps[1] - expected.velocity_mps[1],
                                              actual.velocity_mps[2] - expected.velocity_mps[2]);
                    EXPECT_LT(r, .005);
                    EXPECT_LT(v, 5e-6);
                    max_r = std::max(max_r, r);
                    max_v = std::max(max_v, v);
                }
                const auto events = session.maneuvers(Epoch{0}, stop);
                ASSERT_EQ(events.size(), 4U);
                EXPECT_EQ(events[1].after.velocity_mps, events[2].before.velocity_mps);
                EXPECT_EQ(events.back().after.velocity_mps,
                          session.sample(std::array{stop}).states()[0].velocity_mps);
                EXPECT_THROW((void)session.sample(std::array{Epoch{stop.tai_nanoseconds() + 1}}),
                             core::PropagationError);
            }
        }
        RecordProperty("max_position_error_m", max_r);
        RecordProperty("max_velocity_error_mps", max_v);
    }
}
TEST(HpopProjectInterpolation, LazyGenerationCancelsAndRetainsItsImmutableScientificCache) {
    for (bool regularized : {false, true}) {
        SCOPED_TRACE(regularized);
        application::OrbitInput input;
        input.hpop.time_regularization = regularized;
        input.hpop.steps_per_orbit = 8192;
        input.propagator = OrbitPropagator::hpop;
        input.last = Epoch{600'000'000'000};
        input.hpop.gravity_degree = input.hpop.gravity_order = 0;
        input.hpop.third_body(core::ThirdBody::moon).enabled = true;
        auto moon = std::make_shared<BoundedMoon>(input.first, input.last);
        core::HpopEnvironment environment;
        environment.ephemeris = moon;
        const auto session = runtime::make_orbit_session(input, 1, nullptr, environment);
        const auto initial_calls = moon->calls;
        EXPECT_EQ(initial_calls, 1U);
        int checkpoints = 0;
        EXPECT_THROW((void)session.discontinuities([&] {
            if (++checkpoints == 10)
                throw std::runtime_error("cancel lazy generation");
        }),
                     std::runtime_error);
        EXPECT_GT(moon->calls, initial_calls);
        const std::array times{Epoch{123'123'456'789}, Epoch{1}, input.last};
        const auto before = session.sample(times);
        const auto calls = moon->calls;
        moon->queries_disabled = true;
        auto concurrent = std::async(std::launch::async, [&] { return session.sample(times); });
        EXPECT_NO_THROW((void)session.discontinuities());
        EXPECT_EQ(session.sample(times).states()[0].position_m, before.states()[0].position_m);
        EXPECT_EQ(concurrent.get().states()[0].position_m, before.states()[0].position_m);
        EXPECT_EQ(moon->calls, calls);
    }
}
TEST(HpopProjectInterpolation, PersistenceHashesReportsAndExactOutputGridUseTheSameConfiguration) {
    application::Project input{"Interpolation", Epoch{0}, Epoch{600'000'000'000}};
    application::ProjectSatellite satellite;
    satellite.orbit.propagator = OrbitPropagator::hpop;
    satellite.orbit.first = input.first;
    satellite.orbit.last = input.last;
    satellite.orbit.step_seconds = 5;
    auto &options = satellite.orbit.hpop;
    options.gravity_degree = options.gravity_order = 0;
    options.interpolation = {OrbitInterpolationMethod::hermite, 5};
    options.report_ephemeris_on_fixed_step = true;
    input.satellites.push_back(satellite);
    const auto hash = application::analysis_input_sha256(input);
    const auto report_hash = runtime::report_input_sha256(input);
    auto restored = application::decode_project(application::encode_project(input));
    EXPECT_EQ(restored.satellites[0].orbit.hpop, options);
    auto project = Project::from_data(restored);
    const auto object = project.objects().get("satellite-1");
    const auto analysis = project.prepare();
    const Epoch at{123'123'456'789};
    const auto state = analysis.state(object, at, FrameId::gcrf);
    const auto report =
        analysis.report({.kind = ReportKind::cartesian_state,
                         .object_ids = {object.id()},
                         .follow_scenario = false,
                         .first = at,
                         .last = at,
                         .frame = FrameId::gcrf},
                        {.columns = {{"state.x", "m", ""}}, .time_scale = ReportTimeScale::tai});
    ASSERT_EQ(report.table.rows().size(), 1U);
    EXPECT_EQ(std::get<double>(report.table.rows()[0][0]), state.position_m[0]);
    restored.satellites[0].orbit.step_seconds = 10;
    EXPECT_NE(application::analysis_input_sha256(restored), hash);
    EXPECT_NE(runtime::report_input_sha256(restored), report_hash);
    restored.satellites[0].orbit.hpop.report_ephemeris_on_fixed_step = false;
    const auto accepted_hash = application::analysis_input_sha256(restored);
    restored.satellites[0].orbit.step_seconds = 5;
    EXPECT_EQ(application::analysis_input_sha256(restored), accepted_hash);
    restored.satellites[0].orbit.hpop.interpolation.order = 7;
    EXPECT_NE(application::analysis_input_sha256(restored), accepted_hash);
    EXPECT_EQ(analysis.state(object, at, FrameId::gcrf).position_m, state.position_m);
    const auto explicit_session = runtime::make_hpop_ephemeris_session(
        satellite.orbit.initial, input.last, options, {}, options.interpolation, 5'000'000'000);
    EXPECT_EQ(explicit_session.sample(std::array{at}).states()[0].position_m, state.position_m);
}

TEST(HpopProjectInterpolation, RegularizedTinyEpochsAndMaximumOrderStayInsideTheCoast) {
    if (!runtime::two_body_available()) {
        GTEST_SKIP();
    }
    for (auto integrator : {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                            HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
        for (auto sign : {-1LL, 1LL}) {
            const core::CartesianState initial{Epoch{8'000'000'000'000'000'000LL},
                                               core::OriginId::earth,
                                               FrameId::gcrf,
                                               {7e6, 0, 0},
                                               {0, 7000, 3000}};
            const auto truth = runtime::make_two_body_session(
                initial, core::GravitationalParameter{core::egm96_mu}, "cspice");
            for (auto interpolation :
                 {core::OrbitInterpolationOptions{OrbitInterpolationMethod::lagrange, 0},
                  core::OrbitInterpolationOptions{OrbitInterpolationMethod::hermite, 3},
                  core::OrbitInterpolationOptions{OrbitInterpolationMethod::lagrange, 29},
                  core::OrbitInterpolationOptions{OrbitInterpolationMethod::hermite, 29},
                  core::OrbitInterpolationOptions{OrbitInterpolationMethod::vop, 0},
                  core::OrbitInterpolationOptions{OrbitInterpolationMethod::vop, 29}}) {
                SCOPED_TRACE(::testing::Message()
                             << static_cast<int>(integrator) << " sign=" << sign
                             << " method=" << static_cast<int>(interpolation.method)
                             << " order=" << interpolation.order);
                const auto span = interpolation.order < 4 ? 1LL : 1'000'000'017LL;
                const Epoch end{initial.epoch.tai_nanoseconds() + sign * span};
                auto moon = std::make_shared<BoundedMoon>(sign > 0 ? initial.epoch : end,
                                                          sign > 0 ? end : initial.epoch);
                core::HpopEnvironment environment;
                environment.ephemeris = moon;
                core::HpopOptions options;
                options.gravity_degree = options.gravity_order = 0;
                options.integrator = integrator;
                options.time_regularization = true;
                options.interpolation = interpolation;
                options.third_body(core::ThirdBody::moon).enabled = true;
                const auto session = make_hpop_interpolated_session(
                    initial, sign > 0 ? initial.epoch : end, sign > 0 ? end : initial.epoch,
                    options, environment);
                const std::array epochs{initial.epoch, end,
                                        Epoch{initial.epoch.tai_nanoseconds() + sign * (span / 2)}};
                const auto actual = session.sample(epochs), expected = truth.sample(epochs);
                for (std::size_t i = 0; i < epochs.size(); ++i) {
                    EXPECT_EQ(actual.states()[i].epoch, epochs[i]);
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        EXPECT_NEAR(actual.states()[i].position_m[axis],
                                    expected.states()[i].position_m[axis], .001);
                        EXPECT_NEAR(actual.states()[i].velocity_mps[axis],
                                    expected.states()[i].velocity_mps[axis], 1e-6);
                    }
                }
                moon->queries_disabled = true;
                EXPECT_EQ(session.sample(epochs).states()[1].position_m,
                          actual.states()[1].position_m);
            }
        }
    }
}

TEST(HpopProjectInterpolation,
     VopMuOnlyChangesEffectiveAnalysisReportAndSessionIdentityWhenActive) {
    for (auto method : {OrbitInterpolationMethod::lagrange, OrbitInterpolationMethod::hermite,
                        OrbitInterpolationMethod::vop}) {
        application::Project project{"VOP GM", Epoch{0}, Epoch{600'000'000'000}};
        application::ProjectSatellite satellite;
        satellite.orbit.propagator = OrbitPropagator::hpop;
        satellite.orbit.first = project.first;
        satellite.orbit.last = project.last;
        satellite.orbit.hpop.gravity_degree = satellite.orbit.hpop.gravity_order = 0;
        satellite.orbit.hpop.interpolation = {method, 7, 3.986004415e14};
        project.satellites.push_back(satellite);
        const auto input = application::analysis_input_sha256(project);
        const auto report = runtime::report_input_sha256(project);
        const auto algorithm =
            runtime::make_orbit_session(project.satellites[0].orbit).provenance().algorithm;
        project.satellites[0].orbit.hpop.interpolation.vop_mu = 4.1e14;
        const bool active = method == OrbitInterpolationMethod::vop;
        EXPECT_EQ(application::analysis_input_sha256(project) != input, active);
        EXPECT_EQ(runtime::report_input_sha256(project) != report, active);
        EXPECT_EQ(runtime::make_orbit_session(project.satellites[0].orbit).provenance().algorithm !=
                      algorithm,
                  active);
        const auto restored = application::decode_project(application::encode_project(project));
        EXPECT_EQ(restored.satellites[0].orbit.hpop.interpolation.vop_mu, 4.1e14);
    }
}
