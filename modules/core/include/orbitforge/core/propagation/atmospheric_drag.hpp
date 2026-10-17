#pragma once
#include <array>
#include <optional>
#include <orbitforge/core/frames/geodetic.hpp>
#include <orbitforge/core/propagation/force_differential.hpp>
#include <orbitforge/core/propagation/space_weather.hpp>
#include <orbitforge/core/time/time_provider.hpp>

namespace orbitforge::core {
enum class LowAltitudeDensityModel { none, nrlmsise00 };
struct DragOptions {
    bool enabled = false;
    double cd = 2.2;
    double area_mass_ratio_m2_per_kg = .02;
    AtmosphereModel atmosphere = AtmosphereModel::jacchia_roberts;
    LowAltitudeDensityModel low_altitude_model = LowAltitudeDensityModel::none;
    double blending_range_m = 30000;
    // Manual constant fluxes in sfu; NRLMSISE uses the daily value for the previous day.
    double f107 = 150, f107_average = 150, kp = 3;
    // Geometric positions by default; apparent Sun requires a certifying ephemeris.
    bool use_apparent_sun_position = false;
    // Density height above the WGS84 radial intersection; geodetic latitude stays exact.
    bool use_approximate_altitude = false;
    // Empty: manual constants. Otherwise an immutable catalog selection, never a path.
    std::string weather_model;
    GeomagneticUpdate geomagnetic_update = GeomagneticUpdate::daily;
    GeomagneticSource geomagnetic_source = GeomagneticSource::ap;
    bool operator==(const DragOptions &) const = default;
};
void validate_drag_options(const DragOptions &options);
[[nodiscard]] DragOptions effective_drag_options(DragOptions options);
// Standard 1/3-Kp to Ap table; linear interpolation for intermediate Kp. [0,9].
[[nodiscard]] double kp_to_ap(double kp);
[[nodiscard]] GeodeticPosition drag_geodetic_position(const std::array<double, 3> &itrf_m);
struct AtmosphereInput {
    UtcDateTime utc{2000, 1, 1, 12, 0, 0, 0};
    GeodeticPosition geodetic{};
    // Angle east of the Sun meridian in Earth's equatorial plane. Local solar time=12+h*12/pi.
    double solar_hour_angle_rad = 0, sun_declination_rad = 0;
    double f107 = 150, f107_average = 150;
    double kp = 3;
    // NRLMSISE-only alternatives to Kp, in native Ap units [0,400]. Mutually exclusive;
    // when either is provided, kp is inactive. daily_ap selects SW[9]=1;
    // ap_history selects SW[9]=-1 and owns seven values in this order:
    // daily Ap; current 3 h ap; ap 3, 6 and 9 h earlier; mean ap 12..33 h earlier;
    // mean ap 36..57 h earlier (each mean spans eight 3 h values).
    // The caller supplies a complete history for this UTC instant; no files,
    // interpolation, extrapolation or substitution of missing values is implied.
    std::optional<double> daily_ap{};
    std::optional<std::array<double, 7>> ap_history{};
};
struct AtmosphereEvaluation {
    double density_kg_m3, temperature_k, exospheric_temperature_k;
};
// Pure SI density model, no files or implicit time/ephemeris queries. JR: 90..2500 km;
// NRLMSISE-00 gtd7d (including anomalous oxygen): 0..1000 km. Invalid inputs throw.
[[nodiscard]] AtmosphereEvaluation evaluate_atmosphere(AtmosphereModel model,
                                                       const AtmosphereInput &input);
struct DragEvaluation {
    std::array<double, 3> acceleration_itrf_mps2{};
    double density_kg_m3 = 0;
    GeodeticPosition geodetic{};   // Always the exact WGS84 geodetic position.
    double density_altitude_m = 0; // Height actually supplied to density and blending.
};
// Satellite velocity is relative to the co-rotating atmosphere in ITRF. The caller
// must use a full inertial-to-ITRF state transform, including the rotation-rate term.
// Sun is geocentric in the same ITRF axes; no atmospheric winds. File mode requires
// a sample for the main model and, when blending, one for the low-altitude model,
// each sampled with its own lag. This pure evaluator performs no file access.
[[nodiscard]] DragEvaluation
evaluate_drag(const std::array<double, 3> &position_itrf_m,
              const std::array<double, 3> &velocity_itrf_mps,
              const std::array<double, 3> &sun_itrf_m, const UtcDateTime &utc,
              const DragOptions &options, const std::optional<SpaceWeatherSample> &weather = {},
              const std::optional<SpaceWeatherSample> &low_weather = {});
struct DragDifferential {
    DragEvaluation nominal{};
    ForceDifferential force{};
    std::array<double, 3> density_gradient_kg_m4{};
    // Derivative with respect to STK's D = Cd * Area/Mass [m^2/kg], in Pa.
    // This is not a Cd-only derivative and remains nonzero when D is zero.
    std::array<double, 3> drag_parameter_jacobian_pa{};
};
// Fixed UTC, Sun, weather and model parameters. Both state blocks use ITRF axes;
// velocity is relative to co-rotating air, as for evaluate_drag. Density position
// derivatives use fourth-order finite differences; velocity and D are analytic.
// Historical-Ap latitude derivatives keep their hemisphere (north at zero).
// One-sided density derivatives at outer altitude limits; rejects interior model
// branch boundaries (within 1e-5 m), angular cuts (within 1e-12 rad), and
// longitude poles (|cos(latitude)| < 1e-8).
// Full inertial partials also need the position-to-velocity rotation-rate coupling.
// Disabled drag returns zero; invalid input or non-finite derivatives throw.
[[nodiscard]] DragDifferential
drag_differential(const std::array<double, 3> &position_itrf_m,
                  const std::array<double, 3> &velocity_itrf_mps,
                  const std::array<double, 3> &sun_itrf_m, const UtcDateTime &utc,
                  const DragOptions &options, const std::optional<SpaceWeatherSample> &weather = {},
                  const std::optional<SpaceWeatherSample> &low_weather = {});
} // namespace orbitforge::core
