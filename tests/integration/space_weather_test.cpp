#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/space_weather.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <sstream>
using namespace orbitforge;
namespace {
using Model = core::AtmosphereModel;
using Update = core::GeomagneticUpdate;
using Source = core::GeomagneticSource;
std::vector<core::SpaceWeatherDay> records() {
    std::vector<core::SpaceWeatherDay> result;
    for (int i = 0; i < 14; ++i) {
        core::SpaceWeatherDay d;
        d.date = i < 8 ? core::UtcDateTime{2016, 12, 24 + i, 0, 0, 0, 0}
                       : core::UtcDateTime{2017, 1, i - 7, 0, 0, 0, 0};
        for (int j = 0; j < 8; ++j) {
            d.kp[j] = j / 3.0;
            d.ap[j] = i * 8 + j;
        }
        d.daily_kp = 3;
        d.daily_ap = 100 + i;
        d.f107_observed = 100 + 10 * i;
        d.observed_center81 = 200 + 2 * i;
        d.f107_adjusted = 500;
        d.observed_last81 = 600;
        result.push_back(d);
    }
    return result;
}
class WeatherFiles : public ::testing::Test {
  protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("of-weather-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { std::filesystem::create_directories(root); }
    void TearDown() override { std::filesystem::remove_all(root); }
    auto write(const std::string &s) {
        const auto p = root / "weather.csv";
        std::ofstream out(p, std::ios::binary);
        out << s;
        out.close();
        return p;
    }
    const std::string header =
        "DATE,BSRN,ND,KP1,KP2,KP3,KP4,KP5,KP6,KP7,KP8,KP_SUM,"
        "AP1,AP2,AP3,AP4,AP5,AP6,AP7,AP8,AP_AVG,CP,C9,ISN,F10.7_OBS,F10.7_ADJ,"
        "F10.7_DATA_TYPE,F10.7_OBS_CENTER81,F10.7_OBS_LAST81,F10.7_ADJ_CENTER81,F10.7_ADJ_LAST81\n";
    const std::string example =
        "2000-01-01,2272,7,53,47,40,33,43,30,43,37,327,56,39,27,18,32,15,32,22,"
        "30,1.3,6,71,129.9,125.6,OBS,166.2,179.0,161.1,175.0\n";
    std::string daily_file() {
        std::ostringstream out;
        out << header;
        for (const auto &d : records()) {
            out << d.date.year << '-' << (d.date.month < 10 ? "0" : "") << d.date.month << '-'
                << (d.date.day < 10 ? "0" : "") << d.date.day
                << ",1,1,30,30,30,30,30,30,30,30,240,15,15,15,15,15,15,15,15,15,0,0,0,150,500,OBS,"
                   "150,600,500,600\n";
        }
        return out.str();
    }
};
} // namespace
TEST(SpaceWeather, FluxUses20UtcLinearInterpolationAndModelSpecificLags) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const core::SpaceWeather w{{}, records()};
    const auto at = time->from_utc({2016, 12, 29, 20, 0, 0, 0});
    const auto n = w.sample(at, *time, Model::nrlmsise00, Update::daily, Source::ap);
    EXPECT_DOUBLE_EQ(n.f107, 140);
    EXPECT_DOUBLE_EQ(n.f107_average, 210);
    EXPECT_DOUBLE_EQ(n.ap, 105);
    EXPECT_FALSE(n.ap_history);
    const auto j = w.sample(at, *time, Model::jacchia_roberts, Update::three_hourly, Source::ap);
    EXPECT_DOUBLE_EQ(j.f107, 140);
    EXPECT_DOUBLE_EQ(j.f107_average, 208);
    EXPECT_DOUBLE_EQ(j.ap, 44); // 13:18 UTC after the JR 6.7-hour lag.
    const auto half = w.sample(time->from_utc({2016, 12, 29, 8, 0, 0, 0}), *time, Model::nrlmsise00,
                               Update::daily, Source::kp);
    EXPECT_DOUBLE_EQ(half.f107, 135);
    EXPECT_DOUBLE_EQ(half.f107_average, 209);
    EXPECT_DOUBLE_EQ(half.ap, 15);
}
TEST(SpaceWeather, NativeApHistoryAndKpConversionPrecedeWindowAverages) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const core::SpaceWeather w{{}, records()};
    const auto at = time->from_utc({2016, 12, 29, 20, 0, 0, 0});
    const auto ap = w.sample(at, *time, Model::nrlmsise00, Update::three_hourly, Source::ap);
    ASSERT_TRUE(ap.ap_history);
    EXPECT_EQ(*ap.ap_history, (std::array<double, 7>{105, 46, 45, 44, 43, 38.5, 30.5}));
    const auto kp = w.sample(at, *time, Model::nrlmsise00, Update::three_hourly, Source::kp);
    ASSERT_TRUE(kp.ap_history);
    const std::array<double, 7> expected{15, 7, 6, 5, 4, 4.5, 4.5};
    for (std::size_t i = 0; i < 7; ++i)
        EXPECT_NEAR((*kp.ap_history)[i], expected[i], 1e-13);
    for (int i = 0; i <= 27; ++i)
        EXPECT_NEAR(core::ap_to_kp(core::kp_to_ap(i / 3.0)), i / 3.0, 1e-14);
    EXPECT_THROW((void)core::ap_to_kp(-1), std::invalid_argument);
    EXPECT_THROW((void)core::ap_to_kp(401), std::invalid_argument);
}
TEST(SpaceWeather, LeapSecondStaysInFinalUtcSlotAndFluxUsesElapsedSeconds) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const core::SpaceWeather w{{}, records()};
    const auto sample = [&](core::UtcDateTime u) {
        return w.sample(time->from_utc(u), *time, Model::nrlmsise00, Update::three_hourly,
                        Source::ap);
    };
    const auto leap = sample({2016, 12, 31, 23, 59, 60, 0}),
               midnight = sample({2017, 1, 1, 0, 0, 0, 0});
    EXPECT_DOUBLE_EQ(leap.ap, 63);
    EXPECT_DOUBLE_EQ(midnight.ap, 64);
    EXPECT_DOUBLE_EQ(leap.ap_history->at(2), 62);
    EXPECT_DOUBLE_EQ(midnight.ap_history->at(2), 63);
    EXPECT_NEAR(midnight.f107_average, 214 + 2 * 14401.0 / 86401, 1e-12);
    EXPECT_NEAR(midnight.f107, 160 + 10 * 14401.0 / 86400, 1e-12);
}
TEST(SpaceWeather, CoverageChecksInteriorGapsSelectedFieldsAndPrehistory) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const auto first = time->from_utc({2016, 12, 29, 20, 0, 0, 0});
    const auto last = time->from_utc({2017, 1, 3, 20, 0, 0, 0});
    auto days = records();
    days[7].ap[2] = {};
    days[7].f107_adjusted = {};
    const core::SpaceWeather w{{}, days};
    EXPECT_NO_THROW(
        (void)w.sample(first, *time, Model::nrlmsise00, Update::three_hourly, Source::ap));
    EXPECT_NO_THROW(
        (void)w.sample(last, *time, Model::nrlmsise00, Update::three_hourly, Source::ap));
    EXPECT_THROW((void)w.validate_interval(first, last, *time, Model::nrlmsise00,
                                           Update::three_hourly, Source::ap),
                 core::SpaceWeatherCoverageError);
    EXPECT_NO_THROW((void)w.validate_interval(first, last, *time, Model::nrlmsise00,
                                              Update::three_hourly, Source::kp));
    days = records();
    days.erase(days.begin() + 7);
    const core::SpaceWeather hole{{}, days};
    EXPECT_THROW((void)hole.validate_interval(first, last, *time, Model::jacchia_roberts,
                                              Update::daily, Source::ap),
                 core::SpaceWeatherCoverageError);
    days = records();
    days[5].quality = core::WeatherQuality::monthly_prediction;
    EXPECT_THROW((void)(core::SpaceWeather{{}, days}.sample(first, *time, Model::nrlmsise00,
                                                            Update::daily, Source::ap)),
                 core::SpaceWeatherCoverageError);
    EXPECT_THROW((void)w.sample(time->from_utc({2016, 12, 25, 20, 0, 0, 0}), *time,
                                Model::nrlmsise00, Update::three_hourly, Source::ap),
                 core::SpaceWeatherCoverageError);
    days = records();
    days[1].date = days[0].date;
    EXPECT_THROW((void)(core::SpaceWeather{{}, days}), std::invalid_argument);
}
TEST_F(WeatherFiles, OfficialCsvExampleAndFixedFormatRetainDistinctFields) {
    const auto csv = runtime::load_space_weather(write(header + example));
    const auto &r = csv->days()[0];
    EXPECT_NEAR(*r.kp[0], 16.0 / 3, 1e-14);
    EXPECT_DOUBLE_EQ(*r.daily_kp, 327.0 / 80);
    EXPECT_DOUBLE_EQ(*r.daily_ap, 30);
    EXPECT_DOUBLE_EQ(*r.f107_observed, 129.9);
    EXPECT_DOUBLE_EQ(*r.observed_center81, 166.2);
    EXPECT_DOUBLE_EQ(*r.observed_last81, 179);
    const std::string line = "2000 01 01 2272  7 53 47 40 33 43 30 43 37 327  56  39  27  18  32  "
                             "15  32  22  30 1.3 6  71 125.6 0 161.1 175.0 129.9 166.2 179.0";
    ASSERT_EQ(line.size(), 130U);
    const auto fixed = runtime::load_space_weather(
        write("DATATYPE CssiSpaceWeather\nVERSION 1.2\nNUM_OBSERVED_POINTS 1\nBEGIN OBSERVED\n" +
              line + "\nEND OBSERVED\n"));
    const auto &f = fixed->days()[0];
    EXPECT_EQ(r.kp, f.kp);
    EXPECT_EQ(r.ap, f.ap);
    EXPECT_EQ(r.daily_kp, f.daily_kp);
    EXPECT_EQ(r.f107_observed, f.f107_observed);
    EXPECT_EQ(r.adjusted_last81, f.adjusted_last81);
    EXPECT_NE(csv->info().sha256, fixed->info().sha256);
    auto predicted = example;
    predicted.replace(predicted.find(",53,"), 4, ",22,");
    predicted.replace(predicted.find("OBS"), 3, "PRD");
    EXPECT_DOUBLE_EQ(*runtime::load_space_weather(write(header + predicted))->days()[0].kp[0], 2.2);
    auto missing = example;
    missing.replace(missing.find(",56,"), 4, ",,");
    EXPECT_FALSE(runtime::load_space_weather(write(header + missing))->days()[0].ap[0]);
    EXPECT_THROW((void)runtime::load_space_weather(write(header + example + example)),
                 std::invalid_argument);
    EXPECT_THROW((void)runtime::load_space_weather(write(header + "broken\n")), std::runtime_error);
    EXPECT_THROW((void)runtime::load_space_weather(
                     write("DATATYPE CssiSpaceWeather\nNUM_OBSERVED_POINTS 2\nBEGIN OBSERVED\n" +
                           line + "\nEND OBSERVED\n")),
                 std::runtime_error);
}
TEST_F(WeatherFiles, CatalogPublishesValidatedBytesAndRetainsPreviousOnFailureOrCancellation) {
    if (!runtime::data_catalog_available())
        GTEST_SKIP();
    runtime::DataCatalog catalog(root / "catalog");
    const auto first = catalog.install_space_weather(write(header + example));
    const auto captured = catalog.load_space_weather("celestrak");
    EXPECT_THROW((void)catalog.install_space_weather(write("corrupt")), std::runtime_error);
    EXPECT_EQ(catalog.weather_sources().at(0).info.sha256, first.info.sha256);
    const auto valid = write(header + example + "\n");
    EXPECT_THROW((void)catalog.install_space_weather(valid, "celestrak", "",
                                                     runtime::DataAcquisition::imported,
                                                     [] { throw std::runtime_error("cancel"); }),
                 std::runtime_error);
    EXPECT_EQ(catalog.weather_sources().at(0).info.sha256, first.info.sha256);
    const auto second = catalog.install_space_weather(valid);
    EXPECT_NE(second.info.sha256, first.info.sha256);
    EXPECT_EQ(captured->info().sha256, first.info.sha256);
    EXPECT_DOUBLE_EQ(*captured->days()[0].daily_ap, 30);
    std::ofstream tamper(second.file, std::ios::binary);
    tamper << header << example;
    tamper.close();
    EXPECT_THROW((void)catalog.load_space_weather("celestrak"), std::runtime_error);
}
TEST_F(WeatherFiles, HpopFileWeatherMatchesManualConstantAndPinsSessionReportAndSavedProject) {
    if (!runtime::time_frames_available() || !runtime::ephemeris_available())
        GTEST_SKIP();
    const auto data =
        runtime::DataSnapshot::load(ORBITFORGE_DRAG_EOP)
            .with_ephemeris("de440s",
                            runtime::make_third_body_ephemeris(
                                runtime::load_spice_snapshot(ORBITFORGE_INTEGRATION_SPICE, root)));
    const auto first = data.time()->from_utc({2016, 12, 30, 12, 0, 0, 0});
    const core::Epoch last{first.tai_nanoseconds() + 600000000000};
    application::Project p{"Weather", first, last};
    application::ProjectSatellite satellite;
    auto &orbit = satellite.orbit;
    orbit.first = first;
    orbit.last = last;
    orbit.initial.epoch = first;
    orbit.initial.position_m = {6778137, 0, 0};
    orbit.initial.velocity_mps = {0, 6300, 4300};
    orbit.propagator = application::OrbitPropagator::hpop;
    orbit.hpop.gravity_degree = orbit.hpop.gravity_order = 0;
    orbit.hpop.drag.enabled = true;
    orbit.hpop.drag.atmosphere = Model::nrlmsise00;
    p.satellites.push_back(satellite);
    const auto manual = Project::from_data(p);
    const auto expected =
        manual.prepare(data).state(manual.objects().get("satellite-1"), last, FrameId::gcrf);
    auto &drag = p.satellites[0].orbit.hpop.drag;
    drag.weather_model = "celestrak";
    const auto project = Project::from_data(p);
    EXPECT_THROW((void)project.prepare(data), runtime::AnalysisCapabilityError);
    const auto file = write(daily_file());
    const auto weather = data.with_space_weather_file("celestrak", file);
    const auto analysis = project.prepare(weather);
    const auto object = project.objects().get("satellite-1");
    const auto actual = analysis.state(object, last, FrameId::gcrf);
    EXPECT_EQ(actual.position_m, expected.position_m);
    application::ReportDefinition definition;
    definition.id = "weather";
    definition.name = "Weather";
    definition.query.kind = application::ReportKind::cartesian_state;
    definition.query.object_ids = {object.id()};
    definition.query.frame = FrameId::gcrf;
    definition.query.step_nanoseconds = 300000000000;
    definition.format.time_scale = application::ReportTimeScale::tai;
    const auto report = analysis.report(definition);
    ASSERT_EQ(report.table.rows().size(), 3U);
    for (std::size_t i = 0; i < 3; ++i)
        EXPECT_EQ(std::get<double>(report.table.rows().back()[4 + i]), actual.position_m[i]);
    project.save(root / "weather.orbitforge", analysis);
    const auto reopened = Project::open(root / "weather.orbitforge", weather);
    EXPECT_EQ(reopened.to_data().satellites[0].orbit.hpop.drag, drag);
    EXPECT_EQ(reopened.prepare(weather)
                  .state(reopened.objects().get("satellite-1"), last, FrameId::gcrf)
                  .position_m,
              actual.position_m);
    auto changed = daily_file();
    std::size_t pos = 0;
    while ((pos = changed.find(",150,500,", pos)) != std::string::npos) {
        changed.replace(pos, 9, ",250,500,");
        pos += 9;
    }
    const auto changed_data = data.with_space_weather_file("celestrak", write(changed));
    const auto changed_project = Project::from_data(p);
    EXPECT_NE(changed_project.prepare(changed_data)
                  .state(changed_project.objects().get("satellite-1"), last, FrameId::gcrf)
                  .position_m,
              actual.position_m);
    EXPECT_EQ(analysis.state(object, last, FrameId::gcrf).position_m, actual.position_m);
    EXPECT_THROW((void)reopened.prepare(changed_data), runtime::DataChangedError);
    const auto identity = application::analysis_input_sha256(p);
    drag.f107 = 999;
    drag.kp = 8;
    EXPECT_EQ(application::analysis_input_sha256(p), identity);
    drag.geomagnetic_update = Update::three_hourly;
    EXPECT_NE(application::analysis_input_sha256(p), identity);
}

namespace {
std::vector<core::SpaceWeatherDay> spline_records() {
    std::vector<core::SpaceWeatherDay> days(16);
    for (int i = 0; i < 16; ++i) {
        auto &d = days[i];
        d.date = i < 10 ? core::UtcDateTime{2016, 12, 22 + i, 0, 0, 0, 0}
                        : core::UtcDateTime{2017, 1, i - 9, 0, 0, 0, 0};
        d.f107_observed = d.observed_center81 = 150;
        d.daily_ap = 15;
        d.daily_kp = 3;
    }
    std::ifstream input(ORBITFORGE_WEATHER_SPLINE_REFERENCE);
    if (!input)
        throw std::runtime_error("Missing SciPy weather spline reference");
    std::string line;
    while (std::getline(input, line)) {
        if (!line.starts_with("D "))
            continue;
        std::istringstream row(line.substr(2));
        int day, slot;
        double ap, kp;
        row >> day >> slot >> ap >> kp;
        if (!row)
            throw std::runtime_error("Invalid SciPy weather input row");
        days.at(day + 2).ap.at(slot) = ap;
        days.at(day + 2).kp.at(slot) = kp;
    }
    return days;
}
core::Epoch jr_at(const core::ITimeProvider &time, core::UtcDateTime weather_utc) {
    return core::Epoch{time.from_utc(weather_utc).tai_nanoseconds() + 24120000000000LL};
}
} // namespace
TEST(SpaceWeather, NaturalCubicMatchesIndependentScipyAcrossKnotsAndLeapSecond) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const core::SpaceWeather w{{}, spline_records()};
    std::ifstream input(ORBITFORGE_WEATHER_SPLINE_REFERENCE);
    std::string line;
    std::size_t count = 0;
    double maximum = 0;
    while (std::getline(input, line)) {
        if (!line.starts_with("C "))
            continue;
        std::istringstream row(line.substr(2));
        int source;
        std::int64_t ns;
        double expected;
        ASSERT_TRUE(row >> source >> ns >> expected);
        const auto value =
            w.sample(core::Epoch{ns}, *time, Model::jacchia_roberts,
                     Update::three_hourly_cubic_spline, source == 0 ? Source::ap : Source::kp);
        const double actual = source == 0 ? value.ap : value.kp;
        maximum = std::max(maximum, std::abs(actual - expected));
        EXPECT_NEAR(actual, expected, source == 0 ? 5e-12 : 5e-13) << line;
        EXPECT_DOUBLE_EQ(value.f107, 150);
        EXPECT_DOUBLE_EQ(value.f107_average, 150);
        ++count;
    }
    EXPECT_EQ(count, 1112U);
    RecordProperty("maximum_index_error", maximum);
    for (auto source : {Source::ap, Source::kp})
        EXPECT_NO_THROW(w.validate_interval(
            jr_at(*time, {2016, 12, 24, 1, 30, 0, 0}), jr_at(*time, {2017, 1, 6, 22, 30, 0, 0}),
            *time, Model::jacchia_roberts, Update::three_hourly_cubic_spline, source));
}
TEST(SpaceWeather, NaturalCubicGapsEndpointsAndInteriorOvershootAreExplicit) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const auto sample = [&](const core::SpaceWeather &w, core::UtcDateTime u,
                            Source source = Source::ap) {
        return w.sample(jr_at(*time, u), *time, Model::jacchia_roberts,
                        Update::three_hourly_cubic_spline, source);
    };
    auto days = spline_records();
    const core::SpaceWeather original{{}, days};
    EXPECT_NO_THROW((void)sample(original, {2016, 12, 24, 1, 30, 0, 0}));
    EXPECT_NO_THROW((void)sample(original, {2017, 1, 6, 22, 30, 0, 0}));
    EXPECT_THROW((void)sample(original, {2016, 12, 24, 1, 29, 59, 999999999}),
                 core::SpaceWeatherCoverageError);
    EXPECT_THROW((void)sample(original, {2017, 1, 6, 22, 30, 0, 1}),
                 core::SpaceWeatherCoverageError);
    days[7].ap[2] = {};
    const core::SpaceWeather hole{{}, days};
    EXPECT_NO_THROW((void)sample(hole, {2016, 12, 29, 4, 30, 0, 0}));
    EXPECT_NO_THROW((void)sample(hole, {2016, 12, 29, 10, 30, 0, 0}));
    EXPECT_THROW(hole.validate_interval(jr_at(*time, {2016, 12, 29, 4, 30, 0, 0}),
                                        jr_at(*time, {2016, 12, 29, 10, 30, 0, 0}), *time,
                                        Model::jacchia_roberts, Update::three_hourly_cubic_spline,
                                        Source::ap),
                 core::SpaceWeatherCoverageError);
    EXPECT_NO_THROW((void)sample(hole, {2016, 12, 29, 7, 30, 0, 0}, Source::kp));
    days = spline_records();
    for (auto &d : days)
        for (auto &v : d.ap)
            if (v)
                v = 0;
    days[7].ap[2] = 400;
    const core::SpaceWeather storm{{}, days};
    // Endpoints are legal zeros, but the interior natural spline is negative.
    EXPECT_DOUBLE_EQ(sample(storm, {2016, 12, 29, 10, 30, 0, 0}).ap, 0);
    EXPECT_DOUBLE_EQ(sample(storm, {2016, 12, 29, 13, 30, 0, 0}).ap, 0);
    EXPECT_THROW((void)sample(storm, {2016, 12, 29, 12, 0, 0, 0}), core::SpaceWeatherCoverageError);
    EXPECT_THROW(storm.validate_interval(jr_at(*time, {2016, 12, 29, 10, 30, 0, 0}),
                                         jr_at(*time, {2016, 12, 29, 13, 30, 0, 0}), *time,
                                         Model::jacchia_roberts, Update::three_hourly_cubic_spline,
                                         Source::ap),
                 core::SpaceWeatherCoverageError);
}
TEST(SpaceWeather, CubicChoiceRetainsNativeNrlHistoryAndCanonicalIdentity) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const core::SpaceWeather w{{}, spline_records()};
    for (auto source : {Source::ap, Source::kp}) {
        for (const auto u : {core::UtcDateTime{2016, 12, 31, 23, 59, 60, 0},
                             core::UtcDateTime{2017, 1, 1, 0, 0, 0, 0}}) {
            const auto at = time->from_utc(u);
            const auto a = w.sample(at, *time, Model::nrlmsise00, Update::three_hourly, source);
            const auto b =
                w.sample(at, *time, Model::nrlmsise00, Update::three_hourly_cubic_spline, source);
            EXPECT_EQ(a.ap_history, b.ap_history);
            EXPECT_EQ(a.ap, b.ap);
            EXPECT_EQ(a.kp, b.kp);
            core::AtmosphereInput in;
            in.utc = u;
            in.geodetic = {.4, .6, 400000};
            in.f107 = a.f107;
            in.f107_average = a.f107_average;
            in.ap_history = a.ap_history;
            const auto rho = core::evaluate_atmosphere(Model::nrlmsise00, in).density_kg_m3;
            in.ap_history = b.ap_history;
            EXPECT_EQ(core::evaluate_atmosphere(Model::nrlmsise00, in).density_kg_m3, rho);
        }
    }
    core::DragOptions d;
    d.enabled = true;
    d.weather_model = "test";
    d.atmosphere = Model::nrlmsise00;
    d.geomagnetic_update = Update::three_hourly_cubic_spline;
    EXPECT_EQ(core::effective_drag_options(d).geomagnetic_update, Update::three_hourly);
    d.atmosphere = Model::jacchia_roberts;
    EXPECT_EQ(core::effective_drag_options(d).geomagnetic_update,
              Update::three_hourly_cubic_spline);
    d.weather_model.clear();
    EXPECT_EQ(core::effective_drag_options(d).geomagnetic_update, Update::daily);
}

TEST(SpaceWeather, SplineDrivenJacchiaRobertsMatchesOriginalGmatDensity) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const core::SpaceWeather w{{}, spline_records()};
    std::ifstream input(ORBITFORGE_WEATHER_SPLINE_REFERENCE);
    std::string line;
    std::size_t count = 0;
    double maximum = 0;
    while (std::getline(input, line)) {
        if (!line.starts_with("R "))
            continue;
        std::istringstream row(line.substr(2));
        int source, year, month, day;
        std::int64_t ns;
        double seconds, height, latitude, angle, declination, expected;
        ASSERT_TRUE(row >> source >> ns >> year >> month >> day >> seconds >> height >> latitude >>
                    angle >> declination >> expected);
        const auto sample =
            w.sample(core::Epoch{ns}, *time, Model::jacchia_roberts,
                     Update::three_hourly_cubic_spline, source == 0 ? Source::ap : Source::kp);
        core::AtmosphereInput in;
        in.utc = time->to_utc(core::Epoch{ns});
        EXPECT_EQ(in.utc.year, year);
        EXPECT_EQ(in.utc.month, month);
        EXPECT_EQ(in.utc.day, day);
        EXPECT_NEAR(in.utc.hour * 3600 + in.utc.minute * 60 + in.utc.second +
                        in.utc.nanosecond * 1e-9,
                    seconds, 1e-10);
        in.geodetic = {.5, latitude, height};
        in.solar_hour_angle_rad = angle;
        in.sun_declination_rad = declination;
        in.f107 = sample.f107;
        in.f107_average = sample.f107_average;
        in.kp = sample.kp;
        const auto actual = core::evaluate_atmosphere(Model::jacchia_roberts, in).density_kg_m3;
        maximum = std::max(maximum, std::abs(actual / expected - 1));
        EXPECT_NEAR(actual, expected, expected * 2e-10) << line;
        ++count;
    }
    EXPECT_EQ(count, 66U);
    RecordProperty("maximum_relative_density_error", maximum);
}

TEST_F(WeatherFiles, InterpolatedTrajectoriesConvergeAndSurviveProjectReopen) {
    if (!runtime::time_frames_available() || !runtime::ephemeris_available())
        GTEST_SKIP();
    for (const auto update :
         {Update::three_hourly_interpolated, Update::three_hourly_cubic_spline}) {
        SCOPED_TRACE(static_cast<int>(update));
        auto text = daily_file();
        const std::string old = ",15,15,15,15,15,15,15,15,15,0,0,0,";
        const std::string varied = ",15,20,30,50,40,30,20,15,27,0,0,0,";
        std::size_t offset = 0;
        while ((offset = text.find(old, offset)) != std::string::npos) {
            text.replace(offset, old.size(), varied);
            offset += varied.size();
        }
        const auto data =
            runtime::DataSnapshot::load(ORBITFORGE_DRAG_EOP)
                .with_ephemeris(
                    "de440s", runtime::make_third_body_ephemeris(
                                  runtime::load_spice_snapshot(ORBITFORGE_INTEGRATION_SPICE, root)))
                .with_space_weather_file("celestrak", write(text));
        const auto epoch = data.time()->from_utc({2017, 1, 1, 1, 0, 0, 0});
        const core::Epoch first{epoch.tai_nanoseconds() - 21600000000000LL};
        const core::Epoch last{epoch.tai_nanoseconds() + 21600000000000LL};
        application::Project settings{"Spline", first, last};
        application::ProjectSatellite satellite;
        auto &orbit = satellite.orbit;
        orbit.propagator = application::OrbitPropagator::hpop;
        orbit.first = first;
        orbit.last = last;
        orbit.initial.epoch = epoch;
        orbit.initial.position_m = {6778137, 0, 0};
        orbit.initial.velocity_mps = {0, 6300, 4300};
        auto &h = orbit.hpop;
        h.gravity_degree = h.gravity_order = 0;
        h.max_step_seconds = 60;
        h.relative_tolerance = 1e-13;
        h.position_tolerance_m = 1e-6;
        h.velocity_tolerance_mps = 1e-9;
        h.drag.enabled = true;
        h.drag.atmosphere = Model::jacchia_roberts;
        h.drag.weather_model = "celestrak";
        h.drag.geomagnetic_update = update;
        settings.satellites.push_back(satellite);
        const auto project = Project::from_data(settings);
        const auto analysis = project.prepare(data);
        const auto object = project.objects().get("satellite-1");
        const auto a = analysis.state(object, first, FrameId::gcrf),
                   b = analysis.state(object, last, FrameId::gcrf);
        settings.satellites[0].orbit.hpop.max_step_seconds = 7;
        const auto refined = Project::from_data(settings);
        const auto finer = refined.prepare(data);
        double maximum = 0;
        for (const auto &state : {a, b}) {
            const auto other =
                finer.state(refined.objects().get("satellite-1"), state.epoch, FrameId::gcrf);
            for (std::size_t i = 0; i < 3; ++i) {
                maximum = std::max(maximum, std::abs(state.position_m[i] - other.position_m[i]));
                EXPECT_NEAR(state.position_m[i], other.position_m[i], 1e-3);
                EXPECT_NEAR(state.velocity_mps[i], other.velocity_mps[i], 1e-6);
            }
        }
        RecordProperty("maximum_step_refinement_position_m_" +
                           std::to_string(static_cast<int>(update)),
                       maximum);
        settings.satellites[0].orbit.hpop.drag.geomagnetic_update = Update::three_hourly;
        const auto stepped = Project::from_data(settings);
        EXPECT_NE(stepped.prepare(data)
                      .state(stepped.objects().get("satellite-1"), last, FrameId::gcrf)
                      .position_m,
                  b.position_m);
        const auto file =
            root / ("interpolation-" + std::to_string(static_cast<int>(update)) + ".orbitforge");
        project.save(file, analysis);
        const auto reopened = Project::open(file, data);
        EXPECT_EQ(reopened.to_data().satellites[0].orbit.hpop.drag.geomagnetic_update, update);
        EXPECT_EQ(reopened.prepare(data)
                      .state(reopened.objects().get("satellite-1"), last, FrameId::gcrf)
                      .position_m,
                  b.position_m);
        EXPECT_EQ(analysis.state(object, last, FrameId::gcrf).position_m, b.position_m);
    }
}

TEST_F(WeatherFiles, HpopEphemerisSplitsTheLeapSecondWeatherJumpInBothDirections) {
    if (!runtime::time_frames_available() || !runtime::ephemeris_available()) {
        GTEST_SKIP();
    }
    auto csv = daily_file();
    const auto start = csv.find("2017-01-01");
    ASSERT_NE(start, std::string::npos);
    const auto values = csv.find(",15,15,15,15,15,15,15,15,15,", start);
    ASSERT_NE(values, std::string::npos);
    csv.replace(values, 28, ",400,400,400,400,400,400,400,400,400,");
    const auto data =
        runtime::DataSnapshot::load(ORBITFORGE_DRAG_EOP)
            .with_ephemeris("de440s",
                            runtime::make_third_body_ephemeris(
                                runtime::load_spice_snapshot(ORBITFORGE_INTEGRATION_SPICE, root)))
            .with_space_weather_file("celestrak", write(csv));
    const auto first = data.time()->from_utc({2016, 12, 31, 23, 50, 0, 0});
    const auto boundary = data.time()->from_utc({2017, 1, 1, 0, 0, 0, 0});
    const auto last = data.time()->from_utc({2017, 1, 1, 0, 10, 0, 0});
    EXPECT_DOUBLE_EQ(boundary.seconds_since(first), 601);
    for (bool regularized : {false, true}) {
        SCOPED_TRACE(regularized);
        for (auto integrator : {HpopIntegrator::rk4, HpopIntegrator::rkf78, HpopIntegrator::rkv89,
                                HpopIntegrator::bulirsch_stoer, HpopIntegrator::gauss_jackson}) {
            for (bool vop : {false, true}) {
                SCOPED_TRACE(vop);

                SCOPED_TRACE(static_cast<int>(integrator));
                for (bool reverse : {false, true}) {
                    application::OrbitInput input;
                    input.propagator = application::OrbitPropagator::hpop;
                    input.initial = {reverse ? last : first,
                                     core::OriginId::earth,
                                     core::FrameId::gcrf,
                                     {6778137, 0, 0},
                                     {0, 6300, 4300}};
                    input.hpop.gravity_degree = input.hpop.gravity_order = 0;
                    input.hpop.integrator = integrator;
                    input.hpop.vop = vop;
                    input.hpop.time_regularization = regularized;
                    input.hpop.steps_per_orbit = 8192;
                    input.hpop.fixed_step_seconds = integrator == HpopIntegrator::rk4 ? 2 : 10;
                    input.hpop.drag.enabled = true;
                    input.hpop.drag.atmosphere = Model::nrlmsise00;
                    input.hpop.drag.weather_model = "celestrak";
                    input.hpop.drag.geomagnetic_update = Update::three_hourly;
                    input.hpop.drag.geomagnetic_source = Source::ap;
                    const auto env = data.force_environment(input);
                    const auto end = reverse ? first : last;
                    for (auto report : {0LL, 37'000'000'000LL}) {
                        const auto arcs = core::propagate_hpop_ephemeris(input.initial, end,
                                                                         input.hpop, env, report);
                        ASSERT_EQ(arcs.size(), 2U);
                        EXPECT_EQ(arcs[0].front().epoch, first);
                        EXPECT_EQ(arcs[1].back().epoch, last);
                        EXPECT_EQ(arcs[0].back().epoch, boundary);
                        EXPECT_EQ(arcs[1].front().epoch, boundary);
                        EXPECT_EQ(arcs[0].back().position_m, arcs[1].front().position_m);
                        EXPECT_EQ(arcs[0].back().velocity_mps, arcs[1].front().velocity_mps);
                        const auto session = make_hpop_ephemeris_session(
                            input.initial, end, input.hpop, env,
                            {OrbitInterpolationMethod::hermite, 7}, report);
                        EXPECT_EQ(session.discontinuities(), (std::vector<core::Epoch>{boundary}));
                        auto fine = input.hpop;
                        fine.time_regularization = false;
                        fine.max_step_seconds = 7;
                        fine.integrator = HpopIntegrator::rkv89;
                        fine.step_control = HpopStepControl::relative_error;
                        const auto reference = runtime::make_hpop_session(input.initial, fine, env);
                        for (auto shift : {-1LL, 0LL, 1LL}) {
                            const std::array epochs{
                                core::Epoch{boundary.tai_nanoseconds() + shift}};
                            const auto actual = session.sample(epochs).states()[0],
                                       expected = reference.sample(epochs).states()[0];
                            for (std::size_t axis = 0; axis < 3; ++axis) {
                                EXPECT_NEAR(actual.position_m[axis], expected.position_m[axis],
                                            .001);
                                EXPECT_NEAR(actual.velocity_mps[axis], expected.velocity_mps[axis],
                                            1e-6);
                            }
                        }
                    }
                    // Force an interpolation stencil inside each three-second weather arc.
                    // No support point may be borrowed from the opposite side of the jump.
                    const core::Epoch short_first{boundary.tai_nanoseconds() - 3'000'000'000},
                        short_last{boundary.tai_nanoseconds() + 3'000'000'000};
                    auto short_initial = input.initial;
                    short_initial.epoch = reverse ? short_last : short_first;
                    auto fine = input.hpop;
                    fine.time_regularization = false;
                    fine.integrator = HpopIntegrator::rkv89;
                    fine.step_control = HpopStepControl::relative_error;
                    fine.max_step_seconds = .2;
                    fine.min_step_seconds = 1e-6;
                    const auto reference = runtime::make_hpop_session(short_initial, fine, env);
                    for (auto method :
                         {OrbitInterpolationMethod::lagrange, OrbitInterpolationMethod::hermite,
                          OrbitInterpolationMethod::vop}) {
                        auto options = input.hpop;
                        options.interpolation = {method, 7};
                        const auto session = make_hpop_interpolated_session(
                            short_initial, short_first, short_last, options, env);
                        EXPECT_EQ(session.discontinuities(), (std::vector<core::Epoch>{boundary}));
                        for (auto shift : {-1LL, 0LL, 1LL}) {
                            const std::array epochs{
                                core::Epoch{boundary.tai_nanoseconds() + shift}};
                            const auto actual = session.sample(epochs).states()[0],
                                       expected = reference.sample(epochs).states()[0];
                            for (std::size_t axis = 0; axis < 3; ++axis) {
                                EXPECT_NEAR(actual.position_m[axis], expected.position_m[axis],
                                            .001);
                                EXPECT_NEAR(actual.velocity_mps[axis], expected.velocity_mps[axis],
                                            1e-6);
                            }
                        }
                    }
                }
            }
        }
    }
}
