#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <numbers>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/hpop.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <sstream>
using namespace orbitforge;
namespace {
const std::filesystem::path references{ORBITFORGE_OCEAN_REFERENCE};
core::GravityInfo field() {
    return {"reference", {}, {}, "tide_free", core::egm96_mu, core::egm96_radius_m, 2, 2};
}
class Frames final : public core::IFrameProvider {
  public:
    core::PositionRotation gcrf_to_itrf(core::Epoch e,
                                        const core::EarthOrientationSample &) const override {
        return {e,
                core::OriginId::earth,
                core::FrameId::gcrf,
                core::FrameId::itrf,
                {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
                "test-identity",
                "test"};
    }
    core::StateTransform gcrf_to_itrf_state(core::Epoch e,
                                            const core::EarthOrientationState &o) const override {
        return {gcrf_to_itrf(e, o.sample), {}, "test-static"};
    }
};
class Eop final : public core::IEarthOrientationProvider {
    core::EopDataInfo info_{"test",
                            {},
                            {},
                            {},
                            {},
                            core::Epoch{-1000000000000000LL},
                            core::Epoch{1000000000000000LL},
                            {"test", {}, {}, 0, 100000}};

  public:
    core::EarthOrientationState at(core::Epoch e) const override {
        return {{e, -31.7, .12 * std::numbers::pi / 648000, .25 * std::numbers::pi / 648000, 0, 0,
                 "test"},
                {}};
    }
    const core::EopDataInfo &data_info() const noexcept override { return info_; }
};
// The moving Sun/Moon used by the independent Orekit solid-tide reference.
class Ephemeris final : public core::IThirdBodyEphemeris {
    core::ThirdBodyDataInfo info_{"test",
                                  {},
                                  "test",
                                  core::Epoch{-90000000000000LL},
                                  core::Epoch{90000000000000LL},
                                  1.3271244004127942e20,
                                  4.9028001184575496e12};

  public:
    std::array<double, 3> position(core::ThirdBody id, core::Epoch epoch) const override {
        if (id != core::ThirdBody::sun && id != core::ThirdBody::moon)
            throw std::invalid_argument("Unexpected tide body");
        const bool sun = id == core::ThirdBody::sun;
        const double radius = sun ? 1.496e11 : 384400000;
        const double rate = 2 * std::numbers::pi / ((sun ? 365.25 : 27.321661) * 86400);
        const double angle = (sun ? .3 : 1.2) + rate * epoch.seconds_since(core::Epoch{0});
        const double tilt = sun ? .409 : .5;
        return {radius * std::cos(angle), radius * std::sin(angle) * std::cos(tilt),
                radius * std::sin(angle) * std::sin(tilt)};
    }
    void validate_interval(core::Epoch first, core::Epoch last,
                           std::span<const core::ThirdBody> bodies) const override {
        EXPECT_EQ(bodies.size(), 2U);
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw std::out_of_range("Tide ephemeris coverage");
    }
    const core::ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
};
void verify(const std::shared_ptr<const core::OceanTideModel> &model, const std::string &name,
            std::array<int, 3> expected_counts, bool combined = false) {
    std::ifstream input(references / name);
    ASSERT_TRUE(input);
    int coefficients = 0, accelerations = 0, trajectories = 0;
    double max_c = 0, max_a = 0, max_r = 0, max_v = 0;
    const auto base = std::make_shared<const core::GravityModel>(
        field(), std::vector<core::GravityCoefficient>{});
    core::HpopEnvironment environment{std::make_shared<Frames>(), std::make_shared<Eop>()};
    environment.ocean_tides = model;
    if (combined)
        environment.ephemeris = std::make_shared<Ephemeris>();
    std::map<std::pair<int, int>, std::shared_ptr<const core::IStateProvider>> providers;
    for (std::string line; std::getline(input, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row(line);
        char type;
        row >> type;
        if (type == 'T') {
            int degree, order, pole;
            long long seconds;
            std::array<double, 3> position{}, velocity{};
            row >> degree >> order >> pole >> seconds;
            for (auto &x : position)
                row >> x;
            for (auto &x : velocity)
                row >> x;
            ASSERT_TRUE(row);
            ASSERT_EQ(pole, 1);
            auto &provider = providers[{degree, order}];
            if (!provider) {
                core::HpopOptions options;
                options.gravity_degree = options.gravity_order = 0;
                options.ocean_tides = {true, "reference", degree, order};
                if (combined)
                    options.solid_tides = {core::SolidTideMode::full, false};
                options.relative_tolerance = 1e-14;
                options.position_tolerance_m = 1e-6;
                options.velocity_tolerance_mps = 1e-9;
                options.max_step_seconds = 30;
                provider = core::make_hpop({core::Epoch{0},
                                            core::OriginId::earth,
                                            core::FrameId::gcrf,
                                            {7e6, 0, 0},
                                            {0, 6200, 5300}},
                                           options, environment);
            }
            const auto actual = provider->state_at(core::Epoch{seconds * 1000000000LL});
            for (int axis = 0; axis < 3; ++axis) {
                max_r = std::max(max_r, std::abs(actual.position_m[axis] - position[axis]));
                max_v = std::max(max_v, std::abs(actual.velocity_mps[axis] - velocity[axis]));
                EXPECT_NEAR(actual.position_m[axis], position[axis], 1e-3) << line;
                EXPECT_NEAR(actual.velocity_mps[axis], velocity[axis], 1e-6) << line;
            }
            ++trajectories;
            continue;
        }
        std::int64_t ns;
        double ut1, xp, yp;
        int degree, order, pole;
        row >> ns >> ut1 >> xp >> yp >> degree >> order >> pole;
        const core::EarthOrientationSample eop{core::Epoch{ns}, ut1, xp, yp, 0, 0, "reference"};
        const auto deltas = model->corrections(field(), eop, degree, order, pole != 0);
        if (type == 'C') {
            int n, m;
            double c, s;
            row >> n >> m >> c >> s;
            ASSERT_TRUE(row);
            const auto found = std::ranges::find_if(
                deltas, [&](const auto &v) { return v.degree == n && v.order == m; });
            ASSERT_NE(found, deltas.end());
            max_c = std::max({max_c, std::abs(found->c - c), std::abs(found->s - s)});
            EXPECT_NEAR(found->c, c, 2e-20) << line;
            EXPECT_NEAR(found->s, s, 2e-20) << line;
            ++coefficients;
        } else {
            ASSERT_EQ(type, 'A');
            std::array<double, 3> position{}, expected{};
            for (auto &x : position)
                row >> x;
            for (auto &x : expected)
                row >> x;
            ASSERT_TRUE(row);
            const auto actual = base->acceleration(position, 0, 0, deltas);
            for (int axis = 0; axis < 3; ++axis) {
                max_a = std::max(max_a, std::abs(actual[axis] - expected[axis]));
                EXPECT_NEAR(actual[axis], expected[axis], 2e-14) << line;
            }
            ++accelerations;
        }
    }
    EXPECT_EQ((std::array{coefficients, accelerations, trajectories}), expected_counts);
    std::cout << name << " max component errors: coefficient=" << max_c << " acceleration=" << max_a
              << " m/s^2 position=" << max_r << " m velocity=" << max_v << " m/s\n";
}
class OceanTideFiles : public ::testing::Test {
  protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("of-ocean-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { std::filesystem::create_directories(root); }
    void TearDown() override { std::filesystem::remove_all(root); }
    std::string example() {
        std::ifstream in(references / "ocean_fes2014bv1_4x4.pot", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>{in}, {});
    }
    std::filesystem::path write(const std::string &bytes) {
        const auto path = root / "model.pot";
        std::ofstream out(path, std::ios::binary);
        out << bytes;
        return path;
    }
    application::Project project(bool enabled = true) {
        application::Project p{"Ocean tides", core::Epoch{0}, core::Epoch{600000000000LL}};
        application::ProjectSatellite s;
        s.orbit.first = p.first;
        s.orbit.last = p.last;
        s.orbit.propagator = application::OrbitPropagator::hpop;
        s.orbit.hpop.gravity_degree = s.orbit.hpop.gravity_order = 0;
        s.orbit.hpop.ocean_tides = {enabled, "custom-ocean", 4, 2};
        p.satellites.push_back(s);
        return p;
    }
};
} // namespace
TEST(OceanTideReference, RealFesExcerptMatchesOrekitCoefficientsForcesAndDayTrajectories) {
    const auto model = runtime::load_ocean_tides(references / "ocean_fes2014bv1_4x4.pot");
    EXPECT_EQ(model->info().sha256,
              "ed53a1225d6e720264bbe22207bbd60933518c4552cd09a0f98d8e0a98cf70d9");
    EXPECT_EQ(model->info().constituents, 36U);
    verify(model, "ocean_tides_small.txt", {84, 144, 4});
}
TEST(OceanTideReference, CombinedSolidAndOceanTruncationsMatchSeparateOrekitForces) {
    const auto model = runtime::load_ocean_tides(references / "ocean_fes2014bv1_4x4.pot");
    verify(model, "ocean_tides_combined.txt", {0, 0, 8}, true);
}
TEST(OceanTideReference, FullOfficialFesMatchesOrekitThroughDegreeOneHundred) {
    std::string source;
#ifdef _WIN32
    char *value = nullptr;
    std::size_t length = 0;
    const auto error = _dupenv_s(&value, &length, "ORBITFORGE_FULL_OCEAN_FILE");
    const std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
    ASSERT_EQ(error, 0);
    if (owned)
        source = owned.get();
#else
    if (const char *value = std::getenv("ORBITFORGE_FULL_OCEAN_FILE"))
        source = value;
#endif
    if (source.empty())
        GTEST_SKIP()
            << "Set ORBITFORGE_FULL_OCEAN_FILE to the pinned full FES2014bv1 potential file";
    const auto model = runtime::load_ocean_tides(source);
    ASSERT_EQ(model->info().sha256, runtime::fes2014bv1_sha256);
    EXPECT_EQ(model->info().degree, 100);
    EXPECT_EQ(model->info().order, 100);
    EXPECT_EQ(model->info().constituents, 36U);
    verify(model, "ocean_tides_full.txt", {216, 240, 12});
}
TEST_F(OceanTideFiles, LoaderChecksHeadersRecordsUnitsAndHashesTheOriginalBytes) {
    auto bytes = example();
    const auto first = runtime::load_ocean_tides(write(bytes));
    auto wrong = bytes;
    wrong.replace(wrong.find("10^-11"), 6, "10^-12");
    const auto scaled = runtime::load_ocean_tides(write(wrong));
    const Eop eop;
    const auto a = first->corrections(field(), eop.at(core::Epoch{0}).sample, 4, 4, false);
    const auto b = scaled->corrections(field(), eop.at(core::Epoch{0}).sample, 4, 4, false);
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_NEAR(a[i].c * .1, b[i].c, 1e-25);
        EXPECT_NEAR(a[i].s * .1, b[i].s, 1e-25);
    }
    EXPECT_NE(first->info().sha256, scaled->info().sha256);
    for (int test = 0; test < 5; ++test) {
        wrong = bytes;
        if (test == 0)
            wrong.replace(wrong.find("Stokes"), 6, "Height");
        if (test == 1)
            wrong.replace(wrong.find("DelC+"), 5, "Cnm");
        if (test == 2)
            wrong += "55.565 Om1 2 0 1 2 3 4\n";
        if (test == 3)
            wrong += "55.565 Om1 5 0 nan 2 3 4\n";
        if (test == 4) {
            const auto at = wrong.find(" 55.565 Om1   2   0");
            ASSERT_NE(at, std::string::npos);
            wrong.erase(at, wrong.find('\n', at) - at + 1);
        }
        EXPECT_THROW((void)runtime::load_ocean_tides(write(wrong)), std::exception) << test;
    }
    EXPECT_THROW((void)runtime::load_ocean_tides(write(bytes), {},
                                                 [] { throw std::runtime_error("cancel"); }),
                 std::runtime_error);
}
TEST_F(OceanTideFiles, RejectsMalformedModelAndUnitHeadersBeforeReadingCoefficients) {
    const auto original = example();
    const std::string model_header = "Ocean tide model: FES2014b up to (4,4)";
    const auto at = original.find(model_header);
    ASSERT_NE(at, std::string::npos);
    for (const std::string header : {
             "Ocean tide model:  up to (4,4)",
             "Ocean tide model: FES2014b up to (,4)",
             "Ocean tide model: FES2014b up to (4,)",
             "Ocean tide model: FES2014b up to (4,4,0)",
             "Ocean tide model: FES2014b up to (+4,4)",
             "Ocean tide model: FES2014b up to (4,-1)",
             "Ocean tide model: FES2014b up to (4, 4)",
             "Ocean tide model: FES2014b up to (4,4) trailing",
             "Ocean tide model: FES2014b up to (99999999999999999999,4)",
             "Ocean tide model: FES2014b up to (1,1)",
             "Ocean tide model: FES2014b up to (4,5)",
             "Ocean tide model: FES2014b up to (4,4",
             "Ocean tide model: FES2014b up to )",
         }) {
        auto changed = original;
        changed.replace(at, model_header.size(), header);
        EXPECT_THROW((void)runtime::load_ocean_tides(write(changed)), std::runtime_error) << header;
    }
    const auto exponent_at = original.find("10^-11)");
    ASSERT_NE(exponent_at, std::string::npos);
    for (const std::string exponent : {"10^-13)", "10^-11) trailing", "10^-11", "10^-011)"}) {
        auto changed = original;
        changed.replace(exponent_at, 7, exponent);
        EXPECT_THROW((void)runtime::load_ocean_tides(write(changed)), std::runtime_error)
            << exponent;
    }
    auto changed = original;
    changed.replace(at, model_header.size(), "Ocean tide model: Custom up to (test) up to (04,04)");
    changed.insert(0, "\xef\xbb\xbf# imported model\n");
    const auto parsed = runtime::load_ocean_tides(write(changed));
    EXPECT_EQ(parsed->info().model, "Custom up to (test)");
    EXPECT_EQ(parsed->info().degree, 4);
    EXPECT_EQ(parsed->info().order, 4);
}
TEST_F(OceanTideFiles, CatalogPublicationIsCancellablePinnedAndKeepsExistingSnapshots) {
    if (!runtime::data_catalog_available())
        GTEST_SKIP();
    runtime::DataCatalog catalog(root / "catalog");
    const auto file = write(example());
    const auto entry = catalog.install_ocean_tides(file, "custom-ocean");
    const auto frozen = catalog.load_ocean_tides("custom-ocean");
    EXPECT_EQ(entry.info.sha256, frozen->info().sha256);
    EXPECT_THROW((void)catalog.install_ocean_tides(file), std::runtime_error);
    auto changed = example();
    changed += "# new source bytes\n";
    const auto current_receipt = catalog.setting("ocean-tides/custom-ocean");
    int calls = 0;
    EXPECT_THROW((void)catalog.install_ocean_tides(write(changed), "custom-ocean", {},
                                                   runtime::DataAcquisition::imported,
                                                   [&] {
                                                       if (++calls == 4)
                                                           throw std::runtime_error("cancel");
                                                   }),
                 std::runtime_error);
    EXPECT_EQ(catalog.setting("ocean-tides/custom-ocean"), current_receipt);
    const auto updated = catalog.install_ocean_tides(write(changed), "custom-ocean");
    EXPECT_NE(updated.info.sha256, frozen->info().sha256);
    EXPECT_EQ(frozen->info().sha256, entry.info.sha256);
    EXPECT_THROW((void)catalog.install_ocean_tides(file, "../bad"), std::runtime_error);
    std::ofstream damage(updated.file, std::ios::binary);
    damage << example();
    damage.close();
    EXPECT_THROW((void)catalog.load_ocean_tides("custom-ocean"), std::runtime_error);
}
TEST_F(OceanTideFiles, ProjectAnalysisReportsAndDataChangesUseOneFrozenModel) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    const auto data = runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP)
                          .with_ocean_tide_file("custom-ocean", write(example()));
    application::ProjectDocument document{project()};
    const auto analysis = runtime::prepare(document, data);
    const auto id = document.objects().get("satellite-1");
    const auto original = analysis.state(id, project().last, core::FrameId::gcrf);
    runtime::save_project(document, root / "test.orbitforge", analysis);
    const auto saved = application::load_project(root / "test.orbitforge");
    ASSERT_EQ(saved.force_data.size(), 1U);
    EXPECT_EQ(saved.force_data[0].role, "ocean-tides/custom-ocean");
    EXPECT_EQ(saved.satellites[0].orbit.hpop.ocean_tides,
              project().satellites[0].orbit.hpop.ocean_tides);
    runtime::ReportContext context{saved, data.time(), data.frames(),           data.eop(), {},
                                   {},    {},          data.ocean_tide_models()};
    EXPECT_NO_THROW((void)context.force_environment(saved.satellites[0].orbit));
    EXPECT_NO_THROW((void)runtime::make_project_scenario(
        saved, data.time().get(), data.force_environment(saved.satellites[0].orbit)));
    auto changed = example();
    changed.replace(changed.find("4.34727"), 7, "8.34727");
    const auto next = data.with_ocean_tide_file("custom-ocean", write(changed));
    runtime::ProjectOpenPlan plan(root / "test.orbitforge", next);
    ASSERT_EQ(plan.data_changes().size(), 1U);
    EXPECT_EQ(plan.data_changes()[0].role, "ocean-tides/custom-ocean");
    EXPECT_THROW((void)plan.open(), runtime::OpenDataChangedError);
    EXPECT_NO_THROW((void)plan.open(true));
    context.ocean_tides = next.ocean_tide_models();
    EXPECT_THROW((void)context.force_environment(saved.satellites[0].orbit), std::runtime_error);
    EXPECT_EQ(analysis.state(id, project().last, core::FrameId::gcrf).position_m,
              original.position_m);
    EXPECT_THROW(
        (void)runtime::prepare(document, runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP)),
        runtime::AnalysisCapabilityError);
}
TEST_F(OceanTideFiles, InactiveDraftsRoundtripWithoutDataAndDoNotChangePhysicalIdentity) {
    auto p = project(false);
    const auto hash = application::analysis_input_sha256(p),
               report = runtime::report_input_sha256(p);
    p.satellites[0].orbit.hpop.ocean_tides = {false, "unused", 100, 12};
    EXPECT_EQ(application::analysis_input_sha256(p), hash);
    EXPECT_EQ(runtime::report_input_sha256(p), report);
    const auto saved = application::decode_project(application::encode_project(p));
    EXPECT_EQ(saved.satellites[0].orbit.hpop.ocean_tides, p.satellites[0].orbit.hpop.ocean_tides);
    EXPECT_NO_THROW((void)runtime::prepare(application::ProjectDocument{saved}));
    p.satellites[0].orbit.hpop.ocean_tides = {true, "unused", 1, 1};
    EXPECT_EQ(application::analysis_input_sha256(p), hash);
    EXPECT_EQ(runtime::report_input_sha256(p), report);
    p.satellites[0].orbit.hpop.ocean_tides = {true, "custom-ocean", 4, 2};
    EXPECT_NE(application::analysis_input_sha256(p), hash);
    auto q = p;
    q.satellites[0].orbit.hpop.ocean_tides.order = 1;
    EXPECT_NE(application::analysis_input_sha256(q), application::analysis_input_sha256(p));
}
