#pragma once
#include <array>

namespace orbitforge::core {
// Acceleration and first state derivatives at one fixed force epoch. All vectors
// and matrix axes share the force's stated Cartesian reference frame; row i is
// acceleration component i and column j is position/velocity component j.
// External ephemerides, pole, GM and model parameters are held fixed.
struct ForceDifferential {
    std::array<double, 3> acceleration_mps2{};
    std::array<std::array<double, 3>, 3> position_jacobian_per_s2{};
    std::array<std::array<double, 3>, 3> velocity_jacobian_per_s{};
};
} // namespace orbitforge::core
