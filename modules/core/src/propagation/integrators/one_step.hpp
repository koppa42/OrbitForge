#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <orbitforge/core/propagation/state_provider.hpp>

namespace orbitforge::core::detail {
// The state dimension and component scales belong to the equation formulation.
// Stages, coefficients and extrapolation are shared by Cartesian and augmented ODEs.
template <std::size_t N> struct ErrorControl {
    bool fixed = false;
    bool local_increments = false;
    double relative = 0;
    std::array<double, N> absolute{};
    double max_step = std::numeric_limits<double>::infinity();
    // An equation formulation may measure a signed local defect in physical coordinates.
    using State = std::array<double, N>;
    std::function<double(const State &, const State &, const State &)> mapped_error;
    double error(const State &before, const State &after, const State &defect) const {
        if (fixed)
            return 0;
        if (mapped_error)
            return mapped_error(before, after, defect);
        double value = 0;
        for (std::size_t i = 0; i < N; ++i)
            value = std::max(value, std::abs(defect[i]) / scale(i, before[i], after[i]));
        return value;
    }
    double scale(std::size_t i, double before, double after) const {
        return absolute[i] + relative * std::max(std::abs(before), std::abs(after));
    }
};
template <std::size_t N> struct TrialStep {
    std::array<double, N> value{};
    double error = 0, exponent = 0;
    // Preserve the local increment before adding it to a large coordinate/clock.
    // Augmented formulations can accumulate it with compensation between steps.
    std::array<double, N> increment{};
};
template <std::size_t N> struct CompensatedState {
    std::array<double, N> value{}, compensation{};
    void advance(const std::array<double, N> &increment) {
        for (std::size_t i = 0; i < N; ++i) {
            const double change = increment[i] - compensation[i];
            const double next = value[i] + change;
            compensation[i] = (next - value[i]) - change;
            value[i] = next;
        }
    }
};
// Fehlberg, NASA TR R-287 (1968), Table X, p. 65. Advance the seventh-order
// member, as in STK RKF 7(8); estimate error using the embedded eighth-order
// member. All stage epochs stay within a step.
constexpr std::array<double, 13> rkf_c{0,       2.0 / 27, 1.0 / 9, 1.0 / 6, 5.0 / 12, 0.5, 5.0 / 6,
                                       1.0 / 6, 2.0 / 3,  1.0 / 3, 1,       0,        1};
constexpr double rkf_a[13][13] = {
    {},
    {2.0 / 27},
    {1.0 / 36, 1.0 / 12},
    {1.0 / 24, 0, 1.0 / 8},
    {5.0 / 12, 0, -25.0 / 16, 25.0 / 16},
    {1.0 / 20, 0, 0, 1.0 / 4, 1.0 / 5},
    {-25.0 / 108, 0, 0, 125.0 / 108, -65.0 / 27, 125.0 / 54},
    {31.0 / 300, 0, 0, 0, 61.0 / 225, -2.0 / 9, 13.0 / 900},
    {2, 0, 0, -53.0 / 6, 704.0 / 45, -107.0 / 9, 67.0 / 90, 3},
    {-91.0 / 108, 0, 0, 23.0 / 108, -976.0 / 135, 311.0 / 54, -19.0 / 60, 17.0 / 6, -1.0 / 12},
    {2383.0 / 4100, 0, 0, -341.0 / 164, 4496.0 / 1025, -301.0 / 82, 2133.0 / 4100, 45.0 / 82,
     45.0 / 164, 18.0 / 41},
    {3.0 / 205, 0, 0, 0, 0, -6.0 / 41, -3.0 / 205, -3.0 / 41, 3.0 / 41, 6.0 / 41},
    {-1777.0 / 4100, 0, 0, -341.0 / 164, 4496.0 / 1025, -289.0 / 82, 2193.0 / 4100, 51.0 / 82,
     33.0 / 164, 12.0 / 41, 0, 1}};
constexpr std::array<double, 13> rkf_b7{
    41.0 / 840, 0, 0, 0, 0, 34.0 / 105, 9.0 / 35, 9.0 / 35, 9.0 / 280, 9.0 / 280, 41.0 / 840, 0, 0};
#include "rkv89_coefficients.inc"
template <std::size_t N, class Derivative>
TrialStep<N> rk4_trial(double t, const std::array<double, N> &s, double h,
                       const Derivative &derivative) {
    using StepState = std::array<double, N>;
    const auto k1 = derivative(t, s);
    StepState tmp{};
    for (std::size_t i = 0; i < s.size(); ++i)
        tmp[i] = s[i] + h * 0.5 * k1[i];
    const auto k2 = derivative(t + h * 0.5, tmp);
    for (std::size_t i = 0; i < s.size(); ++i)
        tmp[i] = s[i] + h * 0.5 * k2[i];
    const auto k3 = derivative(t + h * 0.5, tmp);
    for (std::size_t i = 0; i < s.size(); ++i)
        tmp[i] = s[i] + h * k3[i];
    const auto k4 = derivative(t + h, tmp);
    StepState increment{};
    for (std::size_t i = 0; i < s.size(); ++i) {
        increment[i] = h / 6 * (k1[i] + 2 * k2[i] + 2 * k3[i] + k4[i]);
        tmp[i] = s[i] + increment[i];
    }
    return {tmp, 0, 0, increment};
}
template <std::size_t N, class Derivative>
std::array<double, N> rk4(double t, const std::array<double, N> &s, double h,
                          const Derivative &derivative) {
    return rk4_trial(t, s, h, derivative).value;
}
template <std::size_t N, class Derivative>
TrialStep<N> rkf78(double t, const std::array<double, N> &s, double h, const Derivative &derivative,
                   const ErrorControl<N> &control) {
    using StepState = std::array<double, N>;
    std::array<StepState, 13> k;
    for (std::size_t stage = 0; stage < 13; ++stage) {
        auto value = s;
        for (std::size_t i = 0; i < s.size(); ++i) {
            double sum = 0;
            for (std::size_t j = 0; j < stage; ++j)
                sum += rkf_a[stage][j] * k[j][i];
            value[i] += h * sum;
        }
        k[stage] = derivative(t + rkf_c[stage] * h, value);
    }
    StepState result = s, increment{}, defect{};
    for (std::size_t i = 0; i < s.size(); ++i) {
        double sum = 0;
        for (std::size_t j = 0; j < 13; ++j)
            sum += rkf_b7[j] * k[j][i];
        increment[i] = h * sum;
        result[i] += increment[i];
        if (control.fixed)
            continue;
        defect[i] = h * (41.0 / 840) * (k[11][i] + k[12][i] - k[0][i] - k[10][i]);
    }
    return {result, control.error(s, result, defect), -1.0 / 8, increment};
}
template <std::size_t N, class Derivative>
TrialStep<N> rkv89(double t, const std::array<double, N> &s, double h, const Derivative &derivative,
                   const ErrorControl<N> &control) {
    using StepState = std::array<double, N>;
    std::array<StepState, 16> k;
    for (std::size_t stage = 0; stage < k.size(); ++stage) {
        auto value = s;
        for (std::size_t i = 0; i < s.size(); ++i) {
            double sum = 0;
            for (std::size_t j = 0; j < stage; ++j)
                sum += rkv_a[stage][j] * k[j][i];
            value[i] += h * sum;
        }
        const double at = t + rkv_c[stage] * h;
        k[stage] = derivative(at, value);
    }
    StepState result = s, increment{};
    double error = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        double advance = 0, difference = 0;
        for (std::size_t j = 0; j < k.size(); ++j) {
            advance += rkv_b8[j] * k[j][i];
            difference += rkv_error[j] * k[j][i];
        }
        increment[i] = h * advance;
        result[i] += increment[i];
        if (control.fixed)
            continue;
        const double scale = control.scale(i, s[i], result[i]);
        error = std::max(error, std::abs(h * difference) / scale);
    }
    return {result, error, -1.0 / 9, increment};
}
template <std::size_t N, class Derivative>
TrialStep<N> bulirsch_stoer(double t, const std::array<double, N> &s, double h,
                            const Derivative &derivative, const ErrorControl<N> &control,
                            const std::function<void()> &checkpoint = {}, bool startup = false) {
    using StepState = std::array<double, N>;
    // Smoothed modified midpoint, polynomial Richardson extrapolation in 1/n^2.
    // Even substep counts preserve the even-power expansion: orders 4..18.
    constexpr std::size_t levels = 9;
    const bool fixed = !startup && control.fixed;
    // Wider Bulirsch spacing keeps the order-14..18 extrapolation weights well
    // conditioned. Equally spaced 2..18 sequences amplify force roundoff, which
    // is visible in the physical clock of highly eccentric, long-period orbits.
    const std::array<int, levels> substeps =
        fixed || startup ? std::array<int, levels>{2, 4, 6, 8, 12, 16, 24, 32, 48}
                         : std::array<int, levels>{2, 4, 6, 8, 10, 12, 14, 16, 18};
    // High-order extrapolation of absolute coordinates amplifies cancellation,
    // particularly in GJ's order 14..18 starter. Extrapolate local increments.
    const bool offsets = fixed || startup || control.local_increments;
    const auto absolute_state = [&](const StepState &value) {
        if (!offsets)
            return value;
        StepState absolute = s;
        for (std::size_t i = 0; i < s.size(); ++i)
            absolute[i] += value[i];
        return absolute;
    };
    const auto trial = [&](const StepState &estimate, double error, double exponent) {
        StepState increment = estimate;
        if (!offsets)
            for (std::size_t i = 0; i < s.size(); ++i)
                increment[i] -= s[i];
        return TrialStep<N>{absolute_state(estimate), error, exponent, increment};
    };
    std::array<StepState, levels> table{};
    const auto first = derivative(t, s);
    TrialStep<N> best{}, rejected{};
    bool accepted = false;
    double best_work = std::numeric_limits<double>::infinity();
    int evaluations = 1;
    for (std::size_t level = 0; level < levels; ++level) {
        const int n = substeps[level];
        const double substep = h / n;
        StepState previous = offsets ? StepState{} : s, current = previous;
        for (std::size_t i = 0; i < s.size(); ++i)
            current[i] += substep * first[i];
        for (int j = 1; j < n; ++j) {
            if (checkpoint)
                checkpoint();
            const double at = t + h * (static_cast<double>(j) / n);
            const auto slope = derivative(at, absolute_state(current));
            auto next = previous;
            for (std::size_t i = 0; i < s.size(); ++i)
                next[i] += 2 * substep * slope[i];
            previous = current;
            current = next;
        }
        if (checkpoint)
            checkpoint();
        const auto last = derivative(t + h, absolute_state(current));
        StepState estimate{}, correction{};
        for (std::size_t i = 0; i < s.size(); ++i)
            estimate[i] = .5 * (previous[i] + current[i] + substep * last[i]);
        evaluations += n;
        for (std::size_t column = 0; column < level; ++column) {
            const auto lower = estimate;
            const double ratio = static_cast<double>(n) / substeps[level - column - 1];
            for (std::size_t i = 0; i < s.size(); ++i) {
                correction[i] = (estimate[i] - table[column][i]) / (ratio * ratio - 1);
                estimate[i] += correction[i];
            }
            table[column] = lower;
        }
        table[level] = estimate;
        if (fixed) {
            // Fixed-step BS uses all nine midpoint sequences: order 18,
            // independent of inactive tolerances, with no rejection or step adjustment.
            if (level + 1 == levels)
                return trial(estimate, 0, 0);
            continue;
        }
        if (level == 0 || (startup && level < 6))
            continue; // GJ startup uses at least order 14; normal BS needs an error estimate.
        const auto result = absolute_state(estimate);
        for (std::size_t i = 0; i < s.size(); ++i)
            if (!std::isfinite(estimate[i]) || !std::isfinite(correction[i]))
                throw PropagationError("HPOP extrapolation produced a nonfinite result");
        const double error = control.error(s, result, correction);
        const auto candidate = trial(estimate, error, -1.0 / (2 * level + 1));
        rejected = candidate;
        const bool lookahead = accepted;
        if (error <= 1) {
            const double factor =
                error == 0 ? 4 : std::clamp(.9 * std::pow(error, candidate.exponent), .1, 4.);
            // One lookahead column compares force evaluations per attainable next second.
            const double limit = startup ? std::abs(h) : control.max_step;
            const double proposed = std::min(std::abs(h) * factor, limit);
            const double work = evaluations / proposed;
            if (work < best_work) {
                best = candidate;
                best_work = work;
            }
            accepted = true;
        }
        if (lookahead)
            return best;
    }
    return accepted ? best : rejected;
}
} // namespace orbitforge::core::detail
