#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <orbitforge/core/ephemeris/ephemeris_provider.hpp>
#include <orbitforge/core/propagation/central_body.hpp>
#include <orbitforge/core/propagation/j2_mean_elements.hpp>
#include <orbitforge/core/propagation/j4_mean_elements.hpp>
#include <orbitforge/core/propagation/keplerian_elements.hpp>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <orbitforge/core/units/quantities.hpp>

namespace orbitforge::providers::spice {
// The caller supplies a writable workspace for owned temporary kernel copies.
// All CSPICE access to this linked instance must use this wrapper.
[[nodiscard]] std::unique_ptr<core::IEphemerisProvider>
load_snapshot(const std::filesystem::path &manifest, const std::filesystem::path &workspace);

// Copy only physical planet-centre segments 499..999 relative to their own
// system barycentre, in J2000. No numerical refit. Returns the retained IDs;
// files are named centre-<ID>.bsp in output_directory and must not already exist.
// Source must remain immutable during the call. On failure only files created
// by this call are removed. The cancellation callback must not re-enter SPICE.
[[nodiscard]] std::vector<std::int32_t>
extract_planetary_centres(const std::filesystem::path &source,
                          const std::filesystem::path &output_directory,
                          const std::function<void()> &checkpoint = {});

// Kernel-free ideal two-body dynamics. SI values and elapsed TAI seconds;
// shares the same CSPICE global-state lock as ephemeris queries.
[[nodiscard]] std::shared_ptr<const core::IStateProvider>
make_two_body(core::CartesianState initial, core::GravitationalParameter mu);
[[nodiscard]] core::CartesianState to_cartesian(core::KeplerianElements elements,
                                                core::GravitationalParameter mu);
[[nodiscard]] core::KeplerianElements to_keplerian(core::CartesianState state,
                                                   core::GravitationalParameter mu);
[[nodiscard]] core::CartesianState j2_mean_ellipse_state(core::J2MeanElements elements,
                                                         const core::CentralBody &body);
[[nodiscard]] core::J2MeanElements j2_mean_ellipse_elements(core::CartesianState auxiliary_state,
                                                            const core::CentralBody &body);
[[nodiscard]] std::shared_ptr<const core::IStateProvider> make_j2(core::J2MeanElements initial,
                                                                  const core::CentralBody &body,
                                                                  core::J2SecularOptions options);
[[nodiscard]] std::shared_ptr<const core::IStateProvider>
make_j4(core::J4MeanElements initial, const core::CentralBody &body = core::j4_earth());
} // namespace orbitforge::providers::spice
