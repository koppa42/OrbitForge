#include <chrono>
#include <gtest/gtest.h>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/hpop.hpp>
#include <orbitforge/runtime/time_frames.hpp>

using namespace orbitforge;
namespace {
application::Project drag_input(core::Epoch first = core::Epoch{0}) {
    application::Project p{"Drag", first, core::Epoch{first.tai_nanoseconds() + 600000000000}};
    application::ProjectSatellite s;
    s.orbit.initial.epoch = first;
    s.orbit.initial.position_m = {6778137, 0, 0};
    s.orbit.initial.velocity_mps = {0, 6300, 4300};
    s.orbit.propagator = application::OrbitPropagator::hpop;
    s.orbit.first = p.first;
    s.orbit.last = p.last;
    s.orbit.hpop.gravity_degree = s.orbit.hpop.gravity_order = 0;
    p.satellites.push_back(s);
    return p;
}
auto drag_identity(const application::Project &p) {
    return std::pair{application::analysis_input_sha256(p), runtime::report_input_sha256(p)};
}
class DragFiles : public ::testing::Test {
  protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbitforge-drag-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { std::filesystem::create_directories(root); }
    void TearDown() override { std::filesystem::remove_all(root); }
};
} // namespace
TEST(DragPersistence, DraftsRoundtripAndOnlyEffectiveFieldsChangeIdentity) {
    auto p = drag_input();
    auto &options = p.satellites[0].orbit.hpop;
    auto &drag = options.drag;
    auto original = drag_identity(p);
    drag.cd = 2.7;
    drag.f107 = 200;
    drag.kp = 8;
    drag.use_apparent_sun_position = true;
    drag.use_approximate_altitude = true;
    drag.low_altitude_model = core::LowAltitudeDensityModel::nrlmsise00;
    drag.blending_range_m = 45000;
    options.ephemeris_model = "draft-only";
    EXPECT_EQ(drag_identity(p), original);
    EXPECT_EQ(application::decode_project(application::encode_project(p)).satellites[0].orbit.hpop,
              options);
    drag.enabled = true;
    EXPECT_NE(drag_identity(p), original);
    EXPECT_TRUE(p.satellites[0].orbit.uses_eop_force());
    drag.atmosphere = core::AtmosphereModel::nrlmsise00;
    original = drag_identity(p);
    drag.low_altitude_model = core::LowAltitudeDensityModel::none;
    drag.blending_range_m += 1000;
    EXPECT_EQ(drag_identity(p), original);
    drag.use_approximate_altitude = false;
    EXPECT_NE(drag_identity(p), original);
    original = drag_identity(p);
    drag.cd += .1;
    EXPECT_NE(drag_identity(p), original);
    original = drag_identity(p);
    options.ephemeris_model = "de440s";
    EXPECT_NE(drag_identity(p), original);
}
TEST_F(DragFiles, PointMassDragRetainsTimeEopSunAcrossSessionReportAndReopen) {
    if (!runtime::ephemeris_available() || !runtime::time_frames_available())
        GTEST_SKIP();
    auto raw = runtime::load_spice_snapshot(ORBITFORGE_INTEGRATION_SPICE, root);
    const auto first = core::Epoch{raw->data_info().first_epoch.tai_nanoseconds() + 3600000000000};
    auto p = drag_input(first);
    auto &options = p.satellites[0].orbit.hpop;
    options.drag.enabled = true;
    options.drag.use_apparent_sun_position = true;
    options.drag.use_approximate_altitude = true;
    const auto ephemeris = runtime::make_third_body_ephemeris(std::move(raw));
    const auto time_only =
        runtime::DataSnapshot::builtin_time().with_ephemeris("de440s", ephemeris);
    const auto data =
        runtime::DataSnapshot::load(ORBITFORGE_DRAG_EOP).with_ephemeris("de440s", ephemeris);
    const auto project = Project::from_data(p);
    EXPECT_THROW((void)project.prepare(time_only), std::invalid_argument);
    const auto analysis = project.prepare(data);
    const auto sat = project.objects().get("satellite-1");
    const auto state = analysis.state(sat, p.last, FrameId::gcrf);
    const auto &orbit = p.satellites[0].orbit;
    const auto session =
        runtime::make_hpop_session(orbit.initial, options, data.force_environment(orbit));
    EXPECT_NE(session.provenance().algorithm.find("density-altitude=radial-wgs84-v1"),
              std::string::npos);
    EXPECT_EQ(session.sample(std::array{p.last}).states()[0].position_m, state.position_m);
    application::ReportDefinition definition;
    definition.id = "drag-state";
    definition.name = "Drag state";
    definition.query.kind = application::ReportKind::cartesian_state;
    definition.query.object_ids = {sat.id()};
    definition.query.frame = FrameId::gcrf;
    definition.query.step_nanoseconds = 300000000000;
    definition.format.time_scale = application::ReportTimeScale::tai;
    definition.format.units = application::ReportUnits::si;
    const auto report = analysis.report(definition);
    ASSERT_EQ(report.table.rows().size(), 3U);
    for (std::size_t axis = 0; axis < 3; ++axis)
        EXPECT_EQ(std::get<double>(report.table.rows().back()[4 + axis]), state.position_m[axis]);
    project.save(root / "drag.orbitforge", analysis);
    const auto reopened = Project::open(root / "drag.orbitforge", data);
    EXPECT_EQ(reopened.to_data().satellites[0].orbit.hpop.drag, options.drag);
    EXPECT_EQ(reopened.prepare(data)
                  .state(reopened.objects().get("satellite-1"), p.last, FrameId::gcrf)
                  .position_m,
              state.position_m);
    options.drag.cd *= 1.1;
    const auto changed = Project::from_data(p);
    EXPECT_NE(changed.prepare(data)
                  .state(changed.objects().get("satellite-1"), p.last, FrameId::gcrf)
                  .position_m,
              state.position_m);
    EXPECT_EQ(analysis.state(sat, p.last, FrameId::gcrf).position_m, state.position_m);
}
