#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <orbitforge/core/propagation/state_provider.hpp>

namespace orbitforge::core::detail {
#include "gauss_jackson_coefficients.inc"
// N second-order coordinates plus K first-order states. The latter use the
// method's single-integral sums/coefficients, with no artificial second derivative.
// State layout: coordinates[N], derivatives[N], first-order states[K].
// Thirteen acceleration/derivative samples; compensated sums avoid division by h.
template <std::size_t N, std::size_t K = 0> struct GaussJacksonSystem12 {
    using State = std::array<double, 2 * N + K>;
    using Acceleration = std::array<double, N + K>;
    State state{};
    std::array<Acceleration, 13> history{};
    std::size_t count = 0;
    Acceleration sum1{}, sum2{}, compensation1{}, compensation2{};

    void reset() { count = 0; }
    static void add(Acceleration &value, Acceleration &compensation,
                    const Acceleration &increment) {
        for (std::size_t i = 0; i < N + K; ++i) {
            const double change = increment[i] - compensation[i];
            const double next = value[i] + change;
            compensation[i] = (next - value[i]) - change;
            value[i] = next;
        }
    }
    static Acceleration dot(const std::array<double, 13> &weights,
                            const std::array<Acceleration, 13> &values, double total) {
        Acceleration result{}, compensation{};
        for (std::size_t i = 0; i < N + K; ++i)
            result[i] = total * values[0][i];
        // The exact coefficient sum is known. Subtract the newest acceleration to
        // avoid cancellation of a large, nearly constant central acceleration.
        for (std::size_t j = 1; j < values.size(); ++j) {
            Acceleration term{};
            for (std::size_t i = 0; i < N + K; ++i)
                term[i] = weights[j] * (values[j][i] - values[0][i]);
            add(result, compensation, term);
        }
        return result;
    }
    void seed(const State &next, const Acceleration &acceleration, double h,
              const State &state_compensation = {}) {
        for (std::size_t j = std::min(count, history.size() - 1); j > 0; --j)
            history[j] = history[j - 1];
        history[0] = acceleration;
        state = next;
        count = std::min(count + 1, history.size());
        if (count == history.size()) {
            const auto p = dot(gj_position, history, 1.0 / 12);
            const auto v = dot(gj_velocity, history, 0);
            compensation1 = compensation2 = {};
            // Initialize the sums as compensated differences, including the
            // starter's residual and the product rounding. Dropping these bits
            // shifts the physical phase of a long, highly eccentric orbit.
            const auto subtract = [](double value, double residual, double product,
                                     double product_error, double &sum, double &compensation) {
                sum = value - product;
                const double virtual_b = sum - value;
                const double addition_error = (value - (sum - virtual_b)) + (-product - virtual_b);
                compensation = residual + product_error - addition_error;
            };
            for (std::size_t i = 0; i < N + K; ++i) {
                const double velocity_term = h * v[i];
                subtract(state[i + N], state_compensation[i + N], velocity_term,
                         std::fma(h, v[i], -velocity_term), sum1[i], compensation1[i]);
                if (i < N) {
                    const double square = h * h, position_term = square * p[i];
                    const double product_error =
                        std::fma(square, p[i], -position_term) + std::fma(h, h, -square) * p[i];
                    subtract(state[i], state_compensation[i], position_term, product_error, sum2[i],
                             compensation2[i]);
                }
            }
        }
    }
    template <class Force, class Correction, class Converged>
    void advance(double h, bool full_correction, const Force &force, const Correction &correction,
                 const Converged &converged) {
        Acceleration increment{};
        for (std::size_t i = 0; i < N; ++i)
            increment[i] = h * (sum1[i] - compensation1[i]) + h * h * .5 * history[0][i];
        auto next_sum2 = sum2, next_compensation2 = compensation2;
        add(next_sum2, next_compensation2, increment);
        const auto p = dot(gj_position_predict, history, 1.0 / 12);
        const auto v = dot(gj_velocity_predict, history, .5);
        State estimate{};
        for (std::size_t i = 0; i < N + K; ++i) {
            if (i < N)
                estimate[i] = next_sum2[i] + (h * h * p[i] - next_compensation2[i]);
            estimate[i + N] = sum1[i] + (h * (.5 * history[0][i] + v[i]) - compensation1[i]);
        }
        auto shifted = history;
        for (std::size_t j = shifted.size() - 1; j > 0; --j)
            shifted[j] = history[j - 1];
        for (int iteration = 0; iteration < 12; ++iteration) {
            shifted[0] = force(estimate);
            auto next_sum1 = sum1, next_compensation1 = compensation1;
            for (std::size_t i = 0; i < N + K; ++i)
                increment[i] = h * .5 * (history[0][i] + shifted[0][i]);
            add(next_sum1, next_compensation1, increment);
            const auto corrected_p = dot(gj_position, shifted, 1.0 / 12);
            const auto corrected_v = dot(gj_velocity, shifted, 0);
            State corrected{};
            for (std::size_t i = 0; i < N + K; ++i) {
                if (i < N)
                    corrected[i] = next_sum2[i] + (h * h * corrected_p[i] - next_compensation2[i]);
                corrected[i + N] = next_sum1[i] + (h * corrected_v[i] - next_compensation1[i]);
            }
            if (converged(estimate, corrected)) {
                // STK's choice affects the final, post-convergence force update.
                // Pseudo retains perturbations from the last EC evaluation.
                if (full_correction) {
                    shifted[0] = force(corrected);
                } else {
                    shifted[0] = correction(estimate, corrected, shifted[0]);
                }
                for (std::size_t i = 0; i < N + K; ++i)
                    increment[i] = h * .5 * (history[0][i] + shifted[0][i]);
                add(sum1, compensation1, increment);
                sum2 = next_sum2;
                compensation2 = next_compensation2;
                history = shifted;
                state = corrected;
                return;
            }
            estimate = corrected;
        }
        throw PropagationError(
            "HPOP Gauss-Jackson corrector did not converge; reduce the fixed step");
    }
};
// Ordinary Cartesian-time adapter retains the established convergence and
// pseudo-correction semantics. Other formulations supply their own scales and
// transformed two-body term, including their kinematic clock terms.
struct GaussJackson12 : GaussJacksonSystem12<3> {
    static Acceleration central(const State &s, double mu) {
        const double r = std::hypot(s[0], s[1], s[2]);
        const double scale = -mu / (r * r * r);
        return {scale * s[0], scale * s[1], scale * s[2]};
    }
    template <class Force>
    void advance(double h, double mu, bool full_correction, const Force &force) {
        const auto converged = [](const State &estimate, const State &corrected) {
            double position_change = 0, velocity_change = 0;
            for (std::size_t i = 0; i < 3; ++i) {
                position_change = std::max(position_change, std::abs(corrected[i] - estimate[i]));
                velocity_change =
                    std::max(velocity_change, std::abs(corrected[i + 3] - estimate[i + 3]));
            }
            constexpr double convergence = 64 * std::numeric_limits<double>::epsilon();
            const double position_scale =
                std::max(1., std::hypot(corrected[0], corrected[1], corrected[2]));
            const double velocity_scale =
                std::max(1., std::hypot(corrected[3], corrected[4], corrected[5]));
            return position_change <= convergence * position_scale &&
                   velocity_change <= convergence * velocity_scale;
        };
        const auto correction = [mu](const State &before, const State &after, Acceleration value) {
            const auto old_central = central(before, mu), new_central = central(after, mu);
            for (std::size_t i = 0; i < 3; ++i)
                value[i] += new_central[i] - old_central[i];
            return value;
        };
        GaussJacksonSystem12<3>::advance(h, full_correction, force, correction, converged);
    }
};
} // namespace orbitforge::core::detail
