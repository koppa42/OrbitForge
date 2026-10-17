#include <chrono>
#include <gtest/gtest.h>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/hpop.hpp>

using namespace orbitforge;
namespace {
application::Project input(core::Epoch first = core::Epoch{0}) {
    application::Project p{"SRP", first, core::Epoch{first.tai_nanoseconds() + 600000000000}};
    application::ProjectSatellite s;
    s.orbit.initial.epoch = first;
    s.orbit.propagator = application::OrbitPropagator::hpop;
    s.orbit.first = p.first;
    s.orbit.last = p.last;
    s.orbit.hpop.gravity_degree = s.orbit.hpop.gravity_order = 0;
    p.satellites.push_back(s);
    return p;
}
auto identity(const application::Project &p) {
    return std::pair{application::analysis_input_sha256(p), runtime::report_input_sha256(p)};
}
class SrpFiles : public ::testing::Test {
  protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbitforge-srp-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { std::filesystem::create_directories(root); }
    void TearDown() override { std::filesystem::remove_all(root); }
};
} // namespace

TEST(SrpPersistence, StoresInactiveDraftsButHashesOnlyEffectiveInputs) {
    auto p = input();
    auto &hpop = p.satellites[0].orbit.hpop;
    auto &srp = hpop.srp;
    auto original = identity(p);
    srp.sun_position_method = core::SrpSunPositionMethod::apparent;
    srp.cr = 1.7;
    srp.area_mass_ratio_m2_per_kg = .03;
    srp.atmospheric_altitude_m = 23000;
    srp.moon_shadow = false;
    srp.sun_radius_m = 696000000;
    hpop.ephemeris_model = "inactive-draft";
    EXPECT_EQ(identity(p), original);
    EXPECT_EQ(application::decode_project(application::encode_project(p)).satellites[0].orbit.hpop,
              hpop);
    srp.enabled = true;
    EXPECT_NE(identity(p), original);
    original = identity(p);
    srp.moon_radius_m += 1000; // An unassigned occultor does not change the calculation.
    EXPECT_EQ(identity(p), original);
    srp.cr += .1;
    EXPECT_NE(identity(p), original);
    original = identity(p);
    hpop.ephemeris_model = "de440s"; // Sun positions remain active without third-body gravity.
    EXPECT_NE(identity(p), original);
    srp.shadow_model = core::SrpShadowModel::none;
    original = identity(p);
    srp.earth_shadow = false;
    srp.moon_shadow = true;
    srp.earth_radius_m += 1000;
    srp.moon_radius_m += 1000;
    srp.atmospheric_altitude_m += 1000;
    EXPECT_EQ(identity(p), original);
    srp.pressure_at_reference_pa *= 1.01;
    EXPECT_NE(identity(p), original);
    original = identity(p);
    srp.reference_distance_m *= 1.01;
    EXPECT_NE(identity(p), original);
    original = identity(p);
    srp.sun_radius_m *= 1.01;
    EXPECT_NE(identity(p), original);
}

TEST_F(SrpFiles, SrpOnlyUsesImmutableSpiceAcrossSessionReportAndReopen) {
    if (!runtime::ephemeris_available())
        GTEST_SKIP();
    auto raw = runtime::load_spice_snapshot(ORBITFORGE_INTEGRATION_SPICE, root);
    auto p = input(core::Epoch{raw->data_info().first_epoch.tai_nanoseconds() + 600000000000});
    const auto ephemeris = runtime::make_third_body_ephemeris(std::move(raw));
    const auto data = runtime::DataSnapshot::builtin_time().with_ephemeris("de440s", ephemeris);
    auto &orbit = p.satellites[0].orbit;
    orbit.hpop.srp.enabled = true;
    orbit.hpop.srp.cr = 1.7;
    auto project = Project::from_data(p);
    EXPECT_THROW((void)project.prepare(), runtime::AnalysisCapabilityError);
    const auto analysis = project.prepare(data);
    const auto sat = project.objects().get("satellite-1");
    const auto state = analysis.state(sat, p.last, FrameId::gcrf);
    const auto session =
        runtime::make_hpop_session(orbit.initial, orbit.hpop, data.force_environment(orbit));
    EXPECT_NE(session.provenance().algorithm.find("srp=spherical"), std::string::npos);
    EXPECT_NE(session.provenance().algorithm.find("ephemeris="), std::string::npos);
    EXPECT_EQ(session.sample(std::array{p.last}).states()[0].position_m, state.position_m);
    application::ReportDefinition definition;
    definition.id = "srp-state";
    definition.name = "SRP state";
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
    project.save(root / "srp.orbitforge", analysis);
    const auto reopened = Project::open(root / "srp.orbitforge", data);
    EXPECT_EQ(reopened.to_data().satellites[0].orbit.hpop.srp, orbit.hpop.srp);
    ASSERT_EQ(reopened.to_data().force_data.size(), 1U);
    EXPECT_EQ(reopened.to_data().force_data[0].role, "ephemeris/de440s");
    EXPECT_EQ(reopened.prepare(data)
                  .state(reopened.objects().get("satellite-1"), p.last, FrameId::gcrf)
                  .position_m,
              state.position_m);
    const auto original = identity(p);
    orbit.hpop.third_body(core::ThirdBody::sun).user_mu = 1;
    EXPECT_EQ(identity(p), original); // Gravity GM does not enter a radiation force.
    orbit.hpop.srp.enabled = false;
    const auto unlit_project = Project::from_data(p);
    EXPECT_NE(unlit_project.prepare()
                  .state(unlit_project.objects().get("satellite-1"), p.last, FrameId::gcrf)
                  .position_m,
              state.position_m);
    EXPECT_EQ(analysis.state(sat, p.last, FrameId::gcrf).position_m, state.position_m);
    EXPECT_EQ(analysis.report(definition).input_sha256, report.input_sha256);
}
