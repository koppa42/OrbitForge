#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <limits>
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
constexpr double au = 149597870700.0;
const std::filesystem::path reference = ORBITFORGE_WEATHER_FXM_DIR;
std::string read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Missing FXM validation input");
    return {std::istreambuf_iterator<char>(input), {}};
}
std::function<double(core::Epoch)> distance(const core::ITimeProvider &time) {
    return [&time](core::Epoch at) {
        const auto utc = time.to_utc(at);
        if (utc.hour != 20 || utc.minute || utc.second || utc.nanosecond)
            throw std::runtime_error("Flux correction must query 20:00 UTC nodes");
        const int day = utc.month == 12 ? utc.day - 24 : utc.day + 7;
        return au * (.97 + .005 * day);
    };
}
class FxmWeather : public ::testing::Test {
  protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("of-fxm-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { std::filesystem::create_directories(root); }
    void TearDown() override { std::filesystem::remove_all(root); }
    auto write(const std::string &text, const std::string &name = "weather.fxm") {
        const auto path = root / name;
        std::ofstream out(path, std::ios::binary);
        out << text;
        out.close();
        return path;
    }
};
} // namespace
TEST_F(FxmWeather, ReadsObservedAndSeparatePredictionsWithoutInventingMissingData) {
    const auto weather = runtime::load_space_weather(reference / "weather_fxm.fxm");
    EXPECT_EQ(weather->info().format, "stk-flux-geomag");
    EXPECT_TRUE(weather->info().solar_flux_adjusted);
    ASSERT_EQ(weather->days().size(), 14U);
    const auto &first = weather->days().front(), &last = weather->days().back();
    EXPECT_EQ(first.date.year, 2016);
    EXPECT_EQ(first.date.day, 24);
    EXPECT_EQ(first.f107_adjusted, 110);
    EXPECT_EQ(first.adjusted_center81, 140);
    EXPECT_EQ(first.daily_kp, 3);
    EXPECT_EQ(first.ap[0], 7);
    EXPECT_FALSE(first.f107_observed);
    EXPECT_FALSE(first.observed_center81);
    EXPECT_FALSE(first.adjusted_last81);
    EXPECT_EQ(last.date.year, 2017);
    EXPECT_EQ(last.date.day, 6);
    EXPECT_EQ(last.f107_adjusted, 201);
    EXPECT_EQ(last.adjusted_center81, 151.7);
    EXPECT_EQ(last.daily_ap, 12);
    EXPECT_EQ(last.quality, core::WeatherQuality::daily_prediction);
    EXPECT_FALSE(last.daily_kp);
    EXPECT_FALSE(last.ap[0]);
    auto text = read(reference / "weather_fxm.fxm");
    const auto begin = text.find('\n') + 1;
    text[begin + 72] = '3';
    const auto missing = runtime::load_space_weather(write(text));
    EXPECT_EQ(missing->days()[0].flux_qualifier, 3);
    EXPECT_FALSE(missing->days()[0].f107_adjusted);
    EXPECT_EQ(missing->days()[0].adjusted_center81, 140);
}
TEST_F(FxmWeather, RejectsMalformedSectionsDatesFieldsAndOverlappingPredictions) {
    const auto original = read(reference / "weather_fxm.fxm");
    const auto begin = original.find('\n') + 1;
    const auto reject = [&](std::string value) {
        EXPECT_ANY_THROW((void)runtime::load_space_weather(write(value)));
    };
    reject(original.substr(0, original.find("END AP_PREDICT")));
    reject(original + "END AP_PREDICT\n");
    reject(original + "garbage\n");
    auto value = original;
    value.replace(value.find("BEGIN F10_PREDICT"), 17, "BEGIN AP_PREDICT");
    reject(value);
    value = original;
    value.insert(begin, original.substr(begin, 79));
    reject(value);
    value = original;
    value.replace(begin + 4, 4, "0230");
    reject(value);
    value = original;
    value.replace(begin + 14, 2, "22"); // Invalid observed Kp encoding.
    reject(value);
    value = original;
    value.replace(begin + 33, 3, "401");
    reject(value);
    value = original;
    value[begin + 72] = '4';
    reject(value);
    value = original;
    value.erase(begin + 77, 1);
    reject(value);
    value = original;
    const auto prediction = value.find("20170103");
    value.replace(prediction, 8, "20170102");
    reject(value);
    value = original;
    value.insert(prediction, original.substr(prediction, 19));
    reject(value);
    value = original;
    value[prediction + 8] = ',';
    reject(value);
    // F10 and Ap forecasts need not have identical end dates. A missing field
    // remains missing, and query coverage decides whether it is required.
    value = original;
    value.erase(value.find("20170106", value.find("BEGIN AP_PREDICT")), 13);
    const auto partial = runtime::load_space_weather(write(value));
    EXPECT_EQ(partial->days().back().f107_adjusted, 201);
    EXPECT_FALSE(partial->days().back().daily_ap);
}
TEST_F(FxmWeather, RequiresDistancesAndValidatesEveryLaggedNodeAndRequestedIndex) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const auto weather = runtime::load_space_weather(reference / "weather_fxm.fxm");
    const auto at = time->from_utc({2016, 12, 29, 20, 0, 0, 0});
    const auto sample = [&](const std::function<double(core::Epoch)> &provider) {
        return weather->sample(at, *time, Model::nrlmsise00, Update::daily, Source::ap, provider);
    };
    EXPECT_THROW((void)sample({}), core::SpaceWeatherCoverageError);
    for (double invalid : {0.0, -au, 1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()})
        EXPECT_THROW((void)sample([invalid](core::Epoch) { return invalid; }),
                     core::SpaceWeatherCoverageError);
    const auto first = time->from_utc({2016, 12, 29, 20, 0, 0, 0});
    const auto last = time->from_utc({2017, 1, 2, 20, 0, 0, 0});
    const auto gap = [&](core::Epoch node) {
        if (time->to_utc(node).day == 30)
            throw core::SpaceWeatherCoverageError("Synthetic Sun ephemeris gap");
        return distance(*time)(node);
    };
    EXPECT_NO_THROW((void)sample(gap));
    EXPECT_NO_THROW(
        (void)weather->sample(last, *time, Model::nrlmsise00, Update::daily, Source::ap, gap));
    EXPECT_THROW(weather->validate_interval(first, last, *time, Model::nrlmsise00, Update::daily,
                                            Source::ap, gap),
                 core::SpaceWeatherCoverageError);
    const auto forecast = time->from_utc({2017, 1, 4, 12, 0, 0, 0});
    EXPECT_NO_THROW((void)weather->sample(forecast, *time, Model::nrlmsise00, Update::daily,
                                          Source::ap, distance(*time)));
    EXPECT_THROW((void)weather->sample(forecast, *time, Model::nrlmsise00, Update::three_hourly,
                                       Source::ap, distance(*time)),
                 core::SpaceWeatherCoverageError);
    EXPECT_THROW((void)weather->sample(forecast, *time, Model::nrlmsise00, Update::daily,
                                       Source::kp, distance(*time)),
                 core::SpaceWeatherCoverageError);
}
TEST_F(FxmWeather, MatchesIndependentScipyFluxAndOriginalGmatDensityAcrossLeapSecond) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const auto weather = runtime::load_space_weather(reference / "weather_fxm.fxm");
    std::istringstream input(read(reference / "weather_fxm.txt"));
    std::string line;
    int samples = 0, densities = 0;
    double maximum_flux = 0, maximum_density = 0;
    while (std::getline(input, line)) {
        std::istringstream row(line.substr(2));
        if (line.starts_with("S ")) {
            int model;
            std::int64_t ns;
            double flux, average, ap, kp;
            ASSERT_TRUE(row >> model >> ns >> flux >> average >> ap >> kp);
            const auto actual = weather->sample(
                core::Epoch{ns}, *time, model == 0 ? Model::jacchia_roberts : Model::nrlmsise00,
                Update::daily, Source::ap, distance(*time));
            for (const auto error :
                 {std::abs(actual.f107 - flux), std::abs(actual.f107_average - average)})
                maximum_flux = std::max(maximum_flux, error);
            EXPECT_NEAR(actual.f107, flux, 3e-11) << line;
            EXPECT_NEAR(actual.f107_average, average, 3e-11) << line;
            EXPECT_DOUBLE_EQ(actual.ap, ap);
            EXPECT_NEAR(actual.kp, kp, 1e-14);
            ++samples;
        } else if (line.starts_with("R ")) {
            std::int64_t ns;
            int year, month, day;
            double seconds, height, latitude, angle, declination, expected;
            ASSERT_TRUE(row >> ns >> year >> month >> day >> seconds >> height >> latitude >>
                        angle >> declination >> expected);
            const auto sample = weather->sample(core::Epoch{ns}, *time, Model::jacchia_roberts,
                                                Update::daily, Source::ap, distance(*time));
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
            const double actual =
                core::evaluate_atmosphere(Model::jacchia_roberts, in).density_kg_m3;
            maximum_density = std::max(maximum_density, std::abs(actual / expected - 1));
            EXPECT_NEAR(actual, expected, expected * 2e-10) << line;
            ++densities;
        }
    }
    EXPECT_EQ(samples, 74);
    EXPECT_EQ(densities, 37);
    RecordProperty("maximum_flux_error_sfu", maximum_flux);
    RecordProperty("maximum_relative_density_error", maximum_density);
}
TEST_F(FxmWeather, CatalogFailureAndCancellationPreserveRawBytesAndCapturedWeather) {
    if (!runtime::data_catalog_available())
        GTEST_SKIP();
    runtime::DataCatalog catalog(root / "catalog");
    const auto original = read(reference / "weather_fxm.fxm");
    const auto installed = catalog.install_space_weather(write(original), "fxm");
    const auto captured = catalog.load_space_weather("fxm");
    EXPECT_EQ(read(installed.file), original);
    EXPECT_TRUE(captured->info().solar_flux_adjusted);
    EXPECT_TRUE(catalog.weather_sources().at(0).info.solar_flux_adjusted);
    EXPECT_EQ(runtime::DataCatalog(root / "catalog").load_space_weather("fxm")->info().sha256,
              captured->info().sha256);
    EXPECT_THROW((void)catalog.install_space_weather(write("BEGIN OBSERVED\n"), "fxm"),
                 std::runtime_error);
    EXPECT_THROW((void)catalog.install_space_weather(write(original + "\n"), "fxm", "",
                                                     runtime::DataAcquisition::imported,
                                                     [] { throw std::runtime_error("cancel"); }),
                 std::runtime_error);
    EXPECT_EQ(catalog.load_space_weather("fxm")->info().sha256, captured->info().sha256);
    auto info = captured->info();
    const auto adjusted = runtime::space_weather_reference("fxm", info);
    info.solar_flux_adjusted = false;
    EXPECT_NE(adjusted, runtime::space_weather_reference("fxm", info));
}
TEST_F(FxmWeather, HpopUsesFrozenGeometricDistanceAndMatchesObservedFileThroughProjectReopen) {
    if (!runtime::time_frames_available() || !runtime::ephemeris_available())
        GTEST_SKIP();
    const auto ephemeris = runtime::make_third_body_ephemeris(
        runtime::load_spice_snapshot(ORBITFORGE_INTEGRATION_SPICE, root));
    const auto base =
        runtime::DataSnapshot::load(ORBITFORGE_DRAG_EOP).with_ephemeris("de440s", ephemeris);
    const auto raw = runtime::load_space_weather(reference / "weather_fxm.fxm");
    std::ostringstream csv;
    csv << "DATE,BSRN,ND,KP1,KP2,KP3,KP4,KP5,KP6,KP7,KP8,KP_SUM,AP1,AP2,AP3,AP4,AP5,AP6,AP7,AP8,AP_"
           "AVG,CP,C9,ISN,F10.7_OBS,F10.7_ADJ,F10.7_DATA_TYPE,F10.7_OBS_CENTER81,F10.7_OBS_LAST81,"
           "F10.7_ADJ_CENTER81,F10.7_ADJ_LAST81\n";
    csv << std::setprecision(17);
    for (const auto &d : raw->days()) {
        if ((d.date.month == 12 && d.date.day < 29) || (d.date.month == 1 && d.date.day > 3))
            continue;
        auto utc = d.date;
        utc.hour = 20;
        const auto sun = ephemeris->position(core::ThirdBody::sun, base.time()->from_utc(utc));
        const double factor = std::pow(au / std::hypot(sun[0], sun[1], sun[2]), 2);
        csv << d.date.year << '-' << std::setw(2) << std::setfill('0') << d.date.month << '-'
            << std::setw(2) << d.date.day
            << ",1,1,30,30,30,30,30,30,30,30,240,15,15,15,15,15,15,15,15," << *d.daily_ap
            << ",0,0,0," << *d.f107_adjusted * factor << ",150,PRD,"
            << *d.adjusted_center81 * factor << ",150,150,150\n";
    }
    const auto observed = base.with_space_weather_file("weather", write(csv.str(), "observed.csv"));
    const auto adjusted = base.with_space_weather_file("weather", reference / "weather_fxm.fxm");
    const auto first = base.time()->from_utc({2017, 1, 3, 0, 0, 0, 0});
    const core::Epoch last{first.tai_nanoseconds() + 600000000000LL};
    double maximum = 0;
    for (auto model : {Model::jacchia_roberts, Model::nrlmsise00}) {
        application::Project p{"FXM", first, last};
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
        orbit.hpop.drag.atmosphere = model;
        orbit.hpop.drag.use_apparent_sun_position = true;
        orbit.hpop.drag.low_altitude_model = core::LowAltitudeDensityModel::nrlmsise00;
        orbit.hpop.drag.weather_model = "weather";
        p.satellites.push_back(satellite);
        const auto project = Project::from_data(p);
        const auto object = project.objects().get("satellite-1");
        const auto analysis = project.prepare(adjusted);
        const auto actual = analysis.state(object, last, FrameId::gcrf);
        const auto expected = project.prepare(observed).state(object, last, FrameId::gcrf);
        for (int i = 0; i < 3; ++i) {
            maximum = std::max(maximum, std::abs(actual.position_m[i] - expected.position_m[i]));
            EXPECT_NEAR(actual.position_m[i], expected.position_m[i], 1e-5);
            EXPECT_NEAR(actual.velocity_mps[i], expected.velocity_mps[i], 1e-8);
        }
        const auto saved =
            root / (model == Model::jacchia_roberts ? "jr.orbitforge" : "nrl.orbitforge");
        project.save(saved, analysis);
        const auto reopened = Project::open(saved, adjusted);
        EXPECT_EQ(reopened.prepare(adjusted)
                      .state(reopened.objects().get("satellite-1"), last, FrameId::gcrf)
                      .position_m,
                  actual.position_m);
        EXPECT_THROW((void)reopened.prepare(observed), runtime::DataChangedError);
        EXPECT_EQ(analysis.state(object, last, FrameId::gcrf).position_m, actual.position_m);
    }
    RecordProperty("maximum_observed_file_position_difference_m", maximum);
}
