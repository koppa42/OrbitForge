#include <algorithm>
#include <cmath>
#include <fstream>
#include <full_gravity.hpp>
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <numbers>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/hpop.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <sstream>
using namespace orbitforge;
namespace {
const std::filesystem::path references{ORBITFORGE_SECULAR_REFERENCE};
class Eop final : public core::IEarthOrientationProvider {
    core::EopDataInfo info_{"reference",
                            {},
                            {},
                            {},
                            {},
                            core::Epoch{-4000000000000000000LL},
                            core::Epoch{4000000000000000000LL},
                            {"test", {}, {}, 0, 100000}};

  public:
    core::EarthOrientationState at(core::Epoch e) const override {
        return {{e, 0, 0, 0, 0, 0, "test"}, {}};
    }
    const core::EopDataInfo &data_info() const noexcept override { return info_; }
};
std::shared_ptr<const core::GravityModel> model(int profile = 0) {
    return runtime::load_gravity(references /
                                 ("gravity_secular_" + std::to_string(profile) + ".grv"));
}
core::HpopEnvironment environment(std::shared_ptr<const core::GravityModel> field = model()) {
    return {std::make_shared<full_gravity_test::Frames>(), std::make_shared<Eop>(),
            std::move(field)};
}
core::CartesianState initial(core::Epoch at = core::Epoch{0}) {
    return {at, core::OriginId::earth, core::FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
}
core::HpopOptions options() {
    core::HpopOptions result;
    result.gravity_model = "custom";
    result.include_secular_variations = true;
    result.gravity_degree = 8;
    result.gravity_order = 4;
    result.relative_tolerance = 1e-14;
    result.position_tolerance_m = 1e-8;
    result.velocity_tolerance_mps = 1e-11;
    result.max_step_seconds = 30;
    return result;
}
std::string example() {
    std::ifstream f(references / "gravity_secular_0.grv", std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
class SecularFiles : public ::testing::Test {
  protected:
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("of-secular-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { std::filesystem::create_directories(root); }
    void TearDown() override { std::filesystem::remove_all(root); }
    std::filesystem::path write(const std::string &bytes) {
        const auto path = root / "input.grv";
        std::ofstream out(path, std::ios::binary);
        out << bytes;
        return path;
    }
    std::string block(const std::string &contents) {
        auto text = example();
        const auto first = text.find("Begin SecularVariations"),
                   last = text.find("End SecularVariations") +
                          std::string("End SecularVariations\n").size();
        text.replace(first, last - first,
                     "Begin SecularVariations\n" + contents + "End SecularVariations\n");
        return text;
    }
};
} // namespace
TEST(SecularGravity, IndependentOrekitCoefficientsAccelerationAndBidirectionalTrajectories) {
    const std::array models{model(0), model(1), model(2)};
    std::ifstream input(references / "gravity_secular.txt");
    ASSERT_TRUE(input);
    int coefficients = 0, accelerations = 0, trajectories = 0;
    double max_c = 0, max_a = 0, max_r = 0, max_v = 0;
    for (std::string line; std::getline(input, line);) {
        if (line.empty() || line.front() == '#')
            continue;
        std::istringstream row(line);
        char type = 0;
        int profile = 0, degree = 0, order = 0;
        std::int64_t ticks = 0;
        row >> type >> profile >> ticks >> degree >> order;
        ASSERT_TRUE(row);
        ASSERT_GE(profile, 0);
        ASSERT_LT(profile, 3);
        const auto &field = models[profile];
        const core::Epoch epoch{ticks};
        SCOPED_TRACE(line);
        if (type == 'C') {
            double c = 0, s = 0;
            row >> c >> s;
            const auto actual = field->secular_corrections(epoch, 8, 4);
            const auto found = std::ranges::find_if(
                actual, [&](const auto &v) { return v.degree == degree && v.order == order; });
            ASSERT_NE(found, actual.end());
            max_c = std::max({max_c, std::abs(found->c - c), std::abs(found->s - s)});
            EXPECT_NEAR(found->c, c, 2e-23);
            EXPECT_NEAR(found->s, s, 2e-23);
            ++coefficients;
        } else {
            int enabled = 0;
            row >> enabled;
            if (type == 'A') {
                std::array<double, 3> r{}, expected{};
                for (auto &v : r)
                    row >> v;
                for (auto &v : expected)
                    row >> v;
                const auto actual = field->acceleration(
                    r, degree, order, {}, enabled ? std::optional{epoch} : std::nullopt);
                for (std::size_t i = 0; i < 3; ++i) {
                    max_a = std::max(max_a, std::abs(actual[i] - expected[i]));
                    EXPECT_NEAR(actual[i], expected[i], 2e-13);
                }
                ++accelerations;
            } else {
                ASSERT_EQ(type, 'T');
                std::int64_t elapsed = 0;
                std::array<double, 3> r{}, v{};
                row >> elapsed;
                for (auto &x : r)
                    row >> x;
                for (auto &x : v)
                    row >> x;
                auto o = options();
                o.gravity_degree = degree;
                o.gravity_order = order;
                o.include_secular_variations = enabled != 0;
                const auto provider = core::make_hpop(initial(epoch), o, environment(field));
                const auto at = core::Epoch{ticks + elapsed * 1000000000LL};
                const auto actual = provider->state_at(at);
                for (std::size_t i = 0; i < 3; ++i) {
                    max_r = std::max(max_r, std::abs(actual.position_m[i] - r[i]));
                    max_v = std::max(max_v, std::abs(actual.velocity_mps[i] - v[i]));
                    EXPECT_NEAR(actual.position_m[i], r[i], 1e-3);
                    EXPECT_NEAR(actual.velocity_mps[i], v[i], 1e-6);
                }
                // Earlier queries cannot overwrite the epoch-anchored cached trajectory.
                (void)provider->state_at(core::Epoch{ticks - 600000000000LL});
                EXPECT_EQ(provider->state_at(at).position_m, actual.position_m);
                ++trajectories;
            }
        }
        ASSERT_TRUE(row);
    }
    EXPECT_EQ(coefficients, 48);
    EXPECT_EQ(accelerations, 288);
    EXPECT_EQ(trajectories, 48);
    std::cout << "Secular gravity max component errors: " << max_c << " coefficients, " << max_a
              << " m/s^2, " << max_r << " m, " << max_v << " m/s\n";
}
TEST(SecularGravity, ExplicitEpochImmutableMetadataAndIndependentTruncation) {
    const auto field = model();
    ASSERT_TRUE(field->info().secular_variations);
    const auto epoch = field->info().secular_variations->reference_epoch;
    EXPECT_EQ(epoch.tai_nanoseconds(), -32184000000LL);
    const auto at_reference = field->secular_corrections(epoch, 8, 4);
    for (const auto &v : at_reference) {
        EXPECT_EQ(v.c, 0);
        EXPECT_EQ(v.s, 0);
    }
    const std::array<double, 3> r{4e6, -3e6, 5e6};
    EXPECT_EQ(field->acceleration(r, 8, 4), field->acceleration(r, 8, 4, {}, epoch));
    EXPECT_TRUE(field->secular_corrections(core::Epoch{0}, 0, 0).empty());
    EXPECT_EQ(field->secular_corrections(core::Epoch{0}, 2, 0).size(), 1U);
    EXPECT_EQ(field->secular_corrections(core::Epoch{0}, 2, 1).size(), 2U);
    EXPECT_EQ(model(1)->info().secular_variations->reference_epoch.tai_nanoseconds(),
              -441849632184000000LL);
    EXPECT_EQ(model(2)->info().secular_variations->reference_epoch.tai_nanoseconds(),
              10634482570667LL);
    auto info = field->info();
    const auto ref = runtime::gravity_reference("custom", info);
    info.secular_variations.value().reference_epoch = core::Epoch{0};
    EXPECT_NE(runtime::gravity_reference("custom", info), ref);
    EXPECT_EQ(field->info().secular_variations->reference_epoch, epoch);
    auto changed_rate = field->info();
    auto &rates = changed_rate.secular_variations.value().rates_per_year;
    ASSERT_FALSE(rates.empty());
    rates.front().c *= 2;
    EXPECT_NE(runtime::gravity_reference("custom", changed_rate), ref);
    auto high_info = core::builtin_egm96()->info();
    high_info.degree = high_info.order = 2190;
    high_info.secular_variations =
        core::GravitySecularVariations{epoch, {{2190, 2159, 1e-12, -2e-12}}};
    core::GravityModel high(high_info, {});
    EXPECT_TRUE(high.secular_corrections(core::Epoch{0}, 2189, 2189).empty());
    EXPECT_TRUE(high.secular_corrections(core::Epoch{0}, 2190, 2158).empty());
    EXPECT_EQ(high.secular_corrections(core::Epoch{0}, 2190, 2159).size(), 1U);
    EXPECT_THROW((void)high.secular_corrections(epoch, 2191, 0), std::invalid_argument);
}
TEST(SecularGravity, RejectsInvalidRateMetadataAndOverflowAndRequiresSelectedResource) {
    auto info = model()->info();
    for (const auto &rates : std::vector<std::vector<core::GravityCoefficient>>{
             {},
             {{1, 0, 1e-11, 0}},
             {{9, 0, 1e-11, 0}},
             {{8, 5, 1e-11, 0}},
             {{2, 0, 0, 1e-11}},
             {{2, 1, 0, 0}, {2, 1, 1e-11, 0}},
             {{2, 0, std::numeric_limits<double>::infinity(), 0}}}) {
        info.secular_variations.value().rates_per_year = rates;
        EXPECT_THROW((void)core::GravityModel(info, {}), std::invalid_argument);
    }
    auto &overflow_rates = info.secular_variations.value().rates_per_year;
    overflow_rates.clear();
    overflow_rates.push_back({2, 0, 1e308, 0});
    const core::GravityModel huge(info, {});
    EXPECT_THROW(
        (void)huge.secular_corrections(core::Epoch{std::numeric_limits<std::int64_t>::max()}, 2, 0),
        core::PropagationError);
    auto o = options();
    EXPECT_THROW((void)core::make_hpop(initial(), o, environment(core::builtin_egm96())),
                 std::invalid_argument);
    o.gravity_model.clear();
    o.gravity_degree = o.gravity_order = 0;
    const auto enabled = core::make_hpop(initial(), o);
    o.include_secular_variations = false;
    const auto disabled = core::make_hpop(initial(), o);
    EXPECT_EQ(enabled->state_at(core::Epoch{600000000000LL}).position_m,
              disabled->state_at(core::Epoch{600000000000LL}).position_m);
    auto extremes = model()->info();
    extremes.secular_variations->reference_epoch =
        core::Epoch{std::numeric_limits<std::int64_t>::min()};
    const core::GravityModel extreme(extremes, {});
    EXPECT_NO_THROW((void)extreme.secular_corrections(
        core::Epoch{std::numeric_limits<std::int64_t>::max()}, 8, 4));
}
TEST_F(SecularFiles, ParsesWholeBlockBeforeOrAfterCoefficientsAndPreservesLiteralTtEpoch) {
    const std::string rate = "LinearRate Cos 2 0 1.162755D-11\n";
    for (const char *epoch : {"2451545", "+2451545.0", "2.451545D+6", "24515450e-1"}) {
        const auto field =
            runtime::load_gravity(write(block("RefEpochTDT " + std::string(epoch) + "\n" + rate)));
        EXPECT_EQ(field->info().secular_variations->reference_epoch.tai_nanoseconds(),
                  -32184000000LL);
    }
    auto text = example();
    const auto first = text.find("Begin SecularVariations"), last = text.find("Begin Coefficients");
    const auto secular = text.substr(first, last - first);
    text.erase(first, last - first);
    text += secular;
    const auto after = runtime::load_gravity(write(text));
    const auto bom = runtime::load_gravity(write("\xef\xbb\xbf" + text));
    EXPECT_EQ(after->acceleration({7e6, 0, 0}, 8, 4, {}, core::Epoch{0}),
              model()->acceleration({7e6, 0, 0}, 8, 4, {}, core::Epoch{0}));
    EXPECT_NE(after->info().sha256, bom->info().sha256);
    EXPECT_EQ(after->info().secular_variations->reference_epoch,
              bom->info().secular_variations->reference_epoch);
}
TEST_F(SecularFiles, RejectsIncompleteDuplicateMalformedAndOutOfRangeSecularBlocks) {
    const std::string epoch = "RefEpochTDT 2451545\n", rate = "LinearRate Cos 2 0 1e-11\n";
    for (const auto &content : std::vector<std::string>{"",
                                                        epoch,
                                                        rate,
                                                        epoch + epoch + rate,
                                                        epoch + rate + rate,
                                                        epoch + "LinearRate Bad 2 0 1e-11\n",
                                                        epoch + "LinearRate Cos 2 0 nan\n",
                                                        epoch + "LinearRate Cos 2 0 inf\n",
                                                        epoch + "LinearRate Cos 2 0 1e-11 0\n",
                                                        epoch + "LinearRate Cos 1 0 1e-11\n",
                                                        epoch + "LinearRate Sin 2 0 1e-11\n",
                                                        epoch + "LinearRate Cos 9 0 1e-11\n",
                                                        epoch + "LinearRate Cos 8 5 1e-11\n",
                                                        epoch + "LinearRate Cos 2191 0 1e-11\n",
                                                        epoch + "Periodic 1\n",
                                                        "RefEpochTDT nan\n" + rate,
                                                        "RefEpochTDT 999999999999999\n" + rate,
                                                        "RefEpochTDT 2344792\n" + rate,
                                                        "RefEpochTDT 2558297\n" + rate,
                                                        "RefEpochTDT 2451545 0\n" + rate,
                                                        epoch + "Begin Coefficients\n" + rate}) {
        EXPECT_THROW((void)runtime::load_gravity(write(block(content))), std::runtime_error)
            << content;
    }
    EXPECT_THROW((void)runtime::load_gravity(write(example() + "Begin SecularVariations\n" + epoch +
                                                   rate + "End SecularVariations\n")),
                 std::runtime_error);
    auto missing_end = example();
    const auto at = missing_end.find("End SecularVariations");
    missing_end.erase(at, std::string("End SecularVariations").size());
    EXPECT_THROW((void)runtime::load_gravity(write(missing_end)), std::runtime_error);
}
TEST_F(SecularFiles, CatalogMetadataAndCancellationPreserveFrozenResources) {
    if (!runtime::data_catalog_available())
        GTEST_SKIP();
    std::shared_ptr<const core::GravityModel> frozen;
    {
        runtime::DataCatalog catalog(root / "catalog");
        const auto receipt = catalog.install_gravity(write(example()), "custom");
        ASSERT_TRUE(receipt.info.secular_variations);
        frozen = catalog.load_gravity("custom");
        const auto saved = catalog.setting("gravity/custom");
        EXPECT_THROW((void)catalog.install_gravity(write(block("RefEpochTDT 2451545\n")), "custom"),
                     std::runtime_error);
        EXPECT_THROW((void)catalog.install_gravity(write(example()), "custom", {},
                                                   runtime::DataAcquisition::imported,
                                                   [] { throw std::runtime_error("cancel"); }),
                     std::runtime_error);
        EXPECT_EQ(catalog.setting("gravity/custom"), saved);
    }
    runtime::DataCatalog catalog(root / "catalog");
    const auto receipt = catalog.gravity_models().front();
    ASSERT_TRUE(receipt.info.secular_variations);
    EXPECT_EQ(receipt.info.secular_variations->reference_epoch.tai_nanoseconds(), -32184000000LL);
    EXPECT_EQ(receipt.info.secular_variations->rates_per_year.size(), 4U);
    const auto saved = catalog.setting("gravity/custom");
    auto tampered = saved;
    const std::string original_epoch = "\"epoch_tai_ns\":-32184000000";
    const auto location = tampered.find(original_epoch);
    ASSERT_NE(location, std::string::npos);
    tampered.replace(location, original_epoch.size(), "\"epoch_tai_ns\":0");
    catalog.set_setting("gravity/custom", tampered);
    EXPECT_THROW((void)catalog.load_gravity("custom"), std::runtime_error);
    EXPECT_EQ(frozen->info().secular_variations->reference_epoch.tai_nanoseconds(), -32184000000LL);
    catalog.set_setting("gravity/custom", saved);
    EXPECT_EQ(catalog.load_gravity("custom")->info().sha256, frozen->info().sha256);
    auto changed = example();
    changed.replace(changed.find("2451545.0"), 9, "2453371.5");
    const auto updated = catalog.install_gravity(write(changed), "custom");
    EXPECT_NE(updated.info.sha256, frozen->info().sha256);
    EXPECT_EQ(frozen->info().secular_variations->reference_epoch.tai_nanoseconds(), -32184000000LL);
    EXPECT_NE(catalog.load_gravity("custom")->info().secular_variations->reference_epoch,
              frozen->info().secular_variations->reference_epoch);
}
TEST_F(SecularFiles, ProjectReportsAndReprepareRequireConfirmationForEpochChanges) {
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    auto data = runtime::DataSnapshot::load(ORBITFORGE_REPORT_EOP)
                    .with_gravity_file("custom", write(example()));
    application::Project p{"Secular gravity", core::Epoch{0}, core::Epoch{600000000000LL}};
    application::ProjectSatellite sat;
    sat.orbit.first = p.first;
    sat.orbit.last = p.last;
    sat.orbit.propagator = application::OrbitPropagator::hpop;
    sat.orbit.hpop = options();
    p.satellites.push_back(sat);
    application::ProjectDocument document{p};
    const auto analysis = runtime::prepare(document, data);
    const auto object = document.objects().get("satellite-1");
    const auto state = analysis.state(object, p.last, core::FrameId::gcrf);
    runtime::save_project(document, root / "scenario.orbitforge", analysis);
    const auto stored = application::load_project(root / "scenario.orbitforge");
    EXPECT_TRUE(stored.satellites[0].orbit.hpop.include_secular_variations);
    runtime::ReportContext context{stored, data.time(), data.frames(), data.eop(),
                                   data.gravity_models()};
    const auto session = runtime::make_hpop_session(sat.orbit.initial, options(),
                                                    context.force_environment(sat.orbit));
    EXPECT_NE(session.provenance().algorithm.find("secular=file-linear-julian-year-v1"),
              std::string::npos);
    auto changed = example();
    changed.replace(changed.find("2451545.0"), 9, "2453371.5");
    const auto next = data.with_gravity_file("custom", write(changed));
    runtime::ProjectOpenPlan plan(root / "scenario.orbitforge", next);
    ASSERT_EQ(plan.data_changes().size(), 1U);
    EXPECT_EQ(plan.data_changes()[0].role, "gravity/custom");
    EXPECT_THROW((void)plan.open(), runtime::OpenDataChangedError);
    auto accepted = plan.open(true);
    const auto after = runtime::prepare(accepted, next);
    EXPECT_NE(
        after.state(accepted.objects().get("satellite-1"), p.last, core::FrameId::gcrf).position_m,
        state.position_m);
    context.gravity = next.gravity_models();
    EXPECT_THROW((void)context.force_environment(sat.orbit), std::runtime_error);
    EXPECT_EQ(analysis.state(object, p.last, core::FrameId::gcrf).position_m, state.position_m);
    const auto active_input = application::analysis_input_sha256(p),
               active_report = runtime::report_input_sha256(p);
    p.satellites[0].orbit.hpop.include_secular_variations = false;
    EXPECT_NE(application::analysis_input_sha256(p), active_input);
    EXPECT_NE(runtime::report_input_sha256(p), active_report);
    p.satellites[0].orbit.hpop.gravity_degree = p.satellites[0].orbit.hpop.gravity_order = 0;
    const auto inactive_input = application::analysis_input_sha256(p),
               inactive_report = runtime::report_input_sha256(p);
    p.satellites[0].orbit.hpop.include_secular_variations = true;
    EXPECT_EQ(application::analysis_input_sha256(p), inactive_input);
    EXPECT_EQ(runtime::report_input_sha256(p), inactive_report);
    EXPECT_TRUE(application::decode_project(application::encode_project(p))
                    .satellites[0]
                    .orbit.hpop.include_secular_variations);
}

namespace {
class TideEop final : public core::IEarthOrientationProvider {
    Eop broad_;

  public:
    core::EarthOrientationState at(core::Epoch e) const override {
        return {{e, -31.7, .12 * std::numbers::pi / 648000, .25 * std::numbers::pi / 648000, 0, 0,
                 "test"},
                {}};
    }
    const core::EopDataInfo &data_info() const noexcept override { return broad_.data_info(); }
};
class TideEphemeris final : public core::IThirdBodyEphemeris {
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
} // namespace
TEST(SecularGravity, IndependentOrekitThreeForceCompositionKeepsEveryCorrectionAndCutoff) {
    const auto ocean = runtime::load_ocean_tides(references / "ocean_fes2014bv1_4x4.pot");
    std::ifstream input(references / "gravity_secular_tides.txt");
    ASSERT_TRUE(input);
    int count = 0;
    double max_r = 0, max_v = 0;
    for (std::string line; std::getline(input, line);) {
        if (line.empty() || line.front() == '#')
            continue;
        std::istringstream row(line);
        char type = 0;
        int profile = 0;
        std::int64_t elapsed = 0;
        auto o = options();
        row >> type >> profile >> o.gravity_degree >> o.gravity_order >> elapsed;
        ASSERT_EQ(type, 'X');
        o.solid_tides = {core::SolidTideMode::full, false, false};
        o.ocean_tides = {true, "custom-ocean", 4, 4};
        auto env = environment(model(profile));
        env.eop = std::make_shared<TideEop>();
        env.ephemeris = std::make_shared<TideEphemeris>();
        env.ocean_tides = ocean;
        const auto actual =
            core::make_hpop(initial(), o, env)->state_at(core::Epoch{elapsed * 1000000000LL});
        std::array<double, 3> r{}, v{};
        for (auto &x : r)
            row >> x;
        for (auto &x : v)
            row >> x;
        ASSERT_TRUE(row);
        for (std::size_t i = 0; i < 3; ++i) {
            max_r = std::max(max_r, std::abs(actual.position_m[i] - r[i]));
            max_v = std::max(max_v, std::abs(actual.velocity_mps[i] - v[i]));
            EXPECT_NEAR(actual.position_m[i], r[i], 1e-3) << line;
            EXPECT_NEAR(actual.velocity_mps[i], v[i], 1e-6) << line;
        }
        ++count;
    }
    EXPECT_EQ(count, 8);
    std::cout << "Secular + solid + ocean max component errors: " << max_r << " m, " << max_v
              << " m/s\n";
}
