#include "orbit_scene.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <orbitforge/runtime/j2.hpp>
#include <orbitforge/runtime/project.hpp>

namespace orbitforge::desktop {
application::Session make_orbit_session(const OrbitInput &input, std::uint64_t revision,
                                        const core::ITimeProvider *time,
                                        core::HpopEnvironment environment) {
    return runtime::make_orbit_session(input, revision, time, std::move(environment));
}
std::shared_ptr<const OrbitScene>
prepare_orbit_scene(OrbitInput input, application::Session session,
                    const core::IFrameProvider &frames, const core::IEarthOrientationProvider &eop,
                    const std::atomic_bool &cancelled,
                    std::optional<std::pair<core::Epoch, core::Epoch>> cache_interval) {
    const auto started = std::chrono::steady_clock::now();
    const auto check_cancel = [&] {
        if (cancelled.load())
            throw OrbitCancelled{};
    };
    check_cancel();
    const auto &info = eop.data_info();
    const auto analysis_first = input.first.tai_nanoseconds(),
               analysis_last = input.last.tai_nanoseconds();
    const auto first = cache_interval ? cache_interval->first.tai_nanoseconds() : analysis_first;
    const auto last = cache_interval ? cache_interval->second.tai_nanoseconds() : analysis_last;
    if (last <= first)
        throw std::invalid_argument("Analysis end must be later than its start");
    if (!core::eop_covers(info, input.first) || !core::eop_covers(info, input.last))
        throw std::out_of_range("Analysis interval is outside the installed EOP coverage");
    if (first < analysis_first || last > analysis_last)
        throw std::out_of_range("Display cache must lie inside the analysis interval");
    if (input.step_seconds < 1 || input.step_seconds > 60)
        throw std::invalid_argument("Display step must be between 1 and 60 seconds");
    const auto step = static_cast<std::int64_t>(input.step_seconds) * 1'000'000'000;
    if (static_cast<double>(last - first) * 1e-9 > input.step_seconds * 10000.0)
        throw std::invalid_argument("Display cache exceeds 10001 nodes; shorten the interval");
    const auto sample = [&](core::Epoch epoch) {
        check_cancel();
        const auto result = session.sample(std::array{epoch}, check_cancel);
        return result.states().front();
    };
    std::vector<core::CartesianState> states;
    std::vector<core::CartesianState> inertial_states;
    double min_radius = std::numeric_limits<double>::infinity(), max_radius = 0;
    std::vector<std::int64_t> epochs;
    for (auto ns = first;;) {
        epochs.push_back(ns);
        if (ns == last)
            break;
        ns += std::min(step, last - ns);
    }
    // Adjacent TAI nanoseconds straddle an impulse: no representable query can
    // interpolate across the velocity discontinuity. Both smooth arcs retain
    // their own endpoint tangents, including for a burn off the display grid.
    for (const auto epoch : session.discontinuities(check_cancel)) {
        const auto ns = epoch.tai_nanoseconds();
        if (ns >= first && ns <= last) {
            if (ns > first)
                epochs.push_back(ns - 1);
            epochs.push_back(ns);
        }
    }
    std::ranges::sort(epochs);
    epochs.erase(std::unique(epochs.begin(), epochs.end()), epochs.end());
    if (epochs.size() > 10001)
        throw std::invalid_argument("Display cache including maneuvers exceeds 10001 nodes");
    for (const auto ns : epochs) {
        auto native = sample(core::Epoch{ns});
        if (input.propagator == OrbitPropagator::sgp4 && analysis_last - analysis_first >= 4) {
            // The conventional SGP4 velocity is not exactly the derivative of its
            // approximate position solution. Preserve it in Session/reports, but use
            // position tangents for the display-only Hermite cache. All stencil queries
            // remain inside the analysis interval (including endpoints near decay).
            const auto h =
                std::min<std::int64_t>(125'000'000, (analysis_last - analysis_first) / 4);
            const double seconds = static_cast<double>(h) * 1e-9;
            const bool left = ns - analysis_first < h, right = analysis_last - ns < h;
            if (left || right) {
                const auto direction = left ? 1 : -1;
                const auto a = sample(core::Epoch{ns + direction * h});
                const auto b = sample(core::Epoch{ns + direction * 2 * h});
                for (std::size_t k = 0; k < 3; ++k)
                    native.velocity_mps[k] =
                        direction *
                        (-3 * native.position_m[k] + 4 * a.position_m[k] - b.position_m[k]) /
                        (2 * seconds);
            } else {
                const auto a = sample(core::Epoch{ns - h}), b = sample(core::Epoch{ns + h});
                for (std::size_t k = 0; k < 3; ++k)
                    native.velocity_mps[k] = (b.position_m[k] - a.position_m[k]) / (2 * seconds);
            }
        }
        const auto orientation = eop.at(native.epoch);
        auto state = core::transform_state(native, core::FrameId::itrf, frames, orientation);
        auto inertial = native.frame == core::FrameId::gcrf
                            ? native
                            : frames.gcrf_to_itrf_state(native.epoch, orientation).inverse(state);
        inertial_states.push_back(inertial);
        const auto &r = state.position_m;
        const auto radius = std::hypot(r[0], r[1], r[2]);
        min_radius = std::min(min_radius, radius);
        max_radius = std::max(max_radius, radius);
        states.push_back(std::move(state));
    }
    auto provenance = session.provenance();
    if (input.propagator == OrbitPropagator::sgp4)
        provenance.algorithm += ";display-position-tangent-fd2-0.125s";
    visualization::StateTrack track{results::StateSeries{provenance, std::move(states)}};
    visualization::StateTrack inertial_track{
        results::StateSeries{provenance, std::move(inertial_states)}};
    double max_error = 0;
    double max_inertial_error = 0;
    const auto &nodes = track.samples().states();
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        const auto a = nodes[i - 1].epoch.tai_nanoseconds(), b = nodes[i].epoch.tai_nanoseconds();
        for (const auto numerator : {1, 2, 3}) {
            const core::Epoch epoch{a + (b - a) * numerator / 4};
            const auto native = sample(epoch);
            const auto orientation = eop.at(epoch);
            auto direct_position = native.position_m;
            if (native.frame == core::FrameId::teme) {
                const auto fixed_position = frames.teme_to_itrf(epoch, orientation.sample)
                                                .apply_position(native.position_m);
                direct_position =
                    frames.gcrf_to_itrf(epoch, orientation.sample).inverse_position(fixed_position);
            }
            const auto interpolated = track.sample(epoch).markers.front().position_m;
            // Quarter-segment validation needs positions, not the costly R-dot velocity
            // transform. Cache nodes still use the full state transform for Hermite tangents.
            const auto r =
                frames.gcrf_to_itrf(epoch, orientation.sample).apply_position(direct_position);
            const auto error =
                std::hypot(r[0] - interpolated[0], r[1] - interpolated[1], r[2] - interpolated[2]);
            max_error = std::max(max_error, error);
            const auto inertial_interpolated =
                inertial_track.sample(epoch).markers.front().position_m;
            const auto inertial_error = std::hypot(direct_position[0] - inertial_interpolated[0],
                                                   direct_position[1] - inertial_interpolated[1],
                                                   direct_position[2] - inertial_interpolated[2]);
            max_inertial_error = std::max(max_inertial_error, inertial_error);
            if (error > 1 || inertial_error > 1)
                throw std::runtime_error(
                    "Display interpolation check exceeds 1 m; reduce the display step");
        }
    }
    check_cancel();
    auto eop_source = eop.at(nodes.front().epoch).sample.source_id;
    if (info.extrapolation_days > 0)
        eop_source +=
            ";policy=endpoint-linear-tai-v1;limit-days=" + std::to_string(info.extrapolation_days) +
            ";analysis-first-tai-ns=" + std::to_string(analysis_first) +
            ";analysis-last-tai-ns=" + std::to_string(analysis_last);
    return std::make_shared<const OrbitScene>(OrbitScene{
        std::move(input), std::move(session), std::move(track), std::move(inertial_track),
        max_error, max_inertial_error, min_radius, max_radius, eop_source,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()});
}
bool cache_covers(const OrbitScene &orbit, core::Epoch first, core::Epoch last) {
    const auto &nodes = orbit.track.samples().states();
    return nodes.front().epoch.tai_nanoseconds() <= first.tai_nanoseconds() &&
           nodes.back().epoch.tai_nanoseconds() >= last.tai_nanoseconds();
}
} // namespace orbitforge::desktop
