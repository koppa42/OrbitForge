#include <algorithm>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <orbitforge/core/propagation/solar_radiation_pressure.hpp>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <sstream>
using namespace orbitforge::core;
namespace {
using Vector = std::array<double, 3>;
using Matrix = std::array<Vector, 3>;
double norm(const Vector &v) { return std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2])}); }
double norm(const Matrix &m) { return std::max({norm(m[0]), norm(m[1]), norm(m[2])}); }
void read(std::istream &in, Vector &v) {
    for (auto &x : v)
        in >> x;
}
void read(std::istream &in, Matrix &m) {
    for (auto &v : m)
        read(in, v);
}
void check_sphere_reference(const char *path, unsigned rows, double light_tolerance,
                            double gradient_tolerance) {
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open());
    unsigned count = 0;
    double max_r = 0, max_sun = 0, max_light = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream in(line);
        char model = 0;
        SrpOptions options;
        options.enabled = true;
        options.moon_shadow = false;
        double earth = 0, lighting = 0;
        Vector r{}, sun{}, grad_r{}, grad_sun{}, a{}, dk{};
        Matrix jr{}, jv{}, js{};
        in >> model >> options.cr >> options.area_mass_ratio_m2_per_kg >> earth >>
            options.sun_radius_m;
        read(in, r);
        read(in, sun);
        in >> lighting;
        read(in, grad_r);
        read(in, grad_sun);
        read(in, a);
        read(in, jr);
        read(in, jv);
        read(in, js);
        read(in, dk);
        ASSERT_TRUE(in);
        options.atmospheric_altitude_m = earth - options.earth_radius_m;
        options.shadow_model = model == 'D'   ? SrpShadowModel::dual_cone
                               : model == 'C' ? SrpShadowModel::cylindrical
                                              : SrpShadowModel::none;
        const auto actual = srp_differential(r, sun, options);
        EXPECT_EQ(actual.nominal.acceleration_mps2,
                  evaluate_srp(r, sun, options).acceleration_mps2);
        EXPECT_EQ(actual.force.acceleration_mps2, actual.nominal.acceleration_mps2);
        EXPECT_EQ(actual.force.velocity_jacobian_per_s, Matrix{});
        EXPECT_EQ(jv, Matrix{});
        max_light = std::max(max_light, std::abs(actual.nominal.lighting_ratio - lighting));
        EXPECT_NEAR(actual.nominal.lighting_ratio, lighting, light_tolerance);
        const double distance = std::hypot(sun[0] - r[0], sun[1] - r[1], sun[2] - r[2]);
        const double pressure =
            options.pressure_at_reference_pa * std::pow(options.reference_distance_m / distance, 2);
        const double force_scale =
            pressure * std::abs(options.cr * options.area_mass_ratio_m2_per_kg);
        for (unsigned i = 0; i < 3; ++i) {
            const auto er = std::abs(actual.lighting_position_gradient_per_m[i] - grad_r[i]);
            const auto es = std::abs(actual.lighting_sun_gradient_per_m[i] - grad_sun[i]);
            if (norm(grad_r) > 0)
                max_r = std::max(max_r, er / norm(grad_r));
            if (norm(grad_sun) > 0)
                max_sun = std::max(max_sun, es / norm(grad_sun));
            EXPECT_NEAR(actual.lighting_position_gradient_per_m[i], grad_r[i],
                        gradient_tolerance * norm(grad_r) + 1e-25);
            EXPECT_NEAR(actual.lighting_sun_gradient_per_m[i], grad_sun[i],
                        gradient_tolerance * norm(grad_sun) + 1e-25);
            EXPECT_NEAR(actual.force.acceleration_mps2[i], a[i],
                        light_tolerance * force_scale + 1e-22);
            EXPECT_NEAR(actual.srp_parameter_jacobian_pa[i], dk[i],
                        light_tolerance * pressure + 1e-21);
            for (unsigned j = 0; j < 3; ++j) {
                EXPECT_NEAR(actual.force.position_jacobian_per_s2[i][j], jr[i][j],
                            gradient_tolerance * norm(jr) +
                                light_tolerance * force_scale / distance + 1e-30);
                EXPECT_NEAR(actual.sun_position_jacobian_per_s2[i][j], js[i][j],
                            gradient_tolerance * norm(js) +
                                light_tolerance * force_scale / distance + 1e-30);
            }
        }
        ++count;
    }
    EXPECT_EQ(count, rows);
    ::testing::Test::RecordProperty("max_lighting_error", max_light);
    ::testing::Test::RecordProperty("max_scaled_lighting_position_gradient", max_r);
    ::testing::Test::RecordProperty("max_scaled_lighting_sun_gradient", max_sun);
}
} // namespace
TEST(SrpDifferential, SingleSphereAndIsotropicFluxMatchIndependentOrekitGradient) {
    // The independent high-precision audit bounds Orekit's annular clipping at
    // 2.46e-9 in lighting and 4.75e-9 in gradient. Separate stricter tests below
    // retain sensitivity to our own numerical error.
    check_sphere_reference(ORBITFORGE_SRP_DIFFERENTIAL_REFERENCE, 274, 3e-9, 8e-9);
}
TEST(SrpDifferential, SingleSphereMatchesHighPrecisionOverlapDerivative) {
    check_sphere_reference(ORBITFORGE_SRP_DIFFERENTIAL_HIGH_REFERENCE, 256, 1e-11, 1e-10);
}
TEST(SrpDifferential, ZeroAndSignedConsiderParametersAndDisabledModel) {
    SrpOptions options;
    options.enabled = true;
    options.moon_shadow = false;
    const Vector r{7e6, 1e6, 2e6}, sun{149597870700., 0, 0};
    const auto positive = srp_differential(r, sun, options);
    options.cr = 0;
    const auto zero = srp_differential(r, sun, options);
    EXPECT_EQ(zero.force.acceleration_mps2, Vector{});
    EXPECT_EQ(zero.force.position_jacobian_per_s2, Matrix{});
    EXPECT_EQ(zero.sun_position_jacobian_per_s2, Matrix{});
    EXPECT_EQ(zero.srp_parameter_jacobian_pa, positive.srp_parameter_jacobian_pa);
    options.cr = -1;
    const auto negative = srp_differential(r, sun, options);
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) {
            EXPECT_EQ(negative.force.position_jacobian_per_s2[i][j],
                      -positive.force.position_jacobian_per_s2[i][j]);
            EXPECT_EQ(negative.sun_position_jacobian_per_s2[i][j],
                      -positive.sun_position_jacobian_per_s2[i][j]);
        }
    options.area_mass_ratio_m2_per_kg = 0;
    EXPECT_EQ(srp_differential(r, sun, options).srp_parameter_jacobian_pa,
              positive.srp_parameter_jacobian_pa);
    options.enabled = false;
    const auto disabled = srp_differential({}, {}, options);
    EXPECT_EQ(disabled.srp_parameter_jacobian_pa, Vector{});
    EXPECT_EQ(disabled.force.position_jacobian_per_s2, Matrix{});
}
TEST(SrpDifferential, ContactsCoincidentDisksAndInvalidGeometryAreExplicit) {
    SrpOptions options;
    options.enabled = true;
    options.earth_shadow = false;
    options.moon_shadow = true;
    const Vector observer{}, sun{149597870700., 0, 0};
    options.shadow_model = SrpShadowModel::cylindrical;
    const Vector moon{4e8, options.moon_radius_m, 0};
    EXPECT_NO_THROW((void)evaluate_srp(observer, sun, options, moon));
    EXPECT_THROW((void)srp_differential(observer, sun, options, moon), PropagationError);
    options.shadow_model = SrpShadowModel::dual_cone;
    EXPECT_THROW((void)srp_differential(observer, sun, options, sun), PropagationError);
    options.moon_radius_m = 4e8 * options.sun_radius_m / 149597870700.;
    EXPECT_THROW((void)srp_differential(observer, sun, options, Vector{4e8, 0, 0}),
                 PropagationError);
    options.moon_radius_m = 1e6;
    EXPECT_NO_THROW((void)srp_differential(observer, sun, options, Vector{4e8, 0, 0}));
    EXPECT_THROW((void)srp_differential(observer, sun, options), std::invalid_argument);
    EXPECT_THROW((void)srp_differential(observer, sun, options, observer), PropagationError);
    options.cr = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)srp_differential(observer, sun, options, moon), std::invalid_argument);
}

TEST(SrpDifferential, DiskUnionMatchesIndependentScipyQuadratureAndCartesianDifferences) {
    std::ifstream file(ORBITFORGE_SRP_UNION_DIFFERENTIAL_REFERENCE);
    ASSERT_TRUE(file.is_open());
    unsigned count = 0;
    double max_r = 0, max_sun = 0, max_light = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream in(line);
        unsigned bodies = 0;
        SrpOptions options;
        options.enabled = true;
        double earth = 0, lighting = 0, step_r = 0, step_s = 0;
        Vector r{}, sun{}, grad_r{}, grad_sun{}, a{}, dk{};
        Matrix jr{}, js{};
        in >> bodies >> options.cr >> options.area_mass_ratio_m2_per_kg >> earth;
        ASSERT_LE(bodies, 9U);
        options.earth_shadow = earth > 0;
        if (options.earth_shadow)
            options.earth_radius_m = earth;
        options.moon_shadow = bodies > 0;
        read(in, r);
        read(in, sun);
        std::optional<Vector> moon;
        std::vector<SrpBodyPosition> positions;
        for (unsigned i = 0; i < bodies; ++i) {
            double radius = 0;
            Vector centre{};
            in >> radius;
            read(in, centre);
            if (i == 0) {
                options.moon_radius_m = radius;
                moon = centre;
            } else {
                const auto body = third_body_list[i + 1];
                options.additional_eclipsing_bodies.push_back({body, radius});
                positions.push_back({body, centre});
            }
        }
        in >> step_r >> step_s >> lighting;
        read(in, grad_r);
        read(in, grad_sun);
        read(in, a);
        read(in, jr);
        read(in, js);
        read(in, dk);
        ASSERT_TRUE(in);
        ASSERT_GT(step_r, 0);
        ASSERT_GT(step_s, 0);
        const auto actual = srp_differential(r, sun, options, moon, positions);
        const double distance = std::hypot(sun[0] - r[0], sun[1] - r[1], sun[2] - r[2]);
        const double pressure =
            options.pressure_at_reference_pa * std::pow(options.reference_distance_m / distance, 2);
        const double force_scale =
            pressure * std::abs(options.cr * options.area_mass_ratio_m2_per_kg);
        max_light = std::max(max_light, std::abs(actual.nominal.lighting_ratio - lighting));
        EXPECT_NEAR(actual.nominal.lighting_ratio, lighting, 2e-11);
        EXPECT_EQ(actual.force.velocity_jacobian_per_s, Matrix{});
        for (unsigned i = 0; i < 3; ++i) {
            max_r =
                std::max(max_r, std::abs(actual.lighting_position_gradient_per_m[i] - grad_r[i]) /
                                    (norm(grad_r) + 2e-10));
            max_sun =
                std::max(max_sun, std::abs(actual.lighting_sun_gradient_per_m[i] - grad_sun[i]) /
                                      (norm(grad_sun) + 1e-12));
            EXPECT_NEAR(actual.lighting_position_gradient_per_m[i], grad_r[i],
                        1e-7 * norm(grad_r) + 2e-17);
            EXPECT_NEAR(actual.lighting_sun_gradient_per_m[i], grad_sun[i],
                        1e-7 * norm(grad_sun) + 1e-19);
            EXPECT_NEAR(actual.force.acceleration_mps2[i], a[i], 2e-11 * force_scale + 1e-22);
            EXPECT_NEAR(actual.srp_parameter_jacobian_pa[i], dk[i], 2e-11 * pressure + 1e-21);
            for (unsigned j = 0; j < 3; ++j) {
                EXPECT_NEAR(actual.force.position_jacobian_per_s2[i][j], jr[i][j],
                            1e-7 * norm(jr) + 2e-17 * force_scale + 1e-30);
                EXPECT_NEAR(actual.sun_position_jacobian_per_s2[i][j], js[i][j],
                            1e-7 * norm(js) + 1e-19 * force_scale + 1e-30);
            }
        }
        std::reverse(options.additional_eclipsing_bodies.begin(),
                     options.additional_eclipsing_bodies.end());
        std::reverse(positions.begin(), positions.end());
        const auto reversed = srp_differential(r, sun, options, moon, positions);
        EXPECT_EQ(reversed.nominal.acceleration_mps2, actual.nominal.acceleration_mps2);
        EXPECT_EQ(reversed.lighting_position_gradient_per_m,
                  actual.lighting_position_gradient_per_m);
        EXPECT_EQ(reversed.lighting_sun_gradient_per_m, actual.lighting_sun_gradient_per_m);
        EXPECT_EQ(reversed.force.position_jacobian_per_s2, actual.force.position_jacobian_per_s2);
        EXPECT_EQ(reversed.sun_position_jacobian_per_s2, actual.sun_position_jacobian_per_s2);
        ++count;
    }
    EXPECT_EQ(count, 111U);
    RecordProperty("max_lighting_error", max_light);
    RecordProperty("max_scaled_lighting_position_gradient", max_r);
    RecordProperty("max_scaled_lighting_sun_gradient", max_sun);
}
TEST(SrpDifferential, PhysicalDuplicatesAreSmoothButDifferentDistanceCoincidencesAreRejected) {
    SrpOptions options;
    options.enabled = true;
    options.earth_shadow = false;
    options.moon_radius_m = 1e6;
    const Vector r{}, sun{149597870700., 0, 0}, moon{4e8, 0, 0};
    const auto single = srp_differential(r, sun, options, moon);
    options.additional_eclipsing_bodies = {{ThirdBody::venus, 1e6}};
    const std::array duplicate{SrpBodyPosition{ThirdBody::venus, moon}};
    const auto both = srp_differential(r, sun, options, moon, duplicate);
    EXPECT_EQ(both.nominal.lighting_ratio, single.nominal.lighting_ratio);
    EXPECT_EQ(both.force.position_jacobian_per_s2, single.force.position_jacobian_per_s2);
    EXPECT_EQ(both.sun_position_jacobian_per_s2, single.sun_position_jacobian_per_s2);
    options.additional_eclipsing_bodies.front().radius_m = 2e6;
    const std::array different{SrpBodyPosition{ThirdBody::venus, {8e8, 0, 0}}};
    EXPECT_NO_THROW((void)evaluate_srp(r, sun, options, moon, different));
    EXPECT_THROW((void)srp_differential(r, sun, options, moon, different), PropagationError);
}
