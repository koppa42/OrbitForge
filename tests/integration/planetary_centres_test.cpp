#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <sstream>

using namespace orbitforge;
namespace {
class PlanetaryCentres : public ::testing::Test {
  protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbitforge-centres-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path samples =
        std::filesystem::path(ORBITFORGE_INTEGRATION_PLANETS).parent_path().parent_path();
    void SetUp() override { std::filesystem::create_directories(root); }
    void TearDown() override { std::filesystem::remove_all(root); }
};
} // namespace
TEST_F(PlanetaryCentres, OriginalMarsMatchesIndependentJplephemAndLeavesBaseBodiesUnchanged) {
    if (!runtime::ephemeris_available() || !runtime::data_catalog_available())
        GTEST_SKIP();
    runtime::DataCatalog catalog(root / "catalog");
    const auto base = catalog.install_ephemeris_snapshot(ORBITFORGE_INTEGRATION_PLANETS);
    const auto before = catalog.load_ephemeris("de440s");
    const auto entry =
        catalog.install_planetary_centres(samples / "mar099s_2016_leap/mar099s_subset.bsp",
                                          "urn:orbitforge:validation:mar099s-subset");
    EXPECT_NE(entry.info.content_sha256, base.info.content_sha256);
    const auto after = catalog.load_ephemeris("de440s");
    const auto raw = runtime::load_spice_snapshot(entry.manifest, root / "queries");
    const std::array mars{core::ThirdBody::mars};
    EXPECT_NO_THROW(
        after->validate_eclipsing_body_interval(entry.info.first, entry.info.last, mars));
    EXPECT_THROW(before->validate_eclipsing_body_interval(entry.info.first, entry.info.last, mars),
                 core::EphemerisCoverageError);
    std::ifstream input(samples / "mar099s_2016_leap/mars_jplephem.txt");
    ASSERT_TRUE(input);
    std::string line;
    unsigned cases = 0;
    double max_local_p = 0, max_local_v = 0, max_earth_p = 0, max_earth_v = 0;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        SCOPED_TRACE(line);
        std::istringstream row(line);
        std::int64_t ns;
        double et;
        std::array<double, 12> reference;
        ASSERT_TRUE(row >> ns >> et);
        for (auto &value : reference)
            ASSERT_TRUE(row >> value);
        const core::Epoch at{ns};
        const auto local = raw->geometric_state(499, 4, at);
        const auto earth = raw->geometric_state(499, 399, at);
        EXPECT_EQ(local.tdb.seconds_past_j2000, et);
        EXPECT_EQ(after->eclipsing_body_position(core::ThirdBody::mars, at), earth.position_m);
        for (std::size_t i = 0; i < 3; ++i) {
            max_local_p = std::max(max_local_p, std::abs(local.position_m[i] - reference[i]));
            max_local_v = std::max(max_local_v, std::abs(local.velocity_mps[i] - reference[3 + i]));
            max_earth_p = std::max(max_earth_p, std::abs(earth.position_m[i] - reference[6 + i]));
            max_earth_v = std::max(max_earth_v, std::abs(earth.velocity_mps[i] - reference[9 + i]));
            EXPECT_NEAR(local.position_m[i], reference[i], 1e-9);
            EXPECT_NEAR(local.velocity_mps[i], reference[3 + i], 1e-12);
            EXPECT_NEAR(earth.position_m[i], reference[6 + i], 2e-4);
            EXPECT_NEAR(earth.velocity_mps[i], reference[9 + i], 2e-10);
        }
        for (auto body : core::third_body_list)
            EXPECT_EQ(before->position(body, at), after->position(body, at));
        ++cases;
    }
    EXPECT_EQ(cases, 37U);
    RecordProperty("mars_local_position_error_m", max_local_p);
    RecordProperty("mars_local_velocity_error_mps", max_local_v);
    RecordProperty("mars_geocentric_position_error_m", max_earth_p);
    RecordProperty("mars_geocentric_velocity_error_mps", max_earth_v);
    const auto at = core::Epoch{entry.info.first.tai_nanoseconds() + 3600000000000LL};
    EXPECT_EQ(before->apparent_sun_from_earth(at), after->apparent_sun_from_earth(at));
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.srp.enabled = true;
    options.srp.additional_eclipsing_bodies.push_back(
        core::default_srp_eclipsing_body(core::ThirdBody::mars));
    const core::CartesianState initial{
        at, core::OriginId::earth, core::FrameId::gcrf, {7e6, 0, 0}, {0, 7500, 0}};
    core::HpopEnvironment environment;
    environment.ephemeris = before;
    EXPECT_THROW((void)core::make_hpop(initial, options, environment),
                 core::EphemerisCoverageError);
    environment.ephemeris = after;
    EXPECT_NO_THROW((void)core::make_hpop(initial, options, environment)
                        ->state_at(core::Epoch{at.tai_nanoseconds() + 30000000000LL}));
    const auto repeated =
        catalog.install_planetary_centres(samples / "mar099s_2016_leap/mar099s_subset.bsp",
                                          "urn:orbitforge:validation:mar099s-subset");
    EXPECT_EQ(repeated.info.content_sha256, entry.info.content_sha256);
    runtime::DataCatalog reopened(root / "catalog");
    EXPECT_EQ(reopened.load_ephemeris("de440s")->eclipsing_body_position(core::ThirdBody::mars, at),
              after->eclipsing_body_position(core::ThirdBody::mars, at));
}
TEST_F(PlanetaryCentres, FailedCancelledAndConcurrentImportsRetainThePublishedSnapshot) {
    if (!runtime::ephemeris_available() || !runtime::data_catalog_available())
        GTEST_SKIP();
    runtime::DataCatalog catalog(root / "catalog");
    const auto source = samples / "mar099s_2016_leap/mar099s_subset.bsp";
    EXPECT_THROW((void)catalog.install_planetary_centres(source), std::runtime_error);
    const auto base = catalog.install_ephemeris_snapshot(ORBITFORGE_INTEGRATION_PLANETS);
    EXPECT_THROW(
        (void)catalog.install_planetary_centres(source, {}, runtime::DataAcquisition::imported,
                                                [] { throw std::runtime_error("cancel"); }),
        std::runtime_error);
    std::ofstream(root / "bad.bsp") << "not an SPK";
    EXPECT_THROW((void)catalog.install_planetary_centres(root / "bad.bsp"), std::exception);
    EXPECT_EQ(catalog.ephemerides().at(0).info.content_sha256, base.info.content_sha256);
    bool changed = false;
    EXPECT_THROW((void)catalog.install_planetary_centres(
                     source, {}, runtime::DataAcquisition::imported,
                     [&] {
                         if (!changed) {
                             changed = true;
                             runtime::DataCatalog other(root / "catalog");
                             (void)other.install_ephemeris_snapshot(
                                 ORBITFORGE_INTEGRATION_PLANETS,
                                 runtime::DataAcquisition::downloaded);
                         }
                     }),
                 std::runtime_error);
    EXPECT_TRUE(changed);
    EXPECT_EQ(catalog.ephemerides().at(0).info.content_sha256, base.info.content_sha256);
    EXPECT_EQ(catalog.ephemerides().at(0).acquisition, runtime::DataAcquisition::downloaded);
    EXPECT_TRUE(std::filesystem::is_empty(root / "catalog/staging"));
}
