#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/report.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>
#include <random>

using namespace orbitforge;
namespace {
runtime::ReportContext stations(std::int64_t last = 2'000'000'001) {
    application::Project p("Reports", core::Epoch{0}, core::Epoch{last});
    p.ground_stations.push_back({"station", "海岸,\"一号\"\n站", {0, 0, 0}});
    return {p, {}, {}, {}};
}
application::ReportDefinition
definition(application::ReportKind kind = application::ReportKind::cartesian_state) {
    application::ReportDefinition d;
    d.id = "report-a";
    d.name = "报告";
    d.query.kind = kind;
    d.query.scope = application::ReportScope::all_compatible;
    d.query.frame = core::FrameId::itrf;
    d.format.time_scale = application::ReportTimeScale::tai;
    d.query.step_nanoseconds = 1'000'000'000;
    return d;
}
std::string read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, {}};
}
} // namespace
TEST(Reports, SamplesIntegerNanosecondsAndIncludesExactStopOnce) {
    auto c = stations();
    auto d = definition();
    const auto result = runtime::generate_report(c, d);
    ASSERT_EQ(result.table.rows().size(), 4U);
    const std::array<std::int64_t, 4> expected{0, 1'000'000'000, 2'000'000'000, 2'000'000'001};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(std::get<core::Epoch>(result.table.rows()[i][2]).tai_nanoseconds(), expected[i]);
        EXPECT_DOUBLE_EQ(std::get<double>(result.table.rows()[i][4]), 6378137);
        EXPECT_DOUBLE_EQ(std::get<double>(result.table.rows()[i][7]), 0);
    }
    EXPECT_EQ(result.cell_text(3, 2), "2000-01-01T12:00:02.000000001");
    d.query.follow_scenario = false;
    d.query.first = core::Epoch{1};
    d.query.last = core::Epoch{1};
    EXPECT_EQ(runtime::generate_report(c, d).table.rows().size(), 1U);
    d.query.first = c.project.first;
    d.query.last = core::Epoch{2'000'000'000};
    EXPECT_EQ(runtime::generate_report(c, d).table.rows().size(), 3U);
}
TEST(Reports, StaticTemplatesHaveTypedValuesAndDoNotRequireOrbitLibraries) {
    auto c = stations();
    auto summary =
        runtime::generate_report(c, definition(application::ReportKind::scenario_summary));
    ASSERT_EQ(summary.table.rows().size(), 7U);
    EXPECT_EQ(std::get<std::int64_t>(summary.table.rows()[5][1]), 1);
    auto objects = runtime::generate_report(c, definition(application::ReportKind::object_summary));
    EXPECT_EQ(std::get<std::string>(objects.table.rows()[0][3]), "WGS84 fixed");
    const auto geo =
        runtime::generate_report(c, definition(application::ReportKind::ground_stations));
    EXPECT_EQ(geo.table.columns()[2].unit, "rad");
    EXPECT_EQ(geo.column_title(2), "Longitude (deg)");
    EXPECT_EQ(geo.column_title(5), "ITRF X (km)");
    EXPECT_DOUBLE_EQ(std::stod(geo.cell_text(0, 5)), 6378.137);
}
TEST(Reports, IndependentScientificSamplesMatchSessionRegardlessOfDisplayStep) {
    if (!runtime::two_body_available())
        GTEST_SKIP();
    auto c = stations(120'000'000'000);
    application::ProjectSatellite satellite;
    satellite.orbit.first = c.project.first;
    satellite.orbit.last = c.project.last;
    satellite.orbit.step_seconds = 60;
    c.project.satellites.push_back(satellite);
    auto d = definition();
    d.query.frame = core::FrameId::gcrf;
    d.query.scope = application::ReportScope::objects;
    d.query.object_ids = {satellite.orbit.object_id};
    d.query.step_nanoseconds = 13'000'000'001;
    const auto result = runtime::generate_report(c, d);
    const auto session = runtime::make_orbit_session(satellite.orbit);
    for (const auto &row : result.table.rows()) {
        const auto epoch = std::get<core::Epoch>(row[2]);
        const auto expected = session.sample(std::array{epoch}).states()[0];
        for (std::size_t axis = 0; axis < 3; ++axis) {
            EXPECT_DOUBLE_EQ(std::get<double>(row[4 + axis]), expected.position_m[axis]);
            EXPECT_DOUBLE_EQ(std::get<double>(row[7 + axis]), expected.velocity_mps[axis]);
        }
    }
    c.project.satellites[0].orbit.step_seconds = 1;
    EXPECT_EQ(runtime::generate_report(c, d).input_sha256, result.input_sha256);
}
TEST(Reports, FrameConversionRetainsEarthRotationVelocityAndConfirmedData) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    auto c = stations();
    c.time = runtime::make_builtin_time_provider();
    c.frames = runtime::make_frame_provider();
    c.eop = runtime::load_eop_snapshot(ORBITFORGE_REPORT_EOP);
    c.project.earth_orientation = c.eop->data_info();
    auto d = definition();
    d.query.frame = core::FrameId::gcrf;
    const auto result = runtime::generate_report(c, d);
    const auto fixed =
        core::GroundStation(c.project.ground_stations[0].location).state_at(c.project.first);
    const auto expected =
        c.frames->gcrf_to_itrf_state(fixed.epoch, c.eop->at(fixed.epoch)).inverse(fixed);
    double speed = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        EXPECT_DOUBLE_EQ(std::get<double>(result.table.rows()[0][4 + axis]),
                         expected.position_m[axis]);
        EXPECT_DOUBLE_EQ(std::get<double>(result.table.rows()[0][7 + axis]),
                         expected.velocity_mps[axis]);
        speed += expected.velocity_mps[axis] * expected.velocity_mps[axis];
    }
    EXPECT_GT(std::sqrt(speed), 400);
    EXPECT_EQ(result.data_sha256, c.project.data_lock().sha256());
    c.project.earth_orientation->sha256[0] =
        c.project.earth_orientation->sha256[0] == '0' ? '1' : '0';
    EXPECT_THROW((void)runtime::generate_report(c, d), std::invalid_argument);
    c.project.earth_orientation = c.eop->data_info();
    c.project.earth_orientation->time_data.sha256[0] = 'x';
    EXPECT_THROW((void)runtime::generate_report(c, d), std::exception);
}
TEST(Reports, UtcLeapSecondTaiAndTtFormattingPreserveEpoch) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    auto c = stations();
    c.time = runtime::make_builtin_time_provider();
    c.project = c.project.with_settings("Leap", c.time->from_utc({2016, 12, 31, 23, 59, 59, 0}),
                                        c.time->from_utc({2017, 1, 1, 0, 0, 0, 0}));
    auto d = definition();
    d.format.time_scale = application::ReportTimeScale::utc;
    auto r = runtime::generate_report(c, d);
    ASSERT_EQ(r.table.rows().size(), 3U);
    EXPECT_EQ(r.cell_text(1, 2), "2016-12-31T23:59:60.000000000Z");
    EXPECT_EQ(r.cell_text(2, 2), "2017-01-01T00:00:00.000000000Z");
    d.format.time_scale = application::ReportTimeScale::tt;
    r = runtime::generate_report(stations(), d);
    EXPECT_EQ(r.cell_text(0, 2), "2000-01-01T12:00:32.184000000");
}
TEST(Reports, RejectsMissingObjectsIncompatibleBindingsInvalidRangesAndExcessRows) {
    auto c = stations();
    auto d = definition();
    d.query.scope = application::ReportScope::objects;
    d.query.object_ids = {"missing"};
    EXPECT_THROW((void)runtime::generate_report(c, d), std::invalid_argument);
    d.query.object_ids.clear();
    d.query.scope = application::ReportScope::all_compatible;
    d.query.follow_scenario = false;
    d.query.first = core::Epoch{-1};
    d.query.last = c.project.last;
    EXPECT_THROW((void)runtime::estimate_report_rows(c.project, d), std::invalid_argument);
    d.query.follow_scenario = true;
    d.query.step_nanoseconds = 1;
    EXPECT_THROW((void)runtime::estimate_report_rows(c.project, d), std::invalid_argument);
    d.query.step_nanoseconds = 0;
    EXPECT_THROW((void)runtime::estimate_report_rows(c.project, d), std::invalid_argument);
    d = definition();
    c.project.ground_stations.clear();
    EXPECT_THROW((void)runtime::generate_report(c, d), std::invalid_argument);
}
TEST(Reports, CancellationIsObservedBetweenBatchesWithoutPublishingPartialResult) {
    auto c = stations(2000'000'000'000);
    auto d = definition();
    std::atomic_bool cancel = false;
    std::size_t seen = 0;
    EXPECT_THROW((void)runtime::generate_report(c, d, &cancel,
                                                [&](std::size_t count, std::size_t total) {
                                                    EXPECT_EQ(total, 2001U);
                                                    seen = count;
                                                    if (count >= 512)
                                                        cancel.store(true);
                                                }),
                 runtime::ReportCancelled);
    EXPECT_EQ(seen, 512U);
    EXPECT_EQ(runtime::generate_report(c, d).table.rows().size(), 2001U);
}
TEST(Reports, CsvQuotesUnicodeAndPreservesExistingFilesOnFailureAndCancellation) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("orbitforge-report-test-" + std::to_string(std::random_device{}()) + ".csv");
    const auto result = runtime::generate_report(stations(), definition());
    runtime::export_report_csv(result, path);
    const auto original = read(path);
    EXPECT_TRUE(original.starts_with("\xEF\xBB\xBF\"Object ID\""));
    EXPECT_NE(original.find("\"海岸,\"\"一号\"\"\n站\""), std::string::npos);
    EXPECT_NE(original.find("\"Time (TAI)\""), std::string::npos);
    EXPECT_THROW(runtime::export_report_csv(result, path), std::invalid_argument);
    std::atomic_bool cancel = true;
    EXPECT_THROW(runtime::export_report_csv(result, path, true, &cancel), runtime::ReportCancelled);
    EXPECT_EQ(read(path), original);
    runtime::export_report_csv(result, path, true);
    EXPECT_EQ(read(path), original);
    std::filesystem::remove(path);
}
TEST(Reports, SavedDefinitionsRoundTripAndMissingBindingsDoNotPreventProjectLoading) {
    auto p = stations().project;
    auto d = definition();
    d.query.scope = application::ReportScope::objects;
    d.query.object_ids = {"deleted-station"};
    d.query.follow_scenario = false;
    d.query.first = core::Epoch{17};
    d.query.last = core::Epoch{19};
    d.query.step_nanoseconds = 3;
    d.format.time_scale = application::ReportTimeScale::tt;
    p.reports.push_back(d);
    const auto bytes = application::encode_project(p);
    EXPECT_EQ(bytes[8], 40);
    const auto restored = application::decode_project(bytes);
    ASSERT_EQ(restored.reports.size(), 1U);
    EXPECT_EQ(application::encode_project(restored), bytes);
    EXPECT_EQ(restored.reports[0].query.object_ids, d.query.object_ids);
    EXPECT_THROW((void)runtime::estimate_report_rows(restored, restored.reports[0]),
                 std::invalid_argument);
    p.reports.push_back(d);
    EXPECT_THROW((void)application::encode_project(p), application::ProjectError);
}
TEST(Reports, ProvenanceIgnoresPlaybackAndConfigurationButDetectsNumericalChanges) {
    auto p = stations().project;
    const auto hash = runtime::report_input_sha256(p);
    p.current = core::Epoch{1};
    p.reports.push_back(definition());
    EXPECT_EQ(runtime::report_input_sha256(p), hash);
    p.ground_stations[0].location.height_m = 10;
    EXPECT_NE(runtime::report_input_sha256(p), hash);
    auto c = stations();
    auto r = runtime::generate_report(c, definition());
    c.project.ground_stations[0].location.height_m = 100;
    EXPECT_EQ(r.input_sha256, hash);
    EXPECT_DOUBLE_EQ(std::get<double>(r.table.rows()[0][4]), 6378137);
}
TEST(Reports, DerivedTemplatesUsePhysicalFramesAndPreserveTwoBodyInvariants) {
    if (!runtime::two_body_available())
        GTEST_SKIP();
    auto c = stations(7200'000'000'000);
    application::ProjectSatellite s;
    s.orbit.first = c.project.first;
    s.orbit.last = c.project.last;
    const auto mu = s.orbit.mu();
    const core::KeplerianElements input{c.project.first, 8e6, .15, .6, .7, .4, 0};
    s.orbit.initial = runtime::to_cartesian(input, core::GravitationalParameter{mu});
    c.project.satellites.push_back(s);
    auto d = definition(application::ReportKind::classical_elements);
    d.query.step_nanoseconds = 137'000'000'000;
    // Empty selection includes satellites only; an ITRF preference cannot rotate elements.
    const auto elements = runtime::generate_report(c, d);
    EXPECT_EQ(elements.table.rows().size(), 54U);
    for (const auto &row : elements.table.rows()) {
        EXPECT_EQ(std::get<std::string>(row[0]), s.orbit.object_id);
        EXPECT_EQ(std::get<std::string>(row[3]), "GCRF");
        EXPECT_NEAR(std::get<double>(row[4]), input.semi_major_axis_m, 1e-5);
        EXPECT_NEAR(std::get<double>(row[5]), input.eccentricity, 1e-12);
        EXPECT_NEAR(std::get<double>(row[6]), input.inclination_rad, 1e-12);
        EXPECT_NEAR(std::get<double>(row[7]), input.raan_rad, 1e-12);
        EXPECT_NEAR(std::get<double>(row[8]), input.argument_of_periapsis_rad, 1e-12);
    }
    EXPECT_EQ(elements.column_unit(4), "km");
    EXPECT_NEAR(*elements.numeric_value(0, 4), 8000, 1e-8);
    EXPECT_EQ(elements.column_unit(6), "deg");
    EXPECT_NEAR(*elements.numeric_value(0, 6), .6 * 180 / std::numbers::pi, 1e-11);
    EXPECT_FALSE(elements.numeric_value(0, 2));
    d.query.kind = application::ReportKind::orbital_motion;
    const auto motion = runtime::generate_report(c, d);
    for (const auto &row : motion.table.rows()) {
        const auto r = std::get<double>(row[4]), v = std::get<double>(row[5]);
        const auto radial = std::get<double>(row[6]), transverse = std::get<double>(row[7]);
        EXPECT_NEAR(v * v / 2 - mu / r, -mu / (2 * input.semi_major_axis_m), 1e-6);
        EXPECT_NEAR(std::hypot(radial, transverse), v, 1e-8);
        EXPECT_NEAR(
            r * transverse,
            std::sqrt(mu * input.semi_major_axis_m * (1 - input.eccentricity * input.eccentricity)),
            1e-3);
        EXPECT_NEAR(std::get<double>(row[8]), std::atan2(radial, transverse), 1e-14);
    }
    EXPECT_NEAR(std::get<double>(motion.table.rows()[0][4]), 6.8e6, 1e-6);
    EXPECT_NEAR(std::get<double>(motion.table.rows()[0][6]), 0, 1e-9);
    EXPECT_EQ(std::get<core::Epoch>(motion.table.rows().back()[2]), c.project.last);
    for (auto kind :
         {application::ReportKind::classical_elements, application::ReportKind::orbital_motion}) {
        d.query.kind = kind;
        d.query.scope = application::ReportScope::objects;
        d.query.object_ids = {"station"};
        EXPECT_THROW((void)runtime::estimate_report_rows(c.project, d), std::invalid_argument);
    }
    // The shared element conversion also accepts a non-singular hyperbolic orbit.
    c.project.satellites[0].orbit.initial = runtime::to_cartesian(
        {c.project.first, -2e7, 1.5, .4, .3, .2, 0}, core::GravitationalParameter{mu});
    d.query.kind = application::ReportKind::classical_elements;
    d.query.object_ids.clear();
    d.query.scope = application::ReportScope::all_compatible;
    const auto hyperbolic = runtime::generate_report(c, d);
    EXPECT_NEAR(std::get<double>(hyperbolic.table.rows().back()[4]), -2e7, .001);
    EXPECT_NEAR(std::get<double>(hyperbolic.table.rows().back()[5]), 1.5, 1e-11);
}
TEST(Reports, GeodeticCoordinatesMatchPublishedWgs84ReferenceAndPoleGeometry) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    core::CartesianState reference{core::Epoch{0},
                                   core::OriginId::earth,
                                   core::FrameId::itrf,
                                   {-5599000.5577049947, 233011.67223479203, -3040909.4706983363},
                                   {0, 0, 0}};
    // ERFA's published t_gd2gc WGS84 vector; invert independently specified coordinates.
    const auto geo = runtime::itrf_to_wgs84(reference);
    EXPECT_NEAR(geo.longitude_rad, 3.1, 1e-14);
    EXPECT_NEAR(geo.latitude_rad, -.5, 1e-14);
    EXPECT_NEAR(geo.height_m, 2500, 1e-7);
    reference.position_m = {0, 0, 6356752.314245179 + 400000};
    const auto pole = runtime::itrf_to_wgs84(reference);
    EXPECT_NEAR(pole.latitude_rad, std::numbers::pi / 2, 1e-14);
    EXPECT_NEAR(pole.height_m, 400000, 1e-7);
    reference.frame = core::FrameId::gcrf;
    EXPECT_THROW((void)runtime::itrf_to_wgs84(reference), std::invalid_argument);
    reference.frame = core::FrameId::itrf;
    reference.position_m = {0, 0, 0};
    EXPECT_THROW((void)runtime::itrf_to_wgs84(reference), std::invalid_argument);
    auto c = stations();
    c.project.ground_stations[0].location = {-std::numbers::pi, .6, -400};
    auto d = definition(application::ReportKind::geodetic_position);
    d.query.frame =
        core::FrameId::gcrf; // Template fixes ITRF without needing EOP for a fixed station.
    const auto r = runtime::generate_report(c, d);
    ASSERT_EQ(r.table.rows().size(), 4U);
    EXPECT_EQ(std::get<std::string>(r.table.rows()[0][3]), "ITRF");
    EXPECT_NEAR(*r.numeric_value(0, 4), -180, 1e-12);
    EXPECT_NEAR(*r.numeric_value(0, 6), -.4, 1e-9);
    EXPECT_EQ(std::get<core::Epoch>(r.table.rows().back()[2]), c.project.last);
}
TEST(Reports, SatelliteGeodeticRequiresConfirmedEopAndNewTemplatesRoundTrip) {
    if (!runtime::two_body_available() || !runtime::time_frames_available())
        GTEST_SKIP();
    auto c = stations(120'000'000'000);
    c.time = runtime::make_builtin_time_provider();
    c.frames = runtime::make_frame_provider();
    c.eop = runtime::load_eop_snapshot(ORBITFORGE_REPORT_EOP);
    c.project.earth_orientation = c.eop->data_info();
    const core::CartesianState fixed{c.project.first,
                                     core::OriginId::earth,
                                     core::FrameId::itrf,
                                     {6378137 + 400000.0, 0, 0},
                                     {0, 7200, 0}};
    application::ProjectSatellite s;
    s.orbit.first = c.project.first;
    s.orbit.last = c.project.last;
    s.orbit.initial =
        c.frames->gcrf_to_itrf_state(c.project.first, c.eop->at(c.project.first)).inverse(fixed);
    c.project.satellites.push_back(s);
    auto d = definition(application::ReportKind::geodetic_position);
    d.query.scope = application::ReportScope::objects;
    d.query.object_ids = {s.orbit.object_id};
    d.query.follow_scenario = false;
    d.query.first = d.query.last = c.project.first;
    const auto r = runtime::generate_report(c, d);
    EXPECT_NEAR(std::get<double>(r.table.rows()[0][4]), 0, 1e-12);
    EXPECT_NEAR(std::get<double>(r.table.rows()[0][5]), 0, 1e-12);
    EXPECT_NEAR(std::get<double>(r.table.rows()[0][6]), 400000, 1e-6);
    c.eop.reset();
    EXPECT_THROW((void)runtime::generate_report(c, d), std::invalid_argument);
    for (auto kind : application::report_kinds) {
        d.query.kind = kind;
        d.query.scope = application::ReportScope::all_compatible;
        d.query.object_ids.clear();
        if (application::report_is_access(kind)) {
            d.query.scope = application::ReportScope::objects;
            d.query.object_ids = {"station", "satellite-1"};
        }
        d.id = "template-" + std::to_string(static_cast<int>(kind));
        c.project.reports.push_back(d);
    }
    const auto encoded = application::encode_project(c.project);
    const auto restored = application::decode_project(encoded);
    ASSERT_EQ(restored.reports.size(), application::report_kinds.size());
    EXPECT_EQ(application::encode_project(restored), encoded);
}
TEST(Reports, TablesRejectNonFiniteDataAndMismatchedColumnCounts) {
    EXPECT_THROW(
        (void)(results::Table{{{"x", "X", "m"}}, {{std::numeric_limits<double>::quiet_NaN()}}}),
        std::invalid_argument);
    EXPECT_THROW((void)(results::Table{{{"x", "X", "m"}}, {{1.0, 2.0}}}), std::invalid_argument);
    EXPECT_THROW((void)(results::Table{{{"x", "X", "m"}, {"x", "Y", "m"}}, {}}),
                 std::invalid_argument);
}
TEST(Reports, TemplatesReorderConvertAndRestoreCapturedFieldsWithoutRecalculation) {
    auto context = stations();
    auto d = definition();
    const auto original = runtime::generate_report(context, d);
    auto format = d.format;
    format.columns = {{"state.x", "km", "距离"}, {"time.epoch", "TT", ""}};
    const auto projected = original.with_template(format);
    ASSERT_EQ(projected.table.columns().size(), 2U);
    EXPECT_EQ(projected.column_title(0), "距离 (km)");
    EXPECT_DOUBLE_EQ(std::get<double>(projected.table.rows()[0][0]), 6378137);
    EXPECT_DOUBLE_EQ(*projected.numeric_value(0, 0), 6378.137);
    EXPECT_EQ(projected.cell_text(0, 1), "2000-01-01T12:00:32.184000000");
    context.project.ground_stations.clear();
    format.columns = {};
    format.units = application::ReportUnits::si;
    const auto restored = projected.with_template(format);
    EXPECT_EQ(restored.table.rows(), original.table.rows());
    EXPECT_EQ(restored.input_sha256, original.input_sha256);
    EXPECT_EQ(restored.input_revision, original.input_revision);
    EXPECT_EQ(restored.data_sha256, original.data_sha256);
    EXPECT_EQ(restored.algorithm, original.algorithm);
    EXPECT_EQ(restored.column_unit(4), "m");
    EXPECT_EQ(original.column_unit(4), "km");
}
TEST(Reports, InvalidTemplatesFailBeforeEvaluationAndKeepOriginalResult) {
    auto d = definition();
    const auto result = runtime::generate_report(stations(), d);
    for (const auto &columns : std::vector<std::vector<application::ReportColumnSpec>>{
             {{"missing", "", ""}},
             {{"state.x", "deg", ""}},
             {{"state.x", "m", ""}, {"state.x", "km", ""}},
             {{"time.epoch", "epoch", ""}}}) {
        d.format.columns = columns;
        bool evaluated = false;
        EXPECT_THROW((void)runtime::generate_report(stations(), d, nullptr,
                                                    [&](auto, auto) { evaluated = true; }),
                     std::invalid_argument);
        EXPECT_FALSE(evaluated);
        EXPECT_THROW((void)result.with_template(d.format), std::invalid_argument);
    }
    EXPECT_EQ(result.table.columns().size(), 10U);
}
TEST(Reports, EmptyObjectSelectionNeverMeansAllAndExplicitAllIncludesNewObjects) {
    auto context = stations();
    auto d = definition();
    d.query.scope = application::ReportScope::objects;
    EXPECT_THROW((void)runtime::generate_report(context, d), std::invalid_argument);
    d.query.scope = application::ReportScope::all_compatible;
    d.query.kind = application::ReportKind::object_summary;
    EXPECT_EQ(runtime::generate_report(context, d).table.rows().size(), 1U);
    context.project.ground_stations.push_back({"second", "Second", {0, 0, 0}});
    EXPECT_EQ(runtime::generate_report(context, d).table.rows().size(), 2U);
    d.query.object_ids = {"station"};
    EXPECT_THROW((void)runtime::generate_report(context, d), std::invalid_argument);
}
TEST(Reports, DisplayedTimeFieldsAloneDetermineUtcRequirement) {
    auto d = definition();
    d.format.time_scale = application::ReportTimeScale::utc;
    d.format.columns = {{"state.x", "m", ""}};
    const auto result = runtime::generate_report(stations(), d);
    d.format.columns = {{"time.epoch", "TAI", ""}};
    const auto tai = result.with_template(d.format);
    EXPECT_EQ(tai.cell_text(0, 0), "2000-01-01T12:00:00.000000000");
    d.format.columns = {{"time.epoch", "UTC", ""}};
    EXPECT_THROW((void)result.with_template(d.format), std::invalid_argument);
    EXPECT_DOUBLE_EQ(*result.numeric_value(0, 0), 6378137);
}
TEST(Reports, ScenarioSummaryRetainsTypedEpochsAcrossTimeScaleChanges) {
    const auto original =
        runtime::generate_report(stations(), definition(application::ReportKind::scenario_summary));
    EXPECT_EQ(std::get<core::Epoch>(original.table.rows()[1][1]), core::Epoch{0});
    auto format = original.definition.format;
    format.time_scale = application::ReportTimeScale::tt;
    format.columns = {{"summary.value", "", ""}};
    const auto tt = original.with_template(format);
    EXPECT_EQ(tt.cell_text(1, 0), "2000-01-01T12:00:32.184000000");
    EXPECT_EQ(tt.cell_text(3, 0), "TT");
    EXPECT_EQ(original.cell_text(3, 1), "TAI");
}
