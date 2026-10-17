#pragma once
#include <orbitforge/core/frames/eop_provider.hpp>
#include <orbitforge/core/propagation/gravity.hpp>
#include <span>

namespace orbitforge::core {
enum class SolidTideMode { none, permanent, full };
struct SolidTideOptions {
    SolidTideMode mode = SolidTideMode::none;
    bool truncate_to_gravity = true;
    bool include_time_dependent_terms = false; // Full only: 71 frequency terms and solid pole tide.
    bool operator==(const SolidTideOptions &) const = default;
};
struct TideRaisingBody {
    std::array<double, 3> position_itrf_m{}; // Geometric, Earth-centred, same epoch as the field.
    double mu =
        0; // m^3/s^2, from the ephemeris solution, independent of direct third-body options.
};
void validate_solid_tide_options(const SolidTideOptions &options);
// Canonical calculation settings; inactive drafts remain in the saved options.
[[nodiscard]] SolidTideOptions effective_solid_tide_options(SolidTideOptions options,
                                                            int gravity_degree);
// Fully normalized IERS 2003, anelastic Earth; k20 comes from field.solid_tide_k20.
// The same real k20 controls the degree-2 response and permanent deformation.
// Other Love numbers and frequency/pole terms retain the IERS 2003 values.
// Full includes the instantaneous Sun/Moon deformation (degrees 2/3, induced degree 4).
// Include Time Dependent Terms adds IERS 2003 tables 6.3a/b/c and the printed
// 2003 pole-tide signs (not the corrected 2010 convention). No amplitude filter.
// None removes the separable permanent deformation from a zero-tide base field;
// Permanent includes it exactly once; Full already contains it in the body response.
// Only tide_free / zero_tide fields are supported; mean_tide is rejected explicitly.
// Input/output axes are ITRF; no downloads, frame conversion or ephemeris resolution.
// Full requires exactly two bodies (Sun and Moon); other modes do not read bodies.
// Active time-dependent terms require borrowed EOP at the evaluation epoch and
// its UTC provider. Historical mean pole uses 1900-2000 annual samples, then the
// IERS 2003 linear extension; no silent omission outside time/mean-pole coverage.
// Invalid options/metadata/geometry throw std::invalid_argument; time range errors
// propagate from the provider (std::out_of_range before the mean-pole table).
[[nodiscard]] std::vector<GravityCoefficient> solid_tide_corrections(
    const GravityInfo &field, const SolidTideOptions &options, int gravity_degree,
    int gravity_order, std::span<const TideRaisingBody> bodies = {},
    const EarthOrientationSample *eop = nullptr, const ITimeProvider *time = nullptr);
// Dimensionless normalized permanent deformation; rejects a nonfinite real k20.
[[nodiscard]] double iers2003_permanent_tide_c20(double k20 = 0.30190);
} // namespace orbitforge::core
