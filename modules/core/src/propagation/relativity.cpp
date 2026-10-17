#include <cmath>
#include <orbitforge/core/propagation/relativity.hpp>
#include <stdexcept>

namespace orbitforge::core {
namespace {
using Vector = std::array<double, 3>;
constexpr double c = 299792458, angular_momentum_per_mass = 9.8e8;
double dot(const Vector &a, const Vector &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vector cross(const Vector &a, const Vector &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double norm(const Vector &v) { return std::hypot(v[0], v[1], v[2]); }
void finite(const Vector &v) {
    for (double x : v)
        if (!std::isfinite(x))
            throw std::invalid_argument("Relativistic force requires finite vectors");
}
} // namespace
RelativityEvaluation evaluate_relativity(const Vector &r, const Vector &v, double earth_mu,
                                         const Vector &sun, const Vector &sun_velocity,
                                         double sun_mu, const Vector &pole) {
    for (const auto *input : {&r, &v, &sun, &sun_velocity, &pole})
        finite(*input);
    const double radius = norm(r), distance = norm(sun), speed = norm(v),
                 sun_speed = norm(sun_velocity);
    if (!std::isfinite(earth_mu) || earth_mu <= 0 || !std::isfinite(sun_mu) || sun_mu <= 0 ||
        !std::isfinite(radius) || radius <= 0 || !std::isfinite(distance) || distance <= 0 ||
        speed >= c || sun_speed >= c || std::abs(norm(pole) - 1) > 1e-12)
        throw std::invalid_argument("Invalid relativistic force GM, radius, velocity or unit pole");
    const double scale = earth_mu / (c * c * radius * radius * radius);
    const double radial = 4 * earth_mu / radius - dot(v, v), along = 4 * dot(r, v);
    const auto orbit_normal = cross(r, v), v_cross_pole = cross(v, pole);
    const double projection = 3 * dot(r, pole) / (radius * radius);
    // Reversing both Earth-relative Sun vectors leaves this cross product unchanged.
    const auto solar_precession = cross(cross(sun, sun_velocity), v);
    const double solar_scale = 3 * sun_mu / (c * c * distance * distance * distance);
    RelativityEvaluation result{};
    for (std::size_t i = 0; i < 3; ++i) {
        result.schwarzschild_mps2[i] = scale * (radial * r[i] + along * v[i]);
        result.lense_thirring_mps2[i] = 2 * scale * angular_momentum_per_mass *
                                        (projection * orbit_normal[i] + v_cross_pole[i]);
        result.de_sitter_mps2[i] = solar_scale * solar_precession[i];
        result.acceleration_mps2[i] =
            result.schwarzschild_mps2[i] + result.lense_thirring_mps2[i] + result.de_sitter_mps2[i];
    }
    finite(result.schwarzschild_mps2);
    finite(result.lense_thirring_mps2);
    finite(result.de_sitter_mps2);
    finite(result.acceleration_mps2);
    return result;
}
RelativityDifferential relativity_differential(const Vector &r, const Vector &v, double earth_mu,
                                               const Vector &sun, const Vector &sun_velocity,
                                               double sun_mu, const Vector &pole) {
    const auto nominal = evaluate_relativity(r, v, earth_mu, sun, sun_velocity, sun_mu, pole);
    RelativityDifferential result;
    result.schwarzschild.acceleration_mps2 = nominal.schwarzschild_mps2;
    result.lense_thirring.acceleration_mps2 = nominal.lense_thirring_mps2;
    result.de_sitter.acceleration_mps2 = nominal.de_sitter_mps2;
    result.total.acceleration_mps2 = nominal.acceleration_mps2;
    const double radius = norm(r), inverse2 = 1 / (radius * radius), distance = norm(sun);
    const double scale = earth_mu / (c * c * radius * radius * radius);
    const double radial = 4 * earth_mu / radius - dot(v, v), along = 4 * dot(r, v);
    const double projection = 3 * dot(r, pole) * inverse2;
    const auto orbit_normal = cross(r, v), solar_normal = cross(sun, sun_velocity);
    const double spin_scale = 2 * scale * angular_momentum_per_mass;
    const double solar_scale = 3 * sun_mu / (c * c * distance * distance * distance);
    for (std::size_t j = 0; j < 3; ++j) {
        Vector axis{};
        axis[j] = 1;
        const auto axis_cross_v = cross(axis, v), r_cross_axis = cross(r, axis),
                   axis_cross_pole = cross(axis, pole),
                   solar_cross_axis = cross(solar_normal, axis);
        const double log_scale_derivative = -3 * r[j] * inverse2;
        const double radial_derivative = -4 * (earth_mu / radius) * r[j] * inverse2;
        const double projection_derivative = (3 * pole[j] - 2 * projection * r[j]) * inverse2;
        for (std::size_t i = 0; i < 3; ++i) {
            const double identity = i == j ? 1 : 0;
            result.schwarzschild.position_jacobian_per_s2[i][j] =
                log_scale_derivative * nominal.schwarzschild_mps2[i] +
                scale * (radial_derivative * r[i] + radial * identity + 4 * v[j] * v[i]);
            result.schwarzschild.velocity_jacobian_per_s[i][j] =
                scale * (-2 * v[j] * r[i] + 4 * r[j] * v[i] + along * identity);
            result.lense_thirring.position_jacobian_per_s2[i][j] =
                log_scale_derivative * nominal.lense_thirring_mps2[i] +
                spin_scale *
                    (projection_derivative * orbit_normal[i] + projection * axis_cross_v[i]);
            result.lense_thirring.velocity_jacobian_per_s[i][j] =
                spin_scale * (projection * r_cross_axis[i] + axis_cross_pole[i]);
            result.de_sitter.velocity_jacobian_per_s[i][j] = solar_scale * solar_cross_axis[i];
            result.total.position_jacobian_per_s2[i][j] =
                result.schwarzschild.position_jacobian_per_s2[i][j] +
                result.lense_thirring.position_jacobian_per_s2[i][j] +
                result.de_sitter.position_jacobian_per_s2[i][j];
            result.total.velocity_jacobian_per_s[i][j] =
                result.schwarzschild.velocity_jacobian_per_s[i][j] +
                result.lense_thirring.velocity_jacobian_per_s[i][j] +
                result.de_sitter.velocity_jacobian_per_s[i][j];
        }
    }
    for (const auto *term :
         {&result.schwarzschild, &result.lense_thirring, &result.de_sitter, &result.total}) {
        for (const auto &row : term->position_jacobian_per_s2)
            finite(row);
        for (const auto &row : term->velocity_jacobian_per_s)
            finite(row);
    }
    return result;
}
} // namespace orbitforge::core
