#pragma once
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <orbitforge/core/time/epoch.hpp>
#include <span>
#include <string>
#include <vector>

namespace orbitforge::core {
// Supports the full EGM2008 degree (2190) and its rectangular order limit (2159).
// Each field retains its declared degree/order. Fully normalized, no Condon-Shortley phase.
inline constexpr int gravity_max_degree = 2190;
struct GravityCoefficient {
    int degree = 0, order = 0;
    double c = 0, s = 0;
};
// Rates of fully normalized coefficients per Julian year (365.25 SI days).
// At reference_epoch the additive correction is exactly zero. The base coefficients
// must describe that epoch; no model-name inference or implicit re-epoching is applied.
struct GravitySecularVariations {
    Epoch reference_epoch{0};
    std::vector<GravityCoefficient> rates_per_year;
};
struct GravityInfo {
    std::string model, sha256, source_url, tide_system;
    double mu = 0, radius_m = 0;
    int degree = 0, order = 0;
    // Dimensionless real Earth k20, finite; STK overrides only this Love number.
    // IERS 2003 default after the March 2006 update.
    double solid_tide_k20 = 0.30190;
    std::optional<GravitySecularVariations> secular_variations{};
};
// Spatial differential at a fixed force epoch and fixed coefficient corrections.
// The Jacobian is d(acceleration_i)/d(position_j), in the same Earth-fixed axes.
struct GravityDifferential {
    std::array<double, 3> acceleration_mps2{};
    std::array<std::array<double, 3>, 3> position_jacobian_per_s2{};
};
// Immutable, Earth-centered reference potential, including its monopole. SI units;
// input/output axes are Earth-fixed. Optional additive normalized corrections are supplied
// by the caller at the force epoch; they never mutate the immutable base field.
class GravityModel {
  public:
    GravityModel(GravityInfo info, const std::vector<GravityCoefficient> &coefficients);
    [[nodiscard]] const GravityInfo &info() const noexcept { return info_; }
    [[nodiscard]] std::array<double, 3>
    acceleration(const std::array<double, 3> &position_m, int degree, int order,
                 std::span<const GravityCoefficient> corrections = {},
                 std::optional<Epoch> secular_at = {}) const;
    // Analytic potential Hessian, including the monopole, through the full supported
    // degree/order and at the poles. No finite-difference displacement. Cancellation
    // throws without modifying this immutable field; normal acceleration calls do
    // not evaluate the extra derivatives. Invalid inputs follow acceleration().
    [[nodiscard]] GravityDifferential
    differential(const std::array<double, 3> &position_m, int degree, int order,
                 std::span<const GravityCoefficient> corrections = {},
                 std::optional<Epoch> secular_at = {},
                 const std::function<void()> &checkpoint = {}) const;
    // Select only rates inside the base field truncation. Throws if metadata is absent,
    // truncation is invalid, or the evolved corrections are not finite. No UTC is needed.
    [[nodiscard]] std::vector<GravityCoefficient> secular_corrections(Epoch at, int degree,
                                                                      int order) const;

  private:
    template <bool Second>
    GravityDifferential evaluate(const std::array<double, 3> &position_m, int degree, int order,
                                 std::span<const GravityCoefficient> corrections,
                                 std::optional<Epoch> secular_at,
                                 const std::function<void()> &checkpoint) const;
    GravityInfo info_;
    struct Term {
        double c = 0, s = 0, offset = 0, b = 0;
    };
    std::vector<Term> terms_;
};
[[nodiscard]] std::shared_ptr<const GravityModel> builtin_egm96();
} // namespace orbitforge::core
