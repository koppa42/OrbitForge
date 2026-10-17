#include "universal_kepler.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <orbitforge/core/propagation/tabulated_orbit.hpp>
#include <span>
#include <utility>

namespace orbitforge::core {
void validate_orbit_interpolation(const OrbitInterpolationOptions &options) {
    if (options.method != OrbitInterpolationMethod::lagrange &&
        options.method != OrbitInterpolationMethod::hermite &&
        options.method != OrbitInterpolationMethod::vop)
        throw std::invalid_argument("Unknown orbit interpolation method");
    if (options.order < 0 || options.order > 29 ||
        (options.method == OrbitInterpolationMethod::hermite &&
         (options.order < 3 || options.order % 2 != 1)))
        throw std::invalid_argument(
            "Lagrange/VOP order must be 0..29; Hermite order must be odd 3..29");
    if (!std::isfinite(options.vop_mu) || options.vop_mu <= 0)
        throw std::invalid_argument("VOP interpolation GM must be finite and positive");
}
namespace {
std::int64_t ticks(const CartesianState &state) { return state.epoch.tai_nanoseconds(); }
bool valid_frame(FrameId frame) {
    return frame == FrameId::gcrf || frame == FrameId::itrf || frame == FrameId::spice_j2000 ||
           frame == FrameId::teme;
}
bool finite(const CartesianState &state) {
    for (std::size_t i = 0; i < 3; ++i)
        if (!std::isfinite(state.position_m[i]) || !std::isfinite(state.velocity_mps[i]))
            return false;
    return true;
}
// Neumaier summation; window sizes are bounded by 30, including at the ends of an arc.
struct Sum {
    double sum = 0, correction = 0;
    void add(double value) {
        const double next = sum + value;
        correction +=
            std::abs(sum) >= std::abs(value) ? (sum - next) + value : (value - next) + sum;
        sum = next;
    }
    double value() const { return sum + correction; }
};
class TabulatedOrbit final : public IStateProvider {
  public:
    TabulatedOrbit(std::vector<OrbitEphemerisArc> arcs, OrbitInterpolationOptions options,
                   const std::function<void()> &checkpoint)
        : arcs_(std::move(arcs)), options_(options) {
        validate_orbit_interpolation(options_);
        count_ = static_cast<std::size_t>(options_.method == OrbitInterpolationMethod::hermite
                                              ? (options_.order + 1) / 2
                                              : options_.order + 1);
        if (arcs_.empty() || arcs_.front().empty())
            throw std::invalid_argument("An orbit ephemeris needs nonempty arcs");
        const auto &first = arcs_.front().front();
        if (!valid_frame(first.frame) || first.origin != OriginId::earth)
            throw std::invalid_argument("Unsupported ephemeris origin/frame");
        if (options_.method == OrbitInterpolationMethod::vop && first.frame != FrameId::gcrf &&
            first.frame != FrameId::spice_j2000)
            throw std::invalid_argument("VOP interpolation requires GCRF or SPICE J2000 axes");
        for (std::size_t a = 0; a < arcs_.size(); ++a) {
            const auto &arc = arcs_[a];
            if (checkpoint)
                checkpoint();
            if (arc.size() < count_)
                throw std::invalid_argument(
                    "Ephemeris arc has too few nodes for interpolation order");
            if (a && (ticks(arc.front()) < ticks(arcs_[a - 1].back()) ||
                      ticks(arc.front()) <= ticks(arcs_[a - 1].front())))
                throw std::invalid_argument("Ephemeris arcs must be ordered without overlap");
            if (a) {
                boundaries_.push_back(arcs_[a - 1].back().epoch);
                boundaries_.push_back(arc.front().epoch);
            }
            for (std::size_t i = 0; i < arc.size(); ++i) {
                if (checkpoint && i % 1024 == 0)
                    checkpoint();
                if (!finite(arc[i]) || arc[i].frame != first.frame || arc[i].origin != first.origin)
                    throw std::invalid_argument(
                        "Ephemeris nodes require finite states in one origin/frame");
                if (options_.method == OrbitInterpolationMethod::vop)
                    (void)transport(arc[i], arc[i].epoch, checkpoint);
                if (i && ticks(arc[i]) <= ticks(arc[i - 1]))
                    throw std::invalid_argument(
                        "Ephemeris nodes must have strictly increasing epochs");
                if (i && options_.order == 0)
                    boundaries_.push_back(arc[i].epoch);
            }
        }
        std::sort(boundaries_.begin(), boundaries_.end(),
                  [](Epoch a, Epoch b) { return a.tai_nanoseconds() < b.tai_nanoseconds(); });
        boundaries_.erase(std::unique(boundaries_.begin(), boundaries_.end()), boundaries_.end());
    }
    std::vector<Epoch> discontinuities() const override { return boundaries_; }
    CartesianState state_at(Epoch epoch) const override { return state_at(epoch, {}); }
    CartesianState state_at(Epoch epoch, const std::function<void()> &checkpoint) const override {
        if (checkpoint)
            checkpoint();
        const auto t = epoch.tai_nanoseconds();
        auto a = std::upper_bound(arcs_.begin(), arcs_.end(), t, [](auto value, const auto &arc) {
            return value < ticks(arc.front());
        });
        if (a == arcs_.begin())
            throw PropagationError("Epoch is outside the tabulated orbit");
        const auto &arc = *std::prev(a);
        if (t > ticks(arc.back()))
            throw PropagationError("Epoch is in an ephemeris gap or outside the tabulated orbit");
        const auto right = std::lower_bound(
            arc.begin(), arc.end(), t, [](const auto &s, auto value) { return ticks(s) < value; });
        if (right != arc.end() && ticks(*right) == t)
            return *right;
        if (options_.order == 0) {
            if (options_.method == OrbitInterpolationMethod::vop)
                return transport(*std::prev(right), epoch, checkpoint);
            auto held = *std::prev(right);
            held.epoch = epoch;
            return held;
        }
        // Keep both bracketing nodes even with very uneven sampling. Selecting only the
        // nearest nodes could put the query outside the polynomial's node interval.
        const auto split = static_cast<std::size_t>(right - arc.begin());
        std::size_t lo = split - 1, hi = split + 1;
        for (std::size_t i = 2; i < count_; ++i) {
            if (lo && (hi == arc.size() || epoch.seconds_since(arc[lo - 1].epoch) <=
                                               arc[hi].epoch.seconds_since(epoch)))
                --lo;
            else
                ++hi;
        }
        const auto window = std::span(arc).subspan(lo, count_);
        const double scale = window.back().epoch.seconds_since(window.front().epoch);
        std::array<double, 30> x{};
        std::size_t anchor = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            x[i] = window[i].epoch.seconds_since(epoch) / scale;
            if (!std::isfinite(x[i]) || x[i] == 0 || (i && x[i] <= x[i - 1]))
                throw PropagationError("Ephemeris window cannot resolve distinct epochs");
            if (std::abs(x[i]) < std::abs(x[anchor]))
                anchor = i;
        }
        auto result = window[anchor];
        if (options_.method == OrbitInterpolationMethod::vop) {
            // The query epoch is the common reference for the osculating parameters.
            // Use original node times for the polynomial; only the states are transported.
            OrbitEphemerisArc parameters;
            parameters.reserve(count_);
            for (std::size_t i = 0; i < count_; ++i)
                parameters.push_back(transport(window[i], epoch, checkpoint));
            result = lagrange(parameters, x, anchor, epoch);
        } else {
            result = options_.method == OrbitInterpolationMethod::lagrange
                         ? lagrange(window, x, anchor, epoch)
                         : hermite(window, x, anchor, scale, epoch);
        }
        if (!finite(result))
            throw PropagationError("Ephemeris interpolation produced a nonfinite state");
        if (checkpoint)
            checkpoint();
        return result;
    }

  private:
    CartesianState transport(const CartesianState &node, Epoch epoch,
                             const std::function<void()> &checkpoint) const {
        const auto &r = node.position_m;
        const auto &v = node.velocity_mps;
        const auto state =
            detail::universal_kepler({r[0], r[1], r[2], v[0], v[1], v[2]},
                                     epoch.seconds_since(node.epoch), options_.vop_mu, checkpoint);
        return {epoch,
                node.origin,
                node.frame,
                {state[0], state[1], state[2]},
                {state[3], state[4], state[5]}};
    }
    CartesianState lagrange(std::span<const CartesianState> nodes, const std::array<double, 30> &x,
                            std::size_t anchor, Epoch epoch) const {
        std::array<double, 30> weights{};
        double largest = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            double product = -x[i];
            for (std::size_t j = 0; j < count_; ++j)
                if (i != j)
                    product *= x[i] - x[j];
            weights[i] = 1 / product;
            largest = std::max(largest, std::abs(weights[i]));
        }
        if (!(largest > 0) || !std::isfinite(largest))
            throw PropagationError("Ephemeris Lagrange window is numerically unresolved");
        Sum denominator, absolute;
        std::array<Sum, 6> numerator{};
        for (std::size_t i = 0; i < count_; ++i) {
            const double w = weights[i] / largest;
            denominator.add(w);
            absolute.add(std::abs(w));
            for (std::size_t axis = 0; axis < 3; ++axis) {
                numerator[axis].add(w *
                                    (nodes[i].position_m[axis] - nodes[anchor].position_m[axis]));
                numerator[axis + 3].add(
                    w * (nodes[i].velocity_mps[axis] - nodes[anchor].velocity_mps[axis]));
            }
        }
        const double total = denominator.value();
        if (std::abs(total) <= 64 * std::numeric_limits<double>::epsilon() * absolute.value())
            throw PropagationError("Ephemeris Lagrange weights are ill-conditioned");
        auto result = nodes[anchor];
        result.epoch = epoch;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            result.position_m[axis] += numerator[axis].value() / total;
            result.velocity_mps[axis] += numerator[axis + 3].value() / total;
        }
        return result;
    }
    CartesianState hermite(std::span<const CartesianState> nodes, const std::array<double, 30> &x,
                           std::size_t anchor, double scale, Epoch epoch) const {
        // Confluent Newton differences of the residual about a local straight line.
        // Removing that line avoids subtracting large absolute orbital coordinates repeatedly.
        std::array<double, 30> z{};
        for (std::size_t i = 0; i < count_; ++i)
            z[2 * i] = z[2 * i + 1] = x[i];
        auto result = nodes[anchor];
        result.epoch = epoch;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            std::array<double, 30> q{};
            const double r0 = nodes[anchor].position_m[axis], v0 = nodes[anchor].velocity_mps[axis];
            for (std::size_t i = 0; i < count_; ++i)
                q[2 * i] = q[2 * i + 1] = (nodes[i].position_m[axis] - r0) -
                                          v0 * nodes[i].epoch.seconds_since(nodes[anchor].epoch);
            const auto size = 2 * count_;
            for (std::size_t level = 1; level < size; ++level)
                for (std::size_t i = size - 1; i >= level; --i)
                    q[i] = level == 1 && i % 2 == 1 ? (nodes[i / 2].velocity_mps[axis] - v0) * scale
                                                    : (q[i] - q[i - 1]) / (z[i] - z[i - level]);
            double value = q[size - 1], derivative = 0;
            for (std::size_t i = size - 1; i-- > 0;) {
                derivative = derivative * (-z[i]) + value;
                value = value * (-z[i]) + q[i];
            }
            result.position_m[axis] = r0 + v0 * epoch.seconds_since(nodes[anchor].epoch) + value;
            result.velocity_mps[axis] = v0 + derivative / scale;
        }
        return result;
    }
    std::vector<OrbitEphemerisArc> arcs_;
    OrbitInterpolationOptions options_;
    std::size_t count_{};
    std::vector<Epoch> boundaries_;
};
} // namespace
std::shared_ptr<const IStateProvider>
make_tabulated_orbit(std::vector<OrbitEphemerisArc> arcs, OrbitInterpolationOptions options,
                     const std::function<void()> &checkpoint) {
    return std::make_shared<TabulatedOrbit>(std::move(arcs), options, checkpoint);
}
} // namespace orbitforge::core
