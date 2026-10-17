#include <cmath>
#include <gtest/gtest.h>
#include <orbitforge/runtime/hpop.hpp>
#include <orbitforge/runtime/j4.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/report.hpp>
#include <orbitforge/runtime/sgp4.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>

using namespace orbitforge;
namespace {
core::Tle tle_at(std::string_view date) {
    core::Tle tle{"1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
                  "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667"};
    tle.line1.replace(18, 14, date);
    int sum = 0;
    for (std::size_t i = 0; i < 68; ++i)
        sum += tle.line1[i] >= '0' && tle.line1[i] <= '9' ? tle.line1[i] - '0'
               : tle.line1[i] == '-'                      ? 1
                                                          : 0;
    tle.line1[68] = static_cast<char>('0' + sum % 10);
    return tle;
}
} // namespace
TEST(Propagators, TleUsesExplicitUtcDataAndContinuousTaiAcrossLeapSecond) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const auto tle = tle_at("16366.50000000");
    const auto session = runtime::make_sgp4_session(tle, *time, "leap");
    const auto before = time->from_utc({2016, 12, 31, 23, 59, 59, 0});
    const auto after = time->from_utc({2017, 1, 1, 0, 0, 0, 0});
    EXPECT_DOUBLE_EQ(after.seconds_since(before), 2);
    const auto a = session.sample(std::array{after}).states().front();
    const auto b = session.sample(std::array{core::Epoch{before.tai_nanoseconds() + 2'000'000'000}})
                       .states()
                       .front();
    EXPECT_EQ(a.position_m, b.position_m);
    EXPECT_NE(session.provenance().algorithm.find(time->data_info().sha256), std::string::npos);
    application::OrbitInput input;
    input.propagator = application::OrbitPropagator::sgp4;
    input.tle = tle;
    EXPECT_THROW((void)runtime::make_orbit_session(input), std::invalid_argument);
    EXPECT_EQ(runtime::make_orbit_session(input, 1, time.get())
                  .sample(std::array{after})
                  .states()[0]
                  .position_m,
              a.position_m);
}
TEST(Propagators, NewModelsRoundTripAndReportsUseConvertedStates) {
    if (!runtime::time_frames_available() || !runtime::two_body_available())
        GTEST_SKIP();
    const std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    const std::shared_ptr<const core::IFrameProvider> frames = runtime::make_frame_provider();
    const std::shared_ptr<const core::IEarthOrientationProvider> eop =
        runtime::load_eop_snapshot(ORBITFORGE_REPORT_EOP);
    const auto tle = tle_at("00001.50000000");
    const auto first = time->from_utc(core::parse_tle(tle).epoch_utc);
    application::Project project{"New propagators", first,
                                 core::Epoch{first.tai_nanoseconds() + 600'000'000'000}};
    project.earth_orientation = eop->data_info();
    application::ProjectSatellite satellite;
    satellite.orbit.object_id = "tle";
    satellite.orbit.propagator = application::OrbitPropagator::sgp4;
    satellite.orbit.tle = tle;
    satellite.orbit.first = project.first;
    satellite.orbit.last = project.last;
    project.satellites.push_back(satellite);
    satellite.orbit.object_id = "j4";
    satellite.orbit.propagator = application::OrbitPropagator::j4_secular;
    satellite.orbit.tle = {};
    satellite.orbit.initial = runtime::j4_mean_ellipse_state({first, 7e6, .01, .5, .1, .2, .3});
    project.satellites.push_back(satellite);
    const auto bytes = application::encode_project(project);
    const auto restored = application::decode_project(bytes);
    EXPECT_EQ(bytes[8], 40);
    EXPECT_EQ(application::encode_project(restored), bytes);
    EXPECT_EQ(restored.satellites[0].orbit.tle, tle);
    const auto scenario = runtime::make_project_scenario(restored, time.get());
    EXPECT_EQ(scenario.objects().size(), 2U);
    application::ReportDefinition d;
    d.id = "states";
    d.name = "States";
    d.query.object_ids = {"tle"};
    d.query.step_nanoseconds = 600'000'000'000;
    const auto native =
        runtime::make_sgp4_session(tle, *time, "tle").sample(std::array{first}).states()[0];
    for (auto frame : {core::FrameId::gcrf, core::FrameId::itrf}) {
        d.query.frame = frame;
        const auto report = runtime::generate_report({restored, time, frames, eop}, d);
        const auto expected = core::transform_state(native, frame, *frames, eop->at(first));
        for (std::size_t i = 0; i < 3; ++i) {
            EXPECT_NEAR(std::get<double>(report.table.rows()[0][4 + i]), expected.position_m[i],
                        1e-8);
            EXPECT_NEAR(std::get<double>(report.table.rows()[0][7 + i]), expected.velocity_mps[i],
                        1e-8);
        }
    }
    EXPECT_THROW((void)runtime::generate_report({restored, time, frames, {}}, d),
                 std::invalid_argument);
    const auto hash = runtime::report_input_sha256(project);
    project.satellites[0].orbit.initial.position_m[0] += 1000;
    EXPECT_EQ(runtime::report_input_sha256(project), hash);
    project.satellites[0].orbit.tle.line1[68] = 'x';
    EXPECT_THROW((void)application::encode_project(project), application::ProjectError);
}

TEST(Propagators, HpopProvenanceIdentifiesTheSeventhOrderRkfMember) {
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    const core::CartesianState initial{core::Epoch{0},
                                       core::OriginId::earth,
                                       core::FrameId::gcrf,
                                       {7e6, 0, 0},
                                       {0, std::sqrt(core::egm96_mu / 7e6), 0}};
    const auto session = runtime::make_hpop_session(initial, options);
    EXPECT_NE(session.provenance().algorithm.find("rkf78-7th-fehlberg-r287-v1;"),
              std::string::npos);
    EXPECT_EQ(session.provenance().algorithm.find("rkf78-8th-v1;"), std::string::npos);
}
