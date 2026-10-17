#include <algorithm>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <sstream>
using namespace orbitforge;
namespace {
using Model = core::AtmosphereModel;
using Update = core::GeomagneticUpdate;
using Source = core::GeomagneticSource;
constexpr auto interp = Update::three_hourly_interpolated;
std::ifstream reference() {
    std::ifstream input(ORBITFORGE_WEATHER_MEAN_REFERENCE);
    if (!input)
        throw std::runtime_error("Missing independent mean-preserving weather reference");
    return input;
}
std::vector<core::SpaceWeatherDay> mean_records() {
    std::vector<core::SpaceWeatherDay> days(16);
    for (int i = 0; i < 16; ++i) {
        auto &d = days[i];
        d.date = i < 10 ? core::UtcDateTime{2016, 12, 22 + i, 0, 0, 0, 0}
                        : core::UtcDateTime{2017, 1, i - 9, 0, 0, 0, 0};
        d.f107_observed = d.observed_center81 = 150;
        d.daily_ap = 15;
        d.daily_kp = 3;
    }
    auto input = reference();
    std::string line;
    while (std::getline(input, line)) {
        if (!line.starts_with("D "))
            continue;
        std::istringstream row(line.substr(2));
        int d, slot;
        double ap, kp;
        if (!(row >> d >> slot >> ap >> kp))
            throw std::runtime_error("Invalid independent mean input");
        if (ap >= 0)
            days.at(d + 2).ap.at(slot) = ap;
        if (kp >= 0)
            days.at(d + 2).kp.at(slot) = kp;
    }
    return days;
}
core::Epoch query(const core::ITimeProvider &time, core::UtcDateTime weather_utc) {
    return core::Epoch{time.from_utc(weather_utc).tai_nanoseconds() + 24120000000000LL};
}
} // namespace
TEST(MeanWeather, MatchesIndependentScipyInQuietStormLimitedAndIsolatedRuns) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const core::SpaceWeather weather{{}, mean_records()};
    auto input = reference();
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
        const auto sample = weather.sample(core::Epoch{ns}, *time, Model::jacchia_roberts, interp,
                                           source == 0 ? Source::ap : Source::kp);
        const double actual = source == 0 ? sample.ap : sample.kp;
        // SciPy fits the cumulative integral (~1e4); its derivative includes that rounding.
        EXPECT_NEAR(actual, expected, source == 0 ? 1e-10 : 1e-11) << line;
        EXPECT_GE(actual, 0);
        EXPECT_LE(actual, source == 0 ? 400 : 9);
        maximum = std::max(maximum, std::abs(actual - expected));
        ++count;
    }
    EXPECT_EQ(count, 1314U);
    RecordProperty("maximum_index_error", maximum);
}
TEST(MeanWeather, GaussQuadratureRetainsEveryWindowMeanIncludingPositiveLeapSecond) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const core::SpaceWeather weather{{}, mean_records()};
    auto input = reference();
    std::string line;
    std::size_t count = 0, leap = 0;
    double maximum = 0;
    while (std::getline(input, line)) {
        if (!line.starts_with("Q "))
            continue;
        std::istringstream row(line.substr(2));
        int source;
        std::int64_t first, last;
        double expected, mean = 0;
        ASSERT_TRUE(row >> source >> first >> last >> expected);
        for (int i = 0; i < 5; ++i) {
            std::int64_t ns;
            double weight;
            ASSERT_TRUE(row >> ns >> weight);
            const auto sample = weather.sample(core::Epoch{ns}, *time, Model::jacchia_roberts,
                                               interp, source == 0 ? Source::ap : Source::kp);
            mean += weight * (source == 0 ? sample.ap : sample.kp);
        }
        maximum = std::max(maximum, std::abs(mean - expected));
        // Quadratic integration is exact; only ns-rounded quadrature positions and FP remain.
        EXPECT_NEAR(mean, expected, 1e-10) << line;
        if (last - first == 10801000000000LL)
            ++leap;
        ++count;
    }
    EXPECT_EQ(count, 218U);
    EXPECT_EQ(leap, 2U);
    RecordProperty("maximum_bin_mean_error", maximum);
}
TEST(MeanWeather, MatchesOriginalGmatDensityWithIndependentInterpolatedInputs) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    const core::SpaceWeather weather{{}, mean_records()};
    auto input = reference();
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
        const auto sample = weather.sample(core::Epoch{ns}, *time, Model::jacchia_roberts, interp,
                                           source == 0 ? Source::ap : Source::kp);
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
    EXPECT_EQ(count, 70U);
    RecordProperty("maximum_relative_density_error", maximum);
}
TEST(MeanWeather, DoesNotBridgeMissingSlotsDatesOrMonthlyRecords) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    auto days = mean_records();
    const core::SpaceWeather weather{{}, days};
    const auto sample = [&](const core::SpaceWeather &w, core::UtcDateTime utc,
                            Source source = Source::ap) {
        return w.sample(query(*time, utc), *time, Model::jacchia_roberts, interp, source);
    };
    EXPECT_NO_THROW((void)sample(weather, {2016, 12, 24, 0, 0, 0, 0}));
    EXPECT_NO_THROW((void)sample(weather, {2017, 1, 6, 23, 59, 59, 999999999}));
    EXPECT_THROW((void)sample(weather, {2017, 1, 7, 0, 0, 0, 0}), core::SpaceWeatherCoverageError);
    EXPECT_THROW((void)sample(weather, {2016, 12, 23, 23, 59, 59, 999999999}),
                 core::SpaceWeatherCoverageError);
    // The single recorded Ap cell between two missing cells has its own constant profile.
    EXPECT_EQ(sample(weather, {2016, 12, 29, 3, 0, 0, 0}).ap, 17);
    EXPECT_EQ(sample(weather, {2016, 12, 29, 5, 59, 59, 999999999}).ap, 17);
    EXPECT_THROW((void)sample(weather, {2016, 12, 29, 6, 0, 0, 0}),
                 core::SpaceWeatherCoverageError);
    EXPECT_NO_THROW((void)sample(weather, {2016, 12, 29, 6, 0, 0, 0}, Source::kp));
    EXPECT_THROW(weather.validate_interval(query(*time, {2016, 12, 28, 23, 0, 0, 0}),
                                           query(*time, {2016, 12, 29, 10, 0, 0, 0}), *time,
                                           Model::jacchia_roberts, interp, Source::ap),
                 core::SpaceWeatherCoverageError);
    for (auto &d : days)
        d.ap.fill(15);
    days.erase(days.begin() + 7); // Missing 2016-12-29 splits otherwise constant runs.
    const core::SpaceWeather date_gap{{}, days};
    EXPECT_EQ(sample(date_gap, {2016, 12, 28, 23, 59, 59, 0}).ap, 15);
    EXPECT_THROW((void)sample(date_gap, {2016, 12, 29, 6, 0, 0, 0}),
                 core::SpaceWeatherCoverageError);
    days = mean_records();
    days[7].quality = core::WeatherQuality::monthly_prediction;
    const core::SpaceWeather monthly{{}, days};
    EXPECT_THROW((void)sample(monthly, {2016, 12, 29, 3, 0, 0, 0}),
                 core::SpaceWeatherCoverageError);
    EXPECT_EQ(sample(weather, {2016, 12, 29, 3, 0, 0, 0}).ap, 17); // Original remains immutable.
}
TEST(MeanWeather, NrlUsesNativeHistoryAndDraftsPreserveTheirOwnIdentity) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto time = runtime::make_builtin_time_provider();
    auto days = mean_records();
    // Complete the deliberate holes needed by native 57-hour Ap history.
    for (auto &d : days) {
        for (auto &v : d.ap)
            if (!v)
                v = 15;
        for (auto &v : d.kp)
            if (!v)
                v = 3;
    }
    const core::SpaceWeather weather{{}, days};
    for (const auto utc : {core::UtcDateTime{2016, 12, 31, 23, 59, 60, 0},
                           core::UtcDateTime{2017, 1, 1, 0, 0, 0, 0}})
        for (const auto source : {Source::ap, Source::kp}) {
            const auto epoch = time->from_utc(utc);
            const auto a =
                weather.sample(epoch, *time, Model::nrlmsise00, Update::three_hourly, source);
            const auto b = weather.sample(epoch, *time, Model::nrlmsise00, interp, source);
            EXPECT_EQ(a.ap_history, b.ap_history);
            EXPECT_EQ(a.ap, b.ap);
            EXPECT_EQ(a.kp, b.kp);
            EXPECT_NO_THROW(
                weather.validate_interval(epoch, epoch, *time, Model::nrlmsise00, interp, source));
        }
    application::Project p{"Mean interpolation", core::Epoch{0}, core::Epoch{1000000000}};
    p.satellites.emplace_back();
    auto &orbit = p.satellites[0].orbit;
    orbit.propagator = application::OrbitPropagator::hpop;
    orbit.first = core::Epoch{0};
    orbit.last = core::Epoch{1000000000};
    orbit.initial.epoch = core::Epoch{0};
    orbit.initial.position_m = {7000000, 0, 0};
    orbit.initial.velocity_mps = {0, 7500, 0};
    orbit.hpop.gravity_degree = orbit.hpop.gravity_order = 0;
    auto &drag = orbit.hpop.drag;
    drag.enabled = true;
    drag.weather_model = "celestrak";
    drag.atmosphere = Model::nrlmsise00;
    drag.geomagnetic_update = interp;
    const auto hash = application::analysis_input_sha256(p);
    EXPECT_EQ(core::effective_drag_options(drag).geomagnetic_update, Update::three_hourly);
    EXPECT_EQ(application::decode_project(application::encode_project(p))
                  .satellites[0]
                  .orbit.hpop.drag.geomagnetic_update,
              interp);
    drag.geomagnetic_update = Update::three_hourly;
    EXPECT_EQ(application::analysis_input_sha256(p), hash);
    drag.atmosphere = Model::jacchia_roberts;
    const auto jr = application::analysis_input_sha256(p);
    drag.geomagnetic_update = interp;
    EXPECT_NE(application::analysis_input_sha256(p), jr);
    drag.enabled = false;
    const auto inactive = application::analysis_input_sha256(p);
    drag.geomagnetic_update = Update::three_hourly_cubic_spline;
    EXPECT_EQ(application::analysis_input_sha256(p), inactive);
}
