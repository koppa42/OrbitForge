#include "atmosphere_models.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <orbitforge/core/propagation/state_provider.hpp>

namespace orbitforge::core {
namespace {
constexpr double pi = std::numbers::pi, a = 6378137, f = 1 / 298.257223563;
constexpr double e2 = f * (2 - f), b = a * (1 - f);
void finite_vector(const std::array<double, 3> &v) {
    if (!std::all_of(v.begin(), v.end(), [](double x) { return std::isfinite(x); }))
        throw std::invalid_argument("Drag requires finite ITRF positions and velocities");
}
std::chrono::sys_days utc_day(const UtcDateTime &utc) {
    if (utc.year < 1600 || utc.year > 2600 || utc.hour < 0 || utc.hour > 23 || utc.minute < 0 ||
        utc.minute > 59 || utc.second < 0 || utc.second > 60 || utc.nanosecond < 0 ||
        utc.nanosecond >= 1000000000 || (utc.second == 60 && (utc.hour != 23 || utc.minute != 59)))
        throw std::invalid_argument("Invalid UTC atmosphere input");
    const auto date = std::chrono::year{utc.year} /
                      std::chrono::month{static_cast<unsigned>(utc.month)} /
                      std::chrono::day{static_cast<unsigned>(utc.day)};
    if (!date.ok())
        throw std::invalid_argument("Invalid UTC atmosphere date");
    return std::chrono::sys_days{date};
}
void weather(double daily, double average, double kp) {
    if (!std::isfinite(daily) || !std::isfinite(average) || daily <= 0 || average <= 0 ||
        daily > 1000 || average > 1000)
        throw std::invalid_argument("F10.7 flux must be finite and within (0,1000] sfu");
    if (!std::isfinite(kp) || kp < 0 || kp > 9)
        throw std::invalid_argument("Kp must be finite and within 0..9");
}
double drag_density(const AtmosphereInput &input, const DragOptions &options,
                    const std::optional<SpaceWeatherSample> &weather,
                    const std::optional<SpaceWeatherSample> &low_weather) {
    const auto density_for = [&](AtmosphereModel model, bool low = false) {
        auto values = input;
        if (!options.weather_model.empty()) {
            const auto &selected = low ? low_weather : weather;
            if (!selected)
                throw std::invalid_argument("File drag requires sampled space weather");
            values.f107 = selected->f107;
            values.f107_average = selected->f107_average;
            values.kp = selected->kp;
            if (model == AtmosphereModel::nrlmsise00) {
                if (options.geomagnetic_update != GeomagneticUpdate::daily) {
                    if (!selected->ap_history)
                        throw std::invalid_argument(
                            "Three-hour NRLMSISE requires complete Ap history");
                    values.ap_history = selected->ap_history;
                } else
                    values.daily_ap = selected->ap;
            }
        } else if (weather || low_weather) {
            throw std::invalid_argument("Manual drag cannot receive file weather samples");
        }
        return evaluate_atmosphere(model, values).density_kg_m3;
    };
    const double density_altitude = input.geodetic.height_m;
    double density;
    if (options.atmosphere == AtmosphereModel::jacchia_roberts &&
        options.low_altitude_model == LowAltitudeDensityModel::nrlmsise00 &&
        density_altitude < 90000 + options.blending_range_m) {
        const double lower = density_for(AtmosphereModel::nrlmsise00, true);
        if (density_altitude <= 90000) {
            density = lower;
        } else {
            const double upper = density_for(AtmosphereModel::jacchia_roberts);
            const double t = (density_altitude - 90000) / options.blending_range_m;
            const double weight = t * t * (3 - 2 * t);
            density = lower + weight * (upper - lower);
        }
    } else if (options.atmosphere == AtmosphereModel::jacchia_roberts &&
               options.low_altitude_model == LowAltitudeDensityModel::nrlmsise00 &&
               density_altitude < 90000) {
        density = density_for(AtmosphereModel::nrlmsise00, true);
    } else {
        density = density_for(options.atmosphere);
    }
    return density;
}

// Fourth-order differences in physical atmosphere coordinates keep density samples
// inside the valid altitude interval, including at the surface and upper limit.
// Subtract the central value first to avoid cancellation of a constant density.
template <class Sample>
double density_derivative(const Sample &sample, double center, double step, double left,
                          double right) {
    if (std::min(left, right) >= 2 * step)
        return (8 * (sample(step) - sample(-step)) - (sample(2 * step) - sample(-2 * step))) /
               (12 * step);
    const double direction = right >= left ? 1 : -1;
    step = std::min(step, std::max(left, right) / 4);
    if (!(step > 0))
        throw PropagationError("Drag density derivative has no valid sampling interval");
    const double h = direction * step;
    return (48 * (sample(h) - center) - 36 * (sample(2 * h) - center) +
            16 * (sample(3 * h) - center) - 3 * (sample(4 * h) - center)) /
           (12 * h);
}
std::array<double, 2> density_interval(double height, const DragOptions &options) {
    const bool jr = options.atmosphere == AtmosphereModel::jacchia_roberts;
    const bool lower = jr && options.low_altitude_model == LowAltitudeDensityModel::nrlmsise00;
    double lo = jr && !lower ? 90000 : 0, hi = jr ? 2500000 : 1000000;
    const auto split = [&](double boundary) {
        if (boundary <= lo || boundary >= hi)
            return;
        // A classical two-sided Jacobian cannot describe model switches. Keep the
        // nominal force available; the variational propagator must handle events.
        if (std::abs(height - boundary) < 1e-5)
            throw PropagationError(
                "Drag density derivative requires model-boundary event handling");
        if (boundary < height)
            lo = boundary;
        else
            hi = boundary;
    };
    if (!jr || (lower && height < 90000 + options.blending_range_m))
        for (const double boundary :
             {10000., 15000., 20000., 32500., 45000., 55000., 62500., 72500., 90000., 100000.,
              110000., 123435., 160000., 200000., 240000., 250000., 300000., 320000., 450000.})
            split(boundary);
    if (jr && (!lower || height >= 90000))
        for (const double boundary : {100000., 125000., 200000., 500000.})
            split(boundary);
    if (lower) {
        split(90000);
        if (options.blending_range_m > 0)
            split(90000 + options.blending_range_m);
    }
    return {height - lo, hi - height};
}
} // namespace
void validate_drag_options(const DragOptions &o) {
    if (o.atmosphere != AtmosphereModel::jacchia_roberts &&
        o.atmosphere != AtmosphereModel::nrlmsise00)
        throw std::invalid_argument("Unsupported atmospheric density model");
    if (o.low_altitude_model != LowAltitudeDensityModel::none &&
        o.low_altitude_model != LowAltitudeDensityModel::nrlmsise00)
        throw std::invalid_argument("Unsupported low altitude density model");
    if (!std::isfinite(o.cd) || o.cd < -100 || o.cd > 100)
        throw std::invalid_argument("Drag Cd must be finite and within -100..100");
    if (!std::isfinite(o.area_mass_ratio_m2_per_kg) || o.area_mass_ratio_m2_per_kg < 0 ||
        o.area_mass_ratio_m2_per_kg > 9999)
        throw std::invalid_argument("Drag area/mass ratio must be within 0..9999 m^2/kg");
    weather(o.f107, o.f107_average, o.kp);
    if (o.weather_model.size() > 64 ||
        !std::all_of(o.weather_model.begin(), o.weather_model.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        }))
        throw std::invalid_argument("Invalid space weather selection ID");
    if ((o.geomagnetic_update != GeomagneticUpdate::daily &&
         o.geomagnetic_update != GeomagneticUpdate::three_hourly &&
         o.geomagnetic_update != GeomagneticUpdate::three_hourly_interpolated &&
         o.geomagnetic_update != GeomagneticUpdate::three_hourly_cubic_spline) ||
        (o.geomagnetic_source != GeomagneticSource::ap &&
         o.geomagnetic_source != GeomagneticSource::kp))
        throw std::invalid_argument("Unsupported geomagnetic update/source");
    if (!std::isfinite(o.blending_range_m) || o.blending_range_m < 0 ||
        o.blending_range_m > 1000000)
        throw std::invalid_argument("Drag blending range must be within 0..1000 km");
    if (o.enabled && o.atmosphere == AtmosphereModel::jacchia_roberts &&
        o.low_altitude_model == LowAltitudeDensityModel::nrlmsise00 && o.blending_range_m > 910000)
        throw std::invalid_argument(
            "JR/NRLMSISE blending must stay within the 1000 km NRLMSISE domain");
}
DragOptions effective_drag_options(DragOptions o) {
    if (!o.enabled)
        return {};
    if (o.weather_model.empty()) {
        o.geomagnetic_update = GeomagneticUpdate::daily;
        o.geomagnetic_source = GeomagneticSource::ap;
    } else {
        o.f107 = DragOptions{}.f107;
        o.f107_average = DragOptions{}.f107_average;
        o.kp = DragOptions{}.kp;
    }
    if (o.atmosphere == AtmosphereModel::nrlmsise00) {
        o.low_altitude_model = LowAltitudeDensityModel::none;
        if (o.geomagnetic_update == GeomagneticUpdate::three_hourly_interpolated ||
            o.geomagnetic_update == GeomagneticUpdate::three_hourly_cubic_spline)
            o.geomagnetic_update = GeomagneticUpdate::three_hourly;
    }
    if (o.low_altitude_model == LowAltitudeDensityModel::none)
        o.blending_range_m = DragOptions{}.blending_range_m;
    return o;
}
double kp_to_ap(double kp) {
    weather(150, 150, kp);
    constexpr std::array table{0.,   2.,   3.,   4.,   5.,   6.,   7.,   9.,  12., 15.,
                               18.,  22.,  27.,  32.,  39.,  48.,  56.,  67., 80., 94.,
                               111., 132., 154., 179., 207., 236., 300., 400.};
    const double index = kp * 3;
    const auto lower = static_cast<std::size_t>(index);
    return lower == table.size() - 1
               ? table.back()
               : table[lower] + (index - lower) * (table[lower + 1] - table[lower]);
}
GeodeticPosition drag_geodetic_position(const std::array<double, 3> &r) {
    finite_vector(r);
    const double p = std::hypot(r[0], r[1]), radius = std::hypot(p, r[2]);
    if (!std::isfinite(radius) || radius < b / 2)
        throw PropagationError("Drag geodetic position is invalid near Earth's centre");
    const double longitude = p > 0 ? std::atan2(r[1], r[0]) : 0;
    if (p < 1e-8)
        return {longitude, std::copysign(pi / 2, r[2]), std::abs(r[2]) - b};
    double latitude = std::atan2(r[2], p * (1 - e2));
    for (unsigned i = 0; i < 15; ++i) {
        const double s = std::sin(latitude), n = a / std::sqrt(1 - e2 * s * s);
        const double next = std::atan2(r[2] + e2 * n * s, p);
        if (std::abs(next - latitude) < 2e-15) {
            latitude = next;
            break;
        }
        latitude = next;
        if (i == 14)
            throw PropagationError("Drag geodetic conversion did not converge");
    }
    const double s = std::sin(latitude), c = std::cos(latitude), n = a / std::sqrt(1 - e2 * s * s);
    const double height = p * c + r[2] * s - n * (1 - e2 * s * s);
    return {longitude, latitude, height};
}
AtmosphereEvaluation evaluate_atmosphere(AtmosphereModel model, const AtmosphereInput &input) {
    if (input.daily_ap && input.ap_history)
        throw std::invalid_argument("Choose either daily Ap or seven-value Ap history");
    if ((input.daily_ap || input.ap_history) && model != AtmosphereModel::nrlmsise00)
        throw std::invalid_argument("Explicit Ap inputs require NRLMSISE-00");
    weather(input.f107, input.f107_average, input.daily_ap || input.ap_history ? 3 : input.kp);
    const auto validate_ap = [](double ap) {
        if (!std::isfinite(ap) || ap < 0 || ap > 400)
            throw std::invalid_argument("Ap must be finite and within 0..400");
    };
    if (input.daily_ap)
        validate_ap(*input.daily_ap);
    if (input.ap_history)
        for (const double ap : *input.ap_history)
            validate_ap(ap);
    const auto &g = input.geodetic;
    if (!std::isfinite(g.longitude_rad) || std::abs(g.longitude_rad) > pi ||
        !std::isfinite(g.latitude_rad) || std::abs(g.latitude_rad) > pi / 2 ||
        !std::isfinite(input.solar_hour_angle_rad) || std::abs(input.solar_hour_angle_rad) > pi ||
        !std::isfinite(input.sun_declination_rad) || std::abs(input.sun_declination_rad) > pi / 2 ||
        !std::isfinite(g.height_m))
        throw std::invalid_argument("Invalid atmosphere geometry input");
    if (model != AtmosphereModel::jacchia_roberts && model != AtmosphereModel::nrlmsise00)
        throw std::invalid_argument("Unsupported atmospheric density model");
    const bool jr = model == AtmosphereModel::jacchia_roberts;
    if (g.height_m < (jr ? 90000 : 0) || g.height_m > (jr ? 2500000 : 1000000))
        throw PropagationError(jr ? "Jacchia-Roberts requires altitude 90..2500 km"
                                  : "NRLMSISE-00 requires altitude 0..1000 km");
    const auto day = utc_day(input.utc);
    const auto january = std::chrono::sys_days{std::chrono::year{input.utc.year} / 1 / 1};
    const auto doy = static_cast<int>((day - january).count()) + 1;
    const double seconds = input.utc.hour * 3600 + input.utc.minute * 60 + input.utc.second +
                           input.utc.nanosecond * 1e-9;
    const double utc_gmat_mjd =
        21544.5 + (day - std::chrono::sys_days{std::chrono::year{2000} / 1 / 1}).count() +
        seconds / 86400;
    const auto value =
        jr ? detail::jacchia_roberts(input, utc_gmat_mjd) : detail::nrlmsise00(input, doy, seconds);
    if (!std::isfinite(value.density_kg_m3) || value.density_kg_m3 < 0 ||
        !std::isfinite(value.temperature_k) || value.temperature_k <= 0 ||
        !std::isfinite(value.exospheric_temperature_k) || value.exospheric_temperature_k <= 0)
        throw PropagationError("Atmospheric density model produced invalid output");
    return value;
}
DragEvaluation evaluate_drag(const std::array<double, 3> &r, const std::array<double, 3> &v,
                             const std::array<double, 3> &sun, const UtcDateTime &utc,
                             const DragOptions &options,
                             const std::optional<SpaceWeatherSample> &weather,
                             const std::optional<SpaceWeatherSample> &low_weather) {
    validate_drag_options(options);
    if (!options.enabled)
        return {};
    finite_vector(v);
    finite_vector(sun);
    const double sun_distance = std::hypot(sun[0], sun[1], sun[2]);
    if (!std::isfinite(sun_distance) || sun_distance == 0)
        throw std::invalid_argument("Atmospheric density requires a finite nonzero Sun position");
    const auto g = drag_geodetic_position(r);
    double density_altitude = g.height_m;
    if (options.use_approximate_altitude) {
        // STK's oblate-spheroid approximation: radius minus the ellipsoid's
        // radial intersection, using geocentric (not geodetic) latitude.
        // Latitude/longitude passed to the atmosphere retain their exact definition.
        const double p = std::hypot(r[0], r[1]), radius = std::hypot(p, r[2]);
        const double cos_lat = p / radius;
        const double surface = p == 0      ? b
                               : r[2] == 0 ? a
                                           : b / std::sqrt(1 - e2 * cos_lat * cos_lat);
        density_altitude = radius - surface;
    }
    const double angle = std::atan2(sun[0] * r[1] - sun[1] * r[0], sun[0] * r[0] + sun[1] * r[1]);
    AtmosphereInput input{utc,          g,
                          angle,        std::atan2(sun[2], std::hypot(sun[0], sun[1])),
                          options.f107, options.f107_average,
                          options.kp};
    input.geodetic.height_m = density_altitude;
    const double density = drag_density(input, options, weather, low_weather);
    const double scale = -.5 * density * options.cd * options.area_mass_ratio_m2_per_kg *
                         std::hypot(v[0], v[1], v[2]);
    const std::array force{scale * v[0], scale * v[1], scale * v[2]};
    finite_vector(force);
    return {force, density, g, density_altitude};
}
DragDifferential drag_differential(const std::array<double, 3> &r, const std::array<double, 3> &v,
                                   const std::array<double, 3> &sun, const UtcDateTime &utc,
                                   const DragOptions &options,
                                   const std::optional<SpaceWeatherSample> &weather,
                                   const std::optional<SpaceWeatherSample> &low_weather) {
    DragDifferential result;
    result.nominal = evaluate_drag(r, v, sun, utc, options, weather, low_weather);
    if (!options.enabled)
        return result;
    result.force.acceleration_mps2 = result.nominal.acceleration_itrf_mps2;
    const auto &g = result.nominal.geodetic;
    const double s = std::sin(g.latitude_rad), c = std::cos(g.latitude_rad);
    if (std::abs(c) < 1e-8)
        throw PropagationError("Drag density derivative is undefined at the longitude pole");
    const double sl = std::sin(g.longitude_rad), cl = std::cos(g.longitude_rad);
    const double angle = std::atan2(sun[0] * r[1] - sun[1] * r[0], sun[0] * r[0] + sun[1] * r[1]);
    AtmosphereInput input{utc,          g,
                          angle,        std::atan2(sun[2], std::hypot(sun[0], sun[1])),
                          options.f107, options.f107_average,
                          options.kp};
    input.geodetic.height_m = result.nominal.density_altitude_m;
    const auto sample = [&](unsigned coordinate, double offset) {
        auto perturbed = input;
        if (coordinate == 0) {
            perturbed.geodetic.longitude_rad = std::remainder(g.longitude_rad + offset, 2 * pi);
            // When the Sun is on the rotation axis, the nominal evaluator uses zero hour angle.
            if (sun[0] != 0 || sun[1] != 0)
                perturbed.solar_hour_angle_rad = std::remainder(angle + offset, 2 * pi);
        } else if (coordinate == 1)
            perturbed.geodetic.latitude_rad += offset;
        else
            perturbed.geodetic.height_m += offset;
        return drag_density(perturbed, options, weather, low_weather);
    };
    const auto interval = density_interval(input.geodetic.height_m, options);
    const double rho = result.nominal.density_kg_m3;
    // NRLMSISE retains its fitted, rounded degree/hour conversion constants.
    // Its density is not exactly periodic at the longitude/local-time cuts;
    // never let a numerical stencil cross either branch of atan2.
    double left = pi + g.longitude_rad, right = pi - g.longitude_rad;
    if (sun[0] != 0 || sun[1] != 0) {
        left = std::min(left, pi + angle);
        right = std::min(right, pi - angle);
    }
    if (std::min(left, right) < 1e-12)
        throw PropagationError("Drag density derivative requires angular-boundary event handling");
    const double longitude =
        density_derivative([&](double h) { return sample(0, h); }, rho, 1e-4, left, right);
    double south = pi / 2 + g.latitude_rad, north_limit = pi / 2 - g.latitude_rad;
    const bool nrl_active = options.atmosphere == AtmosphereModel::nrlmsise00 ||
                            (options.low_altitude_model == LowAltitudeDensityModel::nrlmsise00 &&
                             input.geodetic.height_m < 90000 + options.blending_range_m);
    if (nrl_active && !options.weather_model.empty() &&
        options.geomagnetic_update != GeomagneticUpdate::daily) {
        // The historical-Ap formula contains |latitude|. At the equator use
        // its nonnegative branch, as Orekit's field derivative does; near the
        // equator retain the current hemisphere instead of averaging the cusp.
        if (g.latitude_rad < 0)
            north_limit = std::min(north_limit, -g.latitude_rad);
        else
            south = std::min(south, g.latitude_rad);
    }
    const double latitude =
        density_derivative([&](double h) { return sample(1, h); }, rho, 1e-4, south, north_limit);
    const double height = density_derivative([&](double h) { return sample(2, h); }, rho, 25,
                                             interval[0], interval[1]);
    const double n = a / std::sqrt(1 - e2 * s * s);
    const double m = n * (1 - e2) / (1 - e2 * s * s);
    const std::array north{-s * cl, -s * sl, c}, east{-sl, cl, 0.};
    std::array height_gradient{c * cl, c * sl, s};
    if (options.use_approximate_altitude) {
        const double radius = std::hypot(r[0], r[1], r[2]);
        const double q = (r[0] / radius) * (r[0] / radius) + (r[1] / radius) * (r[1] / radius);
        const double surface = b / std::sqrt(1 - e2 * q);
        for (unsigned j = 0; j < 3; ++j)
            height_gradient[j] = r[j] / radius - surface * e2 / (1 - e2 * q) *
                                                     ((j < 2 ? r[j] : 0) - q * r[j]) /
                                                     (radius * radius);
    }
    for (unsigned j = 0; j < 3; ++j)
        result.density_gradient_kg_m4[j] = height * height_gradient[j] +
                                           latitude * north[j] / (m + g.height_m) +
                                           longitude * east[j] / ((n + g.height_m) * c);
    finite_vector(result.density_gradient_kg_m4);
    const double speed = std::hypot(v[0], v[1], v[2]);
    const double d = options.cd * options.area_mass_ratio_m2_per_kg;
    for (unsigned i = 0; i < 3; ++i) {
        result.drag_parameter_jacobian_pa[i] = -.5 * rho * speed * v[i];
        for (unsigned j = 0; j < 3; ++j) {
            result.force.position_jacobian_per_s2[i][j] =
                -.5 * d * speed * v[i] * result.density_gradient_kg_m4[j];
            result.force.velocity_jacobian_per_s[i][j] =
                speed == 0 ? 0 : -.5 * rho * d * ((i == j ? speed : 0) + v[i] * (v[j] / speed));
        }
        finite_vector(result.force.position_jacobian_per_s2[i]);
        finite_vector(result.force.velocity_jacobian_per_s[i]);
    }
    finite_vector(result.drag_parameter_jacobian_pa);
    return result;
}
} // namespace orbitforge::core
