#include <algorithm>
#include <cmath>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <orbitforge/core/propagation/gravity.hpp>
#include <orbitforge/core/propagation/hpop.hpp>
#include <sstream>

using namespace orbitforge::core;
namespace {
constexpr double mu = 3.986004415e14, radius_m = 6378136.3;
using Vector = std::array<double, 3>;
using Matrix = std::array<Vector, 3>;
GravityModel synthetic(int profile) {
    std::vector<GravityCoefficient> coefficients;
    for (int n = 2; n <= 2190; ++n) {
        for (int m = 0; m <= n; ++m) {
            const double amplitude = 1e-8 / ((n + 1.0) * (n + 1));
            double c = profile == 2 ? 0 : amplitude * std::sin((n + 1.0) * (m + 2));
            double s = profile == 2 || m == 0 ? 0 : amplitude * std::cos((n + 1.0) * (m + 2));
            if (n == 2 && m == 0)
                c = -4.84165371736e-4;
            if (profile == 2 && n == 2190) {
                if (m == 0)
                    c = 1e-8;
                if (m == 1) {
                    c = 1e-10;
                    s = -2e-10;
                }
                if (m == 2) {
                    c = -2e-10;
                    s = 3e-10;
                }
                if (m == 2159) {
                    c = 2e-8;
                    s = -3e-8;
                }
                if (m == 2190) {
                    c = 1e-8;
                    s = 4e-8;
                }
            }
            if (c != 0 || s != 0)
                coefficients.push_back({n, m, c, s});
        }
    }
    return GravityModel({"Hessian oracle", {}, {}, "tide_free", mu, radius_m, 2190, 2190},
                        coefficients);
}
GravityModel evolving() {
    GravityInfo info{"evolving", {}, {}, "tide_free", mu, radius_m, 2, 2};
    info.secular_variations = GravitySecularVariations{
        Epoch{0}, {{2, 0, 1.7e-11, 0}, {2, 1, -2.3e-11, 3.1e-11}, {2, 2, 1.3e-11, -2.7e-11}}};
    return GravityModel(
        info, {{2, 0, -4.84165371736e-4, 0}, {2, 1, 3e-10, -4e-10}, {2, 2, 7e-11, -5e-11}});
}
const std::array corrections{
    GravityCoefficient{2, 0, 1.2e-9, 0}, GravityCoefficient{2, 1, 2.1e-9, -1.9e-9},
    GravityCoefficient{4, 1, -7e-10, 9e-10}, GravityCoefficient{6, 5, 2.3e-9, -4.7e-9}};
double norm(const Matrix &matrix) {
    double result = 0;
    for (const auto &row : matrix)
        for (const auto value : row)
            result = std::max(result, std::abs(value));
    return result;
}
void invariants(const GravityDifferential &actual) {
    const auto &j = actual.position_jacobian_per_s2;
    for (int a = 0; a < 3; ++a) {
        EXPECT_TRUE(std::isfinite(actual.acceleration_mps2[a]));
        for (int b = 0; b < 3; ++b) {
            EXPECT_TRUE(std::isfinite(j[a][b]));
            EXPECT_EQ(j[a][b], j[b][a]);
        }
    }
    // A static external potential is harmonic, including secular/tidal coefficient changes.
    EXPECT_NEAR(j[0][0] + j[1][1] + j[2][2], 0, 2e-13 * norm(j) + 2e-21);
}
} // namespace

TEST(GravityDifferential, MatchesIndependentOrekitAnalyticHessians) {
    int rows = 0;
    double max_absolute = 0, max_scaled = 0, max_acceleration = 0;
    for (int profile : {0, 2, 3}) {
        const auto model = profile == 3 ? evolving() : synthetic(profile);
        std::ifstream file(ORBITFORGE_GRAVITY_DIFFERENTIAL_REFERENCE);
        ASSERT_TRUE(file.is_open());
        for (std::string line; std::getline(file, line);) {
            if (line.empty() || line[0] == '#')
                continue;
            std::istringstream input(line);
            int which = 0, degree = 0, order = 0;
            double years = 0;
            Vector position{}, expected_acceleration{};
            Matrix expected{};
            input >> which >> degree >> order >> years;
            if (which != profile)
                continue;
            for (auto &v : position)
                input >> v;
            for (auto &v : expected_acceleration)
                input >> v;
            for (auto &row : expected)
                for (auto &v : row)
                    input >> v;
            ASSERT_TRUE(input);
            SCOPED_TRACE(line);
            const auto at = Epoch{static_cast<std::int64_t>(years * (365.25 * 86400) * 1e9)};
            const auto delta = profile == 3 ? std::span<const GravityCoefficient>(corrections)
                                            : std::span<const GravityCoefficient>{};
            const auto epoch = profile == 3 ? std::optional<Epoch>{at} : std::optional<Epoch>{};
            const auto actual = model.differential(position, degree, order, delta, epoch);
            EXPECT_EQ(actual.acceleration_mps2,
                      model.acceleration(position, degree, order, delta, epoch));
            invariants(actual);
            const double scale = norm(expected);
            for (int a = 0; a < 3; ++a) {
                const double acceleration_error =
                    std::abs(actual.acceleration_mps2[a] - expected_acceleration[a]);
                max_acceleration = std::max(max_acceleration, acceleration_error);
                EXPECT_NEAR(actual.acceleration_mps2[a], expected_acceleration[a], 5e-11);
                for (int b = 0; b < 3; ++b) {
                    const double error =
                        std::abs(actual.position_jacobian_per_s2[a][b] - expected[a][b]);
                    max_absolute = std::max(max_absolute, error);
                    max_scaled = std::max(max_scaled, error / scale);
                    // Orekit's spherical-to-Cartesian Hessian has ~4e-12 relative
                    // trace error for the amplified degree-2190 field 2 mm off-axis.
                    // Exact-axis values have a separate, tighter Decimal oracle.
                    EXPECT_NEAR(actual.position_jacobian_per_s2[a][b], expected[a][b],
                                8e-12 * scale + 3e-19);
                }
            }
            ++rows;
        }
    }
    EXPECT_EQ(rows, 168);
    RecordProperty("max_hessian_error_per_s2", max_absolute);
    RecordProperty("max_scaled_hessian_error", max_scaled);
    RecordProperty("max_acceleration_error_mps2", max_acceleration);
}

TEST(GravityDifferential, FullDegreeExactPolesMatchHighPrecisionLegendreIdentities) {
    int rows = 0;
    double max_absolute = 0, max_scaled = 0;
    for (int profile : {0, 2}) {
        const auto model = synthetic(profile);
        std::ifstream file(ORBITFORGE_GRAVITY_DIFFERENTIAL_POLE_REFERENCE);
        ASSERT_TRUE(file.is_open());
        for (std::string line; std::getline(file, line);) {
            if (line.empty() || line[0] == '#')
                continue;
            std::istringstream input(line);
            int which = 0, degree = 0, order = 0;
            Vector position{}, acceleration{};
            Matrix hessian{};
            input >> which >> degree >> order;
            if (which != profile)
                continue;
            for (auto &v : position)
                input >> v;
            for (auto &v : acceleration)
                input >> v;
            for (auto &row : hessian)
                for (auto &v : row)
                    input >> v;
            ASSERT_TRUE(input);
            SCOPED_TRACE(line);
            const auto actual = model.differential(position, degree, order);
            EXPECT_EQ(actual.acceleration_mps2, model.acceleration(position, degree, order));
            invariants(actual);
            const double scale = norm(hessian);
            for (int a = 0; a < 3; ++a) {
                EXPECT_NEAR(actual.acceleration_mps2[a], acceleration[a], 5e-12);
                for (int b = 0; b < 3; ++b) {
                    const double error =
                        std::abs(actual.position_jacobian_per_s2[a][b] - hessian[a][b]);
                    max_absolute = std::max(max_absolute, error);
                    max_scaled = std::max(max_scaled, error / scale);
                    EXPECT_NEAR(actual.position_jacobian_per_s2[a][b], hessian[a][b],
                                2e-13 * scale + 2e-21);
                }
            }
            ++rows;
        }
    }
    EXPECT_EQ(rows, 76);
    RecordProperty("max_hessian_error_per_s2", max_absolute);
    RecordProperty("max_scaled_hessian_error", max_scaled);
}

TEST(GravityDifferential, MonopoleMatchesClosedFormAndRejectsInvalidInputs) {
    const auto model = builtin_egm96();
    for (const Vector position : {Vector{0, 0, 7e6}, Vector{7e6, -2e6, 3e6}, Vector{0, 0, -7e6}}) {
        const auto actual = model->differential(position, 0, 0);
        const double r = std::hypot(position[0], position[1], position[2]);
        const double factor = model->info().mu / (r * r * r);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                EXPECT_NEAR(actual.position_jacobian_per_s2[i][j],
                            factor * (3 * position[i] * position[j] / (r * r) - (i == j ? 1 : 0)),
                            2e-21);
        EXPECT_EQ(actual.acceleration_mps2, model->acceleration(position, 0, 0));
        invariants(actual);
    }
    const Vector position{7e6, 0, 0};
    for (const auto &size : {std::pair{-1, 0}, {1, 0}, {21, 20}, {20, 21}, {20, -1}}) {
        EXPECT_THROW((void)model->differential(position, size.first, size.second),
                     std::invalid_argument);
    }
    for (const Vector invalid :
         {Vector{}, Vector{std::numeric_limits<double>::infinity(), 0, 0},
          Vector{0, std::numeric_limits<double>::quiet_NaN(), 0}, Vector{1e-200, 0, 0}}) {
        EXPECT_THROW((void)model->differential(invalid, 0, 0), PropagationError);
        EXPECT_THROW((void)model->differential(invalid, 20, 20), PropagationError);
    }
    const std::array duplicate{GravityCoefficient{2, 0, 1e-10, 0},
                               GravityCoefficient{2, 0, 2e-10, 0}};
    EXPECT_THROW((void)model->differential(position, 20, 20, duplicate), std::invalid_argument);
    EXPECT_THROW((void)model->differential(position, 20, 20, {}, Epoch{0}), std::invalid_argument);
}

TEST(GravityDifferential, CorrectionsExtendBaseFieldAndCanBeCancelledWithoutMutation) {
    const auto base = evolving();
    const Vector position{0, 0, -6356752.3};
    const std::array high{GravityCoefficient{2190, 0, 1e-8, 0},
                          GravityCoefficient{2190, 1, 1e-10, -2e-10},
                          GravityCoefficient{2190, 2, -2e-10, 3e-10}};
    const auto at = Epoch{315576000000000000LL};
    const auto expected = base.differential(position, 2, 2, high, at);
    auto info = base.info();
    info.degree = 2190;
    info.order = 2190;
    std::vector<GravityCoefficient> coefficients{
        {2, 0, -4.84165371736e-4, 0}, {2, 1, 3e-10, -4e-10}, {2, 2, 7e-11, -5e-11}};
    coefficients.insert(coefficients.end(), high.begin(), high.end());
    const GravityModel combined(info, coefficients);
    const auto actual = combined.differential(position, 2190, 2190, {}, at);
    EXPECT_EQ(actual.acceleration_mps2, expected.acceleration_mps2);
    EXPECT_EQ(actual.position_jacobian_per_s2, expected.position_jacobian_per_s2);
    int visits = 0;
    EXPECT_THROW((void)base.differential(position, 2, 2, high, at,
                                         [&] {
                                             if (++visits == 3)
                                                 throw std::runtime_error(
                                                     "cancel gravity differential");
                                         }),
                 std::runtime_error);
    EXPECT_EQ(visits, 3);
    const auto after = base.differential(position, 2, 2, high, at);
    EXPECT_EQ(after.acceleration_mps2, expected.acceleration_mps2);
    EXPECT_EQ(after.position_jacobian_per_s2, expected.position_jacobian_per_s2);
}

TEST(GravityDifferential, ConcurrentEpochsDoNotShareMutableDerivativeState) {
    const auto model = evolving();
    const Vector position{4e6, -3e6, 5e6};
    std::vector<std::future<GravityDifferential>> tasks;
    std::vector<GravityDifferential> expected;
    for (int i = 0; i < 8; ++i) {
        const Epoch at{(i - 4) * 31557600000000000LL};
        expected.push_back(model.differential(position, 2, 2, corrections, at));
        tasks.push_back(std::async(std::launch::async, [&, at] {
            return model.differential(position, 2, 2, corrections, at);
        }));
    }
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const auto actual = tasks[i].get();
        EXPECT_EQ(actual.acceleration_mps2, expected[i].acceleration_mps2);
        EXPECT_EQ(actual.position_jacobian_per_s2, expected[i].position_jacobian_per_s2);
    }
    EXPECT_NE(expected.front().position_jacobian_per_s2, expected.back().position_jacobian_per_s2);
}
