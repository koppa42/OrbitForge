#include <algorithm>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/atmospheric_drag.hpp>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <sstream>

using namespace orbitforge::core;
namespace {
using Vector = std::array<double, 3>;
using Matrix = std::array<Vector, 3>;
const UtcDateTime utc{2000, 2, 29, 13, 23, 12, 500000000};
const Vector sun{1.4e11, -5e10, 3e10}, velocity{1234, -6234, 3456};
const SpaceWeatherSample history{210, 160, 3, 7, std::array{7., 14., 91., 110., 41., 38., 23.}};
double norm(const Vector &v) { return std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2])}); }
void read(std::istream &in, Vector &v) {
    for (auto &x : v)
        in >> x;
}
void read(std::istream &in, Matrix &m) {
    for (auto &row : m)
        read(in, row);
}
} // namespace

TEST(DragDifferential, NrlStateAndConsiderParameterMatchOrekitAutomaticDifferentiation) {
    std::ifstream file(ORBITFORGE_DRAG_DIFFERENTIAL_REFERENCE);
    ASSERT_TRUE(file.is_open());
    unsigned rows = 0;
    double maximum = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream in(line);
        DragOptions options;
        options.enabled = true;
        options.atmosphere = AtmosphereModel::nrlmsise00;
        int mode = 0;
        Vector r{}, v{}, gradient{}, acceleration{}, parameter{};
        Matrix jr{}, jv{};
        double rho = 0;
        in >> mode >> options.use_approximate_altitude >> options.cd >>
            options.area_mass_ratio_m2_per_kg;
        read(in, r);
        read(in, v);
        in >> rho;
        read(in, gradient);
        read(in, acceleration);
        read(in, jr);
        read(in, jv);
        read(in, parameter);
        ASSERT_TRUE(in);
        std::optional<SpaceWeatherSample> weather;
        if (mode) {
            options.weather_model = "fixed-weather";
            weather = mode == 1 ? SpaceWeatherSample{180, 165, 3, 37.25, {}} : history;
            options.geomagnetic_update =
                mode == 1 ? GeomagneticUpdate::daily : GeomagneticUpdate::three_hourly;
        }
        const auto actual = drag_differential(r, v, sun, utc, options, weather);
        EXPECT_EQ(actual.nominal.acceleration_itrf_mps2,
                  evaluate_drag(r, v, sun, utc, options, weather).acceleration_itrf_mps2);
        EXPECT_EQ(actual.force.acceleration_mps2, actual.nominal.acceleration_itrf_mps2);
        EXPECT_NEAR(actual.nominal.density_kg_m3, rho, 3e-11 * rho);
        const double bound = 1e-7 * norm(gradient) + 2e-13 * rho;
        for (unsigned i = 0; i < 3; ++i) {
            const double error = std::abs(actual.density_gradient_kg_m4[i] - gradient[i]);
            maximum = std::max(maximum, error / norm(gradient));
            EXPECT_NEAR(actual.density_gradient_kg_m4[i], gradient[i], bound);
            EXPECT_NEAR(actual.force.acceleration_mps2[i], acceleration[i],
                        3e-11 * norm(acceleration));
            EXPECT_NEAR(actual.drag_parameter_jacobian_pa[i], parameter[i],
                        3e-11 * norm(parameter));
            for (unsigned j = 0; j < 3; ++j) {
                EXPECT_NEAR(actual.force.position_jacobian_per_s2[i][j], jr[i][j],
                            .5 * std::abs(options.cd * options.area_mass_ratio_m2_per_kg) *
                                std::hypot(v[0], v[1], v[2]) * std::abs(v[i]) * bound);
                EXPECT_NEAR(actual.force.velocity_jacobian_per_s[i][j], jv[i][j],
                            3e-11 * norm(jv[i]));
            }
        }
        ++rows;
    }
    EXPECT_EQ(rows, 672U);
    RecordProperty("max_scaled_density_gradient_error", maximum);
}

TEST(DragDifferential, JacchiaRobertsAndBlendMatchIndependentGmatAndAuthorNrl) {
    std::ifstream file(ORBITFORGE_DRAG_DENSITY_REFERENCE);
    ASSERT_TRUE(file.is_open());
    unsigned rows = 0;
    double maximum = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream in(line);
        int mode = 0;
        DragOptions options;
        options.enabled = true;
        Vector r{}, expected{};
        double rho = 0;
        in >> mode >> options.use_approximate_altitude;
        read(in, r);
        in >> rho;
        read(in, expected);
        ASSERT_TRUE(in);
        std::optional<SpaceWeatherSample> weather, lower;
        if (mode) {
            options.weather_model = "fixed-weather";
            options.geomagnetic_update = GeomagneticUpdate::three_hourly;
            weather = SpaceWeatherSample{180, 165, 5.5, 37.25, {}};
        }
        if (mode == 2) {
            options.low_altitude_model = LowAltitudeDensityModel::nrlmsise00;
            lower = history;
        }
        const auto actual = drag_differential(r, velocity, sun, utc, options, weather, lower);
        EXPECT_NEAR(actual.nominal.density_kg_m3, rho, 3e-10 * rho);
        for (unsigned j = 0; j < 3; ++j) {
            const double error = std::abs(actual.density_gradient_kg_m4[j] - expected[j]);
            maximum = std::max(maximum, error / (norm(expected) + rho / 10000));
            EXPECT_NEAR(actual.density_gradient_kg_m4[j], expected[j],
                        2e-7 * norm(expected) + 5e-12 * rho);
        }
        ++rows;
    }
    EXPECT_EQ(rows, 72U);
    RecordProperty("max_scaled_density_gradient_error", maximum);
}

TEST(DragDifferential, HighAltitudeRobertsMatches80DigitOriginalEquations) {
    std::ifstream file(ORBITFORGE_DRAG_DENSITY_HIGH_REFERENCE);
    ASSERT_TRUE(file.is_open());
    unsigned rows = 0;
    double maximum_density = 0, maximum_gradient = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream in(line);
        int mode = 0;
        DragOptions options;
        options.enabled = true;
        Vector r{}, expected{};
        double rho = 0;
        in >> mode >> options.use_approximate_altitude;
        read(in, r);
        in >> rho;
        read(in, expected);
        ASSERT_TRUE(in);
        const std::array<double, 3> flux{150, 180, 70}, average{150, 165, 80}, kp{3, 5.5, 0};
        options.f107 = flux[mode];
        options.f107_average = average[mode];
        options.kp = kp[mode];
        const auto actual = drag_differential(r, velocity, sun, utc, options);
        maximum_density =
            std::max(maximum_density, std::abs(actual.nominal.density_kg_m3 / rho - 1));
        EXPECT_NEAR(actual.nominal.density_kg_m3, rho, 3e-11 * rho);
        for (unsigned j = 0; j < 3; ++j) {
            maximum_gradient =
                std::max(maximum_gradient,
                         std::abs(actual.density_gradient_kg_m4[j] - expected[j]) / norm(expected));
            EXPECT_NEAR(actual.density_gradient_kg_m4[j], expected[j],
                        1e-7 * norm(expected) + 2e-13 * rho);
        }
        ++rows;
    }
    EXPECT_EQ(rows, 216U);
    RecordProperty("max_relative_density_error", maximum_density);
    RecordProperty("max_scaled_density_gradient_error", maximum_gradient);
}

TEST(DragDifferential, ZeroSignedParametersZeroVelocityAndDisabledModel) {
    DragOptions options;
    options.enabled = true;
    options.atmosphere = AtmosphereModel::nrlmsise00;
    const Vector r{6778137, 0, 0};
    const auto positive = drag_differential(r, velocity, sun, utc, options);
    options.cd = 0;
    const auto zero = drag_differential(r, velocity, sun, utc, options);
    EXPECT_EQ(zero.force.acceleration_mps2, Vector{});
    EXPECT_EQ(zero.force.position_jacobian_per_s2, Matrix{});
    EXPECT_EQ(zero.force.velocity_jacobian_per_s, Matrix{});
    EXPECT_EQ(zero.drag_parameter_jacobian_pa, positive.drag_parameter_jacobian_pa);
    options.cd = -2.2;
    const auto negative = drag_differential(r, velocity, sun, utc, options);
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) {
            EXPECT_EQ(negative.force.position_jacobian_per_s2[i][j],
                      -positive.force.position_jacobian_per_s2[i][j]);
            EXPECT_EQ(negative.force.velocity_jacobian_per_s[i][j],
                      -positive.force.velocity_jacobian_per_s[i][j]);
        }
    options.area_mass_ratio_m2_per_kg = 0;
    EXPECT_EQ(drag_differential(r, velocity, sun, utc, options).drag_parameter_jacobian_pa,
              positive.drag_parameter_jacobian_pa);
    const auto stopped = drag_differential(r, {}, sun, utc, options);
    EXPECT_EQ(stopped.force.position_jacobian_per_s2, Matrix{});
    EXPECT_EQ(stopped.force.velocity_jacobian_per_s, Matrix{});
    EXPECT_EQ(stopped.drag_parameter_jacobian_pa, Vector{});
    EXPECT_NE(norm(stopped.density_gradient_kg_m4), 0);
    options.enabled = false;
    const auto disabled = drag_differential({}, {}, {}, {}, options);
    EXPECT_EQ(disabled.force.acceleration_mps2, Vector{});
    EXPECT_EQ(disabled.density_gradient_kg_m4, Vector{});
    EXPECT_EQ(disabled.drag_parameter_jacobian_pa, Vector{});
}

TEST(DragDifferential, BranchBoundariesPolesAndBadInputsHaveExplicitErrors) {
    DragOptions options;
    options.enabled = true;
    for (double altitude : {100000., 125000., 200000., 500000.}) {
        const Vector r{6378137 + altitude, 0, 0};
        EXPECT_NO_THROW((void)evaluate_drag(r, velocity, sun, utc, options));
        EXPECT_THROW((void)drag_differential(r, velocity, sun, utc, options), PropagationError);
        for (double offset : {-1., 1.})
            EXPECT_NO_THROW(
                (void)drag_differential({r[0] + offset, 0, 0}, velocity, sun, utc, options));
    }
    options.atmosphere = AtmosphereModel::nrlmsise00;
    for (double altitude : {0., 1000000.})
        EXPECT_NO_THROW(
            (void)drag_differential({6378137 + altitude, 0, 0}, velocity, sun, utc, options));
    for (double altitude : {72500., 160000., 200000., 450000.})
        EXPECT_THROW(
            (void)drag_differential({6378137 + altitude, 0, 0}, velocity, sun, utc, options),
            PropagationError);
    EXPECT_THROW((void)drag_differential({0, 0, 6800000}, velocity, sun, utc, options),
                 PropagationError);
    EXPECT_THROW((void)drag_differential({6778137, 0, 0}, velocity, {}, utc, options),
                 std::invalid_argument);
    EXPECT_THROW((void)drag_differential({-6778137, 0, 0}, velocity, sun, utc, options),
                 PropagationError);
    const double midnight = std::numbers::pi + std::atan2(sun[1], sun[0]);
    EXPECT_THROW(
        (void)drag_differential({6778137 * std::cos(midnight), 6778137 * std::sin(midnight), 0},
                                velocity, sun, utc, options),
        PropagationError);
    options.weather_model = "missing";
    EXPECT_THROW((void)drag_differential({6778137, 0, 0}, velocity, sun, utc, options),
                 std::invalid_argument);
    options.weather_model.clear();
    options.cd = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)drag_differential({6778137, 0, 0}, velocity, sun, utc, options),
                 std::invalid_argument);
}

TEST(DragDifferential, InertialPositionBlockIncludesAtmosphereRotationCoupling) {
    DragOptions options;
    options.enabled = true;
    options.atmosphere = AtmosphereModel::nrlmsise00;
    const Vector r{6778137, 0, 0}, inertial{300, 7300, 600};
    constexpr double omega = 7.292115e-5;
    const auto relative = [&](const Vector &p) {
        return Vector{inertial[0] + omega * p[1], inertial[1] - omega * p[0], inertial[2]};
    };
    const auto jac = drag_differential(r, relative(r), sun, utc, options);
    for (unsigned j = 0; j < 3; ++j) {
        auto plus = r, minus = r;
        plus[j] += 10;
        minus[j] -= 10;
        const auto pa =
            evaluate_drag(plus, relative(plus), sun, utc, options).acceleration_itrf_mps2;
        const auto ma =
            evaluate_drag(minus, relative(minus), sun, utc, options).acceleration_itrf_mps2;
        for (unsigned i = 0; i < 3; ++i) {
            const double value = jac.force.position_jacobian_per_s2[i][j] +
                                 (j == 0   ? -omega * jac.force.velocity_jacobian_per_s[i][1]
                                  : j == 1 ? omega * jac.force.velocity_jacobian_per_s[i][0]
                                           : 0);
            EXPECT_NEAR(value, (pa[i] - ma[i]) / 20, 3e-8 * std::abs(value) + 1e-21);
        }
    }
}
