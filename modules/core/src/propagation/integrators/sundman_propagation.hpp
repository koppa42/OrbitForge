#pragma once
#include "gauss_jackson.hpp"
#include "one_step.hpp"
#include "sundman.hpp"
#include "vop.hpp"
#include <orbitforge/core/propagation/hpop.hpp>

namespace orbitforge::core::detail {
// Bounded physical-time adapter for the Sundman kernels. A rejected transformed
// stage never calls the force outside the coast/force segment. At a boundary we
// integrate the equivalent inverse equations (r,v,q) in physical time, using the
// selected method (the high-order starter for GJ). No clock/state projection is used.
class SundmanPropagation {
  public:
    using Cartesian = SundmanTransform::Cartesian;
    using Vector = SundmanTransform::FirstOrder;
    using History = GaussJacksonSystem12<3, 1>;
    struct State {
        CompensatedState<7> solution;
        History history;
        Vector startup_compensation{};
        double q = 0, next_step = 0, segment_begin = 0;
        std::size_t grid = 0, regular_steps = 0, boundary_steps = 0, multisteps = 0;
        int direction = 0;
        bool on_grid = true;
        double time() const { return solution.value[6]; }
        Cartesian cartesian() const { return SundmanTransform::cartesian_first(solution.value); }
        void restart_force_segment() {
            history.reset();
            segment_begin = time();
        }
    };
    // Return an integrated prefix to stop at a state-dependent force boundary.
    // The callback must not publish the provisional endpoint. It is also applied
    // to physical-time bridges, so no path can step over a shadow contact.
    using StepBoundary = std::function<std::optional<State>(const State &, const State &)>;
    SundmanPropagation(const Cartesian &initial, double mu, double exponent,
                       std::size_t steps_per_orbit, const HpopOptions &options)
        : transform_(initial, mu, exponent), options_(options), mu_(mu) {
        if (steps_per_orbit == 0)
            throw std::invalid_argument("Sundman steps per orbit must be positive");
        validate_hpop_options(options_);
        fixed_q_ = transform_.period() / static_cast<double>(steps_per_orbit);
        if (!std::isfinite(fixed_q_) || fixed_q_ <= 0)
            throw std::invalid_argument("Sundman step is not representable");
        initial_ = transform_.first_order(initial, 0);
    }
    State initial_state() const {
        State value;
        value.solution.value = initial_;
        value.next_step = fixed_q_;
        return value;
    }
    double nominal_step() const { return fixed_q_; }

    // The caller retains the returned state between reporting gates and resets
    // history only at actual force discontinuities. Work is counted across gates.
    // Input is copied: exceptions/cancellation never publish a partial history.
    template <class Force>
    State propagate(State state, double end, const Force &force, std::size_t &attempts,
                    const std::function<void()> &checkpoint = {},
                    const std::function<void(double, const Cartesian &)> &accepted = {},
                    const StepBoundary &step_boundary = {},
                    const std::function<bool(double, const Cartesian &)> &correction = {}) const {
        if (!std::isfinite(end))
            throw std::invalid_argument("Sundman endpoint must be finite");
        if (state.time() == end)
            return state;
        const int direction = end > state.time() ? 1 : -1;
        if (state.direction && state.direction != direction)
            throw std::invalid_argument("Sundman history cannot reverse propagation direction");
        state.direction = direction;
        const double low = std::min(state.segment_begin, end);
        const double high = std::max(state.segment_begin, end);
        const auto checked_force = [&](double time, const Cartesian &s) {
            if (!std::isfinite(time))
                throw PropagationError("Sundman physical time is not finite");
            if (time < low || time > high)
                throw OutsideInterval{};
            if (checkpoint)
                checkpoint();
            return force(time, s);
        };
        while (state.time() != end) {
            check_work(attempts, checkpoint);
            const bool fixed = options_.uses_fixed_step();
            const double g = transform_.clock_rate(state.solution.value);
            const bool at_minimum = !fixed && state.next_step <= options_.min_step_seconds / g;
            const double target =
                fixed ? direction * static_cast<double>(state.grid + 1) * fixed_q_
                      : state.q + direction * std::clamp(state.next_step,
                                                         options_.min_step_seconds / g,
                                                         options_.max_step_seconds / g);
            // Uniform GJ differences require the identical floating step every time.
            // Subtracting adjacent rounded q grid coordinates would vary it by ulps.
            const double h = fixed && state.on_grid ? direction * fixed_q_ : target - state.q;
            if (!std::isfinite(h) || direction * h <= 0 || state.q + h == state.q)
                throw PropagationError("Sundman step cannot advance its independent variable");
            bool boundary = false;
            const bool accurate = correction && correction(state.time(), state.cartesian());
            try {
                State candidate = state;
                double error = 0, exponent = 0;
                if (options_.integrator == HpopIntegrator::gauss_jackson && !accurate) {
                    advance_gj(candidate, h, checked_force, attempts, checkpoint);
                } else {
                    const double begin = state.time();
                    auto local = state.solution.value;
                    local[6] = 0;
                    const auto derivative = [&](double, const Vector &s) {
                        return transform_.derivative(s, [&](double offset, const Cartesian &v) {
                            return checked_force(begin + offset, v);
                        });
                    };
                    auto control = accurate ? correction_control() : primary_control(fixed);
                    control.max_step = options_.max_step_seconds / g;
                    const auto trial = step(0, local, h, derivative, control, checkpoint, accurate);
                    if (accurate && trial.error > 1)
                        throw OutsideInterval{};
                    if (accurate)
                        candidate.history.reset();
                    error = trial.error;
                    exponent = trial.exponent;
                    candidate.solution.advance(trial.increment);
                }
                validate(candidate.solution.value);
                if (candidate.time() < low || candidate.time() > high)
                    throw OutsideInterval{};
                const double elapsed = direction * (candidate.time() - state.time());
                if (!(elapsed > 0))
                    throw OutsideInterval{}; // Resolve a sub-ulp clock interval in physical time.
                const bool enforce_minimum =
                    elapsed < options_.min_step_seconds || (at_minimum && error > 1);
                if (!fixed && (enforce_minimum || elapsed > options_.max_step_seconds)) {
                    // g changes during a step, so converting a physical bound using
                    // only its initial value does not enforce that bound. Integrate
                    // the equivalent inverse ODE to the binding physical time.
                    const double remaining = std::abs(end - state.time());
                    const double span =
                        std::min(remaining, enforce_minimum ? options_.min_step_seconds
                                                            : options_.max_step_seconds);
                    const double stop = span == remaining ? end : state.time() + direction * span;
                    auto prefix = bridge(state, stop, span, std::numeric_limits<double>::infinity(),
                                         checked_force, attempts, checkpoint,
                                         std::min(span, options_.min_step_seconds), accurate);
                    if (step_boundary)
                        if (auto contact = step_boundary(state, prefix))
                            return *contact;
                    state = std::move(prefix);
                    ++state.regular_steps;
                    if (accepted)
                        accepted(state.time(), state.cartesian());
                    continue;
                }
                if (!fixed && error > 1) {
                    if (elapsed <= options_.min_step_seconds)
                        throw PropagationError("Sundman cannot meet tolerance at the minimum step");
                    state.next_step = std::abs(h) * growth(error, exponent);
                    continue;
                }
                candidate.q = target;
                candidate.on_grid = fixed;
                if (fixed)
                    ++candidate.grid;
                else
                    candidate.next_step = std::min(fixed_q_, std::abs(h) * growth(error, exponent));
                ++candidate.regular_steps;
                if (step_boundary)
                    if (auto contact = step_boundary(state, candidate))
                        return *contact;
                state = std::move(candidate);
            } catch (const OutsideInterval &) {
                boundary = true;
            }
            if (boundary) {
                // A short inverse-equation prefix also handles RK stages at c=0
                // whose augmented clock lies just before the initial force segment.
                // The q grid is retained; this bridge never crosses its next node.
                const double span = std::min(std::abs(end - state.time()), .25 * g * std::abs(h));
                const double proposed_step = state.next_step;
                auto prefix = bridge(state, end, span, std::abs(h), checked_force, attempts,
                                     checkpoint, 0, accurate);
                if (step_boundary)
                    if (auto contact = step_boundary(state, prefix))
                        return *contact;
                state = std::move(prefix);
                // This prefix was shortened to fit a physical gate, not selected
                // as the next regularized error-control step. Retain that proposal;
                // the next full trial still checks its error and physical bounds.
                state.next_step = proposed_step;
            }
            if (accepted && (!boundary || state.time() == end))
                accepted(state.time(), state.cartesian());
        }
        return state;
    }

    // A state-dependent event lies inside the current accepted q step. Integrate
    // its physical-time prefix directly instead of repeatedly colliding with the
    // same q-grid endpoint. This integrates (r,v,q), preserving the real clock
    // relation; it does not interpolate or project the candidate state.
    template <class Force>
    State boundary_prefix(State state, double end, const Force &force, std::size_t &attempts,
                          const std::function<void()> &checkpoint = {},
                          bool accurate = false) const {
        while (state.time() != end)
            state = bridge(state, end, std::abs(end - state.time()),
                           std::numeric_limits<double>::infinity(), force, attempts, checkpoint, 0,
                           accurate);
        if (options_.uses_fixed_step()) {
            // Roundoff in inverse integration may cross the old q endpoint by an
            // ulp. Retain the integrated q and choose the next untouched grid cell.
            while (std::abs(state.q) >= static_cast<double>(state.grid + 1) * fixed_q_)
                ++state.grid;
        }
        return state;
    }

  private:
    struct OutsideInterval {};
    SundmanTransform transform_;
    HpopOptions options_;
    double fixed_q_, mu_;
    Vector initial_;
    static void validate(const Vector &value) {
        if (!std::all_of(value.begin(), value.end(), [](double v) { return std::isfinite(v); }))
            throw PropagationError("Sundman produced a nonfinite state");
    }
    static void check_work(std::size_t &attempts, const std::function<void()> &checkpoint) {
        if (checkpoint)
            checkpoint();
        if (++attempts > 200000)
            throw PropagationError("HPOP exceeded 200000 integration attempts in one query");
    }
    static double growth(double error, double exponent) {
        if (!std::isfinite(error))
            throw PropagationError("Sundman produced a nonfinite error estimate");
        return error == 0 ? 4 : std::clamp(.9 * std::pow(error, exponent), .1, 4.);
    }
    ErrorControl<7> primary_control(bool fixed) const {
        ErrorControl<7> control;
        control.fixed = fixed;
        control.local_increments = true;
        control.relative = options_.relative_tolerance;
        for (std::size_t i = 0; i < 6; ++i)
            control.absolute[i] =
                i < 3 ? options_.position_tolerance_m : options_.velocity_tolerance_mps;
        // The one-step clock is rebased to zero; tolerance cannot grow with the
        // elapsed coast duration. The physical-time adapter has no clock state.
        control.absolute[6] = 1e-11;
        return control;
    }
    ErrorControl<7> correction_control() const {
        auto control = primary_control(false);
        control.relative = 1e-15;
        control.absolute = {1e-10, 1e-10, 1e-10, 1e-13, 1e-13, 1e-13, 1e-12};
        return control;
    }
    template <class Derivative>
    TrialStep<7> step(double t, const Vector &s, double h, const Derivative &derivative,
                      const ErrorControl<7> &control, const std::function<void()> &checkpoint,
                      bool starter = false, bool embedded_time = true) const {
        const auto integrate = [&](double start, const auto &slope, const auto &scales) {
            if (starter || options_.integrator == HpopIntegrator::bulirsch_stoer)
                return bulirsch_stoer(start, s, h, slope, scales, checkpoint, starter);
            switch (options_.integrator) {
            case HpopIntegrator::rk4:
                return rk4_trial(start, s, h, slope);
            case HpopIntegrator::rkf78:
                return rkf78(start, s, h, slope, scales);
            case HpopIntegrator::rkv89:
                return rkv89(start, s, h, slope, scales);
            default:
                throw std::logic_error("Invalid Sundman one-step method");
            }
        };
        if (options_.uses_vop())
            return vop_step(
                t, s, h, mu_, embedded_time, derivative,
                [&](const auto &slope, const auto &scales) { return integrate(0, slope, scales); },
                control, checkpoint);
        return integrate(t, derivative, control);
    }
    template <class Force>
    void advance_gj(State &state, double h, const Force &force, std::size_t &attempts,
                    const std::function<void()> &checkpoint) const {
        auto &history = state.history;
        const double begin = state.time();
        // Keep the first-order clock near zero even on multiweek trajectories.
        // Translate its compensated sum, leaving all derivative history unchanged.
        // The global physical clock is accumulated separately with compensation.
        if (history.count) {
            if (history.count == history.history.size()) {
                History::Acceleration shift{0, 0, 0, -history.state[6]};
                History::add(history.sum1, history.compensation1, shift);
            }
            history.state[6] = 0;
        } else {
            history.state = transform_.second_order(state.cartesian(), 0);
            state.startup_compensation = {};
            const double g = transform_.clock_rate(state.solution.value);
            for (std::size_t i = 0; i < 3; ++i) {
                state.startup_compensation[i] = state.solution.compensation[i];
                // Two-product residual retains the physical velocity through v*g.
                const double residual =
                    std::fma(state.solution.value[i + 3], g, -history.state[i + 3]) -
                    state.solution.compensation[i + 3] * g;
                state.startup_compensation[i + 3] = -residual;
            }
        }
        const auto acceleration = [&](const Vector &s) {
            return transform_.acceleration(
                s, [&](double offset, const Cartesian &v) { return force(begin + offset, v); });
        };
        if (history.count == 0 && state.on_grid)
            history.seed(history.state, acceleration(history.state), h);
        if (history.count < history.history.size() || !state.on_grid) {
            // Continue the existing r, dr/dq state through startup. Reconstructing
            // dr/dq from (dr/dq)/g and multiplying by g at every seed loses bits.
            CompensatedState<7> accumulated{history.state, state.startup_compensation};
            accumulated.compensation[6] = 0;
            const auto derivative = [&](double, const Vector &s) {
                const auto a = acceleration(s);
                return Vector{s[3], s[4], s[5], a[0], a[1], a[2], a[3]};
            };
            auto control = primary_control(false);
            control.relative = 1e-14;
            const double g = transform_.clock_rate(accumulated.value);
            control.absolute = {1e-9, 1e-9, 1e-9, 1e-12 * g, 1e-12 * g, 1e-12 * g, 1e-11};
            double at = 0, magnitude = std::abs(h);
            while (at != h) {
                check_work(attempts, checkpoint);
                const double remaining = std::abs(h - at);
                magnitude = std::min(magnitude, remaining);
                const double delta = std::copysign(magnitude, h);
                if (at + delta == at)
                    throw PropagationError("Sundman starter cannot advance q");
                const auto trial =
                    step(at, accumulated.value, delta, derivative, control, checkpoint, true);
                validate(trial.value);
                if (trial.error <= 1) {
                    accumulated.advance(trial.increment);
                    at = magnitude == remaining ? h : at + delta;
                } else if (magnitude * g <= 1e-9) {
                    throw PropagationError(
                        "Sundman cannot initialize Gauss-Jackson at nanosecond resolution");
                }
                magnitude *= growth(trial.error, trial.exponent);
            }
            const auto next = accumulated.value;
            state.startup_compensation = accumulated.compensation;
            if (!state.on_grid)
                history.reset();
            history.seed(next, acceleration(next), std::copysign(fixed_q_, h),
                         state.startup_compensation);
        } else {
            history.advance(
                h, options_.correction == HpopCorrection::full, acceleration,
                [&](const Vector &before, const Vector &after, const auto &a) {
                    return transform_.pseudo_correction(before, after, a);
                },
                [](const Vector &a, const Vector &b) {
                    constexpr double epsilon = 8 * std::numeric_limits<double>::epsilon();
                    for (std::size_t i = 0; i < 3; ++i) {
                        if (std::abs(a[i] - b[i]) >
                                epsilon * std::max(1., std::hypot(b[0], b[1], b[2])) ||
                            std::abs(a[i + 3] - b[i + 3]) >
                                epsilon * std::max(1., std::hypot(b[3], b[4], b[5])))
                            return false;
                    }
                    return std::abs(a[6] - b[6]) <= epsilon * std::max(1., std::abs(b[6]));
                });
            ++state.multisteps;
        }
        Vector clock{};
        clock[6] = history.state[6];
        state.solution.advance(clock);
        const auto cartesian = transform_.cartesian_second(history.state);
        const double g = transform_.clock_rate(history.state);
        for (std::size_t i = 0; i < 6; ++i) {
            state.solution.value[i] = cartesian[i];
            // Preserve the division residual if a physical-time bridge follows.
            state.solution.compensation[i] =
                i < 3 ? 0 : std::fma(cartesian[i], g, -history.state[i]) / g;
        }
    }
    template <class Force>
    State bridge(State state, double end, double magnitude, double q_remaining, const Force &force,
                 std::size_t &attempts, const std::function<void()> &checkpoint, double minimum = 0,
                 bool accurate = false) const {
        const double begin = state.time(), remaining = std::abs(end - begin);
        const bool starter = accurate || options_.integrator == HpopIntegrator::gauss_jackson;
        auto control = primary_control(!starter && options_.uses_fixed_step());
        if (starter) {
            control.relative = 1e-14;
            control.absolute = {1e-9, 1e-9, 1e-9, 1e-12, 1e-12, 1e-12, 1e-11};
        }
        if (accurate)
            control = correction_control();
        control.absolute[6] /= transform_.clock_rate(state.solution.value);
        control.max_step = options_.max_step_seconds;
        auto local = state.solution.value;
        local[6] = 0; // q increment, not physical time
        for (;;) {
            check_work(attempts, checkpoint);
            const double h = std::copysign(magnitude, end - begin);
            const double finish = magnitude == remaining ? end : begin + h;
            if (finish == begin || magnitude <= 0)
                throw PropagationError("Sundman boundary step cannot advance physical time");
            const auto derivative = [&](double offset, const Vector &s) {
                // Known one-step abscissae are in [0,h]; this clamp only removes
                // floating addition roundoff in converting that interval to time.
                const double time = offset == h
                                        ? finish
                                        : std::clamp(begin + offset, std::min(begin, finish),
                                                     std::max(begin, finish));
                const auto a = force(time, SundmanTransform::cartesian_first(s));
                return Vector{s[3], s[4], s[5], a[0], a[1], a[2], 1 / transform_.clock_rate(s)};
            };
            const auto trial = step(0, local, h, derivative, control, checkpoint, starter, false);
            validate(trial.value);
            const double increment = trial.increment[6];
            if (trial.error <= 1 && increment * state.direction > 0 &&
                std::abs(increment) < q_remaining) {
                auto change = trial.increment;
                change[6] = h;
                state.solution.advance(change);
                // Physical time is this bridge's independent variable, not an
                // integrated clock. Assignment retains the exact requested gate.
                state.solution.value[6] = finish;
                state.solution.compensation[6] = 0;
                state.q += increment;
                if (!options_.uses_fixed_step())
                    state.next_step = std::min(fixed_q_, std::abs(increment) *
                                                             growth(trial.error, trial.exponent));
                state.on_grid = false;
                state.history.reset();
                ++state.boundary_steps;
                return state;
            }
            const double floor =
                minimum > 0 ? minimum : (starter ? 1e-9 : options_.min_step_seconds);
            if (trial.error > 1 && magnitude <= floor)
                throw PropagationError(
                    "Sundman boundary cannot meet tolerance at the minimum step");
            magnitude *= trial.error > 1 ? std::min(.5, growth(trial.error, trial.exponent)) : .5;
            if (minimum > 0)
                magnitude = std::max(minimum, magnitude);
        }
    }
};
} // namespace orbitforge::core::detail
