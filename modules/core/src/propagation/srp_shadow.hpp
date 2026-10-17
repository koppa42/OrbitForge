#pragma once
#include <orbitforge/core/propagation/solar_radiation_pressure.hpp>

namespace orbitforge::core::detail {
// Signed spherical shadow contacts, in canonical Earth/Moon/planet order.
// Cylinder: one external limb. Dual cone: external, full-umbra and annular
// internal contacts. Negative means inside that contact. Gates have exactly the
// same front/depth semantics as evaluate_srp. This is integration infrastructure,
// not a public general eclipse-search API.
struct ShadowContacts {
    std::array<double, 3 * third_body_count> values{};
    std::size_t size = 0;
};
[[nodiscard]] ShadowContacts
srp_shadow_contacts(const std::array<double, 3> &satellite, const std::array<double, 3> &sun,
                    const SrpOptions &options,
                    const std::optional<std::array<double, 3>> &moon = {},
                    std::span<const SrpBodyPosition> additional = {});
} // namespace orbitforge::core::detail
