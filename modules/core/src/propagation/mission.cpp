#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <orbitforge/core/propagation/mission.hpp>
#include <utility>

namespace orbitforge::core {
namespace {
using Vector = std::array<double, 3>;
double norm(const Vector &v) { return std::hypot(v[0], v[1], v[2]); }
Vector cross(const Vector &a, const Vector &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vector unit(Vector v) {
    const auto length = norm(v);
    if (!std::isfinite(length) || length < 1e-14)
        throw PropagationError(
            "Maneuver axes require a nonzero position, velocity and orbit normal");
    for (auto &component : v)
        component /= length;
    return v;
}
void validate_vector(const Vector &v) {
    if (!std::isfinite(norm(v)))
        throw std::invalid_argument("Maneuver vectors must have finite components and magnitude");
}
void validate_frame(ManeuverFrame frame) {
    if (frame != ManeuverFrame::gcrf && frame != ManeuverFrame::vnc && frame != ManeuverFrame::lvlh)
        throw std::invalid_argument("Unsupported maneuver reference axes");
}
class MissionProvider final : public IStateProvider {
  public:
    MissionProvider(CartesianState initial, MissionPlan plan, CoastFactory coast,
                    std::optional<Epoch> stop)
        : first_(initial.epoch), last_(validate_mission(initial.epoch, plan)),
          plan_(std::move(plan)), coast_(std::move(coast)) {
        if (stop) {
            if (stop->tai_nanoseconds() < first_.tai_nanoseconds() ||
                stop->tai_nanoseconds() > last_.tai_nanoseconds())
                throw std::invalid_argument("Mission stop must lie inside its sequence");
            last_ = *stop;
        }
        if (initial.origin != OriginId::earth || initial.frame != FrameId::gcrf)
            throw std::invalid_argument("Mission initial state must be Earth-centered GCRF");
        validate_vector(initial.position_m);
        validate_vector(initial.velocity_mps);
        if (!coast_)
            throw std::invalid_argument("A mission requires a coast provider factory");
        auto epoch = first_.tai_nanoseconds();
        for (std::size_t i = 0; i < plan_.segments.size(); ++i) {
            const auto &segment = plan_.segments[i];
            if (segment.kind == MissionSegmentKind::coast)
                epoch += segment.duration_nanoseconds;
            else if (epoch <= last_.tai_nanoseconds())
                burns_.push_back({Epoch{epoch}, i});
        }
        arcs_.push_back({first_, make_arc(initial, 0, {})});
    }
    CartesianState state_at(Epoch epoch) const override { return state_at(epoch, {}); }
    CartesianState state_at(Epoch epoch, const std::function<void()> &checkpoint) const override {
        check_epoch(epoch);
        if (checkpoint)
            checkpoint();
        std::shared_ptr<const IStateProvider> provider;
        {
            std::lock_guard lock(mutex_);
            prepare(epoch, checkpoint);
            auto arc =
                std::upper_bound(arcs_.begin(), arcs_.end(), epoch, [](Epoch t, const Arc &a) {
                    return t.tai_nanoseconds() < a.first.tai_nanoseconds();
                });
            provider = std::prev(arc)->provider;
        }
        return provider->state_at(epoch, checkpoint);
    }
    std::vector<Epoch> discontinuities() const override { return discontinuities({}); }
    std::vector<Epoch> discontinuities(const std::function<void()> &checkpoint) const override {
        if (checkpoint)
            checkpoint();
        std::lock_guard lock(mutex_);
        prepare(last_, checkpoint);
        std::vector<Epoch> result;
        for (const auto &burn : burns_)
            result.push_back(burn.epoch);
        for (const auto &arc : arcs_) {
            const auto boundaries = arc.provider->discontinuities(checkpoint);
            result.insert(result.end(), boundaries.begin(), boundaries.end());
        }
        std::sort(result.begin(), result.end(),
                  [](Epoch a, Epoch b) { return a.tai_nanoseconds() < b.tai_nanoseconds(); });
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }
    std::vector<ManeuverEvent> maneuvers(Epoch first, Epoch last,
                                         const std::function<void()> &checkpoint) const override {
        check_epoch(first);
        check_epoch(last);
        if (last.tai_nanoseconds() < first.tai_nanoseconds())
            throw std::invalid_argument("Maneuver interval must be ordered");
        if (checkpoint)
            checkpoint();
        std::lock_guard lock(mutex_);
        prepare(last, checkpoint);
        std::vector<ManeuverEvent> result;
        for (const auto &event : events_)
            if (event.before.epoch.tai_nanoseconds() >= first.tai_nanoseconds() &&
                event.before.epoch.tai_nanoseconds() <= last.tai_nanoseconds())
                result.push_back(event);
        return result;
    }

  private:
    struct Burn {
        Epoch epoch;
        std::size_t index;
    };
    struct Arc {
        Epoch first;
        std::shared_ptr<const IStateProvider> provider;
    };
    Epoch first_, last_;
    MissionPlan plan_;
    CoastFactory coast_;
    std::vector<Burn> burns_;
    mutable std::mutex mutex_;
    mutable std::vector<Arc> arcs_;
    mutable std::vector<ManeuverEvent> events_;
    std::shared_ptr<const IStateProvider> make_arc(const CartesianState &state,
                                                   std::size_t next_burn,
                                                   const std::function<void()> &checkpoint) const {
        const Epoch end = next_burn < burns_.size() ? burns_[next_burn].epoch : last_;
        auto result = coast_(state, end, checkpoint);
        if (!result)
            throw std::invalid_argument("Mission coast factory returned a null provider");
        return result;
    }
    void check_epoch(Epoch epoch) const {
        if (epoch.tai_nanoseconds() < first_.tai_nanoseconds() ||
            epoch.tai_nanoseconds() > last_.tai_nanoseconds())
            throw PropagationError(
                "Epoch is outside the mission sequence; extend its coast segments");
    }
    void prepare(Epoch epoch, const std::function<void()> &checkpoint) const {
        // Reserve before publishing either cache entry so allocation failure cannot
        // leave the event and arc lists at different revisions.
        arcs_.reserve(burns_.size() + 1);
        events_.reserve(burns_.size());
        while (events_.size() < burns_.size()) {
            const auto &burn = burns_[events_.size()];
            if (burn.epoch.tai_nanoseconds() > epoch.tai_nanoseconds())
                break;
            if (checkpoint)
                checkpoint();
            const auto &segment = plan_.segments[burn.index];
            const auto before = arcs_.back().provider->state_at(burn.epoch, checkpoint);
            const auto after = apply_impulse(before, segment.delta_v_mps, segment.frame);
            Vector delta{};
            for (std::size_t i = 0; i < 3; ++i)
                delta[i] = after.velocity_mps[i] - before.velocity_mps[i];
            const auto total = (events_.empty() ? 0 : events_.back().cumulative_delta_v_mps) +
                               norm(segment.delta_v_mps);
            if (!std::isfinite(total))
                throw PropagationError("Mission cumulative delta-v is not finite");
            ManeuverEvent event{burn.index, segment.name, before, after, delta, total};
            auto provider = make_arc(after, events_.size() + 1, checkpoint);
            if (checkpoint)
                checkpoint();
            events_.push_back(std::move(event));
            arcs_.push_back({burn.epoch, std::move(provider)});
        }
    }
};
} // namespace
MissionSegment MissionSegment::coast(std::chrono::nanoseconds duration, std::string name) {
    MissionSegment result;
    result.name = std::move(name);
    result.duration_nanoseconds = duration.count();
    return result;
}
MissionSegment MissionSegment::impulse(Vector delta_v, ManeuverFrame frame, std::string name) {
    MissionSegment result;
    result.kind = MissionSegmentKind::impulse;
    result.name = std::move(name);
    result.delta_v_mps = delta_v;
    result.frame = frame;
    return result;
}
Epoch validate_mission(Epoch initial, const MissionPlan &plan) {
    if (plan.coast_model != MissionCoastModel::two_body &&
        plan.coast_model != MissionCoastModel::hpop)
        throw std::invalid_argument("Unsupported mission coast model");
    if (plan.segments.empty() || plan.segments.size() > 256)
        throw std::invalid_argument("A mission requires 1..256 segments");
    constexpr std::int64_t max_duration = 366LL * 86400 * 1'000'000'000;
    std::int64_t duration = 0;
    double total_delta_v = 0;
    for (const auto &segment : plan.segments) {
        if (segment.kind == MissionSegmentKind::coast) {
            if (segment.duration_nanoseconds <= 0 ||
                segment.duration_nanoseconds > max_duration - duration)
                throw std::invalid_argument(
                    "Coast durations must be positive; mission limit is 366 days");
            duration += segment.duration_nanoseconds;
        } else if (segment.kind == MissionSegmentKind::impulse) {
            validate_vector(segment.delta_v_mps);
            validate_frame(segment.frame);
            total_delta_v += norm(segment.delta_v_mps);
            if (!std::isfinite(total_delta_v))
                throw std::invalid_argument("Mission cumulative delta-v must be finite");
        } else {
            throw std::invalid_argument("Unsupported mission segment kind");
        }
    }
    if (duration == 0)
        throw std::invalid_argument("A mission requires at least one positive coast segment");
    if (initial.tai_nanoseconds() > std::numeric_limits<std::int64_t>::max() - duration)
        throw std::invalid_argument("Mission end epoch overflows int64 TAI nanoseconds");
    return Epoch{initial.tai_nanoseconds() + duration};
}
CartesianState apply_impulse(const CartesianState &before, Vector delta_v, ManeuverFrame frame) {
    if (before.origin != OriginId::earth || before.frame != FrameId::gcrf)
        throw std::invalid_argument("Maneuver state must be Earth-centered GCRF");
    validate_vector(before.position_m);
    validate_vector(before.velocity_mps);
    validate_vector(delta_v);
    validate_frame(frame);
    if (frame != ManeuverFrame::gcrf) {
        const auto radial = unit(before.position_m), velocity = unit(before.velocity_mps);
        const auto normal = unit(cross(radial, velocity));
        const auto x = frame == ManeuverFrame::vnc ? velocity : radial;
        const auto y = frame == ManeuverFrame::vnc ? normal : cross(normal, radial);
        const auto z = frame == ManeuverFrame::vnc ? cross(velocity, normal) : normal;
        Vector transformed{};
        for (std::size_t i = 0; i < 3; ++i)
            transformed[i] = delta_v[0] * x[i] + delta_v[1] * y[i] + delta_v[2] * z[i];
        delta_v = transformed;
    }
    auto result = before;
    for (std::size_t i = 0; i < 3; ++i)
        result.velocity_mps[i] += delta_v[i];
    validate_vector(result.velocity_mps);
    return result;
}
std::shared_ptr<const IStateProvider> make_mission(CartesianState initial, MissionPlan plan,
                                                   CoastFactory coast, std::optional<Epoch> stop) {
    return std::make_shared<MissionProvider>(initial, std::move(plan), std::move(coast), stop);
}
} // namespace orbitforge::core
