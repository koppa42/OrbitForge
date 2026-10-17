#pragma once
#include "universal_kepler.hpp"

namespace orbitforge::core::detail {
using KeplerMatrix = std::array<std::array<double, 6>, 6>;
struct KeplerLinearization {
    KeplerState state, increment;
    KeplerMatrix jacobian;
};
namespace kepler_differentials {
// Six first derivatives; these scalars do not differentiate iterations or a wrapped clock.
struct Scalar {
    double value = 0;
    std::array<double, 6> gradient{};
    Scalar() = default;
    Scalar(double v) : value(v) {}
};
inline Scalar operator+(const Scalar &a, const Scalar &b) {
    Scalar c(a.value + b.value);
    for (std::size_t i = 0; i < 6; ++i)
        c.gradient[i] = a.gradient[i] + b.gradient[i];
    return c;
}
inline Scalar operator-(const Scalar &a, const Scalar &b) {
    Scalar c(a.value - b.value);
    for (std::size_t i = 0; i < 6; ++i)
        c.gradient[i] = a.gradient[i] - b.gradient[i];
    return c;
}
inline Scalar operator*(const Scalar &a, const Scalar &b) {
    Scalar c(a.value * b.value);
    for (std::size_t i = 0; i < 6; ++i)
        c.gradient[i] = std::fma(a.gradient[i], b.value, a.value * b.gradient[i]);
    return c;
}
inline Scalar operator/(const Scalar &a, const Scalar &b) {
    Scalar c(a.value / b.value);
    for (std::size_t i = 0; i < 6; ++i)
        c.gradient[i] = (a.gradient[i] - c.value * b.gradient[i]) / b.value;
    return c;
}
inline Scalar sqrt(const Scalar &a) {
    Scalar c(std::sqrt(a.value));
    for (std::size_t i = 0; i < 6; ++i)
        c.gradient[i] = .5 * a.gradient[i] / c.value;
    return c;
}
inline std::array<Scalar, 2> stumpff(const Scalar &z) {
    const auto cs = detail::stumpff(z.value);
    double dc = 0, ds = 0;
    if (std::abs(z.value) < .5) {
        double tc = -1. / 24, ts = -1. / 120;
        dc = tc;
        ds = ts;
        for (int k = 1; k < 32; ++k) {
            const double factor = -z.value * (k + 1.) / k;
            tc *= factor / ((2 * k + 3.) * (2 * k + 4.));
            ts *= factor / ((2 * k + 4.) * (2 * k + 5.));
            dc += tc;
            ds += ts;
            if (std::abs(tc) + std::abs(ts) <
                std::numeric_limits<double>::epsilon() * (std::abs(dc) + std::abs(ds)))
                break;
        }
    } else {
        dc = (1 - z.value * cs[1] - 2 * cs[0]) / (2 * z.value);
        ds = (cs[0] - 3 * cs[1]) / (2 * z.value);
    }
    std::array<Scalar, 2> result{Scalar(cs[0]), Scalar(cs[1])};
    for (std::size_t i = 0; i < 6; ++i) {
        result[0].gradient[i] = dc * z.gradient[i];
        result[1].gradient[i] = ds * z.gradient[i];
    }
    return result;
}
} // namespace kepler_differentials

// Cartesian universal-variable flow and its exact first differential (fixed SI time and GM).
// The root derivative follows from the implicit Stumpff equation, not solver iteration AD.
// Elliptic period reduction retains the period's state dependence, including full periods.
inline KeplerLinearization
universal_kepler_linearized(const KeplerState &initial, double seconds, double mu,
                            const std::function<void()> &checkpoint = {}) {
    const auto flow = universal_kepler_data(initial, seconds, mu, checkpoint);
    using kepler_differentials::Scalar;
    std::array<Scalar, 6> y;
    for (std::size_t i = 0; i < 6; ++i) {
        y[i].value = initial[i];
        y[i].gradient[i] = 1;
    }
    Scalar radius = kepler_differentials::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    radius.value = flow.radius; // Match the overflow-resistant norm used by the flow.
    const auto speed = kepler_differentials::sqrt(Scalar(mu) / radius);
    const auto time_scale = radius / speed;
    Scalar v2, sigma;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto r = y[i] / radius, v = y[i + 3] / speed;
        v2 = v2 + v * v;
        sigma = sigma + r * v;
    }
    auto beta = Scalar(2) - v2;
    beta.value = flow.beta;
    auto tau = Scalar(seconds) / time_scale;
    if (flow.cycles != 0) {
        const auto period =
            Scalar(2 * std::numbers::pi) / (beta * kepler_differentials::sqrt(beta));
        tau = tau - Scalar(flow.cycles) * period;
    }
    tau.value = flow.tau;
    Scalar u(flow.anomaly);
    auto cs = kepler_differentials::stumpff(beta * u * u);
    const auto equation = u + sigma * u * u * cs[0] + (Scalar(1) - beta) * u * u * u * cs[1] - tau;
    const double radial_derivative =
        std::hypot(flow.state[0], flow.state[1], flow.state[2]) / flow.radius;
    for (std::size_t i = 0; i < 6; ++i)
        u.gradient[i] = -equation.gradient[i] / radial_derivative;
    cs = kepler_differentials::stumpff(beta * u * u);
    const auto u2c = u * u * cs[0], u3s = u * u * u * cs[1];
    const auto f_delta = Scalar(0) - u2c, g = time_scale * (tau - u3s);
    std::array<Scalar, 6> result;
    KeplerLinearization out{flow.state, {}, {}};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto change = f_delta * y[i] + g * y[i + 3];
        result[i] = y[i] + change;
        out.increment[i] = change.value;
    }
    const auto r0 = result[0] / radius, r1 = result[1] / radius, r2 = result[2] / radius;
    const auto end_radius = kepler_differentials::sqrt(r0 * r0 + r1 * r1 + r2 * r2);
    const auto fdot = (beta * u3s - u) / (end_radius * time_scale);
    const auto gdot_delta = Scalar(0) - u2c / end_radius;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto change = fdot * y[i] + gdot_delta * y[i + 3];
        result[i + 3] = y[i + 3] + change;
        out.increment[i + 3] = change.value;
    }
    for (std::size_t i = 0; i < 6; ++i) {
        out.jacobian[i] = result[i].gradient;
        if (!std::isfinite(out.increment[i]) ||
            !std::ranges::all_of(out.jacobian[i], [](double v) { return std::isfinite(v); }))
            throw PropagationError("Universal Kepler differential is numerically unresolved");
    }
    if (checkpoint)
        checkpoint();
    return out;
}
} // namespace orbitforge::core::detail
