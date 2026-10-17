#include <chrono>
#include <cmath>
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
const std::filesystem::path reference = ORBITFORGE_WEATHER_MONTHLY_DIR;
std::string read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Missing monthly weather validation input");
    return {std::istreambuf_iterator<char>(input), {}};
}
std::string replace(std::string text, const std::string &from, const std::string &to) {
    const auto at = text.find(from);
    if (at == std::string::npos)
        throw std::runtime_error("Missing mutation marker");
    text.replace(at, from.size(), to);
    return text;
}
std::string field(std::string text, const std::string &date, int column, const std::string &value) {
    auto first = text.find(date);
    if (first == std::string::npos)
        throw std::runtime_error("Missing CSV date");
    for (int i = 0; i < column; ++i)
        first = text.find(',', first) + 1;
    const auto last = text.find(',', first);
    text.replace(first, last - first, value);
    return text;
}
class MonthlyWeather : public ::testing::Test {
  protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("of-monthly-weather-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { std::filesystem::create_directories(root); }
    void TearDown() override { std::filesystem::remove_all(root); }
    auto write(const std::string &text, const std::string &name = "weather.dat") {
        const auto file = root / name;
        std::ofstream output(file, std::ios::binary);
        output << text;
        output.close();
        return file;
    }
};
} // namespace
TEST_F(MonthlyWeather, NominalCssiMetadataAndRawMissingFieldsRemainExplicit) {
    const auto weather = runtime::load_space_weather(reference / "weather_cssi.dat");
    EXPECT_EQ(weather->info().format, "cssi-predicts");
    EXPECT_TRUE(weather->info().cssi_predictions);
    EXPECT_FALSE(weather->info().solar_flux_adjusted);
    ASSERT_EQ(weather->days().size(), 18U);
    const auto &first = weather->days().front();
    EXPECT_EQ(first.date.day, 15);
    EXPECT_EQ(first.quality, core::WeatherQuality::monthly_prediction);
    EXPECT_EQ(first.f107_observed, 90);
    EXPECT_EQ(first.daily_ap, 4);
    EXPECT_FALSE(first.daily_kp);
    EXPECT_FALSE(first.observed_center81);
    EXPECT_FALSE(first.ap[0]);
    const auto monthly = runtime::load_space_weather(reference / "weather_monthly.csv");
    EXPECT_FALSE(monthly->info().cssi_predictions);
    EXPECT_EQ(monthly->days()[4].date.day, 1);
    EXPECT_EQ(monthly->days()[4].observed_center81, 155);
    EXPECT_FALSE(monthly->days()[4].ap[0]);
}
TEST_F(MonthlyWeather, MatchesIndependentScipyFluxIndicesAndNumpyCenteredMeansAndHistory) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const std::array weather{runtime::load_space_weather(reference / "weather_cssi.dat"),
                             runtime::load_space_weather(reference / "weather_monthly.csv")};
    std::istringstream input(read(reference / "weather_monthly.txt"));
    std::string line;
    std::size_t count = 0;
    double max_flux = 0, max_index = 0, max_history = 0;
    while (std::getline(input, line)) {
        if (!line.starts_with("S "))
            continue;
        std::istringstream row(line.substr(2));
        int dataset, model, update, source, history;
        std::int64_t ns;
        double f, avg, kp, ap;
        ASSERT_TRUE(row >> dataset >> model >> update >> source >> ns >> f >> avg >> kp >> ap >>
                    history);
        const auto actual =
            weather.at(dataset)->sample(core::Epoch{ns}, *time, static_cast<Model>(model),
                                        static_cast<Update>(update), static_cast<Source>(source));
        for (double error : {std::abs(actual.f107 - f), std::abs(actual.f107_average - avg)})
            max_flux = std::max(max_flux, error);
        for (double error : {std::abs(actual.kp - kp), std::abs(actual.ap - ap)})
            max_index = std::max(max_index, error);
        EXPECT_NEAR(actual.f107, f, 3e-10) << line;
        EXPECT_NEAR(actual.f107_average, avg, 3e-10) << line;
        EXPECT_NEAR(actual.kp, kp, 3e-11) << line;
        EXPECT_NEAR(actual.ap, ap, 3e-10) << line;
        ASSERT_EQ(actual.ap_history.has_value(), history != 0);
        for (int i = 0; i < 7; ++i) {
            double expected;
            ASSERT_TRUE(row >> expected);
            if (history) {
                max_history = std::max(max_history, std::abs(actual.ap_history->at(i) - expected));
                EXPECT_NEAR(actual.ap_history->at(i), expected, 3e-10) << line;
            }
        }
        ++count;
    }
    EXPECT_EQ(count, 1056U);
    RecordProperty("maximum_flux_error_sfu", max_flux);
    RecordProperty("maximum_index_error", max_index);
    RecordProperty("maximum_history_error", max_history);
}
TEST_F(MonthlyWeather, MonthlyWeatherChainMatchesOriginalGmatDensity) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const std::array weather{runtime::load_space_weather(reference / "weather_cssi.dat"),
                             runtime::load_space_weather(reference / "weather_monthly.csv")};
    std::istringstream input(read(reference / "weather_monthly.txt"));
    std::string line;
    std::size_t count = 0;
    double maximum = 0;
    while (std::getline(input, line)) {
        if (!line.starts_with("R "))
            continue;
        std::istringstream row(line.substr(2));
        int dataset, model, update, source;
        std::int64_t ns;
        double height, latitude, angle, declination, expected;
        ASSERT_TRUE(row >> dataset >> model >> update >> source >> ns >> height >> latitude >>
                    angle >> declination >> expected);
        const auto sample =
            weather.at(dataset)->sample(core::Epoch{ns}, *time, static_cast<Model>(model),
                                        static_cast<Update>(update), static_cast<Source>(source));
        core::AtmosphereInput in;
        in.utc = time->to_utc(core::Epoch{ns});
        in.geodetic = {.5, latitude, height};
        in.solar_hour_angle_rad = angle;
        in.sun_declination_rad = declination;
        in.f107 = sample.f107;
        in.f107_average = sample.f107_average;
        in.kp = sample.kp;
        const double rho = core::evaluate_atmosphere(Model::jacchia_roberts, in).density_kg_m3;
        maximum = std::max(maximum, std::abs(rho / expected - 1));
        EXPECT_NEAR(rho, expected, expected * 2e-10) << line;
        ++count;
    }
    EXPECT_EQ(count, 32U);
    RecordProperty("maximum_relative_density_error", maximum);
}
TEST_F(MonthlyWeather, InvalidHeaderCountSpanColumnsAndValuesAreRejected) {
    const auto original = read(reference / "weather_cssi.dat");
    for (const auto &text :
         {replace(original, "2015 11 2017 4 2015 10 18", "2015 11 2017 4 2015 10 17"),
          replace(original, "2015 11 2017 4 2015 10 18", "2015 10 2017 4 2015 10 18"),
          replace(original, "2015 11 2017 4 2015 10 18", "2015 11 2017 4 2015 13 18"),
          replace(original, "2015 11 110 90 7 4", "2015 11 110 nan 7 4"),
          replace(original, "2015 11 110 90 7 4", "2015 11 110 90 7 -1"),
          replace(original, "2015 11 110 90 7 4", "2015 11 110 90 7 401"),
          replace(original, "2015 11 110 90 7 4", "2015 11 110 90 7 4 0"),
          replace(original, "2015 12 140 120 28 25", "2015 11 140 120 28 25"),
          original + "2017 5 150 130 10 7\n"}) {
        const auto file = write(text);
        EXPECT_THROW((void)runtime::load_space_weather(file), std::exception);
        EXPECT_EQ(read(file), text);
    }
}
TEST_F(MonthlyWeather, MissingMonthsFieldsAndCenteredAverageMarginsAreNotExtrapolated) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const auto original = read(reference / "weather_cssi.dat");
    const auto weather = runtime::load_space_weather(reference / "weather_cssi.dat");
    const auto sample = [&](const auto &w, core::Epoch epoch, Source source = Source::ap) {
        return w->sample(epoch, *time, Model::nrlmsise00, Update::daily, source);
    };
    const auto earliest = time->from_utc({2015, 12, 25, 20, 0, 0, 0});
    EXPECT_NO_THROW((void)sample(weather, earliest));
    EXPECT_NO_THROW(weather->validate_interval(earliest, earliest, *time, Model::nrlmsise00,
                                               Update::daily, Source::kp));
    EXPECT_THROW((void)sample(weather, core::Epoch{earliest.tai_nanoseconds() - 1}),
                 core::SpaceWeatherCoverageError);
    auto missing = replace(original, "2015 11 2017 4 2015 10 18", "2015 11 2017 4 2015 10 17");
    missing = replace(missing, "2016 11 150 130 15 12\n", "");
    const auto gap = runtime::load_space_weather(write(missing));
    const auto first = time->from_utc({2016, 8, 1, 20, 0, 0, 0}),
               last = time->from_utc({2017, 2, 1, 20, 0, 0, 0});
    EXPECT_NO_THROW((void)sample(gap, first));
    EXPECT_NO_THROW((void)sample(gap, last));
    EXPECT_THROW(
        gap->validate_interval(first, last, *time, Model::nrlmsise00, Update::daily, Source::ap),
        core::SpaceWeatherCoverageError);
    const auto monthly = runtime::load_space_weather(reference / "weather_monthly.csv");
    const auto final_node = time->from_utc({2017, 4, 1, 0, 0, 0, 0});
    for (const auto model : {Model::jacchia_roberts, Model::nrlmsise00})
        for (const auto update :
             {Update::daily, Update::three_hourly, Update::three_hourly_interpolated,
              Update::three_hourly_cubic_spline}) {
            const core::Epoch end{final_node.tai_nanoseconds() +
                                  (model == Model::jacchia_roberts ? 24120000000000LL : 0LL)};
            const core::Epoch begin{end.tai_nanoseconds() - 3600000000000LL};
            EXPECT_NO_THROW(
                monthly->validate_interval(begin, end, *time, model, update, Source::ap));
            EXPECT_THROW(monthly->validate_interval(begin, core::Epoch{end.tai_nanoseconds() + 1},
                                                    *time, model, update, Source::ap),
                         core::SpaceWeatherCoverageError);
        }
    auto csv = read(reference / "weather_monthly.csv");
    csv = field(csv, "2017-01-01", 20, "");
    const auto no_ap = runtime::load_space_weather(write(csv, "missing.csv"));
    const auto at = time->from_utc({2017, 1, 4, 12, 0, 0, 0});
    EXPECT_THROW((void)sample(no_ap, at), core::SpaceWeatherCoverageError);
    EXPECT_NO_THROW((void)sample(no_ap, at, Source::kp));
    csv = field(read(reference / "weather_monthly.csv"), "2017-01-01", 27, "");
    const auto no_average = runtime::load_space_weather(write(csv, "no-average.csv"));
    EXPECT_THROW((void)sample(no_average, at), core::SpaceWeatherCoverageError);
    EXPECT_THROW((void)sample(weather, time->from_utc({2017, 4, 15, 20, 0, 0, 0})),
                 core::SpaceWeatherCoverageError);
}
TEST_F(MonthlyWeather, CatalogPreservesRawBytesMetadataAndCanceledReplacement) {
    const auto original = read(reference / "weather_cssi.dat");
    const auto file = write(original);
    runtime::DataCatalog catalog(root / "catalog");
    const auto installed = catalog.install_space_weather(file, "cssi");
    const auto frozen = catalog.load_space_weather("cssi");
    EXPECT_EQ(read(installed.file), original);
    EXPECT_TRUE(catalog.weather_sources().at(0).info.cssi_predictions);
    EXPECT_TRUE(
        runtime::DataCatalog(root / "catalog").weather_sources().at(0).info.cssi_predictions);
    EXPECT_THROW((void)catalog.install_space_weather(write(original + "\n"), "cssi", "",
                                                     runtime::DataAcquisition::imported,
                                                     [] { throw std::runtime_error("cancel"); }),
                 std::runtime_error);
    EXPECT_EQ(catalog.load_space_weather("cssi")->info().sha256, frozen->info().sha256);
    auto info = frozen->info();
    const auto before = runtime::space_weather_reference("cssi", info);
    info.cssi_predictions = false;
    EXPECT_NE(before, runtime::space_weather_reference("cssi", info));
}
TEST_F(MonthlyWeather, HpopConvergesAcrossLeapAndForecastNodesAndReopensFrozenData) {
    if (!runtime::time_frames_available() || !runtime::ephemeris_available())
        GTEST_SKIP();
    const auto base =
        runtime::DataSnapshot::load(ORBITFORGE_DRAG_EOP)
            .with_ephemeris("de440s",
                            runtime::make_third_body_ephemeris(
                                runtime::load_spice_snapshot(ORBITFORGE_INTEGRATION_SPICE, root)));
    const auto epoch = base.time()->from_utc({2017, 1, 1, 0, 0, 0, 0});
    const core::Epoch first{epoch.tai_nanoseconds() - 21600000000000LL},
        last{epoch.tai_nanoseconds() + 21600000000000LL};
    double maximum = 0, maximum_velocity = 0;
    for (int dataset = 0; dataset < 2; ++dataset)
        for (const auto model : {Model::jacchia_roberts, Model::nrlmsise00}) {
            const auto name =
                std::to_string(dataset) + "-" + std::to_string(static_cast<int>(model));
            SCOPED_TRACE(name);
            const auto raw =
                read(reference / (dataset == 0 ? "weather_cssi.dat" : "weather_monthly.csv"));
            const auto file = write(raw, name + ".dat");
            const auto data = base.with_space_weather_file("forecast", file);
            application::Project settings{"Forecast", first, last};
            settings.satellites.emplace_back();
            auto &orbit = settings.satellites[0].orbit;
            orbit.propagator = application::OrbitPropagator::hpop;
            orbit.first = first;
            orbit.last = last;
            orbit.initial.epoch = epoch;
            orbit.initial.position_m = {6778137, 0, 0};
            orbit.initial.velocity_mps = {0, 6300, 4300};
            auto &h = orbit.hpop;
            h.gravity_degree = h.gravity_order = 0;
            h.max_step_seconds = 30;
            // Continuous seventh-order coasts have no artificial 300 s integration restarts.
            // A 30 s ceiling and tighter absolute control retain the six-hour global
            // 1 mm / 1e-6 m/s refinement bounds; local tolerance alone cannot guarantee them.
            h.relative_tolerance = 1e-14;
            h.position_tolerance_m = 1e-8;
            h.velocity_tolerance_mps = 1e-11;
            h.drag.enabled = true;
            h.drag.atmosphere = model;
            h.drag.weather_model = "forecast";
            h.drag.geomagnetic_update = Update::three_hourly_interpolated;
            h.drag.geomagnetic_source = Source::kp;
            const auto project = Project::from_data(settings);
            const auto analysis = project.prepare(data);
            const auto object = project.objects().get("satellite-1");
            const auto a = analysis.state(object, first, FrameId::gcrf),
                       b = analysis.state(object, last, FrameId::gcrf);
            h.max_step_seconds = 7;
            const auto refined = Project::from_data(settings);
            const auto finer = refined.prepare(data);
            for (const auto &state : {a, b}) {
                const auto other =
                    finer.state(refined.objects().get("satellite-1"), state.epoch, FrameId::gcrf);
                for (std::size_t i = 0; i < 3; ++i) {
                    maximum =
                        std::max(maximum, std::abs(state.position_m[i] - other.position_m[i]));
                    maximum_velocity = std::max(
                        maximum_velocity, std::abs(state.velocity_mps[i] - other.velocity_mps[i]));
                    EXPECT_NEAR(state.position_m[i], other.position_m[i], 1e-3);
                    EXPECT_NEAR(state.velocity_mps[i], other.velocity_mps[i], 1e-6);
                }
            }
            const auto saved = root / (name + ".orbitforge");
            project.save(saved, analysis);
            const auto reopened = Project::open(saved, data);
            EXPECT_EQ(reopened.prepare(data)
                          .state(reopened.objects().get("satellite-1"), last, FrameId::gcrf)
                          .position_m,
                      b.position_m);
            if (dataset == 0) {
                const auto changed = replace(raw, "2016 12 100 80 33 30", "2016 12 220 200 33 30");
                const auto changed_data =
                    base.with_space_weather_file("forecast", write(changed, name + "-changed.dat"));
                EXPECT_THROW((void)reopened.prepare(changed_data), runtime::DataChangedError);
                const auto changed_project = Project::from_data(settings);
                EXPECT_NE(
                    changed_project.prepare(changed_data)
                        .state(changed_project.objects().get("satellite-1"), last, FrameId::gcrf)
                        .position_m,
                    b.position_m);
                EXPECT_EQ(analysis.state(object, last, FrameId::gcrf).position_m, b.position_m);
            }
        }
    RecordProperty("maximum_step_refinement_position_m", maximum);
    RecordProperty("maximum_step_refinement_velocity_mps", maximum_velocity);
}
