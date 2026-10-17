#pragma once
#include <locale>
#include <orbitforge/core/propagation/tabulated_orbit.hpp>
#include <sstream>
#include <string>

namespace orbitforge::runtime::detail {
inline std::string interpolation_name(core::OrbitInterpolationMethod method) {
    switch (method) {
    case core::OrbitInterpolationMethod::lagrange:
        return "lagrange";
    case core::OrbitInterpolationMethod::hermite:
        return "hermite";
    case core::OrbitInterpolationMethod::vop:
        return "vop-universal-transport-v1";
    }
    throw std::invalid_argument("Unknown orbit interpolation method");
}
inline std::string interpolation_mu_identity(const core::OrbitInterpolationOptions &options) {
    if (options.method != core::OrbitInterpolationMethod::vop)
        return {};
    std::ostringstream value;
    value.imbue(std::locale::classic());
    value << ";vop-mu=" << std::hexfloat << options.vop_mu;
    return value.str();
}
} // namespace orbitforge::runtime::detail
