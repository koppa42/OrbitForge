#include <algorithm>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <orbitforge/core/propagation/relativity.hpp>
#include <orbitforge/core/propagation/third_body.hpp>
#include <sstream>

using namespace orbitforge::core;
namespace {
using Vector = std::array<double, 3>;
using Matrix = std::array<Vector, 3>;
constexpr double earth_mu = 3.986004415e14, sun_mu = 1.3271244004127942e20;
const Vector sun{1.4e11, 3e10, 2e10}, sun_velocity{-1000, 30000, 2000}, pole{0, 0, 1};
const Vector r{7e6, 2e6, -1e6}, v{1000, 6000, 2000};
double norm(const Vector &vector) {
    double scale = 0;
    for (const double x : vector)
        scale = std::max(scale, std::abs(x));
    return scale;
}
double norm(const Matrix &matrix) {
    double scale = 0;
    for (const auto &row : matrix)
        scale = std::max(scale, norm(row));
    return scale;
}
void read(std::istream &in, Vector &value) {
    for (auto &x : value)
        in >> x;
}
void read(std::istream &in, ForceDifferential &value) {
    read(in, value.acceleration_mps2);
    for (auto &row : value.position_jacobian_per_s2)
        read(in, row);
    for (auto &row : value.velocity_jacobian_per_s)
        read(in, row);
}
} // namespace

TEST(ForceDifferential, ThirdBodyMatchesIndependentOrekitGradient) {
    std::ifstream file(ORBITFORGE_FORCE_DIFFERENTIAL_REFERENCE);
    ASSERT_TRUE(file.is_open());
    int rows = 0;
    double max_scaled = 0, max_absolute = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] != 'T')
            continue;
        std::istringstream in(line);
        char tag;
        double gm = 0;
        Vector rb{}, position{};
        ForceDifferential expected;
        in >> tag >> gm;
        read(in, rb);
        read(in, position);
        read(in, expected);
        ASSERT_TRUE(in);
        SCOPED_TRACE(line);
        const auto actual = third_body_differential(position, rb, gm);
        EXPECT_EQ(actual.acceleration_mps2, third_body_acceleration(position, rb, gm));
        EXPECT_EQ(actual.velocity_jacobian_per_s, Matrix{});
        const double scale = norm(expected.position_jacobian_per_s2);
        for (int i = 0; i < 3; ++i) {
            // Orekit directly subtracts the two body accelerations; allow its
            // roundoff near r=0 while independently testing the analytic Jacobian.
            EXPECT_NEAR(actual.acceleration_mps2[i], expected.acceleration_mps2[i],
                        5e-13 * norm(expected.acceleration_mps2) + 3e-17);
            for (int j = 0; j < 3; ++j) {
                const double error = std::abs(actual.position_jacobian_per_s2[i][j] -
                                              expected.position_jacobian_per_s2[i][j]);
                max_scaled = std::max(max_scaled, error / scale);
                max_absolute = std::max(max_absolute, error);
                EXPECT_NEAR(actual.position_jacobian_per_s2[i][j],
                            expected.position_jacobian_per_s2[i][j], 2e-14 * scale + 1e-29);
                EXPECT_EQ(actual.position_jacobian_per_s2[i][j],
                          actual.position_jacobian_per_s2[j][i]);
                EXPECT_EQ(expected.velocity_jacobian_per_s[i][j], 0);
            }
        }
        const auto &j = actual.position_jacobian_per_s2;
        EXPECT_NEAR(j[0][0] + j[1][1] + j[2][2], 0, 4e-15 * scale);
        ++rows;
    }
    EXPECT_EQ(rows, 42);
    RecordProperty("max_scaled_position_partial_error", max_scaled);
    RecordProperty("max_position_partial_error_per_s2", max_absolute);
}

TEST(ForceDifferential, EveryRelativisticTermMatchesIndependentOrekitGradient) {
    std::ifstream file(ORBITFORGE_FORCE_DIFFERENTIAL_REFERENCE);
    ASSERT_TRUE(file.is_open());
    int rows = 0;
    std::array<double, 4> max_position{}, max_velocity{};
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] != 'R')
            continue;
        std::istringstream in(line);
        char tag;
        double gm = 0, solar_mu = 0;
        Vector position{}, velocity{}, rb{}, vb{}, spin{};
        std::array<ForceDifferential, 4> expected{};
        in >> tag >> gm >> solar_mu;
        for (auto *vector : {&position, &velocity, &rb, &vb, &spin})
            read(in, *vector);
        for (auto &term : expected)
            read(in, term);
        ASSERT_TRUE(in);
        SCOPED_TRACE(line);
        const auto actual = relativity_differential(position, velocity, gm, rb, vb, solar_mu, spin);
        const auto nominal = evaluate_relativity(position, velocity, gm, rb, vb, solar_mu, spin);
        const std::array values{actual.schwarzschild, actual.lense_thirring, actual.de_sitter,
                                actual.total};
        const std::array forces{nominal.schwarzschild_mps2, nominal.lense_thirring_mps2,
                                nominal.de_sitter_mps2, nominal.acceleration_mps2};
        for (std::size_t term = 0; term < 4; ++term) {
            EXPECT_EQ(values[term].acceleration_mps2, forces[term]);
            const double position_scale = norm(expected[term].position_jacobian_per_s2);
            const double velocity_scale = norm(expected[term].velocity_jacobian_per_s);
            for (int i = 0; i < 3; ++i) {
                EXPECT_NEAR(values[term].acceleration_mps2[i], expected[term].acceleration_mps2[i],
                            2e-14 * norm(expected[term].acceleration_mps2) + 5e-32);
                for (int j = 0; j < 3; ++j) {
                    const double pr = std::abs(values[term].position_jacobian_per_s2[i][j] -
                                               expected[term].position_jacobian_per_s2[i][j]);
                    const double pv = std::abs(values[term].velocity_jacobian_per_s[i][j] -
                                               expected[term].velocity_jacobian_per_s[i][j]);
                    if (position_scale > 0)
                        max_position[term] = std::max(max_position[term], pr / position_scale);
                    if (velocity_scale > 0)
                        max_velocity[term] = std::max(max_velocity[term], pv / velocity_scale);
                    EXPECT_NEAR(values[term].position_jacobian_per_s2[i][j],
                                expected[term].position_jacobian_per_s2[i][j],
                                4e-14 * position_scale + 1e-32);
                    EXPECT_NEAR(values[term].velocity_jacobian_per_s[i][j],
                                expected[term].velocity_jacobian_per_s[i][j],
                                4e-14 * velocity_scale + 1e-29);
                }
            }
        }
        EXPECT_EQ(actual.de_sitter.position_jacobian_per_s2, Matrix{});
        ++rows;
    }
    EXPECT_EQ(rows, 63);
    for (std::size_t i = 0; i < 4; ++i) {
        RecordProperty("max_scaled_position_error_term_" + std::to_string(i), max_position[i]);
        RecordProperty("max_scaled_velocity_error_term_" + std::to_string(i), max_velocity[i]);
    }
}

TEST(ForceDifferential, OriginAndZeroVelocityKeepNonzeroStateSensitivity) {
    const auto third = third_body_differential({}, {0, 1e8, 0}, 1e12);
    EXPECT_EQ(third.acceleration_mps2, Vector{});
    EXPECT_NEAR(third.position_jacobian_per_s2[0][0], -1e-12, 1e-27);
    EXPECT_NEAR(third.position_jacobian_per_s2[1][1], 2e-12, 1e-27);
    EXPECT_NEAR(third.position_jacobian_per_s2[2][2], -1e-12, 1e-27);
    EXPECT_EQ(third.velocity_jacobian_per_s, Matrix{});
    const auto rel = relativity_differential(r, {}, earth_mu, sun, sun_velocity, sun_mu, pole);
    EXPECT_EQ(rel.lense_thirring.acceleration_mps2, Vector{});
    EXPECT_EQ(rel.de_sitter.acceleration_mps2, Vector{});
    EXPECT_EQ(rel.schwarzschild.velocity_jacobian_per_s, Matrix{});
    EXPECT_EQ(rel.lense_thirring.position_jacobian_per_s2, Matrix{});
    EXPECT_EQ(rel.de_sitter.position_jacobian_per_s2, Matrix{});
    EXPECT_GT(norm(rel.lense_thirring.velocity_jacobian_per_s), 0);
    EXPECT_GT(norm(rel.de_sitter.velocity_jacobian_per_s), 0);
    for (const auto &matrix :
         {rel.lense_thirring.velocity_jacobian_per_s, rel.de_sitter.velocity_jacobian_per_s})
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                EXPECT_NEAR(matrix[i][j], -matrix[j][i], 1e-28);
}

TEST(ForceDifferential, InertialRotationTransformsBothDerivativeBlocks) {
    // Proper rotation by a unit quaternion (1,2,2,0)/3; fully coupled axes.
    const Matrix rotation{
        {{1.0 / 9, 8.0 / 9, 4.0 / 9}, {8.0 / 9, 1.0 / 9, -4.0 / 9}, {-4.0 / 9, 4.0 / 9, -7.0 / 9}}};
    const auto rotate = [&](const Vector &x) {
        Vector result{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                result[i] += rotation[i][j] * x[j];
        return result;
    };
    const auto before = relativity_differential(r, v, earth_mu, sun, sun_velocity, sun_mu, pole);
    const auto after = relativity_differential(rotate(r), rotate(v), earth_mu, rotate(sun),
                                               rotate(sun_velocity), sun_mu, rotate(pole));
    const std::array originals{before.schwarzschild, before.lense_thirring, before.de_sitter,
                               before.total, third_body_differential(r, sun, sun_mu)};
    const std::array rotated{after.schwarzschild, after.lense_thirring, after.de_sitter,
                             after.total, third_body_differential(rotate(r), rotate(sun), sun_mu)};
    for (std::size_t k = 0; k < originals.size(); ++k) {
        const auto force = rotate(originals[k].acceleration_mps2);
        for (int i = 0; i < 3; ++i)
            EXPECT_NEAR(rotated[k].acceleration_mps2[i], force[i], 1e-13 * norm(force) + 1e-30);
        for (bool velocity : {false, true}) {
            const auto &original = velocity ? originals[k].velocity_jacobian_per_s
                                            : originals[k].position_jacobian_per_s2;
            const auto &actual =
                velocity ? rotated[k].velocity_jacobian_per_s : rotated[k].position_jacobian_per_s2;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) {
                    double expected = 0;
                    for (int a = 0; a < 3; ++a)
                        for (int b = 0; b < 3; ++b)
                            expected += rotation[i][a] * original[a][b] * rotation[j][b];
                    EXPECT_NEAR(actual[i][j], expected, 1e-14 * norm(original) + 1e-31);
                }
        }
    }
}

TEST(ForceDifferential, InvalidInputsCannotReturnPartialResults) {
    const auto inf = std::numeric_limits<double>::infinity();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)third_body_differential(r, {}, sun_mu), std::invalid_argument);
    EXPECT_THROW((void)third_body_differential(sun, sun, sun_mu), std::invalid_argument);
    EXPECT_THROW((void)third_body_differential(r, sun, 0), std::invalid_argument);
    EXPECT_THROW((void)third_body_differential({inf, 0, 0}, sun, sun_mu), std::invalid_argument);
    EXPECT_THROW((void)third_body_differential(r, {0, nan, 0}, sun_mu), std::invalid_argument);
    EXPECT_THROW((void)relativity_differential({}, v, earth_mu, sun, sun_velocity, sun_mu, pole),
                 std::invalid_argument);
    EXPECT_THROW((void)relativity_differential(r, {299792458, 0, 0}, earth_mu, sun, sun_velocity,
                                               sun_mu, pole),
                 std::invalid_argument);
    EXPECT_THROW((void)relativity_differential(r, v, 0, sun, sun_velocity, sun_mu, pole),
                 std::invalid_argument);
    EXPECT_THROW((void)relativity_differential(r, v, earth_mu, {}, sun_velocity, sun_mu, pole),
                 std::invalid_argument);
    EXPECT_THROW((void)relativity_differential(r, v, earth_mu, sun, {nan, 0, 0}, sun_mu, pole),
                 std::invalid_argument);
    EXPECT_THROW(
        (void)relativity_differential(r, v, earth_mu, sun, sun_velocity, sun_mu, {0, 0, 2}),
        std::invalid_argument);
}
