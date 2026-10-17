#include <cstdlib>
#include <fstream>
#include <full_gravity.hpp>
#include <gtest/gtest.h>
#include <memory>
#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/data_catalog.hpp>
#include <orbitforge/runtime/gravity.hpp>
#include <orbitforge/runtime/hpop.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <sstream>
using namespace orbitforge;
namespace {
class GravityFiles : public ::testing::Test {
  protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("orbitforge-gravity-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { std::filesystem::create_directories(root); }
    void TearDown() override { std::filesystem::remove_all(root); }
    std::filesystem::path write(const std::string &bytes) {
        const auto path = root / "input.grv";
        std::ofstream out(path, std::ios::binary);
        out << bytes;
        return path;
    }
    std::string tide_grv(const std::string &block) {
        auto text = grv();
        text.insert(text.find("BEGIN Coefficients"), block);
        return text;
    }
    std::string grv(std::string value = "-4.84165371736e-4", std::string norm = "Yes") {
        return "stk.v.6.0\nModel Test\nDegree 2\nOrder 2\nGm 3.986004415e14\nRefDistance "
               "6378136.3\n"
               "Normalized " +
               norm + "\nIncludesPermTide No\nBEGIN Coefficients\n2 0 " + value +
               " 0\nEND Coefficients\n";
    }
};
} // namespace
TEST_F(GravityFiles, ParsesStaticFormatsNormalizationAndHashesOriginalBytes) {
    const auto field = runtime::load_gravity(write(grv()));
    const auto unnormalized = runtime::load_gravity(write(grv("-1.0826266835531513e-3", "No")));
    const auto a = field->acceleration({4e6, -3e6, 5e6}, 2, 0);
    const auto b = unnormalized->acceleration({4e6, -3e6, 5e6}, 2, 0);
    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(a[i], b[i], 1e-14);
    EXPECT_EQ(field->info().degree, 2);
    const auto icgem = runtime::load_gravity(
        write("product_type gravity_field\nmodelname Test\nearth_gravity_constant 3.986004415e14\n"
              "radius 6378136.3\nmax_degree 2\ntide_system tide_free\nend_of_head\n"
              "gfc 0 0 1 0\ngfc 2 0 -4.84165371736D-4 0\ngfc 2 1 0 0\ngfc 2 2 0 0\n"));
    EXPECT_EQ(icgem->acceleration({4e6, -3e6, 5e6}, 2, 2), a);
    auto crlf = grv();
    for (std::size_t at = 0; (at = crlf.find('\n', at)) != std::string::npos; at += 2)
        crlf.insert(at, 1, '\r');
    EXPECT_NE(runtime::load_gravity(write(crlf))->info().sha256, field->info().sha256);
}
TEST_F(GravityFiles, RejectsTruncationTimeDependenceDuplicatesAndUnsupportedTides) {
    for (const auto &bad :
         {grv() + "BEGIN EarthIERSTideModel\n", grv("nan"), grv("-0.001", "Sometimes"),
          grv().substr(0, grv().find("END Coefficients")),
          grv().substr(0, grv().find("END Coefficients")) + "2 0 0 0\nEND Coefficients\n",
          std::string("product_type gravity_field\nmodelname T\nearth_gravity_constant "
                      "3.986004415e14\nradius 6378136.3\nmax_degree 2\ntide_system "
                      "tide_free\nend_of_head\ngfct 2 0 0 0\n")})
        EXPECT_THROW((void)runtime::load_gravity(write(bad)), std::runtime_error);
}
TEST_F(GravityFiles, CatalogFailureAndCancellationPreserveCurrentAndRunningResources) {
    if (!runtime::data_catalog_available())
        GTEST_SKIP();
    runtime::DataCatalog catalog(root / "catalog");
    const auto installed = catalog.install_gravity(write(grv()), "custom");
    auto frozen = catalog.load_gravity("custom");
    EXPECT_EQ(installed.info.sha256, frozen->info().sha256);
    EXPECT_THROW((void)catalog.install_gravity(write(grv()), "egm2008"), std::runtime_error);
    EXPECT_THROW((void)catalog.install_gravity(write(grv("-0.0006")), "custom", {},
                                               runtime::DataAcquisition::imported,
                                               [] { throw std::runtime_error("cancelled"); }),
                 std::runtime_error);
    EXPECT_EQ(catalog.load_gravity("custom")->info().sha256, installed.info.sha256);
    catalog.install_gravity(write(grv("-0.0006")), "custom");
    EXPECT_NE(catalog.load_gravity("custom")->info().sha256, frozen->info().sha256);
    EXPECT_EQ(frozen->info().sha256, installed.info.sha256);
    EXPECT_THROW((void)catalog.install_gravity(write(grv()), "../bad"), std::runtime_error);
}
TEST_F(GravityFiles, HpopSnapshotProjectRoundtripAndChangedDataConfirmation) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    auto data = runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP)
                    .with_gravity_file("custom", write(grv()));
    application::Project p{"Gravity", core::Epoch{0}, core::Epoch{600'000'000'000}};
    application::ProjectSatellite s;
    s.orbit.first = p.first;
    s.orbit.last = p.last;
    s.orbit.propagator = application::OrbitPropagator::hpop;
    s.orbit.hpop.gravity_model = "custom";
    s.orbit.hpop.gravity_degree = s.orbit.hpop.gravity_order = 2;
    p.satellites.push_back(s);
    application::ProjectDocument project{p};
    const auto analysis = runtime::prepare(project, data);
    const auto ref = project.objects().get("satellite-1");
    const auto original = analysis.state(ref, p.last, core::FrameId::gcrf);
    runtime::save_project(project, root / "scenario.orbitforge", analysis);
    const auto stored = application::load_project(root / "scenario.orbitforge");
    EXPECT_EQ(stored.force_data.size(), 1U);
    EXPECT_EQ(stored.data_lock(), data.data_lock());
    runtime::ReportContext context{stored, data.time(), data.frames(), data.eop(),
                                   data.gravity_models()};
    EXPECT_NO_THROW((void)context.force_environment(s.orbit));
    EXPECT_NO_THROW((void)runtime::make_project_scenario(stored, data.time().get(),
                                                         data.force_environment(s.orbit)));
    auto next = data.with_gravity_file("custom", write(grv("-0.0006")));
    context.gravity = next.gravity_models();
    EXPECT_THROW((void)context.force_environment(s.orbit), std::runtime_error);
    EXPECT_THROW((void)runtime::make_project_scenario(stored, data.time().get(),
                                                      next.force_environment(s.orbit)),
                 std::runtime_error);
    runtime::ProjectOpenPlan plan(root / "scenario.orbitforge", next);
    ASSERT_EQ(plan.data_changes().size(), 1U);
    EXPECT_EQ(plan.data_changes()[0].role, "gravity/custom");
    EXPECT_THROW((void)plan.open(), runtime::OpenDataChangedError);
    EXPECT_NO_THROW((void)plan.open(true));
    EXPECT_EQ(analysis.state(ref, p.last, core::FrameId::gcrf).position_m, original.position_m);
    EXPECT_THROW(
        (void)runtime::prepare(project, runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP)),
        runtime::AnalysisCapabilityError);
}

TEST_F(GravityFiles, LoadsHighestCoefficientAndRejectsRectangularTailDamage) {
    const auto source = grv().replace(grv().find("Degree 2"), 8, "Degree 2190");
    auto bytes = source;
    bytes.replace(bytes.find("Order 2"), 7, "Order 2159");
    bytes.insert(bytes.find("END Coefficients"), "2190 2159 2e-8 -3e-8\n");
    const auto model = runtime::load_gravity(write(bytes));
    EXPECT_EQ(model->info().degree, 2190);
    EXPECT_EQ(model->info().order, 2159);
    const core::GravityModel expected(model->info(),
                                      {{2, 0, -4.84165371736e-4, 0}, {2190, 2159, 2e-8, -3e-8}});
    EXPECT_EQ(model->acceleration({6378146.3, 0, 0}, 2190, 2159),
              expected.acceleration({6378146.3, 0, 0}, 2190, 2159));
    EXPECT_THROW((void)model->acceleration({7e6, 0, 0}, 2190, 2160), std::invalid_argument);
    const std::string header =
        "product_type gravity_field\nmodelname Rectangle\n"
        "earth_gravity_constant 3.986004415e14\nradius 6378136.3\nmax_degree 3\n"
        "tide_system tide_free\nend_of_head\n";
    const std::string rows = "gfc 2 0 -4.84165371736e-4 0\ngfc 2 1 1e-9 2e-9\n"
                             "gfc 3 0 1e-8 0\ngfc 3 1 2e-9 3e-9\n";
    EXPECT_EQ(runtime::load_gravity(write(header + rows))->info().order, 1);
    EXPECT_THROW((void)runtime::load_gravity(write(header + rows + "gfc 2 2 1e-8 2e-8\n")),
                 std::runtime_error);
    EXPECT_THROW((void)runtime::load_gravity(write(header + rows.substr(0, rows.find("gfc 3 1")))),
                 std::runtime_error);
}
TEST_F(GravityFiles, FullOfficialEgm2008MatchesIndependentOrekit) {
    std::string source;
#ifdef _WIN32
    char *value = nullptr;
    std::size_t length = 0;
    const auto error = _dupenv_s(&value, &length, "ORBITFORGE_FULL_GRAVITY_FILE");
    const std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
    ASSERT_EQ(error, 0);
    if (owned)
        source = owned.get();
#else
    if (const char *value = std::getenv("ORBITFORGE_FULL_GRAVITY_FILE"))
        source = value;
#endif
    if (source.empty())
        GTEST_SKIP() << "Set ORBITFORGE_FULL_GRAVITY_FILE to the pinned complete EGM2008.gfc";
    int checkpoints = 0;
    const auto model = runtime::load_gravity(source, {}, [&] { ++checkpoints; });
    ASSERT_EQ(model->info().sha256,
              "ab5b524da073e63b5bdceb7ca47a0de07a26dd44a1c5798f39fc98dc80af70fd");
    EXPECT_EQ(model->info().degree, 2190);
    // The pinned ICGEM export explicitly pads orders 2160..2190 with zeros.
    // Keep its complete coefficient extent, and verify the published 2159-order
    // truncation is identical to evaluating every record in the square export.
    EXPECT_EQ(model->info().order, 2190);
    for (const auto position : {std::array{6378146.3, 0., 0.}, std::array{.001, -.002, 6356752.3}})
        EXPECT_EQ(model->acceleration(position, 2190, 2159),
                  model->acceleration(position, 2190, 2190));
    EXPECT_GT(checkpoints, 2000);
    full_gravity_test::verify(model, 3, ORBITFORGE_EGM2008_FULL_REFERENCE, 80, 2);
}

TEST_F(GravityFiles, ReadsEarthIersAndLegacyLoveNumbersWithExplicitTideSystem) {
    const auto defaults = runtime::load_gravity(write(grv()));
    EXPECT_EQ(defaults->info().solid_tide_k20, .30190);
    for (const std::string block : {"Begin EarthIERSTideModel\nIncludesPermTide No\nLoveNumbers 2 "
                                    "0 +3.0D-1 0.12\nEnd EarthIERSTideModel\n",
                                    "Begin LoveNumbers\n2 0 0.3 -0.12\nEnd LoveNumbers\n"}) {
        const auto bytes = tide_grv("CentralBody Earth\n" + block);
        const auto model = runtime::load_gravity(write(bytes));
        EXPECT_EQ(model->info().solid_tide_k20, .3);
        EXPECT_EQ(model->info().tide_system, "tide_free");
        // File metadata cannot alter the immutable static field.
        EXPECT_EQ(model->acceleration({4e6, -3e6, 5e6}, 2, 2),
                  defaults->acceleration({4e6, -3e6, 5e6}, 2, 2));
        EXPECT_NE(model->info().sha256, defaults->info().sha256);
    }
    auto block_only = tide_grv("Begin EarthIERSTideModel\nIncludesPermTide Yes\n"
                               "LoveNumbers 2 0 0 0\nEnd EarthIERSTideModel\n");
    block_only.erase(block_only.find("IncludesPermTide No\n"),
                     std::string("IncludesPermTide No\n").size());
    const auto zero = runtime::load_gravity(write(block_only));
    EXPECT_EQ(zero->info().tide_system, "zero_tide");
    EXPECT_EQ(zero->info().solid_tide_k20, 0);
    EXPECT_EQ(
        runtime::load_gravity(write(tide_grv("Begin EarthIERSTideModel\nEnd EarthIERSTideModel\n")))
            ->info()
            .solid_tide_k20,
        .30190);
}
TEST_F(GravityFiles, RejectsAmbiguousMalformedAndUnsupportedTideBlocks) {
    for (const std::string block :
         {"Begin EarthIERSTideModel\nIncludesPermTide Yes\nEnd EarthIERSTideModel\n",
          "Begin EarthIERSTideModel\nLoveNumbers 2 1 .3 0\nEnd EarthIERSTideModel\n",
          "Begin EarthIERSTideModel\nLoveNumbers 2 0 nan 0\nEnd EarthIERSTideModel\n",
          "Begin EarthIERSTideModel\nLoveNumbers 2 0 .3 inf\nEnd EarthIERSTideModel\n",
          "Begin EarthIERSTideModel\nLoveNumbers 2 0 .3\nEnd EarthIERSTideModel\n",
          "Begin EarthIERSTideModel\nLoveNumbers 2 0 .3 0\nLoveNumbers 2 0 .4 0\nEnd "
          "EarthIERSTideModel\n",
          "Begin EarthIERSTideModel\nIncludesPermTide No\nIncludesPermTide No\nEnd "
          "EarthIERSTideModel\n",
          "Begin EarthIERSTideModel\nLoveNumbers 2 0 .3 0\nEnd LoveNumbers\n",
          "Begin EarthIERSTideModel\nLoveNumbers 2 0 .3 0\n",
          "Begin EarthIERSTideModel\nK3 .093\nEnd EarthIERSTideModel\n",
          "Begin EarthIERSTideModel\nEnd EarthIERSTideModel\nBegin LoveNumbers\n2 0 .3 0\nEnd "
          "LoveNumbers\n",
          "Begin EarthIERSTideModel\nBegin LoveNumbers\n2 0 .3 0\nEnd LoveNumbers\nEnd "
          "EarthIERSTideModel\n",
          "Begin LoveNumbers\nEnd LoveNumbers\n",
          "Begin SimpleTideModel\nK2 .3\nEnd SimpleTideModel\n",
          "Begin ProcessNoise\nEnd ProcessNoise\n", "CentralBody Moon\n",
          "ReferenceFrame PrincipalAxes_430\n"}) {
        EXPECT_THROW((void)runtime::load_gravity(write(tide_grv(block))), std::runtime_error)
            << block;
    }
}
TEST_F(GravityFiles, LoveNumberCatalogReopenCancellationAndFailedImportPreservePriorResource) {
    if (!runtime::data_catalog_available())
        GTEST_SKIP();
    const std::string custom = tide_grv("Begin EarthIERSTideModel\n"
                                        "LoveNumbers 2 0 .3 0\nEnd EarthIERSTideModel\n");
    std::shared_ptr<const core::GravityModel> frozen;
    {
        runtime::DataCatalog catalog(root / "catalog");
        const auto installed = catalog.install_gravity(write(custom), "custom");
        EXPECT_EQ(installed.info.solid_tide_k20, .3);
        frozen = catalog.load_gravity("custom");
        EXPECT_THROW(
            (void)catalog.install_gravity(
                write(tide_grv(
                    "Begin EarthIERSTideModel\nLoveNumbers 3 0 .4 0\nEnd EarthIERSTideModel\n")),
                "custom"),
            std::runtime_error);
        EXPECT_THROW((void)catalog.install_gravity(write(grv()), "custom", {},
                                                   runtime::DataAcquisition::imported,
                                                   [] { throw std::runtime_error("cancelled"); }),
                     std::runtime_error);
        EXPECT_EQ(catalog.gravity_models().front().info.solid_tide_k20, .3);
        EXPECT_EQ(catalog.load_gravity("custom")->info().sha256, frozen->info().sha256);
    }
    runtime::DataCatalog reopened(root / "catalog");
    EXPECT_EQ(reopened.gravity_models().front().info.solid_tide_k20, .3);
    EXPECT_EQ(reopened.load_gravity("custom")->info().solid_tide_k20, .3);
    reopened.install_gravity(write(grv()), "custom");
    EXPECT_EQ(reopened.load_gravity("custom")->info().solid_tide_k20, .30190);
    EXPECT_EQ(frozen->info().solid_tide_k20, .3);
    const auto receipt = reopened.setting("gravity/custom");
    reopened.set_setting("gravity/custom", R"({"schema":1})");
    EXPECT_THROW((void)reopened.gravity_models(), std::runtime_error);
    reopened.set_setting("gravity/custom", receipt);
    EXPECT_EQ(reopened.load_gravity("custom")->info().solid_tide_k20, .30190);
}
TEST_F(GravityFiles, LoveNumberOnlyResourceChangeRequiresConfirmationAndPreservesOldAnalysis) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    auto data =
        runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP)
            .with_gravity_file("custom",
                               write(tide_grv("Begin EarthIERSTideModel\n"
                                              "LoveNumbers 2 0 .3 0\nEnd EarthIERSTideModel\n")));
    application::Project p{"Love numbers", core::Epoch{0}, core::Epoch{600'000'000'000}};
    application::ProjectSatellite sat;
    sat.orbit.first = p.first;
    sat.orbit.last = p.last;
    sat.orbit.propagator = application::OrbitPropagator::hpop;
    sat.orbit.hpop.gravity_model = "custom";
    sat.orbit.hpop.gravity_degree = sat.orbit.hpop.gravity_order = 2;
    sat.orbit.hpop.solid_tides.mode = core::SolidTideMode::permanent;
    p.satellites.push_back(sat);
    application::ProjectDocument project{p};
    const auto before = runtime::prepare(project, data);
    const auto object = project.objects().get("satellite-1");
    const auto state = before.state(object, p.last, core::FrameId::gcrf);
    runtime::save_project(project, root / "love.orbitforge", before);
    auto changed = data.with_gravity_file("custom", write(grv()));
    runtime::ProjectOpenPlan plan(root / "love.orbitforge", changed);
    ASSERT_EQ(plan.data_changes().size(), 1U);
    EXPECT_EQ(plan.data_changes().front().role, "gravity/custom");
    EXPECT_THROW((void)plan.open(), runtime::OpenDataChangedError);
    auto confirmed = plan.open(true);
    const auto after = runtime::prepare(confirmed, changed);
    EXPECT_NE(
        after.state(confirmed.objects().get("satellite-1"), p.last, core::FrameId::gcrf).position_m,
        state.position_m);
    EXPECT_EQ(before.state(object, p.last, core::FrameId::gcrf).position_m, state.position_m);
}
