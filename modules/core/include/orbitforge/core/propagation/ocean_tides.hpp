#pragma once
#include <memory>
#include <orbitforge/core/frames/eop_provider.hpp>
#include <orbitforge/core/propagation/gravity.hpp>

namespace orbitforge::core {
struct OceanTideOptions {
    bool enabled = false;
    std::string model = "fes2014bv1"; // Catalog ID, never a path.
    int degree = 4, order = 4;        // Independent of the central gravity truncation.
    [[nodiscard]] bool active() const noexcept { return enabled && degree >= 2; }
    bool operator==(const OceanTideOptions &) const = default;
};
void validate_ocean_tide_options(const OceanTideOptions &options);
struct OceanTideInfo {
    std::string model, sha256, source_url;
    int degree = 0, order = 0;
    std::size_t constituents = 0;
    // IERS 2010 normalized potential scaling, SI. Kept independent of the base field.
    double mu = 3.986004415e14, radius_m = 6378136.3;
};
struct OceanTideCoefficient {
    int degree = 0, order = 0;
    double c_plus = 0, s_plus = 0, c_minus = 0, s_minus = 0;
};
struct OceanTideConstituent {
    int doodson = 0; // Six decimal digits, e.g. 055565 is passed as 55565.
    std::vector<OceanTideCoefficient> coefficients;
};
// Immutable normalized potential waves. IERS 2010 eq. 6.15, TT fundamental
// arguments and UT1 GMST. No minor-wave inference, frame conversion or I/O.
// Each wave must contain its complete rectangular degree/order extent from degree 2;
// different waves may have different extents. Degree 0/1 records are checked but
// excluded from the geocentric force. Duplicate, missing or nonfinite terms fail.
class OceanTideModel {
  public:
    OceanTideModel(OceanTideInfo info, std::vector<OceanTideConstituent> constituents);
    [[nodiscard]] const OceanTideInfo &info() const noexcept { return info_; }
    // Additive fully normalized C/S in ITRF, rescaled to field.mu/radius_m.
    // The independent requested truncation must fit this ocean model; 0/1 gives no force.
    // Includes IERS 2010 eq. 6.24 ocean pole tide for degree>=2, order>=1 by default.
    // Uses the original IERS 2010 cubic/linear mean pole, switching at 2010-01-01 TT.
    // Borrowed EOP is evaluated at its own epoch. Pole-off is a low-level validation
    // facility; HPOP always includes the applicable pole term. All reads are concurrent.
    // Invalid inputs throw invalid_argument; the model never silently reduces truncation.
    [[nodiscard]] std::vector<GravityCoefficient> corrections(const GravityInfo &field,
                                                              const EarthOrientationSample &eop,
                                                              int degree, int order,
                                                              bool include_pole = true) const;

  private:
    struct Term {
        int degree, order;
        std::array<double, 4> amplitudes;
    };
    struct Wave {
        std::array<int, 6> arguments; // gamma, l, l', F, D, Omega.
        std::vector<Term> terms;
    };
    OceanTideInfo info_;
    std::vector<Wave> waves_;
};
} // namespace orbitforge::core
