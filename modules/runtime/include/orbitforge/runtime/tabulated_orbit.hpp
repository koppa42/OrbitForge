#pragma once
#include <orbitforge/application/session.hpp>
#include <orbitforge/core/propagation/tabulated_orbit.hpp>

namespace orbitforge::runtime {
// Owns a validated copy of the scientific ephemeris. See make_tabulated_orbit's units,
// frame/time, arc-boundary and failure contract. The caller supplies the source revision.
[[nodiscard]] application::Session make_tabulated_orbit_session(
    std::vector<core::OrbitEphemerisArc> arcs, core::OrbitInterpolationOptions options = {},
    std::string object_id = "satellite-1", std::uint64_t input_revision = 1,
    const std::function<void()> &checkpoint = {});
} // namespace orbitforge::runtime
