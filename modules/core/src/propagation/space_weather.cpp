#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <orbitforge/core/propagation/atmospheric_drag.hpp>

namespace orbitforge::core {
namespace {
using std::chrono::year, std::chrono::month, std::chrono::sys_days, std::chrono::year_month_day;
int day_number(const UtcDateTime &u) {
    const auto ymd = year{u.year} / month{static_cast<unsigned>(u.month)} /
                     std::chrono::day{static_cast<unsigned>(u.day)};
    if (u.year < 1600 || u.year > 2600 || !ymd.ok())
        throw std::invalid_argument("Invalid space weather UTC date");
    return static_cast<int>(sys_days{ymd}.time_since_epoch().count());
}
UtcDateTime utc_date(int number, int hour = 0) {
    const year_month_day d{sys_days{std::chrono::days{number}}};
    return {static_cast<int>(d.year()),
            static_cast<int>(static_cast<unsigned>(d.month())),
            static_cast<int>(static_cast<unsigned>(d.day())),
            hour,
            0,
            0,
            0};
}
Epoch lagged(Epoch at, int seconds) {
    const auto offset = static_cast<std::int64_t>(seconds) * 1000000000;
    if (at.tai_nanoseconds() < std::numeric_limits<std::int64_t>::min() + offset)
        throw SpaceWeatherCoverageError("Space weather lag exceeds the Epoch range");
    return Epoch{at.tai_nanoseconds() - offset};
}
void choices(AtmosphereModel model, GeomagneticUpdate update, GeomagneticSource source) {
    if ((model != AtmosphereModel::jacchia_roberts && model != AtmosphereModel::nrlmsise00) ||
        (update != GeomagneticUpdate::daily && update != GeomagneticUpdate::three_hourly &&
         update != GeomagneticUpdate::three_hourly_interpolated &&
         update != GeomagneticUpdate::three_hourly_cubic_spline) ||
        (source != GeomagneticSource::ap && source != GeomagneticSource::kp))
        throw std::invalid_argument("Unsupported space weather sampling choice");
}
std::string date_text(int d) {
    const auto u = utc_date(d);
    return std::to_string(u.year) + "-" + std::to_string(u.month) + "-" + std::to_string(u.day);
}
double required(std::optional<double> value, int d, const char *field) {
    if (!value)
        throw SpaceWeatherCoverageError("Space weather missing " + std::string(field) + " on " +
                                        date_text(d) + "; import a complete weather resource");
    return *value;
}
void check_value(std::optional<double> value, double min, double max, const char *field) {
    if (value && (!std::isfinite(*value) || *value < min || *value > max))
        throw std::invalid_argument("Invalid space weather " + std::string(field));
}
struct Slot {
    int day, index;
};
Slot slot(const UtcDateTime &utc, int earlier = 0) {
    const int index = utc.hour / 3 - earlier;
    // Floor division, including negative offsets across multiple UTC midnights.
    const int delta = index >= 0 ? index / 8 : (index - 7) / 8;
    return {day_number(utc) + delta, index - 8 * delta};
}
// Left interpolation node; at exactly 20:00 the right node is not required.
std::pair<int, bool> flux_node(const UtcDateTime &u) {
    return {day_number(u) - (u.hour < 20 ? 1 : 0),
            !(u.hour == 20 && u.minute == 0 && u.second == 0 && u.nanosecond == 0)};
}
// Left midpoint node of the interpolation interval, also at an exact knot.
int spline_node(const UtcDateTime &u) {
    const auto s = slot(u);
    const int seconds = (u.hour % 3) * 3600 + u.minute * 60 + u.second;
    return s.day * 8 + s.index - (seconds < 5400 ? 1 : 0);
}
UtcDateTime slot_utc(int node) {
    const int d = node >= 0 ? node / 8 : (node - 7) / 8;
    return utc_date(d, (node - d * 8) * 3);
}
UtcDateTime spline_utc(int node) {
    auto utc = slot_utc(node);
    ++utc.hour;
    utc.minute = 30;
    return utc;
}
double spline_value(const std::array<double, 4> &c, double x) {
    return ((c[3] * x + c[2]) * x + c[1]) * x + c[0];
}
double check_geomagnetic_range(double value, GeomagneticSource source) {
    const double upper = source == GeomagneticSource::ap ? 400 : 9;
    const double roundoff = 64 * std::numeric_limits<double>::epsilon() * upper;
    if (!std::isfinite(value) || value < -roundoff || value > upper + roundoff)
        throw SpaceWeatherCoverageError(
            "Geomagnetic interpolation overshoots the Ap/Kp range; "
            "choose daily/three-hour updates or import a suitable weather resource");
    return std::clamp(value, 0.0, upper); // Only roundoff-sized excursions reach this line.
}
} // namespace
SpaceWeather::SpaceWeather(SpaceWeatherInfo info, std::vector<SpaceWeatherDay> days)
    : info_(std::move(info)), days_(std::move(days)) {
    if (days_.empty() || days_.size() > 366000)
        throw std::invalid_argument("Space weather needs 1..366000 daily records");
    for (const auto &d : days_) {
        const auto n = day_number(d.date);
        if (d.date.hour || d.date.minute || d.date.second || d.date.nanosecond ||
            (!day_numbers_.empty() && n <= day_numbers_.back()))
            throw std::invalid_argument("Space weather dates must be unique, ordered UTC days");
        day_numbers_.push_back(n);
        has_monthly_ = has_monthly_ || d.quality == WeatherQuality::monthly_prediction;
        if (info_.cssi_predictions &&
            (d.quality != WeatherQuality::monthly_prediction || d.date.day != 15))
            throw std::invalid_argument("CSSI Predicts requires monthly nodes on the 15th");
        for (const auto v : d.kp)
            check_value(v, 0, 9, "Kp");
        for (const auto v : d.ap)
            check_value(v, 0, 400, "Ap");
        check_value(d.daily_kp, 0, 9, "daily Kp");
        check_value(d.daily_ap, 0, 400, "daily Ap");
        for (auto v : {d.f107_observed, d.f107_adjusted, d.observed_center81, d.observed_last81,
                       d.adjusted_center81, d.adjusted_last81})
            check_value(v, 1e-12, 1000, "F10.7");
        if (d.quality < WeatherQuality::observed || d.quality > WeatherQuality::monthly_prediction)
            throw std::invalid_argument("Invalid space weather flux quality");
    }
    for (std::size_t source = 0; source < 2; ++source) {
        auto &second = spline_second_[source];
        second.resize(days_.size() * 8, std::numeric_limits<double>::quiet_NaN());
        auto &mean = mean_coefficients_[source];
        mean.resize(second.size());
        const auto value = [&](std::size_t i) {
            const auto &d = days_[i / 8];
            return d.quality == WeatherQuality::monthly_prediction
                       ? std::optional<double>{}
                       : (source == 0 ? d.ap[i % 8] : d.kp[i % 8]);
        };
        std::size_t begin = 0;
        while (begin < second.size()) {
            if (!value(begin)) {
                ++begin;
                continue;
            }
            auto end = begin + 1;
            while (end < second.size() && value(end) &&
                   (end % 8 || day_numbers_[end / 8] == day_numbers_[end / 8 - 1] + 1))
                ++end;
            // Thomas solve: M[i-1] + 4 M[i] + M[i+1] = 6(y[i+1]-2y[i]+y[i-1]).
            // Natural boundary conditions M[0]=M[n-1]=0; two nodes are linear.
            std::vector<double> upper(end - begin, 0);
            second[begin] = second[end - 1] = 0;
            for (auto i = begin + 1; i + 1 < end; ++i) {
                const double denominator = 4 - upper[i - begin - 1];
                upper[i - begin] = 1 / denominator;
                second[i] =
                    (6 * (value(i + 1).value() - 2 * value(i).value() + value(i - 1).value()) -
                     second[i - 1]) /
                    denominator;
            }
            for (auto i = end - 1; i > begin + 1; --i)
                second[i - 1] -= upper[i - begin - 1] * second[i];
            // Natural cubic of the cumulative integral: its edge second
            // derivatives satisfy M[i-1]+4M[i]+M[i+1]=6(y[i]-y[i-1]).
            // Solve using differences directly, avoiding cumulative cancellation.
            const auto count = end - begin;
            std::vector<double> edge(count + 1, 0);
            upper.assign(count + 1, 0);
            for (std::size_t i = 1; i < count; ++i) {
                const double denominator = 4 - upper[i - 1];
                upper[i] = 1 / denominator;
                edge[i] =
                    (6 * (*value(begin + i) - *value(begin + i - 1)) - edge[i - 1]) / denominator;
            }
            for (auto i = count; i > 1; --i)
                edge[i - 1] -= upper[i - 1] * edge[i];
            const double bound = source == 0 ? 400 : 9;
            for (std::size_t i = 0; i < count; ++i) {
                const double y = *value(begin + i);
                double b = edge[i], c = (edge[i + 1] - edge[i]) / 2;
                const double a = y - b / 2 - c / 3;
                const auto polynomial = [&](double x) { return (c * x + b) * x + a; };
                double low = std::min(a, polynomial(1)), high = std::max(a, polynomial(1));
                if (c != 0) {
                    const double vertex = -b / (2 * c);
                    if (vertex > 0 && vertex < 1) {
                        low = std::min(low, polynomial(vertex));
                        high = std::max(high, polynomial(vertex));
                    }
                }
                // Cell-mean scaling limiter, with exact quadratic extrema.
                // Unlike clipping samples, y + theta*(p-y) retains the integral.
                double theta = 1;
                if (low < 0)
                    theta = std::min(theta, y / (y - low));
                if (high > bound)
                    theta = std::min(theta, (bound - y) / (high - y));
                if (y == 0 || y == bound)
                    theta = 0;
                mean[begin + i] = {theta * b, theta * c};
            }
            begin = end;
        }
    }
    info_.first = days_.front().date;
    info_.last = days_.back().date;
}
const SpaceWeatherDay &SpaceWeather::day(int number) const {
    const auto it = std::lower_bound(day_numbers_.begin(), day_numbers_.end(), number);
    if (it == day_numbers_.end() || *it != number)
        throw SpaceWeatherCoverageError("Space weather missing date " + date_text(number) +
                                        "; raw records cover " + date_text(day_numbers_.front()) +
                                        " through " + date_text(day_numbers_.back()) +
                                        "; import data including model lags/history");
    const auto &record = days_[static_cast<std::size_t>(it - day_numbers_.begin())];
    return record;
}
std::optional<SpaceWeather::MonthlyBracket>
SpaceWeather::monthly_bracket(Epoch at, int hour, const ITimeProvider &time) const {
    if (!has_monthly_)
        return {};
    const auto utc = time.to_utc(at);
    const int date = day_number(utc) - (utc.hour < hour ? 1 : 0);
    const auto after = std::upper_bound(day_numbers_.begin(), day_numbers_.end(), date);
    if (after == day_numbers_.begin())
        return {};
    const auto left = static_cast<std::size_t>(after - day_numbers_.begin() - 1);
    const bool monthly = days_[left].quality == WeatherQuality::monthly_prediction;
    if (!monthly && (after == day_numbers_.end() ||
                     days_[left + 1].quality != WeatherQuality::monthly_prediction))
        return {};
    const auto first = time.from_utc(utc_date(day_numbers_[left], hour));
    if (at == first)
        return monthly ? std::optional{MonthlyBracket{left, left, 0}} : std::nullopt;
    if (after == day_numbers_.end()) {
        if (monthly)
            throw SpaceWeatherCoverageError(
                "Monthly space weather cannot extrapolate after its last node");
        return {};
    }
    const auto right = left + 1;
    const bool next_monthly = days_[right].quality == WeatherQuality::monthly_prediction;
    if (!monthly && !next_monthly)
        return {};
    const auto &a = days_[left].date;
    const auto &b = days_[right].date;
    const int months = (b.year - a.year) * 12 + b.month - a.month;
    if (!next_monthly || months != 1 || (monthly && a.day != b.day))
        throw SpaceWeatherCoverageError(
            "Monthly space weather has a missing month or inconsistent node dates");
    const auto last = time.from_utc(utc_date(day_numbers_[right], hour));
    return MonthlyBracket{left, right, at.seconds_since(first) / last.seconds_since(first)};
}
std::optional<double> SpaceWeather::monthly_geomagnetic(Epoch at, const ITimeProvider &time,
                                                        GeomagneticSource source) const {
    const auto bracket = monthly_bracket(at, 0, time);
    if (!bracket)
        return {};
    const auto value = [&](std::size_t i) {
        const auto &d = days_[i];
        return required(source == GeomagneticSource::ap ? d.daily_ap : d.daily_kp, day_numbers_[i],
                        "monthly/daily forecast geomagnetic value");
    };
    const double first = value(bracket->left);
    return first + bracket->fraction * (value(bracket->right) - first);
}
double SpaceWeather::sample_flux(Epoch at, bool average, const ITimeProvider &time,
                                 const std::function<double(Epoch)> &sun_distance_m) const {
    const auto [d, interpolate] = flux_node(time.to_utc(at));
    if (average && info_.cssi_predictions) {
        const auto centered = [&](int date) {
            double total = 0;
            for (int offset = -40; offset <= 40; ++offset)
                total += sample_flux(time.from_utc(utc_date(date + offset, 20)), false, time,
                                     sun_distance_m);
            return total / 81;
        };
        const double first = centered(d);
        if (!interpolate)
            return first;
        const auto a = time.from_utc(utc_date(d, 20)), b = time.from_utc(utc_date(d + 1, 20));
        return first + at.seconds_since(a) / b.seconds_since(a) * (centered(d + 1) - first);
    }
    if (const auto bracket = monthly_bracket(at, 20, time)) {
        const double first = flux_value(day_numbers_[bracket->left], average, time, sun_distance_m);
        return first + bracket->fraction * (flux_value(day_numbers_[bracket->right], average, time,
                                                       sun_distance_m) -
                                            first);
    }
    const double first = flux_value(d, average, time, sun_distance_m);
    if (!interpolate)
        return first;
    const auto a = time.from_utc(utc_date(d, 20)), b = time.from_utc(utc_date(d + 1, 20));
    return first + at.seconds_since(a) / b.seconds_since(a) *
                       (flux_value(d + 1, average, time, sun_distance_m) - first);
}
SpaceWeather::SplineInterval SpaceWeather::mean_interval(int node, const ITimeProvider &time,
                                                         GeomagneticSource source) const {
    const auto utc = slot_utc(node);
    const int d = day_number(utc), i = utc.hour / 3;
    const auto &r = day(d);
    const double y = required(source == GeomagneticSource::ap ? r.ap[i] : r.kp[i], d,
                              "three-hour mean-preserving interpolation bin");
    const auto index = static_cast<std::size_t>(&r - days_.data()) * 8 + i;
    const auto [b, c] = mean_coefficients_[source == GeomagneticSource::ap ? 0 : 1][index];
    return {{{y - b / 2 - c / 3, b, c, 0}}, time.from_utc(utc), time.from_utc(slot_utc(node + 1))};
}
double SpaceWeather::mean_sample(Epoch at, const ITimeProvider &time,
                                 GeomagneticSource source) const {
    const auto bin = slot(time.to_utc(at));
    const auto s = mean_interval(bin.day * 8 + bin.index, time, source);
    return check_geomagnetic_range(
        spline_value(s.coefficients, at.seconds_since(s.first) / s.last.seconds_since(s.first)),
        source);
}
SpaceWeather::SplineInterval SpaceWeather::spline_interval(int node, const ITimeProvider &time,
                                                           GeomagneticSource source) const {
    const auto get = [&](int n) {
        const auto u = spline_utc(n);
        const auto d = day_number(u);
        const auto &r = day(d);
        const auto index = static_cast<std::size_t>(&r - days_.data()) * 8 + u.hour / 3;
        const double y =
            required(source == GeomagneticSource::ap ? r.ap[u.hour / 3] : r.kp[u.hour / 3], d,
                     "three-hour spline node");
        return std::pair{y, spline_second_[source == GeomagneticSource::ap ? 0 : 1][index]};
    };
    const auto [y0, m0] = get(node);
    const auto [y1, m1] = get(node + 1);
    return {{{y0, y1 - y0 - (2 * m0 + m1) / 6, m0 / 2, (m1 - m0) / 6}},
            time.from_utc(spline_utc(node)),
            time.from_utc(spline_utc(node + 1))};
}
double SpaceWeather::spline_sample(Epoch at, const ITimeProvider &time,
                                   GeomagneticSource source) const {
    const auto utc = time.to_utc(at);
    const int node = spline_node(utc);
    const auto first = time.from_utc(spline_utc(node));
    if (at == first) {
        const auto &r = day(day_number(utc));
        return required(source == GeomagneticSource::ap ? r.ap[utc.hour / 3] : r.kp[utc.hour / 3],
                        day_number(utc), "three-hour spline node");
    }
    const auto s = spline_interval(node, time, source);
    const auto result =
        spline_value(s.coefficients, at.seconds_since(s.first) / s.last.seconds_since(s.first));
    return check_geomagnetic_range(result, source);
}
void SpaceWeather::validate_spline(Epoch first, Epoch last, const ITimeProvider &time,
                                   GeomagneticSource source) const {
    const int begin = spline_node(time.to_utc(first)), end = spline_node(time.to_utc(last));
    for (int n = begin; n <= end; ++n) {
        // A final exact knot needs no following record and may be the last available node.
        if (n == end && last == time.from_utc(spline_utc(n)))
            break;
        const auto s = spline_interval(n, time, source);
        const auto &c = s.coefficients;
        const double duration = s.last.seconds_since(s.first);
        const double a = std::max(0.0, first.seconds_since(s.first) / duration);
        const double b = std::min(1.0, last.seconds_since(s.first) / duration);
        check_geomagnetic_range(spline_value(c, a), source);
        check_geomagnetic_range(spline_value(c, b), source);
        const auto extremum = [&](double x) {
            if (x > a && x < b)
                check_geomagnetic_range(spline_value(c, x), source);
        };
        if (c[3] == 0) {
            if (c[2] != 0)
                extremum(-c[1] / (2 * c[2]));
        } else {
            const double discriminant = c[2] * c[2] - 3 * c[3] * c[1];
            if (discriminant >= 0) {
                // Stable quadratic roots, including c[1]==0 and double roots.
                const double q = -c[2] - std::copysign(std::sqrt(discriminant), c[2]);
                extremum(q / (3 * c[3]));
                if (q != 0)
                    extremum(c[1] / q);
            }
        }
    }
}
double ap_to_kp(double ap) {
    if (!std::isfinite(ap) || ap < 0 || ap > 400)
        throw std::invalid_argument("Ap must be finite and within 0..400");
    for (int i = 0; i < 27; ++i) {
        const double lower = kp_to_ap(i / 3.0), upper = kp_to_ap((i + 1) / 3.0);
        if (ap <= upper)
            return (i + (ap - lower) / (upper - lower)) / 3.0;
    }
    return 9;
}
double SpaceWeather::flux_value(int number, bool average, const ITimeProvider &time,
                                const std::function<double(Epoch)> &sun_distance_m) const {
    const auto &r = day(number);
    if (!info_.solar_flux_adjusted)
        return required(average ? r.observed_center81 : r.f107_observed, number,
                        average ? "centered observed F10.7" : "observed F10.7");
    const double adjusted = required(average ? r.adjusted_center81 : r.f107_adjusted, number,
                                     average ? "centered adjusted F10.7" : "adjusted F10.7");
    if (!sun_distance_m)
        throw SpaceWeatherCoverageError(
            "Adjusted FXM flux requires a geometric Sun-Earth distance provider");
    const double distance = sun_distance_m(time.from_utc(utc_date(number, 20)));
    if (!std::isfinite(distance) || distance <= 0)
        throw SpaceWeatherCoverageError(
            "FXM Sun-Earth distance must be finite and positive in metres");
    const double scale = 149597870700.0 / distance; // IAU 2012 exact astronomical unit, SI.
    const double observed = adjusted * scale * scale;
    if (!std::isfinite(observed) || observed <= 0 || observed > 1000)
        throw SpaceWeatherCoverageError(
            "Distance-corrected FXM flux is outside the density model range (0, 1000] sfu");
    return observed;
}
SpaceWeatherSample SpaceWeather::sample(Epoch at, const ITimeProvider &time, AtmosphereModel model,
                                        GeomagneticUpdate update, GeomagneticSource source,
                                        const std::function<double(Epoch)> &sun_distance_m) const {
    choices(model, update, source);
    const bool jr = model == AtmosphereModel::jacchia_roberts;
    if (info_.cssi_predictions)
        source = GeomagneticSource::ap;
    const auto flux = [&](Epoch epoch, bool average) {
        return sample_flux(epoch, average, time, sun_distance_m);
    };
    const auto utc = time.to_utc(lagged(at, jr ? 24120 : 0));
    const int d = day_number(utc);
    const bool from_ap = source == GeomagneticSource::ap;
    const auto daily = [&] {
        if (auto value = monthly_geomagnetic(lagged(at, jr ? 24120 : 0), time, source))
            return *value;
        const auto &record = day(d);
        return required(from_ap ? record.daily_ap : record.daily_kp, d,
                        from_ap ? "daily Ap" : "daily Kp");
    };
    const auto hourly = [&](int earlier) {
        if (auto value =
                monthly_geomagnetic(lagged(at, (jr ? 24120 : 0) + earlier * 10800), time, source))
            return *value;
        const auto s = slot(utc, earlier);
        const auto &r = day(s.day);
        return required(from_ap ? r.ap[s.index] : r.kp[s.index], s.day,
                        from_ap ? "three-hour Ap" : "three-hour Kp");
    };
    const auto forecast = monthly_geomagnetic(lagged(at, jr ? 24120 : 0), time, source);
    const double geomag = forecast                             ? *forecast
                          : update == GeomagneticUpdate::daily ? daily()
                          : jr && update == GeomagneticUpdate::three_hourly_interpolated
                              ? mean_sample(lagged(at, 24120), time, source)
                          : jr && update == GeomagneticUpdate::three_hourly_cubic_spline
                              ? spline_sample(lagged(at, 24120), time, source)
                              : hourly(0);
    SpaceWeatherSample result{flux(lagged(at, 86400), false),
                              flux(lagged(at, jr ? 86400 : 0), true),
                              from_ap ? ap_to_kp(geomag) : geomag,
                              from_ap ? geomag : kp_to_ap(geomag),
                              {}};
    if (!jr && update != GeomagneticUpdate::daily) {
        std::array<double, 7> history{};
        const auto as_ap = [&](double value) { return from_ap ? value : kp_to_ap(value); };
        history[0] = as_ap(daily());
        for (int i = 0; i < 4; ++i)
            history[i + 1] = as_ap(hourly(i));
        for (int i = 4; i < 20; ++i)
            history[i < 12 ? 5 : 6] += as_ap(hourly(i)) / 8;
        result.ap_history = history;
    }
    return result;
}
void SpaceWeather::validate_interval(Epoch first, Epoch last, const ITimeProvider &time,
                                     AtmosphereModel model, GeomagneticUpdate update,
                                     GeomagneticSource source,
                                     const std::function<double(Epoch)> &sun_distance_m) const {
    choices(model, update, source);
    if (first.tai_nanoseconds() > last.tai_nanoseconds())
        throw std::invalid_argument("Space weather interval must be ordered");
    // Monthly geomagnetic coverage ends at the exact final midnight node,
    // potentially before the final 20:00 flux node. Check the requested ends
    // themselves, in addition to the interior daily/three-hour boundaries.
    (void)sample(first, time, model, update, source, sun_distance_m);
    if (first == last)
        return;
    (void)sample(last, time, model, update, source, sun_distance_m);
    const bool jr = model == AtmosphereModel::jacchia_roberts;
    if (info_.cssi_predictions)
        source = GeomagneticSource::ap;
    const auto flux = [&](int lag, bool average) {
        const auto begin = lagged(first, lag), end = lagged(last, lag);
        (void)sample_flux(begin, average, time, sun_distance_m);
        (void)sample_flux(end, average, time, sun_distance_m);
        for (int d = day_number(time.to_utc(begin)); d <= day_number(time.to_utc(end)); ++d) {
            const auto node = time.from_utc(utc_date(d, 20));
            if (node.tai_nanoseconds() >= begin.tai_nanoseconds() &&
                node.tai_nanoseconds() <= end.tai_nanoseconds())
                (void)sample_flux(node, average, time, sun_distance_m);
        }
    };
    flux(86400, false);
    flux(jr ? 86400 : 0, true);
    const auto a = time.to_utc(lagged(first, jr ? 24120 : 0));
    const auto b = time.to_utc(lagged(last, jr ? 24120 : 0));
    const bool ap = source == GeomagneticSource::ap;
    if (update == GeomagneticUpdate::daily || !jr)
        for (int d = day_number(a); d <= day_number(b); ++d) {
            const Epoch at{std::max(lagged(first, jr ? 24120 : 0).tai_nanoseconds(),
                                    time.from_utc(utc_date(d)).tai_nanoseconds())};
            if (monthly_geomagnetic(at, time, source))
                continue;
            const auto &r = day(d);
            (void)required(ap ? r.daily_ap : r.daily_kp, d, ap ? "daily Ap" : "daily Kp");
        }
    if (update != GeomagneticUpdate::daily) {
        const auto start = slot(a, jr ? 0 : 19), stop = slot(b);
        for (int n = start.day * 8 + start.index; n <= stop.day * 8 + stop.index; ++n) {
            const int d = n >= 0 ? n / 8 : (n - 7) / 8, i = n - d * 8;
            const auto node = time.from_utc(slot_utc(n));
            if (monthly_geomagnetic(node, time, source))
                continue;
            if (jr && update == GeomagneticUpdate::three_hourly_cubic_spline) {
                const Epoch begin{
                    std::max(lagged(first, 24120).tai_nanoseconds(), node.tai_nanoseconds())};
                const Epoch end{std::min(lagged(last, 24120).tai_nanoseconds(),
                                         time.from_utc(slot_utc(n + 1)).tai_nanoseconds())};
                if (begin.tai_nanoseconds() < end.tai_nanoseconds()) {
                    const Epoch middle{begin.tai_nanoseconds() +
                                       (end.tai_nanoseconds() - begin.tai_nanoseconds()) / 2};
                    if (!monthly_geomagnetic(middle, time, source))
                        validate_spline(begin, end, time, source);
                }
                continue;
            }
            const auto &r = day(d);
            (void)required(ap ? r.ap[i] : r.kp[i], d, ap ? "three-hour Ap" : "three-hour Kp");
            if (jr && update == GeomagneticUpdate::three_hourly_interpolated)
                (void)mean_interval(n, time, source);
        }
    }
    (void)sample(first, time, model, update, source, sun_distance_m);
    (void)sample(last, time, model, update, source, sun_distance_m);
}
} // namespace orbitforge::core
