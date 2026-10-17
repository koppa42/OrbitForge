#pragma once
#include "../universal_kepler_jacobian.hpp"
#include "one_step.hpp"

namespace orbitforge::core::detail {
// Re-anchor universal-variable osculating Cartesian parameters at each trial start.
// y = Phi(dt,c); dc/dt = D(Phi)^-1 [0,a_pert]. Hamiltonian two-body motion is
// symplectic: only the position Jacobian blocks are needed, dc = [-B^T a, A^T a].
// N=7 supports either physical time in the last state (Sundman q steps), or q in
// the last state with physical time independent (boundary bridges).
template <std::size_t N, class Derivative, class Stepper>
TrialStep<N> vop_step(double time, const std::array<double, N> &initial, double h, double mu,
                      bool embedded_time, const Derivative &physical_derivative,
                      const Stepper &stepper, const ErrorControl<N> &physical_control,
                      const std::function<void()> &checkpoint = {}) {
    static_assert(N == 6 || N == 7);
    using ParameterState = std::array<double, N>;
    const auto elapsed = [&](double at, [[maybe_unused]] const ParameterState &parameters) {
        if constexpr (N == 7)
            if (embedded_time)
                return parameters[6] - initial[6];
        return at;
    };
    const auto flow = [&](double at, const ParameterState &parameters) {
        return universal_kepler_linearized({parameters[0], parameters[1], parameters[2],
                                            parameters[3], parameters[4], parameters[5]},
                                           elapsed(at, parameters), mu, checkpoint);
    };
    const auto central = [&](const KeplerState &state) {
        const double radius = std::hypot(state[0], state[1], state[2]);
        // Match the monopole evaluation order so its subtraction is exact for a pure two-body
        // force.
        const double inverse2 = 1 / (radius * radius), factor = -mu * inverse2 / radius;
        return std::array{factor * state[0], factor * state[1], factor * state[2]};
    };
    const auto derivative = [&](double at, const ParameterState &parameters) {
        const auto mapping = flow(at, parameters);
        auto physical = parameters;
        std::copy(mapping.state.begin(), mapping.state.end(), physical.begin());
        const auto slope = physical_derivative(time + at, physical);
        double clock = 1;
        if constexpr (N == 7)
            if (embedded_time)
                clock = slope[6];
        const auto monopole = central(mapping.state);
        ParameterState result{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double perturbation = slope[axis + 3] - monopole[axis] * clock;
            for (std::size_t i = 0; i < 3; ++i) {
                result[i] = std::fma(-mapping.jacobian[axis][i + 3], perturbation, result[i]);
                result[i + 3] = std::fma(mapping.jacobian[axis][i], perturbation, result[i + 3]);
            }
        }
        if constexpr (N == 7)
            result[6] = slope[6];
        return result;
    };
    auto control = physical_control;
    control.local_increments = true;
    control.mapped_error = [&](const ParameterState &, const ParameterState &parameters,
                               const ParameterState &defect) {
        const auto mapping = flow(h, parameters);
        auto physical = parameters;
        std::copy(mapping.state.begin(), mapping.state.end(), physical.begin());
        ParameterState difference{};
        const auto acceleration = central(mapping.state);
        for (std::size_t i = 0; i < 6; ++i) {
            for (std::size_t j = 0; j < 6; ++j)
                difference[i] = std::fma(mapping.jacobian[i][j], defect[j], difference[i]);
            if constexpr (N == 7)
                if (embedded_time)
                    difference[i] = std::fma(i < 3 ? mapping.state[i + 3] : acceleration[i - 3],
                                             defect[6], difference[i]);
        }
        if constexpr (N == 7)
            difference[6] = defect[6];
        if (!std::ranges::all_of(difference, [](double v) { return std::isfinite(v); }))
            throw PropagationError("VOP produced a nonfinite physical error estimate");
        return physical_control.error(initial, physical, difference);
    };
    auto trial = stepper(derivative, control);
    const auto mapping = flow(h, trial.value);
    for (std::size_t i = 0; i < 6; ++i) {
        trial.value[i] = mapping.state[i];
        trial.increment[i] += mapping.increment[i];
    }
    return trial;
}
} // namespace orbitforge::core::detail
