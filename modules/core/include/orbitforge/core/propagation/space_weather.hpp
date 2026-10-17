#pragma once
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <orbitforge/core/time/time_provider.hpp>
#include <span>
#include <vector>

namespace orbitforge::core {
enum class AtmosphereModel { jacchia_roberts, nrlmsise00 };
enum class GeomagneticUpdate {
    daily,
    three_hourly,
    three_hourly_interpolated,
    three_hourly_cubic_spline
};
enum class GeomagneticSource { ap, kp };
enum class WeatherQuality { observed, interpolated, daily_prediction, monthly_prediction };
struct SpaceWeatherDay {
    UtcDateTime date{2000, 1, 1, 0, 0, 0, 0};
    std::array<std::optional<double>, 8> kp{}, ap{};
    std::optional<double> daily_kp, daily_ap;
    // sfu. Observed and 1-AU-adjusted fluxes are distinct; centered and trailing
    // 81-day means are distinct. Missing input remains missing, never zero-filled.
    std::optional<double> f107_observed, f107_adjusted;
    std::optional<double> observed_center81, observed_last81;
    std::optional<double> adjusted_center81, adjusted_last81;
    std::optional<int> flux_qualifier; // Original fixed-width Q flag, when present.
    WeatherQuality quality = WeatherQuality::observed;
};
struct SpaceWeatherInfo {
    std::string sha256, source_url, format;
    UtcDateTime first{}, last{}; // Inclusive raw record dates, not propagation coverage.
    bool solar_flux_adjusted = false; // FXM: use 1-AU fields and an explicit Sun distance.
    bool cssi_predictions = false;    // Nominal monthly .dat; Ap only, derive centered 81-day flux.
};
struct SpaceWeatherSample {
    double f107, f107_average, kp, ap;
    std::optional<std::array<double, 7>> ap_history;
};
class SpaceWeatherCoverageError : public std::out_of_range {
    using std::out_of_range::out_of_range;
};
// Immutable parsed weather. Thread-safe, owns all values, no file or network access.
// Queries require an explicit UTC provider: lag durations and interpolation use
// elapsed TAI seconds, while daily / three-hour windows follow UTC boundaries.
class SpaceWeather {
  public:
    SpaceWeather(SpaceWeatherInfo info, std::vector<SpaceWeatherDay> days);
    [[nodiscard]] const SpaceWeatherInfo &info() const noexcept { return info_; }
    [[nodiscard]] std::span<const SpaceWeatherDay> days() const noexcept { return days_; }
    // F10.7: observed values at 20:00 UTC, linear interpolation. Daily flux lag
    // 86400 s for both models. Average flux lag: JR 86400 s, NRL 0 s.
    // Geomagnetic lag: JR 24120 s, NRL 0 s. NRL three-hour mode assembles the
    // native seven-value Ap history; JR uses the selected scalar Kp / converted Ap.
    // Monthly predictions interpolate between adjacent calendar months: flux
    // at 20:00 UTC, geomagnetism at 00:00 UTC, with elapsed-TAI fractions.
    // CSSI Predicts uses nominal flux/Ap on day 15 and always selects Ap;
    // its average flux is the centered mean of 81 daily 20:00 UTC samples.
    // Forecast histories sample the continuous monthly curve at the required
    // lags; raw observed three-hour values are not synthesized or overwritten.
    [[nodiscard]] SpaceWeatherSample
    sample(Epoch at, const ITimeProvider &time, AtmosphereModel model, GeomagneticUpdate update,
           GeomagneticSource source, const std::function<double(Epoch)> &sun_distance_m = {}) const;
    // Adjusted-only files require a finite positive geometric Sun-Earth distance
    // in metres, evaluated at each required 20:00 UTC flux node. Convert both
    // daily and centered-81 values by (149597870700 / distance)^2 before
    // interpolation. No callback is used for files with observed fluxes.
    // Cubic spline: natural end conditions on each complete UTC-slot run,
    // knots at 01:30, 04:30, ..., 22:30 UTC. Inter-node fractions use elapsed
    // TAI seconds, including leap seconds. Interpolate the selected Ap/Kp before
    // conversion; reject overshoot outside its physical range. NRL keeps its
    // native un-interpolated observed history for all three-hour choices.
    // Mean-preserving interpolation: natural cubic primitive differentiated to
    // quadratic UTC-bin profiles, scaled about each cell mean to bound Ap/Kp.
    // Each actual UTC-bin duration (including leap seconds) retains its mean.
    // Limiting may create boundary jumps. Gaps split runs; no extrapolation.
    // Checks every required record / slot, including all lags and history, not
    // just the two endpoints. Missing fields, holes and extrapolation are errors.
    void validate_interval(Epoch first, Epoch last, const ITimeProvider &time,
                           AtmosphereModel model, GeomagneticUpdate update,
                           GeomagneticSource source,
                           const std::function<double(Epoch)> &sun_distance_m = {}) const;

  private:
    SpaceWeatherInfo info_;
    std::vector<SpaceWeatherDay> days_;
    std::vector<int> day_numbers_;
    bool has_monthly_ = false;
    // Second derivatives in the dimensionless, uniformly spaced UTC-slot coordinate.
    // Missing values/monthly predictions split runs; no spline crosses a gap.
    std::array<std::vector<double>, 2> spline_second_;
    // Linear/quadratic coefficients in a normalized UTC bin; constant term is
    // reconstructed from its recorded mean. Scaling preserves that mean.
    std::array<std::vector<std::array<double, 2>>, 2> mean_coefficients_;
    struct SplineInterval {
        std::array<double, 4> coefficients;
        Epoch first, last;
    };
    [[nodiscard]] SplineInterval mean_interval(int slot_number, const ITimeProvider &time,
                                               GeomagneticSource source) const;
    [[nodiscard]] double mean_sample(Epoch at, const ITimeProvider &time,
                                     GeomagneticSource source) const;
    [[nodiscard]] SplineInterval spline_interval(int slot_number, const ITimeProvider &time,
                                                 GeomagneticSource source) const;
    [[nodiscard]] double spline_sample(Epoch at, const ITimeProvider &time,
                                       GeomagneticSource source) const;
    void validate_spline(Epoch first, Epoch last, const ITimeProvider &time,
                         GeomagneticSource source) const;
    struct MonthlyBracket {
        std::size_t left, right;
        double fraction;
    };
    [[nodiscard]] std::optional<MonthlyBracket> monthly_bracket(Epoch at, int hour,
                                                                const ITimeProvider &time) const;
    [[nodiscard]] std::optional<double> monthly_geomagnetic(Epoch at, const ITimeProvider &time,
                                                            GeomagneticSource source) const;
    [[nodiscard]] double sample_flux(Epoch at, bool average, const ITimeProvider &time,
                                     const std::function<double(Epoch)> &sun_distance_m) const;
    [[nodiscard]] const SpaceWeatherDay &day(int number) const;
    [[nodiscard]] double flux_value(int number, bool average, const ITimeProvider &time,
                                    const std::function<double(Epoch)> &sun_distance_m) const;
};
[[nodiscard]] double ap_to_kp(double ap);
} // namespace orbitforge::core
