#include "interpolation_identity.hpp"
#include <orbitforge/runtime/tabulated_orbit.hpp>
#include <utility>

namespace orbitforge::runtime {
application::Session make_tabulated_orbit_session(std::vector<core::OrbitEphemerisArc> arcs,
                                                  core::OrbitInterpolationOptions options,
                                                  std::string object_id,
                                                  std::uint64_t input_revision,
                                                  const std::function<void()> &checkpoint) {
    auto provider = core::make_tabulated_orbit(std::move(arcs), options, checkpoint);
    const std::string algorithm = std::string("tabulated-orbit-tai-bracketed-nearest-v1;") +
                                  (options.method == core::OrbitInterpolationMethod::lagrange
                                       ? "lagrange-independent-barycentric"
                                   : options.method == core::OrbitInterpolationMethod::hermite
                                       ? "hermite-posvel-newton"
                                       : "vop-universal-transport-v1") +
                                  ";order=" + std::to_string(options.order) +
                                  detail::interpolation_mu_identity(options);
    return {std::move(provider), {std::move(object_id), input_revision, algorithm}};
}
} // namespace orbitforge::runtime
