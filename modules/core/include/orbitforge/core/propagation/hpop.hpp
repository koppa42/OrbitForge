#pragma once
#include <memory>
#include <optional>
#include <orbitforge/core/frames/frame_provider.hpp>
#include <orbitforge/core/propagation/atmospheric_drag.hpp>
#include <orbitforge/core/propagation/gravity.hpp>
#include <orbitforge/core/propagation/ocean_tides.hpp>
#include <orbitforge/core/propagation/relativity.hpp>
#include <orbitforge/core/propagation/solar_radiation_pressure.hpp>
#include <orbitforge/core/propagation/solid_tides.hpp>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <orbitforge/core/propagation/tabulated_orbit.hpp>
#include <orbitforge/core/propagation/third_body.hpp>
#include <string_view>
#include <vector>

namespace orbitforge::core {
// Static, tide-free EGM96, truncated to degree/order 20. No tides or secular drift.
inline constexpr double egm96_mu = 3.986004415e14;
inline constexpr double egm96_radius_m = 6378136.3;
inline constexpr int egm96_max_degree = 20;
// Validated range for fixed steps and adaptive Min/Max Step, in TAI seconds.
inline constexpr double hpop_max_step_seconds = 86400;
inline constexpr double hpop_max_duration_seconds = 366.0 * 86400;
// RKV uses Verner's efficient 16-stage pair, advancing its eighth-order member.
enum class HpopIntegrator { rkf78, rk4, rkv89, bulirsch_stoer, gauss_jackson };
enum class HpopCorrection { full, pseudo };
enum class HpopStepControl { relative_error, fixed };
struct HpopOptions {
    // Catalog selection, never a file path. Empty selects the built-in 20x20 EGM96.
    std::string gravity_model{};
    // Selected resource's GM, also used when converting initial osculating elements.
    // Resource preparation verifies this value against the file; it cannot override it.
    double gravity_mu = egm96_mu;
    // Uses the selected field's explicit reference epoch and linear rates. Inactive for 0/0;
    // enabling it with a noncentral field lacking these data is an error.
    bool include_secular_variations = false;
    [[nodiscard]] bool uses_secular_gravity() const {
        return include_secular_variations && gravity_degree >= 2;
    }
    std::string ephemeris_model = "de440s";
    std::array<ThirdBodyGravity, third_body_count> third_bodies{};
    // Constant mass in kg. Spherical forces use their explicit A/m ratios;
    // mass determines the compatible areas without changing those accelerations.
    double satellite_mass_kg = 1000;
    // Area in m²; throws invalid_argument for nonpositive/nonfinite mass, invalid
    // A/m or a product outside the representable positive double range. Zero A/m is valid.
    [[nodiscard]] double drag_area_m2() const;
    [[nodiscard]] double srp_area_m2() const;
    SrpOptions srp;
    DragOptions drag;
    SolidTideOptions solid_tides;
    OceanTideOptions ocean_tides;
    bool include_relativistic_accelerations = false; // All three IERS 2003 terms.
    [[nodiscard]] bool uses_full_solid_tides() const {
        return solid_tides.mode == SolidTideMode::full &&
               (!solid_tides.truncate_to_gravity || gravity_degree >= 2);
    }
    [[nodiscard]] bool uses_utc_force() const {
        return drag.enabled ||
               (uses_full_solid_tides() && solid_tides.include_time_dependent_terms);
    }
    [[nodiscard]] bool uses_earth_fixed_gravity() const {
        // None may also remove the permanent component of a zero-tide base field.
        return gravity_degree > 0 || ocean_tides.active() ||
               (solid_tides.mode != SolidTideMode::none && !solid_tides.truncate_to_gravity);
    }
    [[nodiscard]] bool uses_eop_force() const {
        return uses_earth_fixed_gravity() || drag.enabled || include_relativistic_accelerations;
    }
    [[nodiscard]] ThirdBodyGravity &third_body(ThirdBody body) {
        return third_bodies.at(static_cast<std::size_t>(body));
    }
    [[nodiscard]] const ThirdBodyGravity &third_body(ThirdBody body) const {
        return third_bodies.at(static_cast<std::size_t>(body));
    }
    [[nodiscard]] std::vector<ThirdBody> enabled_third_bodies() const {
        std::vector<ThirdBody> result;
        for (auto body : third_body_list)
            if (third_body(body).enabled)
                result.push_back(body);
        return result;
    }
    [[nodiscard]] bool uses_third_bodies() const {
        for (const auto &body : third_bodies)
            if (body.enabled)
                return true;
        return false;
    }
    [[nodiscard]] bool uses_ephemeris() const {
        return uses_third_bodies() || srp.enabled || drag.enabled || uses_full_solid_tides() ||
               include_relativistic_accelerations;
    }
    [[nodiscard]] std::vector<ThirdBody> required_ephemeris_bodies() const {
        std::vector<ThirdBody> result;
        for (auto body : third_body_list)
            if (third_body(body).enabled ||
                ((drag.enabled || include_relativistic_accelerations) && body == ThirdBody::sun) ||
                (uses_full_solid_tides() && (body == ThirdBody::sun || body == ThirdBody::moon)) ||
                (srp.enabled &&
                 (body == ThirdBody::sun || (body == ThirdBody::moon && srp.moon_shadow &&
                                             srp.shadow_model != SrpShadowModel::none))))
                result.push_back(body);
        return result;
    }
    // 0/0 selects the chosen model's monopole. File models support up to degree 2190, within their
    // actual order limit.
    int gravity_degree = 8, gravity_order = 8;
    // RKF 7(8) advances order 7; RKV 8(9) advances order 8. Both use embedded error control.
    HpopIntegrator integrator = HpopIntegrator::rkf78;
    // Universal-variable variation of parameters; only effective for RKF78 / Bulirsch-Stoer.
    // Other methods retain this inactive draft. Independent of VOP ephemeris interpolation.
    bool vop = false;
    [[nodiscard]] bool uses_vop() const noexcept {
        return vop && (integrator == HpopIntegrator::rkf78 ||
                       integrator == HpopIntegrator::bulirsch_stoer);
    }
    // Final force update after Gauss-Jackson corrector convergence; inactive otherwise.
    HpopCorrection correction = HpopCorrection::full;
    // RK4 and Gauss-Jackson always use fixed steps; retain the other methods' draft.
    HpopStepControl step_control = HpopStepControl::relative_error;
    [[nodiscard]] bool uses_fixed_step() const noexcept {
        return integrator == HpopIntegrator::rk4 || integrator == HpopIntegrator::gauss_jackson ||
               step_control == HpopStepControl::fixed;
    }
    double relative_tolerance = 1e-12;
    double position_tolerance_m = 1e-3, velocity_tolerance_mps = 1e-6;
    double min_step_seconds = 0.001, max_step_seconds = 60;
    double fixed_step_seconds = 10; // Inactive when time_regularization is enabled.
    // dS = sqrt(mu) r^-n dt. Uses the initial osculating nonradial bound ellipse
    // to normalize steps per orbit, independently for each coast after an impulse.
    // Fixed methods use a uniform regularized grid; adaptive methods retain its
    // nominal scale while enforcing physical Min/Max Step. Endpoints can be shorter.
    // Finite 0 < exponent <= 10 and positive step count, including inactive drafts.
    // These initial values follow STK's example, not a claimed STK default.
    bool time_regularization = false;
    double regularization_exponent = 1.5;
    int steps_per_orbit = 120;
    // Scientific state queries in project/mission sessions use this interpolation.
    // The direct make_hpop kernel remains available for integration/reference queries.
    OrbitInterpolationOptions interpolation;
    // Uses OrbitInput.step_seconds as the reporting interval when enabled.
    bool report_ephemeris_on_fixed_step = false;
    // Geocentric altitude above the selected model's reference sphere, at each force evaluation.
    double minimum_altitude_m = 0;
    bool operator==(const HpopOptions &) const = default;
};
void validate_hpop_options(const HpopOptions &options);
// Includes the point-mass term. SI, in Earth-fixed axes; nonsingular at the poles.
[[nodiscard]] std::array<double, 3> egm96_acceleration(const std::array<double, 3> &position_m,
                                                       int degree, int order);
[[nodiscard]] std::string_view egm96_model_id() noexcept;
struct HpopEnvironment {
    // Required for noncentral gravity, drag and relativity. Immutable concurrent reads.
    std::shared_ptr<const IFrameProvider> frames;
    std::shared_ptr<const IEarthOrientationProvider> eop;
    std::shared_ptr<const GravityModel> gravity{};
    std::shared_ptr<const IThirdBodyEphemeris> ephemeris{};
    // UTC for atmosphere and time-dependent solid tides. Must match the EOP time identity.
    std::shared_ptr<const ITimeProvider> time{};
    std::shared_ptr<const SpaceWeather> weather{};
    // Owned immutable normalized potential model, required only when ocean tides are active.
    std::shared_ptr<const OceanTideModel> ocean_tides{};
    void validate_ephemeris_interval(const HpopOptions &options, Epoch first, Epoch last) const;
    // Validates complete weather paths, both models when a low-altitude model is selected.
    void validate_weather_interval(const DragOptions &drag, Epoch first, Epoch last) const;
};
// Cowell or universal-variable VOP equations in GCRF, TAI seconds. Initial state is
// osculating, not mean elements. VOP requires a numerically non-radial orbit, uses
// gravity_mu, and supports all conics; time regularization still requires an ellipse.
// Results are independent of query order and output sampling. Ordinary-time
// integration uses deterministic checkpoints. Direct regularized queries integrate
// from the initial state so a short query cannot reuse stages beyond its endpoint;
// project/mission scientific providers use make_hpop_interpolated for cached queries.
// Supports forward/backward propagation within 366 days of the initial epoch.
// No implicit data loading or extrapolation. PropagationError on impact, a failed
// tolerance or work limit. Cancellation checkpoints may throw; no partial result.
[[nodiscard]] std::shared_ptr<const IStateProvider>
make_hpop(CartesianState initial, HpopOptions options = {}, HpopEnvironment environment = {});
// Propagate one coast from initial.epoch to end, in either direction. Returned arcs own
// GCRF / TAI / SI nodes, sorted chronologically, with shared weather/shadow boundaries.
// A reporting step of zero retains accepted integration endpoints (GJ startup internals are
// not output nodes). A positive integer-nanosecond step forces integration to stop on an
// initial-epoch-anchored reporting grid; output includes both endpoints, weather jumps and enabled
// shadow contacts. No stages are evaluated outside this coast. Cannot cross a maneuver: the caller
// must start a new coast with the post-impulse state. Node generation does not choose/reduce an
// interpolation order. Same resource checks and 366-day / 200000-attempt limit as HPOP;
// reporting steps must be 0..86400 s and resolvable in relative double time.
// Cancellation/failure returns no partial ephemeris and changes no existing provider.
[[nodiscard]] std::vector<OrbitEphemerisArc> propagate_hpop_ephemeris(
    CartesianState initial, Epoch end, HpopOptions options = {}, HpopEnvironment environment = {},
    std::int64_t reporting_step_nanoseconds = 0, const std::function<void()> &checkpoint = {});
// Captures validated resources and lazily generates complete coasts covering [first, last]
// on the first query. All queries use scientific interpolation afterwards. Generation can
// be cancelled through state_at/discontinuities checkpoints; a failed cache is never published.
// Accepted-node mode adds in-coast integration stops where needed to support the requested
// order. Fixed reporting mode preserves its grid and rejects arcs with too few nodes.
// Instantaneous [initial, initial] coasts return that exact state without interpolation.
[[nodiscard]] std::shared_ptr<const IStateProvider>
make_hpop_interpolated(CartesianState initial, Epoch first, Epoch last, HpopOptions options = {},
                       HpopEnvironment environment = {},
                       std::int64_t reporting_step_nanoseconds = 0);
} // namespace orbitforge::core
