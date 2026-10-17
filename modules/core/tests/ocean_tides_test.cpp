#include <algorithm>
#include <cmath>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <orbitforge/core/propagation/hpop.hpp>
using namespace orbitforge::core;
namespace {
OceanTideInfo info(int n = 4, int m = 2) { return {"test", {}, {}, n, m}; }
OceanTideConstituent constant(int n = 4, int m = 2) {
    OceanTideConstituent wave{55555, {}}; // Every astronomical argument multiplier is zero.
    for (int degree = 2; degree <= n; ++degree)
        for (int order = 0; order <= std::min(degree, m); ++order)
            wave.coefficients.push_back({degree, order, degree * 1e-11, 4e-12, 2e-12, 1e-12});
    return wave;
}
GravityInfo field() { return {"test", {}, {}, "tide_free", egm96_mu, egm96_radius_m, 4, 2}; }
EarthOrientationSample eop() { return {Epoch{0}, -31.7, 1e-6, 2e-6, 0, 0, "test"}; }
} // namespace
TEST(OceanTides, ConstantWaveAndFieldRescalingMatchAnalyticCoefficients) {
    const OceanTideModel model(info(), {constant()});
    auto base = field();
    base.mu *= 1.2;
    base.radius_m *= 1.1;
    const auto actual = model.corrections(base, eop(), 4, 2, false);
    ASSERT_EQ(actual.size(), 9U);
    for (const auto &v : actual) {
        const double scale = std::pow(1 / 1.1, v.degree) / 1.2;
        EXPECT_NEAR(v.c, (v.degree * 1e-11 + 2e-12) * scale, 1e-25);
        EXPECT_NEAR(v.s, v.order ? 3e-12 * scale : 0, 1e-26);
    }
    // Same physical perturbation for a different base field's GM and reference radius.
    const GravityModel original(field(), {}), rescaled(base, {});
    const std::array<double, 3> position{4e6, -3e6, 5e6};
    const auto a = original.acceleration(position, 0, 0, model.corrections(field(), eop(), 4, 2));
    const auto b = rescaled.acceleration(position, 0, 0, model.corrections(base, eop(), 4, 2));
    const auto a0 = original.acceleration(position, 0, 0),
               b0 = rescaled.acceleration(position, 0, 0);
    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(a[i] - a0[i], b[i] - b0[i], 3e-15);
}
TEST(OceanTides, PoleTermIsAutomaticOnlyForDegreeTwoOrderOneAndReadsAreImmutable) {
    const auto model = std::make_shared<const OceanTideModel>(info(), std::vector{constant()});
    const auto a = model->corrections(field(), eop(), 4, 2);
    const auto b = model->corrections(field(), eop(), 4, 2, false);
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].degree == 2 && a[i].order == 1) {
            EXPECT_NE(a[i].c, b[i].c);
            EXPECT_NE(a[i].s, b[i].s);
        } else {
            EXPECT_EQ(a[i].c, b[i].c);
            EXPECT_EQ(a[i].s, b[i].s);
        }
    }
    auto bad = eop();
    bad.polar_motion_x_rad = std::numeric_limits<double>::quiet_NaN();
    EXPECT_NO_THROW((void)model->corrections(field(), bad, 4, 0));
    EXPECT_THROW((void)model->corrections(field(), bad, 4, 1), std::invalid_argument);
    EXPECT_NO_THROW((void)model->corrections(field(), bad, 4, 1, false));
    bad.ut1_minus_tai_seconds = std::numeric_limits<double>::quiet_NaN();
    EXPECT_TRUE(model->corrections(field(), bad, 1, 1).empty());
    EXPECT_THROW((void)model->corrections(field(), bad, 2, 0), std::invalid_argument);
    auto task = std::async(std::launch::async,
                           [model] { return model->corrections(field(), eop(), 4, 2); });
    const auto repeated = task.get();
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].c, repeated[i].c);
        EXPECT_EQ(a[i].s, repeated[i].s);
    }
}
TEST(OceanTides, SupportsDifferentCompleteWaveExtentsAndHighDegreeRectangularModels) {
    auto small = constant(2, 0);
    small.doodson = 55565;
    const OceanTideModel model(info(100, 12), {constant(100, 12), small});
    EXPECT_EQ(model.info().constituents, 2U);
    const auto high = model.corrections(field(), eop(), 100, 12, false);
    EXPECT_EQ(high.back().degree, 100);
    EXPECT_EQ(high.back().order, 12);
    const OceanTideModel maximum(info(gravity_max_degree, 0), {constant(gravity_max_degree, 0)});
    EXPECT_EQ(maximum.corrections(field(), eop(), gravity_max_degree, 0).size(),
              static_cast<std::size_t>(gravity_max_degree - 1));
    EXPECT_THROW((void)model.corrections(field(), eop(), 101, 0), std::invalid_argument);
    EXPECT_THROW((void)model.corrections(field(), eop(), 100, 13), std::invalid_argument);
}
TEST(OceanTides, RejectsMissingDuplicateNonfiniteOrMisdeclaredPotentialTerms) {
    for (int test = 0; test < 8; ++test) {
        auto metadata = info();
        auto wave = constant();
        switch (test) {
        case 0:
            wave.coefficients.erase(wave.coefficients.begin() + 1);
            break;
        case 1:
            wave.coefficients.push_back(wave.coefficients.front());
            break;
        case 2:
            wave.coefficients[0].c_plus = std::numeric_limits<double>::infinity();
            break;
        case 3:
            wave.doodson = -1;
            break;
        case 4:
            wave.coefficients[0].s_minus = std::numeric_limits<double>::quiet_NaN();
            break;
        case 5:
            metadata.degree = 5;
            break;
        case 6:
            metadata.order = 1;
            break;
        case 7:
            metadata.mu = 0;
            break;
        }
        EXPECT_THROW((void)OceanTideModel(metadata, {wave}), std::invalid_argument) << test;
    }
    EXPECT_THROW((void)OceanTideModel(info(), {constant(), constant()}), std::invalid_argument);
    EXPECT_THROW((void)OceanTideModel(info(), {}), std::invalid_argument);
}
TEST(OceanTides, ActiveDependencyIsIndependentOfGravityAndInactiveDraftNeedsNoData) {
    HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.ocean_tides = {true, "missing", 4, 2};
    EXPECT_TRUE(options.uses_earth_fixed_gravity());
    EXPECT_TRUE(options.uses_eop_force());
    EXPECT_FALSE(options.uses_ephemeris());
    EXPECT_FALSE(options.uses_utc_force());
    const CartesianState initial{
        Epoch{0}, OriginId::earth, FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    EXPECT_THROW((void)make_hpop(initial, options), std::invalid_argument);
    options.ocean_tides.enabled = false;
    EXPECT_FALSE(options.uses_eop_force());
    EXPECT_NO_THROW((void)make_hpop(initial, options));
    options.ocean_tides = {true, "missing", 1, 1};
    EXPECT_FALSE(options.uses_eop_force());
    EXPECT_NO_THROW((void)make_hpop(initial, options));
    for (const auto &bad :
         {OceanTideOptions{false, "../file", 4, 4}, OceanTideOptions{false, "", 4, 4},
          OceanTideOptions{false, "custom", 4, 5}, OceanTideOptions{false, "custom", -1, 0}})
        EXPECT_THROW(validate_ocean_tide_options(bad), std::invalid_argument);
}
