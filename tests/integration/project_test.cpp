#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/time_frames.hpp>

using namespace orbitforge;
TEST(ProjectGroundStation, RoundTripMixedObjectsAndImmutableEditsPreserveIdentityAndLocation) {
    application::Project project("Mixed", core::Epoch{0}, core::Epoch{7200000000000});
    application::ProjectSatellite satellite;
    project.satellites.push_back(satellite);
    project.ground_stations.push_back({"station-a", "海岸观测站", {1.2, -0.4, -350}});
    const auto bytes = application::encode_project(project);
    const auto restored = application::decode_project(bytes);
    EXPECT_EQ(application::encode_project(restored), bytes);
    EXPECT_EQ(restored.ground_stations.at(0).location, project.ground_stations[0].location);
    auto changed = restored.renamed_ground_station("station-a", "岸站");
    changed = changed.with_ground_station_copy("station-a", "station-b", "副本");
    changed = changed.without_ground_station("station-a");
    EXPECT_EQ(changed.ground_stations.at(0).object_id, "station-b");
    EXPECT_EQ(changed.ground_stations[0].location, project.ground_stations[0].location);
    EXPECT_EQ(changed.satellites[0].orbit.object_id, satellite.orbit.object_id);
    EXPECT_EQ(application::encode_project(project), bytes);
    EXPECT_THROW(
        (void)changed.with_ground_station_copy("station-b", satellite.orbit.object_id, "Collision"),
        application::ProjectError);
    EXPECT_THROW(
        (void)changed.with_satellite_copy(satellite.orbit.object_id, "station-b", "Collision"),
        application::ProjectError);
    EXPECT_THROW((void)changed.renamed_ground_station("station-b", " "), application::ProjectError);
    EXPECT_THROW((void)changed.without_ground_station("missing"), application::ProjectError);
}
TEST(ProjectGroundStation, StationOnlyRuntimeWorksWithoutAnOrbitProviderAndKeepsProvenance) {
    application::Project project("Stations", core::Epoch{0}, core::Epoch{1'000'000'001});
    project.ground_stations.push_back({"station", "Station", {0, 0, -400}});
    auto scenario = runtime::make_project_scenario(project);
    const auto result = scenario.sample(std::array{project.first, project.last});
    ASSERT_EQ(result.size(), 1U);
    EXPECT_EQ(result[0].provenance().object_id, "station");
    EXPECT_EQ(result[0].provenance().input_revision, 1U);
    EXPECT_EQ(result[0].states()[0].frame, core::FrameId::itrf);
    EXPECT_EQ(result[0].states()[0].position_m[0], 6377737);
    EXPECT_EQ(result[0].states()[0].position_m, result[0].states()[1].position_m);
    const auto revised = project.with_settings("Shorter", core::Epoch{1}, project.last);
    EXPECT_EQ(revised.ground_stations[0].location, project.ground_stations[0].location);
    EXPECT_EQ(runtime::make_project_scenario(revised).objects().size(), 1U);
}
TEST(ProjectGroundStation, InertialVelocityIncludesEarthRotationAndMatchesPositionDerivative) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const core::GroundStation station({0, 0, 0});
    const auto frames = runtime::make_frame_provider();
    const auto transform = [&](core::Epoch epoch) {
        return frames->gcrf_to_itrf_state(epoch,
                                          {{epoch, -32, 0, 0, 0, 0, "synthetic"}, {0, 0, 0, 0, 0}});
    };
    const auto fixed = station.state_at(core::Epoch{0});
    const auto inertial = transform(fixed.epoch).inverse(fixed);
    const auto before_epoch = core::Epoch{-500000000}, after_epoch = core::Epoch{500000000};
    const auto before = transform(before_epoch).inverse(station.state_at(before_epoch));
    const auto after = transform(after_epoch).inverse(station.state_at(after_epoch));
    const auto restored = transform(fixed.epoch).apply(inertial);
    double speed_squared = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(inertial.velocity_mps[i], after.position_m[i] - before.position_m[i], 0.0003);
        EXPECT_NEAR(restored.position_m[i], fixed.position_m[i], 1e-8);
        EXPECT_NEAR(restored.velocity_mps[i], 0, 1e-9);
        speed_squared += inertial.velocity_mps[i] * inertial.velocity_mps[i];
    }
    EXPECT_NEAR(std::sqrt(speed_squared), 465.1, 0.1);
}
namespace {
application::Project example() {
    application::Project project{"任务 A", core::Epoch{123456789}, core::Epoch{7200123456789}};
    project.current = core::Epoch{456123456789};
    for (const auto kind :
         {application::OrbitPropagator::two_body, application::OrbitPropagator::j2_secular}) {
        application::ProjectSatellite satellite;
        satellite.orbit.object_id =
            kind == application::OrbitPropagator::two_body ? "TwoBody" : "长期项";
        satellite.orbit.propagator = kind;
        satellite.orbit.initial.epoch = project.current;
        satellite.orbit.first = project.first;
        satellite.orbit.last = project.last;
        satellite.orbit.input_frame = core::FrameId::itrf;
        satellite.display.extent = 2;
        satellite.display.past_minutes = 12.345;
        satellite.display.labels_2d = false;
        satellite.display.line_color = 0xe9a2ba;
        satellite.display.line_style = application::LineStyle::dash_dot;
        satellite.display.line_width = 3.25;
        satellite.model_kind = 2;
        satellite.model_path = "模型/satellite.glb";
        project.satellites.push_back(satellite);
    }
    project.earth_orientation = core::EopDataInfo{
        "eop-test",   std::string(64, 'a'),
        "source",     "ITRF2020",
        "hermite-v1", core::Epoch{0},
        project.last, {"time-test", std::string(64, 'b'), "time-source", 40000, 70000}};
    return project;
}
} // namespace
TEST(Project, BinaryRoundTripPreservesExactInputsDisplayDataAndNanoseconds) {
    const auto original = example();
    const auto bytes = application::encode_project(original);
    const auto restored = application::decode_project(bytes);
    EXPECT_EQ(application::encode_project(restored), bytes);
    EXPECT_EQ(restored.name, original.name);
    EXPECT_EQ(restored.current, original.current);
    ASSERT_EQ(restored.satellites.size(), 2U);
    EXPECT_EQ(restored.satellites[1].orbit.initial.velocity_mps,
              original.satellites[1].orbit.initial.velocity_mps);
    EXPECT_EQ(restored.satellites[1].display, original.satellites[1].display);
    EXPECT_EQ(restored.satellites[1].model_path, original.satellites[1].model_path);
    EXPECT_TRUE(application::matching_project_data(*restored.earth_orientation,
                                                   *original.earth_orientation));
    auto data = *restored.earth_orientation;
    data.interpolation = "different";
    EXPECT_FALSE(application::matching_project_data(data, *original.earth_orientation));
    data = *restored.earth_orientation;
    data.time_data.sha256[0] = 'c';
    EXPECT_FALSE(application::matching_project_data(data, *original.earth_orientation));
}
TEST(Project, RejectsTruncationCorruptionFutureVersionAndTrailingBytes) {
    const auto bytes = application::encode_project(example());
    for (std::size_t size = 0; size < bytes.size(); ++size)
        EXPECT_THROW((void)application::decode_project(std::span(bytes).first(size)),
                     application::ProjectError);
    for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
        auto bad = bytes;
        bad[offset] ^= 0x40;
        EXPECT_THROW((void)application::decode_project(bad), application::ProjectError);
    }
    auto bad = bytes;
    bad.push_back(0);
    EXPECT_THROW((void)application::decode_project(bad), application::ProjectError);
}
TEST(Project, SettingsPreserveInitialConditionsAndSynchronizeAllIntervalsAndCurrentTime) {
    const auto original = example();
    const auto bytes = application::encode_project(original);
    const auto first = core::Epoch{original.current.tai_nanoseconds() + 123};
    const auto last = core::Epoch{original.last.tai_nanoseconds() - 456};
    const auto changed = original.with_settings("Adjusted", first, last);
    EXPECT_EQ(changed.current, first);
    EXPECT_EQ(changed.first, first);
    EXPECT_EQ(changed.last, last);
    EXPECT_EQ(changed.data_lock(), original.data_lock());
    const auto restored = application::decode_project(application::encode_project(changed));
    EXPECT_EQ(restored.name, "Adjusted");
    EXPECT_EQ(restored.current, first);
    ASSERT_EQ(restored.satellites.size(), original.satellites.size());
    for (std::size_t i = 0; i < restored.satellites.size(); ++i) {
        const auto &before = original.satellites[i];
        const auto &after = restored.satellites[i];
        EXPECT_EQ(after.orbit.first, first);
        EXPECT_EQ(after.orbit.last, last);
        EXPECT_EQ(after.orbit.initial.epoch, before.orbit.initial.epoch);
        EXPECT_EQ(after.orbit.initial.position_m, before.orbit.initial.position_m);
        EXPECT_EQ(after.orbit.initial.velocity_mps, before.orbit.initial.velocity_mps);
        EXPECT_EQ(after.orbit.propagator, before.orbit.propagator);
        EXPECT_EQ(after.orbit.object_id, before.orbit.object_id);
        EXPECT_EQ(after.display, before.display);
        EXPECT_EQ(after.model_path, before.model_path);
    }
    EXPECT_EQ(original.with_settings(original.name, original.first, original.current).current,
              original.current);
    const auto earlier_end = core::Epoch{original.current.tai_nanoseconds() - 1};
    EXPECT_EQ(original.with_settings(original.name, original.first, earlier_end).current,
              earlier_end);
    EXPECT_EQ(original.with_settings(original.name, original.first, original.last).current,
              original.current);
    EXPECT_EQ(application::encode_project(original), bytes);
}
TEST(Project, SettingsRequireValidDataCoverageAndNeverMutateOnFailure) {
    const auto original = example();
    const auto bytes = application::encode_project(original);
    const auto later = core::Epoch{original.last.tai_nanoseconds() + 1};
    EXPECT_THROW((void)original.with_settings("Extended", original.first, later),
                 application::ProjectError);
    EXPECT_THROW((void)original.with_settings(" ", original.first, original.last),
                 application::ProjectError);
    EXPECT_THROW((void)original.with_settings("Invalid", original.last, original.first),
                 application::ProjectError);
    auto updated_data = original;
    updated_data.earth_orientation->last_epoch = later;
    const auto extended = updated_data.with_settings("Extended", original.first, later);
    EXPECT_EQ(extended.last, later);
    EXPECT_NE(extended.data_lock(), original.data_lock());
    EXPECT_EQ(application::encode_project(original), bytes);
    const application::Project empty{"Empty", core::Epoch{0}, core::Epoch{1}};
    const auto updated_empty = empty.with_settings("Empty revised", core::Epoch{3}, core::Epoch{5});
    EXPECT_TRUE(updated_empty.satellites.empty());
    EXPECT_EQ(updated_empty.current, core::Epoch{3});
}
TEST(Project, RejectsAmbiguousInputsAndInvalidReferencesBeforeWriting) {
    auto project = example();
    project.satellites[1].orbit.object_id = project.satellites[0].orbit.object_id;
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
    project = example();
    project.satellites[0].orbit.initial.position_m[1] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
    project = example();
    project.current = core::Epoch{-1};
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
    project = example();
    project.satellites[0].orbit.first = core::Epoch{-1};
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
    project = example();
    project.name = std::string("\xc0\xaf");
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
    project = example();
    project.earth_orientation->last_epoch = project.first;
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
}
TEST(Project, FileReplacementIsExplicitAndFailedSavePreservesExistingProject) {
    const auto dir = std::filesystem::path(ORBITFORGE_INTEGRATION_WORKSPACE) / "project-file-test";
    std::filesystem::create_directories(dir);
    const auto path = dir / std::filesystem::path(u8"任务.orbitforge");
    const auto original = example();
    application::save_project(path, original, true);
    auto edited = original;
    edited.name = "changed";
    EXPECT_THROW(application::save_project(path, edited), application::ProjectError);
    EXPECT_EQ(application::load_project(path).name, original.name);
    edited.satellites[0].orbit.step_seconds = 0;
    EXPECT_THROW(application::save_project(path, edited, true), application::ProjectError);
    EXPECT_EQ(application::load_project(path).name, original.name);
    edited.satellites[0].orbit.step_seconds = 30;
    application::save_project(path, edited, true);
    EXPECT_EQ(application::load_project(path).name, edited.name);
    EXPECT_THROW(application::save_project(dir, edited, true), std::exception);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir),
                            std::filesystem::directory_iterator{}),
              1);
    std::filesystem::remove(path);
    std::filesystem::remove(dir);
}
TEST(Project, ReconstructedTwoBodyAndJ2StatesAreIdenticalInEitherTimeDirection) {
#if ORBITFORGE_EXPECT_SPICE
    const auto original = example();
    const auto restored = application::decode_project(application::encode_project(original));
    const auto before = runtime::make_project_scenario(original);
    const auto after = runtime::make_project_scenario(restored);
    const std::array epochs{original.first, original.current, original.last};
    const auto a = before.sample(epochs), b = after.sample(epochs);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = 0; j < epochs.size(); ++j) {
            EXPECT_EQ(a[i].states()[j].position_m, b[i].states()[j].position_m);
            EXPECT_EQ(a[i].states()[j].velocity_mps, b[i].states()[j].velocity_mps);
            EXPECT_EQ(a[i].provenance().algorithm, b[i].provenance().algorithm);
        }
#else
    EXPECT_THROW((void)runtime::make_project_scenario(example()), std::runtime_error);
#endif
}
TEST(Project, EmptyProjectAndExtremeEpochsRoundTripWithoutRounding) {
    const auto start = std::numeric_limits<std::int64_t>::max() - 100;
    const application::Project empty{"Empty", core::Epoch{start}, core::Epoch{start + 1}};
    const auto loaded = application::decode_project(application::encode_project(empty));
    EXPECT_EQ(loaded.first, empty.first);
    EXPECT_EQ(loaded.last, empty.last);
    EXPECT_TRUE(loaded.satellites.empty());
    EXPECT_TRUE(runtime::make_project_scenario(loaded).objects().empty());
}
TEST(Project, ObjectEditsPreserveIdentityAndDoNotMutatePreviousSnapshots) {
    const auto original = example();
    const auto renamed = original.renamed_satellite("TwoBody", "观测卫星");
    EXPECT_EQ(original.satellites[0].name(), "TwoBody");
    EXPECT_EQ(renamed.satellites[0].name(), "观测卫星");
    EXPECT_EQ(renamed.satellites[0].orbit.object_id, "TwoBody");
    const auto copied = renamed.with_satellite_copy("TwoBody", "copy-1", "观测卫星 副本");
    ASSERT_EQ(copied.satellites.size(), 3U);
    EXPECT_EQ(copied.satellites.back().orbit.initial.position_m,
              renamed.satellites[0].orbit.initial.position_m);
    EXPECT_EQ(copied.satellites.back().display, renamed.satellites[0].display);
    EXPECT_EQ(copied.satellites.back().model_path, renamed.satellites[0].model_path);
    EXPECT_EQ(copied.satellites.back().orbit.object_id, "copy-1");
    const auto restored = application::decode_project(application::encode_project(copied));
    EXPECT_EQ(restored.satellites[0].display_name, "观测卫星");
    EXPECT_EQ(restored.satellites.back().display_name, "观测卫星 副本");
    const auto removed = copied.without_satellite("TwoBody");
    EXPECT_EQ(removed.satellites[0].orbit.object_id, "长期项");
    EXPECT_EQ(removed.satellites[1].orbit.object_id, "copy-1");
    EXPECT_TRUE(removed.without_satellite("长期项").without_satellite("copy-1").satellites.empty());
    EXPECT_THROW((void)original.renamed_satellite("TwoBody", " \t"), application::ProjectError);
    EXPECT_THROW((void)original.renamed_satellite("missing", "name"), application::ProjectError);
    EXPECT_THROW((void)original.without_satellite("missing"), application::ProjectError);
    EXPECT_THROW((void)original.with_satellite_copy("TwoBody", "长期项", "copy"),
                 application::ProjectError);
}
TEST(Project, CurrentSchemaFixturePreservesInputsRelationshipsAndExactEncoding) {
    const auto project = application::load_project(ORBITFORGE_PROJECT_CURRENT);
    ASSERT_EQ(project.satellites.size(), 2U);
    EXPECT_EQ(project.name, "Schema fixture");
    EXPECT_TRUE(project.satellites[1].orbit.hpop.srp.boundary_mitigation);
    EXPECT_EQ(project.satellites[1].orbit.hpop.srp.additional_eclipsing_bodies,
              (std::vector<core::SrpEclipsingBody>{{core::ThirdBody::mercury, 2440000.125},
                                                   {core::ThirdBody::venus, 6051800}}));
    EXPECT_EQ(project.satellites[0].orbit.object_id, "satellite-a");
    EXPECT_EQ(project.satellites[0].display_name, "观测卫星 A");
    EXPECT_EQ(project.satellites[0].orbit.initial.position_m,
              (std::array<double, 3>{7000000, 0, 0}));
    EXPECT_EQ(project.satellites[0].display.line_style, application::LineStyle::dash_dot);
    EXPECT_EQ(project.satellites[0].display.line_width, 3.25);
    EXPECT_EQ(project.satellites[1].orbit.propagator, application::OrbitPropagator::mission);
    EXPECT_EQ(project.satellites[1].orbit.hpop.gravity_degree, 0);
    EXPECT_FALSE(project.satellites[1].orbit.hpop.include_relativistic_accelerations);
    EXPECT_EQ(project.satellites[1].orbit.hpop.satellite_mass_kg, 1250);
    EXPECT_EQ(project.satellites[1].orbit.hpop.interpolation.method,
              core::OrbitInterpolationMethod::hermite);
    EXPECT_EQ(project.satellites[1].orbit.hpop.interpolation.order, 5);
    EXPECT_EQ(project.satellites[1].orbit.hpop.interpolation.vop_mu, 4.1e14);
    EXPECT_TRUE(project.satellites[1].orbit.hpop.report_ephemeris_on_fixed_step);
    EXPECT_TRUE(project.satellites[1].orbit.hpop.time_regularization);
    EXPECT_EQ(project.satellites[1].orbit.hpop.regularization_exponent, 1.25);
    EXPECT_EQ(project.satellites[1].orbit.hpop.steps_per_orbit, 240);
    EXPECT_EQ(project.satellites[1].orbit.hpop.integrator, core::HpopIntegrator::gauss_jackson);
    EXPECT_TRUE(project.satellites[1].orbit.hpop.vop);
    EXPECT_FALSE(project.satellites[1].orbit.hpop.uses_vop());
    EXPECT_EQ(project.satellites[1].orbit.hpop.correction, core::HpopCorrection::pseudo);
    EXPECT_TRUE(project.satellites[1].orbit.hpop.include_secular_variations);
    EXPECT_EQ(project.satellites[1].orbit.hpop.ocean_tides,
              (core::OceanTideOptions{false, "fixture-ocean", 30, 12}));
    EXPECT_EQ(project.satellites[1].orbit.hpop.drag_area_m2(), 37.5);
    EXPECT_EQ(project.satellites[1].orbit.hpop.srp_area_m2(), 37.5);
    EXPECT_TRUE(project.satellites[1].orbit.hpop.solid_tides.include_time_dependent_terms);
    EXPECT_FALSE(project.satellites[1].orbit.hpop.srp.enabled);
    EXPECT_EQ(project.satellites[1].orbit.hpop.srp.cr, 1.25);
    EXPECT_EQ(project.satellites[1].orbit.hpop.srp.sun_position_method,
              core::SrpSunPositionMethod::apparent);
    EXPECT_EQ(project.satellites[1].orbit.hpop.srp.atmospheric_altitude_m, 23000);
    const auto &drag = project.satellites[1].orbit.hpop.drag;
    EXPECT_FALSE(drag.enabled);
    EXPECT_EQ(drag.cd, 2.3);
    EXPECT_EQ(drag.atmosphere, core::AtmosphereModel::nrlmsise00);
    EXPECT_EQ(drag.low_altitude_model, core::LowAltitudeDensityModel::nrlmsise00);
    EXPECT_EQ(drag.blending_range_m, 42000);
    EXPECT_EQ(drag.f107, 170);
    EXPECT_EQ(drag.kp, 4.3);
    EXPECT_TRUE(drag.use_apparent_sun_position);
    EXPECT_TRUE(drag.use_approximate_altitude);
    EXPECT_EQ(drag.weather_model, "fixture-weather");
    EXPECT_EQ(drag.geomagnetic_update, core::GeomagneticUpdate::three_hourly_cubic_spline);
    EXPECT_EQ(drag.geomagnetic_source, core::GeomagneticSource::kp);
    EXPECT_EQ(project.satellites[1].orbit.hpop.max_step_seconds, 45);
    EXPECT_EQ(project.satellites[1].orbit.hpop.step_control, core::HpopStepControl::fixed);
    EXPECT_EQ(project.satellites[1].orbit.hpop.minimum_altitude_m, 1000);
    const auto &mission = project.satellites[1].orbit.mission;
    ASSERT_EQ(mission.segments.size(), 3U);
    EXPECT_EQ(mission.coast_model, core::MissionCoastModel::hpop);
    EXPECT_EQ(mission.segments[0].duration_nanoseconds, 200'000'000'001LL);
    EXPECT_EQ(mission.segments[1].delta_v_mps, (std::array{12.0, -3.0, 0.5}));
    ASSERT_EQ(project.ground_stations.size(), 1U);
    EXPECT_EQ(project.ground_stations[0].minimum_elevation_rad, 0.2);
    ASSERT_EQ(project.sensors.size(), 1U);
    EXPECT_EQ(project.sensors[0].parent_satellite_id, "satellite-a");
    EXPECT_EQ(project.sensors[0].target_station_id, "station-a");
    EXPECT_EQ(project.sensors[0].pointing, application::SensorPointing::targeted);
    ASSERT_EQ(project.reports.size(), 1U);
    EXPECT_EQ(project.reports[0].query.object_ids,
              (std::vector<std::string>{"station-a", "sensor-a"}));
    EXPECT_EQ(project.reports[0].query.scope, application::ReportScope::objects);
    ASSERT_EQ(project.reports[0].format.columns.size(), 3U);
    EXPECT_EQ(project.reports[0].format.columns[2].field, "aer.range");
    EXPECT_EQ(project.reports[0].format.columns[2].unit, "km");
    EXPECT_EQ(project.reports[0].format.columns[2].title, "Slant range");
    EXPECT_EQ(project.analysis_status(), application::AnalysisStatus::unrecorded);
    std::ifstream input(ORBITFORGE_PROJECT_CURRENT, std::ios::binary);
    const std::vector<std::uint8_t> fixture(std::istreambuf_iterator<char>{input}, {});
    EXPECT_EQ(application::encode_project(project), fixture);
}
TEST(Project, DataLockPinsFilesInterpretationCoverageAndTransitiveTimeDependency) {
    const auto project = example();
    const auto locked = project.data_lock();
    ASSERT_EQ(locked.references().size(), 2U);
    EXPECT_EQ(locked.references()[0].role, "earth-orientation");
    EXPECT_EQ(locked.references()[0].content_sha256, project.earth_orientation->sha256);
    EXPECT_EQ(locked.references()[1].role, "utc-time");
    EXPECT_EQ(locked.references()[1].content_sha256, project.earth_orientation->time_data.sha256);
    auto relocated = project;
    relocated.earth_orientation->source_url = "https://mirror.invalid/data";
    relocated.earth_orientation->time_data.source_url = "https://mirror.invalid/time";
    EXPECT_EQ(relocated.data_lock(), locked);
    auto changed = project;
    changed.earth_orientation->sha256[0] = 'c';
    EXPECT_NE(changed.data_lock().sha256(), locked.sha256());
    changed = project;
    changed.earth_orientation->interpolation += "-new";
    EXPECT_NE(changed.data_lock().sha256(), locked.sha256());
    changed = project;
    changed.earth_orientation->first_epoch = core::Epoch{-1};
    EXPECT_NE(changed.data_lock().sha256(), locked.sha256());
    changed = project;
    changed.earth_orientation->time_data.sha256[0] = 'c';
    const auto other = changed.data_lock();
    EXPECT_NE(other.sha256(), locked.sha256());
    EXPECT_NE(other.references()[0].configuration_sha256,
              locked.references()[0].configuration_sha256);
    EXPECT_EQ(other.references()[0].content_sha256, locked.references()[0].content_sha256);
    EXPECT_EQ(application::decode_project(application::encode_project(project)).data_lock(),
              locked);
}
TEST(Project, DataLocksHaveStableCanonicalOrderAndRejectDuplicateRoles) {
    // Independently computed using .NET SHA256 over the documented empty encoding.
    EXPECT_EQ(application::DataLock{}.sha256(),
              "81700a60bcada79003d1ea38a39cbea749f3997c1cdea22737be0bee6b7aeab1");
    const auto lock = example().data_lock();
    std::vector<application::DataReference> reversed(lock.references().rbegin(),
                                                     lock.references().rend());
    EXPECT_EQ(application::DataLock(reversed), lock);
    EXPECT_EQ(application::DataLock(reversed).canonical_bytes(), lock.canonical_bytes());
    reversed.push_back(reversed.front());
    EXPECT_THROW((void)application::DataLock(reversed), std::invalid_argument);
    reversed.pop_back();
    reversed.front().content_sha256 = "not-a-hash";
    EXPECT_THROW((void)application::DataLock(reversed), std::invalid_argument);
}
TEST(Project, RejectsEveryPreviousVersionAndFutureVersionBeforeReadingPayload) {
    const auto bytes = application::encode_project(example());
    for (std::uint32_t version = 0; version <= 41; ++version) {
        if (version == 40)
            continue;
        SCOPED_TRACE(version);
        auto unsupported = bytes;
        for (unsigned i = 0; i < 4; ++i)
            unsupported[8 + i] = static_cast<std::uint8_t>(version >> (8 * i));
        try {
            (void)application::decode_project(unsupported);
            FAIL() << "Unsupported version was accepted";
        } catch (const application::ProjectError &error) {
            EXPECT_NE(std::string(error.what()).find("version " + std::to_string(version) + ";"),
                      std::string::npos);
            EXPECT_NE(std::string(error.what()).find("reads only version 40"), std::string::npos);
        }
    }
}
TEST(Project, ExtrapolationPolicyRoundTripsAndParticipatesInProvenanceWithoutExtendingRawCoverage) {
    auto project = example();
    const auto original_hash = project.data_lock();
    project.earth_orientation->extrapolation_days = 1;
    EXPECT_NE(project.data_lock(), original_hash);
    const auto changed_hash = project.data_lock();
    project.earth_orientation->extrapolation_days = 2;
    EXPECT_NE(project.data_lock(), changed_hash);
    project.earth_orientation->last_epoch =
        core::Epoch{project.last.tai_nanoseconds() - 86'400'000'000'000};
    project.earth_orientation->first_epoch =
        core::Epoch{project.earth_orientation->last_epoch.tai_nanoseconds() - 86'400'000'000'000};
    const auto loaded = application::decode_project(application::encode_project(project));
    EXPECT_EQ(loaded.earth_orientation->extrapolation_days, 2);
    EXPECT_EQ(loaded.earth_orientation->last_epoch, project.earth_orientation->last_epoch);
    EXPECT_EQ(loaded.data_lock(), project.data_lock());
    project.earth_orientation->extrapolation_days = 0;
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
    project.earth_orientation->extrapolation_days = 31;
    EXPECT_THROW(application::validate_project(project), application::ProjectError);
}
