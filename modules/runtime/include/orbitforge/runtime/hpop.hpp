#pragma once
#include <orbitforge/application/session.hpp>
#include <orbitforge/core/propagation/hpop.hpp>

namespace orbitforge::runtime {
[[nodiscard]] application::Session make_hpop_session(core::CartesianState initial,
                                                     core::HpopOptions options = {},
                                                     core::HpopEnvironment environment = {},
                                                     std::string object_id = "satellite-1",
                                                     std::uint64_t input_revision = 1);
// Eager, cancellable generation of one immutable coast, followed by scientific interpolation.
// Both endpoints are exact TAI epochs; no extrapolation or cross-weather-boundary windows.
// Each generated arc must contain enough nodes for the explicitly requested order, otherwise
// construction fails. No implicit lowering of order, extra propagation or direct-integration
// fallback. Propagation and subsequent queries share the same source identity; ordinary project
// HPOP sessions are still configured separately and are not changed by this opt-in factory.
[[nodiscard]] application::Session make_hpop_ephemeris_session(
    core::CartesianState initial, core::Epoch end, core::HpopOptions options,
    core::HpopEnvironment environment, core::OrbitInterpolationOptions interpolation,
    std::int64_t reporting_step_nanoseconds = 0, std::string object_id = "satellite-1",
    std::uint64_t input_revision = 1, const std::function<void()> &checkpoint = {});
// Project/mission query path: lazily prepare bounded scientific interpolation, using
// options.interpolation; reporting step zero retains accepted nodes. See the Core contract.
[[nodiscard]] application::Session make_hpop_interpolated_session(
    core::CartesianState initial, core::Epoch first, core::Epoch last,
    core::HpopOptions options = {}, core::HpopEnvironment environment = {},
    std::int64_t reporting_step_nanoseconds = 0, std::string object_id = "satellite-1",
    std::uint64_t input_revision = 1);
} // namespace orbitforge::runtime
