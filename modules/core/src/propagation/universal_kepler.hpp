#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/state_provider.hpp>

namespace orbitforge::core::detail {
// Universal-variable two-body flow in inertial Cartesian axes. SI units and
// continuous seconds; no epochs, orbital-element singularities or frame conversion.
// The radius-scaled Stumpff equation covers elliptic, parabolic and hyperbolic
// motion. Radial collision or numerically unresolved inputs fail explicitly.
using KeplerState = std::array<double, 6>;
inline std::array<double, 2> stumpff(double z) {
    if (std::abs(z) < .5) {
        double c = .5, s = 1. / 6, dc = c, ds = s;
        for (int k = 1; k < 32; ++k) {
            dc *= -z / ((2 * k + 1.) * (2 * k + 2.));
            ds *= -z / ((2 * k + 2.) * (2 * k + 3.));
            c += dc;
            s += ds;
            if (std::abs(dc) + std::abs(ds) <
                std::numeric_limits<double>::epsilon() * (std::abs(c) + std::abs(s)))
                break;
        }
        return {c, s};
    }
    const double x = std::sqrt(std::abs(z));
    if (z > 0) {
        const double half = std::sin(.5 * x);
        return {2 * half * half / z, (x - std::sin(x)) / (x * z)};
    }
    const double half = std::sinh(.5 * x);
    return {2 * half * half / (-z), (std::sinh(x) - x) / (x * (-z))};
}
struct KeplerFlowData {
    KeplerState state;
    double radius = 0, beta = 0, tau = 0, anomaly = 0, cycles = 0;
};
inline KeplerFlowData universal_kepler_data(const KeplerState &initial, double seconds, double mu,
                                            const std::function<void()> &checkpoint = {}) {
    if (checkpoint)
        checkpoint();
    if (!std::isfinite(mu) || mu <= 0 || !std::isfinite(seconds) ||
        !std::ranges::all_of(initial, [](double v) { return std::isfinite(v); }))
        throw std::invalid_argument(
            "Universal Kepler flow requires finite state/time and positive GM");
    const double radius = std::hypot(initial[0], initial[1], initial[2]);
    const double speed = std::sqrt(mu / radius), time_scale = radius / speed;
    if (!(radius > 0) || !std::isfinite(radius) || !(time_scale > 0) ||
        !std::isfinite(time_scale) || !std::isfinite(speed) || speed <= 0)
        throw std::invalid_argument("Universal Kepler scales are numerically unresolved");
    std::array<double, 3> r{}, v{};
    double v2 = 0, sigma = 0;
    for (std::size_t k = 0; k < 3; ++k) {
        r[k] = initial[k] / radius;
        v[k] = initial[k + 3] / speed;
        v2 = std::fma(v[k], v[k], v2);
        sigma = std::fma(r[k], v[k], sigma);
    }
    const double h =
        std::hypot(r[1] * v[2] - r[2] * v[1], r[2] * v[0] - r[0] * v[2], r[0] * v[1] - r[1] * v[0]);
    if (!std::isfinite(v2) ||
        h <= 64 * std::numeric_limits<double>::epsilon() * std::max(1., std::sqrt(v2)))
        throw std::invalid_argument("Universal Kepler flow cannot resolve a non-radial orbit");
    const double beta = 2 - v2;
    if (seconds == 0)
        return {initial, radius, beta, 0, 0, 0};
    double tau = seconds / time_scale;
    if (!std::isfinite(tau))
        throw PropagationError("Universal Kepler time is outside numerical range");
    double cycles = 0;
    if (beta > 0) {
        const double period = 2 * std::numbers::pi / (beta * std::sqrt(beta));
        if (std::isfinite(period)) {
            cycles = std::nearbyint(tau / period);
            tau = std::remainder(tau, period);
        }
    }
    if (tau == 0)
        return {initial, radius, beta, tau, 0, cycles};
    const double sign = std::copysign(1., tau), target = std::abs(tau);
    const auto equation = [&](double x) {
        const double u = sign * x, z = beta * u * u;
        const auto cs = stumpff(z);
        const double u2c = u * u * cs[0], u3s = u * u * u * cs[1];
        const double f = std::fma(1 - beta, u3s, std::fma(sigma, u2c, u)) * sign - target;
        const double d = std::fma(1 - beta, u2c, std::fma(sigma * u, 1 - z * cs[1], 1.));
        return std::array{f, d};
    };
    double lo = 0, hi = std::min(1., target);
    for (unsigned i = 0; equation(hi)[0] < 0; ++i) {
        if (i == 128 || !std::isfinite(hi *= 2))
            throw PropagationError("Universal Kepler root cannot be bracketed");
        if (checkpoint)
            checkpoint();
    }
    double x = .5 * hi;
    bool converged = false;
    for (unsigned i = 0; i < 128; ++i) {
        if (checkpoint)
            checkpoint();
        const auto fd = equation(x);
        if (!std::isfinite(fd[0]) || !std::isfinite(fd[1]) || fd[1] <= 0) {
            hi = x;
            x = .5 * (lo + hi);
            continue;
        }
        if (std::abs(fd[0]) <= 4 * std::numeric_limits<double>::epsilon() * target) {
            converged = true;
            break;
        }
        if (fd[0] > 0)
            hi = x;
        else
            lo = x;
        double next = x - fd[0] / fd[1];
        if (!(next > lo && next < hi))
            next = .5 * (lo + hi);
        if (next == x || next == lo || next == hi) {
            converged = true;
            break;
        }
        x = next;
    }
    if (!converged)
        throw PropagationError("Universal Kepler root did not converge");
    const double u = sign * x;
    const auto cs = stumpff(beta * u * u);
    const double u2c = u * u * cs[0], u3s = u * u * u * cs[1];
    const double f = 1 - u2c, g = time_scale * (tau - u3s);
    KeplerState result;
    for (std::size_t k = 0; k < 3; ++k)
        result[k] = std::fma(f, initial[k], g * initial[k + 3]);
    const double end_radius = std::hypot(result[0], result[1], result[2]) / radius;
    if (!std::isfinite(end_radius) || end_radius <= 0)
        throw PropagationError("Universal Kepler flow reached an unresolved radius");
    const double fdot = (beta * u3s - u) / (end_radius * time_scale);
    const double gdot = 1 - u2c / end_radius;
    for (std::size_t k = 0; k < 3; ++k)
        result[k + 3] = std::fma(fdot, initial[k], gdot * initial[k + 3]);
    if (!std::ranges::all_of(result, [](double v) { return std::isfinite(v); }))
        throw PropagationError("Universal Kepler flow produced a nonfinite state");
    return {result, radius, beta, tau, u, cycles};
}
inline KeplerState universal_kepler(const KeplerState &initial, double seconds, double mu,
                                    const std::function<void()> &checkpoint = {}) {
    return universal_kepler_data(initial, seconds, mu, checkpoint).state;
}
} // namespace orbitforge::core::detail
