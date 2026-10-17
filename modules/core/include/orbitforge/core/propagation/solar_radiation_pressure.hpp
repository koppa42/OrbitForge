#pragma once
#include <array>
#include <optional>
#include <orbitforge/core/propagation/force_differential.hpp>
#include <orbitforge/core/propagation/third_body.hpp>
#include <span>
#include <vector>

namespace orbitforge::core {
enum class SrpShadowModel { none, cylindrical, dual_cone };
// Sun direction only; occultor centres remain geometric at the receiving epoch.
enum class SrpSunPositionMethod { geometric, apparent_to_true_cb, apparent };
// An additional spherical occultor. Planets only; Earth and Moon have their own settings.
// The centre is the physical body centre, never its planetary-system barycentre.
// Radius is explicit in metres; shape constants do not come from gravitational GM.
struct SrpEclipsingBody {
    ThirdBody body = ThirdBody::mercury;
    double radius_m = 2440530;
    bool operator==(const SrpEclipsingBody &) const = default;
};
struct SrpBodyPosition {
    ThirdBody body = ThirdBody::mercury;
    std::array<double, 3> position_m{};
};
// Physical-centre NAIF ID for a selectable additional planet, or invalid_argument.
[[nodiscard]] std::int32_t srp_eclipsing_body_naif_id(ThirdBody body);
// Starting spherical radii: largest axis in NAIF pck00011 (IAU 2015); editable.
// This is a spherical approximation, not a triaxial or STK shape-file model.
[[nodiscard]] SrpEclipsingBody default_srp_eclipsing_body(ThirdBody body);
struct SrpOptions {
    bool enabled = false;
    double cr = 1;
    double area_mass_ratio_m2_per_kg = 0.02;
    SrpShadowModel shadow_model = SrpShadowModel::dual_cone;
    SrpSunPositionMethod sun_position_method = SrpSunPositionMethod::apparent_to_true_cb;
    bool earth_shadow = true, moon_shadow = true;
    // Locate spherical shadow contacts and restart the HPOP force segment.
    bool boundary_mitigation = false;
    double atmospheric_altitude_m = 0;
    double pressure_at_reference_pa = 4.56e-6;
    double reference_distance_m = 149597870700.0;
    double sun_radius_m = 695700000, earth_radius_m = 6378137, moon_radius_m = 1737400;
    // Unique additional planets with explicit sphere radii. Saved order is a draft;
    // effective settings are sorted so selection order does not change calculation identity.
    std::vector<SrpEclipsingBody> additional_eclipsing_bodies;
    bool operator==(const SrpOptions &) const = default;
};
// Throws invalid_argument for invalid enums, nonfinite values, Cr outside [-100,100],
// A/m outside [0,9999] m²/kg, nonpositive constants or negative atmospheric altitude.
// Disabled settings are also validated so persistent drafts remain well defined.
void validate_srp_options(const SrpOptions &options);
// Normalizes inactive drafts for calculation identity; does not change saved settings.
[[nodiscard]] SrpOptions effective_srp_options(SrpOptions options);
struct SrpEvaluation {
    std::array<double, 3> acceleration_mps2{};
    double lighting_ratio = 1;
};
// SI, geocentric positions in the same inertial axes at one receiving TAI epoch.
// This geometry-only evaluator consumes the already selected Sun vector; it has
// no ephemeris and does not apply sun_position_method itself. HPOP resolves that
// method at every integration stage, preserving geometric occultor centres.
// Spherical satellite and occultors. Dual cone uses the uniform apparent solar disk
// and the union of all assigned apparent disks, including annular eclipses.
// Every assigned additional planet requires exactly one matching position; supplied
// duplicate or unassigned positions are rejected when shadows are active. With no
// shadows, additional positions are not inspected. Coincident disks count only once.
// Throws invalid_argument for missing required Moon geometry; PropagationError for
// nonfinite positions/force or an observer inside the Sun/assigned occultor.
// Moon is required only with SRP and a non-none shadow model assigning the Moon.
// Disabled SRP does not inspect positions. No ellipsoidal limb or atmospheric
// refraction is implied. boundary_mitigation is consumed by the HPOP integrator;
// it does not change this instantaneous force evaluator.
[[nodiscard]] SrpEvaluation
evaluate_srp(const std::array<double, 3> &satellite_m, const std::array<double, 3> &sun_m,
             const SrpOptions &options, const std::optional<std::array<double, 3>> &moon_m = {},
             std::span<const SrpBodyPosition> additional_positions = {});
struct SrpDifferential {
    SrpEvaluation nominal{};
    ForceDifferential force{};
    std::array<double, 3> lighting_position_gradient_per_m{};
    std::array<double, 3> lighting_sun_gradient_per_m{};
    std::array<std::array<double, 3>, 3> sun_position_jacobian_per_s2{};
    // STK consider parameter K = Cr * SRPArea/Mass [m^2/kg]; derivative in Pa.
    std::array<double, 3> srp_parameter_jacobian_pa{};
};
// Same geometry contract as evaluate_srp. Fixed receiving epoch, selected Sun
// vector and occultor centres; velocity block is zero at fixed geometry. Returns
// analytic local derivatives, including the uniform-disk union's area variation.
// The separate Sun-position block must be chained with the ephemeris derivatives
// for spacecraft-apparent Sun (which depends on spacecraft position AND velocity).
// Does not resolve that apparent ephemeris or perform shadow-event jump handling.
// Cylindrical contacts, depth-gate contacts and coincident apparent disks with
// different first variations throw PropagationError; no finite Jacobian describes
// those switches. Disabled SRP returns zero derivatives without inspecting positions.
[[nodiscard]] SrpDifferential
srp_differential(const std::array<double, 3> &satellite_m, const std::array<double, 3> &sun_m,
                 const SrpOptions &options, const std::optional<std::array<double, 3>> &moon_m = {},
                 std::span<const SrpBodyPosition> additional_positions = {});
} // namespace orbitforge::core
