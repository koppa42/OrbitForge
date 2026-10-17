#include <cmath>
#include <fstream>
#include <full_gravity.hpp>
#include <gtest/gtest.h>
#include <orbitforge/core/propagation/gravity.hpp>
#include <sstream>
using namespace orbitforge::core;
TEST(Gravity, HighDegreeNormalizedFieldMatchesIndependentOrekitIncludingNearPoles) {
    std::vector<GravityCoefficient> coefficients;
    for (int n = 2; n <= 360; ++n)
        for (int m = 0; m <= n; ++m) {
            const double amplitude = 1e-8 / ((n + 1.0) * (n + 1));
            coefficients.push_back(
                {n, m,
                 n == 2 && m == 0 ? -4.84165371736e-4 : amplitude * std::sin((n + 1.0) * (m + 2)),
                 m == 0 ? 0 : amplitude * std::cos((n + 1.0) * (m + 2))});
        }
    GravityModel model({"synthetic", {}, {}, "tide_free", 3.986004415e14, 6378136.3, 360, 360},
                       coefficients);
    std::ifstream file(ORBITFORGE_GRAVITY_REFERENCE);
    ASSERT_TRUE(file.is_open());
    int rows = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream input(line);
        int degree = 0, order = 0;
        std::array<double, 3> position{}, expected{};
        input >> degree >> order;
        for (auto &v : position)
            input >> v;
        for (auto &v : expected)
            input >> v;
        ASSERT_TRUE(input);
        const auto actual = model.acceleration(position, degree, order);
        for (int axis = 0; axis < 3; ++axis)
            EXPECT_NEAR(actual[axis], expected[axis], 5e-12) << line;
        ++rows;
    }
    EXPECT_EQ(rows, 40);
    const auto exact = model.acceleration({0, 0, 7e6}, 360, 360);
    const auto near = model.acceleration({1e-7, 0, 7e6}, 360, 360);
    for (int axis = 0; axis < 3; ++axis)
        EXPECT_NEAR(exact[axis], near[axis], 5e-12);
    EXPECT_THROW((void)model.acceleration({7e6, 0, 0}, 361, 360), std::invalid_argument);
}

TEST(Gravity, FullDegreeAndRectangularFieldsMatchIndependentOrekit) {
    for (int profile = 0; profile < 3; ++profile) {
        const int order = profile == 1 ? 2159 : 2190;
        std::vector<GravityCoefficient> coefficients;
        for (int n = 2; n <= 2190; ++n)
            for (int m = 0; m <= std::min(n, order); ++m) {
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
        auto model = std::make_shared<const GravityModel>(
            GravityInfo{
                "full-reference", {}, {}, "tide_free", 3.986004415e14, 6378136.3, 2190, order},
            coefficients);
        full_gravity_test::verify(model, profile, ORBITFORGE_GRAVITY_FULL_REFERENCE,
                                  profile == 2 ? 24 : 48, profile == 1 ? 2 : 0);
        if (profile < 2) {
            const auto polar = model->acceleration({0, 0, 6356752.3}, 2190, order);
            const auto nearby = model->acceleration({1e-7, 0, 6356752.3}, 2190, order);
            for (int i = 0; i < 3; ++i)
                EXPECT_NEAR(polar[i], nearby[i], 5e-12);
        }
        EXPECT_EQ(model->acceleration({6378146.3, 0, 0.0}, 2190, order),
                  model->acceleration({6378146.3, 0, -0.0}, 2190, order));
        EXPECT_THROW((void)model->acceleration({7e6, 0, 0}, 2191, order), std::invalid_argument);
        if (profile == 1) {
            EXPECT_THROW((void)model->acceleration({7e6, 0, 0}, 2190, 2160), std::invalid_argument);
        }
    }
}

TEST(Gravity, FullDegreeZonalFieldAtPoleMatchesClosedForm) {
    const GravityModel model({"polar", {}, {}, "tide_free", 3.986004415e14, 6378136.3, 2190, 0},
                             {{2, 0, -4.84165371736e-4, 0}, {2190, 0, 1e-8, 0}});
    // Independent 70-digit evaluation of P_n(1)=1 and P'_n(1)=n(n+1)/2,
    // with exact binary64 inputs. The transverse field has a large, real gradient:
    // its change over 1e-7 m must not be mistaken for a polar singularity.
    const auto pole = model.acceleration({0, 0, 6356752.3}, 2190, 0);
    const auto nearby = model.acceleration({1e-7, 0, 6356752.3}, 2190, 0);
    EXPECT_EQ(pole[0], 0);
    EXPECT_EQ(pole[1], 0);
    EXPECT_NEAR(pole[2], -32.200032206602485771, 5e-12);
    EXPECT_NEAR(nearby[0], -3.858116305292898920e-10, 1e-20);
    EXPECT_NEAR(nearby[2], pole[2], 1e-13);
    const GravityModel base({"low degree", {}, {}, "tide_free", 3.986004415e14, 6378136.3, 2, 0},
                            {{2, 0, -4.84165371736e-4, 0}});
    const std::array corrections{GravityCoefficient{2190, 0, 1e-8, 0}};
    EXPECT_EQ(base.acceleration({0, 0, 6356752.3}, 2, 0, corrections), pole);
}
