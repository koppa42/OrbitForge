#include "../src/propagation/universal_kepler_jacobian.hpp"
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>

using namespace orbitforge::core;
TEST(UniversalKepler, MatchesIndependentOrekitAcrossAllConicsAndDirections) {
    std::ifstream input(ORBITFORGE_UNIVERSAL_KEPLER_REFERENCE);
    ASSERT_TRUE(input);
    std::string line;
    std::size_t count = 0;
    double max_position = 0, max_velocity = 0, max_roundtrip = 0;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#')
            continue;
        int kind;
        double mu, time;
        detail::KeplerState initial{}, expected{};
        std::istringstream row(line);
        row >> kind >> mu >> time;
        for (auto &v : initial)
            row >> v;
        for (auto &v : expected)
            row >> v;
        ASSERT_TRUE(row);
        SCOPED_TRACE(line);
        const auto actual = detail::universal_kepler(initial, time, mu);
        const auto restored = detail::universal_kepler(actual, -time, mu);
        for (std::size_t i = 0; i < 6; ++i) {
            const double error = std::abs(actual[i] - expected[i]);
            if (i < 3) {
                max_position = std::max(max_position, error);
                max_roundtrip = std::max(max_roundtrip, std::abs(restored[i] - initial[i]));
                EXPECT_NEAR(actual[i], expected[i], 5e-4);
                EXPECT_NEAR(restored[i], initial[i], 5e-4);
            } else {
                max_velocity = std::max(max_velocity, error);
                EXPECT_NEAR(actual[i], expected[i], 5e-7);
                EXPECT_NEAR(restored[i], initial[i], 5e-7);
            }
        }
        ++count;
    }
    EXPECT_EQ(count, 80U);
    RecordProperty("position_error_m", max_position);
    RecordProperty("velocity_error_mps", max_velocity);
    RecordProperty("roundtrip_position_m", max_roundtrip);
}
TEST(UniversalKepler, RejectsInvalidGeometryAndSupportsCancellation) {
    const detail::KeplerState initial{7e6, 0, 0, 0, 7500, 0};
    constexpr double mu = 3.986004415e14;
    EXPECT_EQ(detail::universal_kepler(initial, 0, mu), initial);
    EXPECT_THROW(detail::universal_kepler(initial, 10, 0), std::invalid_argument);
    EXPECT_THROW(detail::universal_kepler({}, 10, mu), std::invalid_argument);
    EXPECT_THROW(detail::universal_kepler({7e6, 0, 0, 1000, 0, 0}, 10, mu), std::invalid_argument);
    EXPECT_THROW(detail::universal_kepler(initial, std::numeric_limits<double>::infinity(), mu),
                 std::invalid_argument);
    int checks = 0;
    EXPECT_THROW(detail::universal_kepler(initial, 6000, mu,
                                          [&] {
                                              if (++checks == 3)
                                                  throw std::runtime_error("cancelled");
                                          }),
                 std::runtime_error);
    EXPECT_EQ(checks, 3);
    EXPECT_NO_THROW(detail::universal_kepler(initial, 6000, mu));
}

TEST(UniversalKepler, DifferentialMatchesIndependentOrekitAndSymplecticInverse) {
    std::ifstream input(ORBITFORGE_UNIVERSAL_KEPLER_STM_REFERENCE);
    ASSERT_TRUE(input);
    std::string line;
    std::size_t count = 0;
    double max_stm = 0, max_inverse = 0, max_symplectic = 0;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#')
            continue;
        std::istringstream row(line);
        int kind;
        double mu, seconds;
        detail::KeplerState initial{}, expected{};
        detail::KeplerMatrix reference{};
        row >> kind >> mu >> seconds;
        for (auto &v : initial)
            row >> v;
        for (auto &v : expected)
            row >> v;
        for (auto &r : reference)
            for (auto &v : r)
                row >> v;
        ASSERT_TRUE(row);
        SCOPED_TRACE(kind);
        const auto flow = detail::universal_kepler_linearized(initial, seconds, mu);
        const auto reverse = detail::universal_kepler_linearized(flow.state, -seconds, mu);
        EXPECT_EQ(flow.state, detail::universal_kepler(initial, seconds, mu));
        const double radius = std::hypot(initial[0], initial[1], initial[2]);
        const double speed = std::sqrt(mu / radius);
        const auto scale = [&](std::size_t i) { return i < 3 ? radius : speed; };
        detail::KeplerMatrix normalized{};
        for (std::size_t i = 0; i < 6; ++i) {
            EXPECT_NEAR(flow.state[i], expected[i], i < 3 ? 5e-4 : 5e-7);
            EXPECT_NEAR(initial[i] + flow.increment[i], flow.state[i], i < 3 ? 1e-6 : 1e-9);
            for (std::size_t j = 0; j < 6; ++j) {
                const double error =
                    std::abs(flow.jacobian[i][j] - reference[i][j]) * scale(j) / scale(i);
                max_stm = std::max(max_stm, error);
                EXPECT_LT(error, 2e-8);
                normalized[i][j] = flow.jacobian[i][j] * scale(j) / scale(i);
                double identity = 0;
                for (std::size_t k = 0; k < 6; ++k)
                    identity = std::fma(reverse.jacobian[i][k], flow.jacobian[k][j], identity);
                const double inverse_error =
                    std::abs(identity - (i == j ? 1. : 0.)) * scale(j) / scale(i);
                max_inverse = std::max(max_inverse, inverse_error);
                EXPECT_LT(inverse_error, 2e-8);
            }
        }
        for (std::size_t i = 0; i < 6; ++i)
            for (std::size_t j = 0; j < 6; ++j) {
                double value = 0;
                for (std::size_t k = 0; k < 3; ++k) {
                    value += normalized[k][i] * normalized[k + 3][j];
                    value -= normalized[k + 3][i] * normalized[k][j];
                }
                const double canonical =
                    i < 3 && j == i + 3 ? 1. : (i >= 3 && j == i - 3 ? -1. : 0.);
                const double error = std::abs(value - canonical);
                max_symplectic = std::max(max_symplectic, error);
                EXPECT_LT(error, 2e-8);
            }
        ++count;
    }
    EXPECT_EQ(count, 88U);
    RecordProperty("max_dimensionless_stm_error", max_stm);
    RecordProperty("max_dimensionless_inverse_error", max_inverse);
    RecordProperty("max_symplectic_error", max_symplectic);
}
