#include <chrono>
#include <cmath>
#include <cspice/SpiceUsr.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <orbitforge/providers/spice/provider.hpp>
#include <picosha2.h>
#ifdef ORBITFORGE_SPICE_TEST_ERFA
#include <erfa.h>
#include <orbitforge/providers/erfa/provider.hpp>
#endif

using namespace orbitforge;
namespace {
using Json = nlohmann::json;
std::string read(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
void spice_ok() {
    if (failed_c()) {
        char message[2048]{};
        getmsg_c("LONG", sizeof(message), message);
        reset_c();
        throw std::runtime_error(message);
    }
}
void write_kernel(const std::filesystem::path &path, double offset) {
    // Test-only construction, in a single thread before any Provider is created.
    std::filesystem::remove(path);
    char action[] = "RETURN", report[] = "NONE";
    erract_c("SET", 0, action);
    errprt_c("SET", 0, report);
    SpiceInt handle = 0;
    spkopn_c(path.string().c_str(), "OrbitForge synthetic Chebyshev cubic", 0, &handle);
    spice_ok();
    const double coefficients[]{7000 + offset, 10, 2, 0.5, -1200, 0.2, 3, 0, 500, -0.3, 0, 0.6};
    spkw02_c(handle, 100001, 399, "J2000", -3600, 3600, "OrbitForge synthetic cubic", 7200, 1, 3,
             coefficients, -3600);
    spice_ok();
    spkcls_c(handle);
    spice_ok();
}
struct Fixture {
    std::filesystem::path root;
    std::filesystem::path manifest;
    std::filesystem::path workspace;
    Json document;
    Fixture() {
        root = std::filesystem::path(ORBITFORGE_SPICE_TEST_DIR) /
               ::testing::UnitTest::GetInstance()->current_test_info()->name();
        std::filesystem::create_directories(root);
        manifest = root / "manifest.json";
        workspace = root / "runtime";
        std::filesystem::copy_file(
            std::filesystem::path(ORBITFORGE_SPICE_SAMPLE).parent_path() / "naif0012.tls",
            root / "naif0012.tls", std::filesystem::copy_options::overwrite_existing);
        write_kernel(root / "a.bsp", 0);
        write_kernel(root / "b.bsp", 100);
        document = {
            {"schema_version", 1},
            {"snapshot_id", "synthetic-cubic"},
            {"model", "artificial-Chebyshev-cubic"},
            {"first_tai_ns", -3600000000000LL},
            {"last_tai_ns", 3600000000000LL},
            {"kernels", Json::array({kernel("naif0012.tls", "LSK"), kernel("a.bsp", "SPK")})}};
        save();
    }
    Json kernel(const std::string &name, const std::string &kind) const {
        const auto hash = picosha2::hash256_hex_string(read(root / name));
        return {{"file", name},          {"kind", kind},
                {"sha256", hash},        {"source_url", "synthetic-test-fixture"},
                {"source_sha256", hash}, {"producer", "OrbitForge test suite"}};
    }
    void save() const { std::ofstream(manifest) << document.dump(2); }
    auto load() const { return providers::spice::load_snapshot(manifest, workspace); }
};
void analytic(const core::EphemerisState &state, double offset = 0) {
    const double s = state.tdb.seconds_past_j2000 / 3600;
    const double x = 7000 + offset + 10 * s + 2 * (2 * s * s - 1) + 0.5 * (4 * s * s * s - 3 * s);
    const double y = -1200 + 0.2 * s + 3 * (2 * s * s - 1);
    const double z = 500 - 0.3 * s + 0.6 * (4 * s * s * s - 3 * s);
    const std::array positions{x, y, z};
    const std::array velocities{(10 + 8 * s + 0.5 * (12 * s * s - 3)) / 3600, (0.2 + 12 * s) / 3600,
                                (-0.3 + 0.6 * (12 * s * s - 3)) / 3600};
    for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_NEAR(state.position_m[k], positions[k] * 1000, 1e-6);
        EXPECT_NEAR(state.velocity_mps[k], velocities[k] * 1000, 1e-9);
    }
}
} // namespace

TEST(Spice, AnalyticSpkHasExplicitUnitsAxesAndVelocityScale) {
    Fixture input;
    const auto provider = input.load();
    for (const auto ns :
         {-3000000000000LL, -32184000000LL, 0LL, 1234567890123LL, 3000000000000LL}) {
        const auto state = provider->geometric_state(100001, 399, core::Epoch(ns));
        EXPECT_EQ(state.epoch, core::Epoch(ns));
        EXPECT_EQ(state.frame, core::FrameId::spice_j2000);
        EXPECT_EQ(state.velocity_time_scale, core::TimeScale::tdb);
        EXPECT_EQ(state.target_naif_id, 100001);
        EXPECT_EQ(state.observer_naif_id, 399);
        EXPECT_NE(state.source_id.find(provider->data_info().manifest_sha256), std::string::npos);
        analytic(state);
    }
}

TEST(Spice, ThirdBodyCoverageRejectsInteriorGapsAndChecksEveryRequiredChain) {
    Fixture input;
    const auto file = input.root / "third.bsp";
    std::filesystem::remove(file);
    SpiceInt handle = 0;
    spkopn_c(file.string().c_str(), "synthetic DE chains with interior gap", 0, &handle);
    spice_ok();
    const double coefficients[]{10000, 0, 0, 0, 1000, 0, 0, 0, 2000, 0, 0, 0};
    for (int body : {1, 3, 5, 10, 199, 399, 301, 599}) {
        for (const auto &interval : {std::pair{-3600., -100.}, std::pair{100., 3600.}}) {
            spkw02_c(handle, body,
                     body < 11     ? 0
                     : body == 199 ? 1
                     : body == 599 ? 5
                                   : 3,
                     "J2000", interval.first, interval.second, "synthetic segment",
                     interval.second - interval.first, 1, 3, coefficients, interval.first);
            spice_ok();
        }
    }
    spkcls_c(handle);
    spice_ok();
    input.document["kernels"][1] = input.kernel("third.bsp", "SPK");
    input.save();
    const auto provider = input.load();
    const core::Epoch before{-1000000000000}, after{1000000000000};
    EXPECT_NO_THROW((void)provider->geometric_state(10, 399, before));
    EXPECT_NO_THROW((void)provider->geometric_state(10, 399, after));
    EXPECT_THROW(provider->validate_earth_gravity_interval(before, after, true, true),
                 core::EphemerisCoverageError);
    EXPECT_NO_THROW(provider->validate_earth_gravity_interval(after, after, true, true));
    EXPECT_NO_THROW(
        provider->validate_earth_gravity_interval(core::Epoch{200000000000}, after, false, true));
    EXPECT_THROW(provider->validate_earth_gravity_interval(after, before, true, true),
                 core::EphemerisCoverageError);
    for (auto target : {199, 5, 599}) {
        const std::array targets{std::int32_t(target)};
        EXPECT_THROW(provider->validate_earth_gravity_interval(before, after, targets),
                     core::EphemerisCoverageError);
        EXPECT_NO_THROW(provider->validate_earth_gravity_interval(after, after, targets));
    }
    const std::array missing{std::int32_t{6}};
    EXPECT_THROW(provider->validate_earth_gravity_interval(after, after, missing),
                 core::EphemerisCoverageError);
}

TEST(Spice, TaiTdbRoundTripAndEpochZeroAreCorrect) {
    Fixture input;
    const auto provider = input.load();
    for (const auto ns :
         {-3600000000000LL, -32184000000LL, -1LL, 0LL, 1LL, 1234567890123LL, 3600000000000LL}) {
        const auto time = provider->to_tdb(core::Epoch(ns));
        EXPECT_LE(std::abs(provider->from_tdb(time).seconds_since(core::Epoch(ns))), 1e-6);
    }
    EXPECT_NEAR(provider->to_tdb(core::Epoch(0)).seconds_past_j2000, 32.183927, 2e-6);
    EXPECT_THROW((void)provider->to_tdb(core::Epoch(3600000000001LL)),
                 core::EphemerisCoverageError);
    EXPECT_THROW((void)provider->from_tdb({1e20}), core::EphemerisCoverageError);
    EXPECT_THROW((void)provider->from_tdb({std::numeric_limits<double>::quiet_NaN()}),
                 std::invalid_argument);
}

TEST(Spice, MissingActualCoverageAndQueryErrorsDoNotPoisonEngine) {
    Fixture input;
    const auto provider = input.load();
    // This is inside the declared TAI window but outside the actual SPK ET range.
    EXPECT_THROW((void)provider->geometric_state(100001, 399, core::Epoch(3600000000000LL)),
                 core::EphemerisCoverageError);
    EXPECT_THROW((void)provider->geometric_state(999999, 399, core::Epoch(0)),
                 core::EphemerisCoverageError);
    analytic(provider->geometric_state(100001, 399, core::Epoch(0)));
}

TEST(Spice, LskIndirectionAndIncompleteTimeConstantsAreRejected) {
    Fixture input;
    const auto original = read(input.root / "naif0012.tls");
    const std::vector<std::string> invalid{"DELTET/M = ( 6.239996 )",
                                           "DELTET/DELTA_T_A = ( 32 )",
                                           "DELTET/K = ( 'not-numeric' )",
                                           "KERNELS_TO_LOAD = ( '" +
                                               (input.root / "b.bsp").generic_string() + "' )",
                                           "PATH_SYMBOLS = ( 'EXTRA' )",
                                           "PATH_VALUES = ( '.' )"};
    // First attempt in this process has no previous valid UNITIM cache.
    std::ofstream(input.root / "naif0012.tls") << original << "\n\\begindata\n"
                                               << invalid.front() << '\n';
    input.document["kernels"][0] = input.kernel("naif0012.tls", "LSK");
    input.save();
    EXPECT_THROW((void)input.load(), core::EphemerisDataError);
    std::ofstream(input.root / "naif0012.tls") << original;
    input.document["kernels"][0] = input.kernel("naif0012.tls", "LSK");
    input.save();
    const auto good = input.load();
    for (const auto &line : invalid) {
        std::ofstream(input.root / "naif0012.tls") << original << "\n\\begindata\n" << line << '\n';
        input.document["kernels"][0] = input.kernel("naif0012.tls", "LSK");
        input.save();
        EXPECT_THROW((void)input.load(), core::EphemerisDataError);
        analytic(good->geometric_state(100001, 399, core::Epoch(0)));
    }
}

TEST(Spice, ExtremeEpochRoundtripAccountsForDoubleTimeQuantization) {
    Fixture input;
    input.document["first_tai_ns"] = INT64_MIN;
    input.document["last_tai_ns"] = INT64_MAX;
    input.save();
    const auto provider = input.load();
    const std::array<std::int64_t, 7> samples{
        INT64_MIN,        INT64_MIN + 4096, -8589934587521630764LL, 0, 8589934587521630764LL,
        INT64_MAX - 4096, INT64_MAX};
    for (const auto ns : samples) {
        const auto tdb = provider->to_tdb(core::Epoch(ns));
        const double ulp = std::abs(
            std::nextafter(tdb.seconds_past_j2000, std::numeric_limits<double>::infinity()) -
            tdb.seconds_past_j2000);
        EXPECT_LE(std::abs(provider->from_tdb(tdb).seconds_since(core::Epoch(ns))),
                  std::max(1e-6, 4 * ulp));
    }
}

TEST(Spice, KernelOrderAndContextSwitchingAreDeterministic) {
    Fixture input;
    input.document["kernels"].push_back(input.kernel("b.bsp", "SPK"));
    input.save();
    const auto second_wins = input.load();
    std::swap(input.document["kernels"][1], input.document["kernels"][2]);
    input.save();
    const auto first_wins = input.load();
    for (int i = 0; i < 4; ++i) {
        analytic(second_wins->geometric_state(100001, 399, core::Epoch(0)), 100);
        analytic(first_wins->geometric_state(100001, 399, core::Epoch(0)));
    }
    EXPECT_NE(first_wins->data_info().manifest_sha256, second_wins->data_info().manifest_sha256);
}

TEST(Spice, ConcurrentAlternatingSnapshotsMatchSerialQueries) {
    Fixture input;
    const auto a = input.load();
    input.document["kernels"][1] = input.kernel("b.bsp", "SPK");
    input.save();
    const auto b = input.load();
    const std::vector<core::Epoch> epochs{core::Epoch(-1000000000000LL), core::Epoch(0),
                                          core::Epoch(1000000000000LL)};
    const auto expected_a = a->geometric_states(100001, 399, epochs);
    const auto expected_b = b->geometric_states(100001, 399, epochs);
    std::vector<std::future<void>> tasks;
    for (int thread = 0; thread < 8; ++thread) {
        tasks.push_back(std::async(std::launch::async, [&, thread] {
            for (int i = 0; i < 24; ++i) {
                const bool choose_a = (i + thread) % 2 == 0;
                const auto actual = (choose_a ? a : b)->geometric_states(100001, 399, epochs);
                const auto &expected = choose_a ? expected_a : expected_b;
                for (std::size_t k = 0; k < epochs.size(); ++k) {
                    EXPECT_EQ(actual[k].position_m, expected[k].position_m);
                    EXPECT_EQ(actual[k].velocity_mps, expected[k].velocity_mps);
                    EXPECT_EQ(actual[k].source_id, expected[k].source_id);
                }
            }
        }));
    }
    for (auto &task : tasks) {
        task.get();
    }
}

TEST(Spice, OwnedCopiesIgnoreSourceChangesAndAreCleanedAfterClose) {
    Fixture input;
    {
        const auto provider = input.load();
        std::ofstream(input.root / "a.bsp", std::ios::binary | std::ios::trunc) << "corrupted";
        analytic(provider->geometric_state(100001, 399, core::Epoch(0)));
        EXPECT_THROW((void)input.load(), core::EphemerisDataError);
        analytic(provider->geometric_state(100001, 399, core::Epoch(0)));
    }
    EXPECT_TRUE(std::filesystem::is_empty(input.workspace));
}

TEST(Spice, InvalidSnapshotDoesNotReplaceLiveContext) {
    Fixture input;
    const auto provider = input.load();
    const auto original = input.document;
    for (const auto &value : {Json(1.5), Json(2), Json("1")}) {
        input.document["schema_version"] = value;
        input.save();
        EXPECT_THROW((void)input.load(), core::EphemerisDataError);
    }
    input.document = original;
    input.document["kernels"][1]["file"] = "../outside.bsp";
    input.save();
    EXPECT_THROW((void)input.load(), core::EphemerisDataError);
    input.document = original;
    // Valid LSK header but no time constants: exercise CSPICE failure/reset after activation.
    std::ofstream(input.root / "naif0012.tls") << "KPL/LSK\n\\begindata\nX = 1\n";
    input.document["kernels"][0] = input.kernel("naif0012.tls", "LSK");
    input.save();
    EXPECT_THROW((void)input.load(), core::EphemerisDataError);
    analytic(provider->geometric_state(100001, 399, core::Epoch(0)));
}

#ifdef ORBITFORGE_SPICE_TEST_ERFA
TEST(Spice, RealSnapshotSunMoonAndIndependentTdbModel) {
    const auto workspace = std::filesystem::path(ORBITFORGE_SPICE_TEST_DIR) / "real-runtime";
    const auto provider = providers::spice::load_snapshot(ORBITFORGE_SPICE_SAMPLE, workspace);
    const auto time = providers::erfa::make_builtin_time_provider();
    for (const auto utc : {core::UtcDateTime{2016, 12, 28, 0, 0, 0, 0},
                           core::UtcDateTime{2016, 12, 31, 23, 59, 60, 123456789},
                           core::UtcDateTime{2017, 1, 4, 0, 0, 0, 0}}) {
        const auto epoch = time->from_utc(utc);
        const auto tdb = provider->to_tdb(epoch);
        EXPECT_LE(std::abs(provider->from_tdb(tdb).seconds_since(epoch)), 1e-6);
        const auto tt = time->julian_date(epoch, core::TimeScale::tt);
        const auto independent_delta = eraDtdb(tt.day, tt.fraction, 0, 0, 0, 0);
        EXPECT_NEAR(tdb.seconds_past_j2000 - (epoch.seconds_since(core::Epoch(0)) + 32.184),
                    independent_delta, 50e-6);
        const auto sun = provider->geometric_state(10, 399, epoch);
        const auto moon = provider->geometric_state(301, 399, epoch);
        EXPECT_NEAR(std::hypot(sun.position_m[0], sun.position_m[1], sun.position_m[2]), 1.47e11,
                    1e9);
        EXPECT_GT(std::hypot(moon.position_m[0], moon.position_m[1], moon.position_m[2]), 3.5e8);
        EXPECT_LT(std::hypot(moon.position_m[0], moon.position_m[1], moon.position_m[2]), 4.1e8);
    }
}
#endif

namespace {
constexpr std::array<double, 3> sun_origin{-8e7, 1e7, 2e7}, sun_velocity{.01, -.02, .03};
constexpr std::array<double, 3> earth_origin{2e7, 4e7, -3e7}, earth_velocity{20, -15, 3};
void linear_sun_kernel(Fixture &fixture, bool sun_gap = false, double earth_first = -4000) {
    const auto path = fixture.root / "a.bsp";
    std::filesystem::remove(path);
    SpiceInt handle = 0;
    spkopn_c(path.string().c_str(), "Linear Sun and Earth", 0, &handle);
    spice_ok();
    const auto segment = [&](int body, int centre, double first, double last,
                             const std::array<double, 3> &position,
                             const std::array<double, 3> &velocity) {
        double coefficients[6]{};
        for (std::size_t i = 0; i < 3; ++i) {
            coefficients[2 * i] = position[i] + velocity[i] * (first + last) / 2;
            coefficients[2 * i + 1] = velocity[i] * (last - first) / 2;
        }
        spkw02_c(handle, body, centre, "J2000", first, last, "Linear reference", last - first, 1, 1,
                 coefficients, first);
        spice_ok();
    };
    segment(3, 0, earth_first, 4000, earth_origin, earth_velocity);
    segment(399, 3, earth_first, 4000, {}, {});
    if (sun_gap) {
        segment(10, 0, -4000, -700, sun_origin, sun_velocity);
        segment(10, 0, -100, 4000, sun_origin, sun_velocity);
    } else
        segment(10, 0, -4000, 4000, sun_origin, sun_velocity);
    spkcls_c(handle);
    spice_ok();
    fixture.document["kernels"] =
        Json::array({fixture.kernel("naif0012.tls", "LSK"), fixture.kernel("a.bsp", "SPK")});
    fixture.save();
}
} // namespace
TEST(Spice, MovingSunLightTimeAndAberration) {
    for (bool custom_clock : {false, true}) {
        SCOPED_TRACE(custom_clock);
        Fixture fixture;
        if (custom_clock) {
            // Amplify the rate difference to detect omission or reversal of the
            // TAI/TDB velocity derivative. This is an artificial clock model.
            std::ofstream(fixture.root / "naif0012.tls", std::ios::app)
                << "\n\\begindata\nDELTET/K = 5\nDELTET/EB = 0.1\n"
                   "DELTET/M = (0.4 0.002)\n\\begintext\n";
        }
        linear_sun_kernel(fixture);
        const auto provider = fixture.load();
        provider->validate_spacecraft_apparent_sun_interval(core::Epoch{0},
                                                            core::Epoch{100000000000});
        for (double sign : {-1., 0., 1.}) {
            const std::array<double, 3> position{7e6, 2e6, -1e6};
            const std::array<double, 3> velocity{sign * 1000, sign * 7500, sign * -2000};
            const auto actual =
                provider->apparent_sun_from_spacecraft(core::Epoch{0}, position, velocity);
            const long double et = provider->to_tdb(core::Epoch{0}).seconds_past_j2000;
            const long double k = custom_clock ? 5 : 1.657e-3;
            const long double eb = custom_clock ? .1 : 1.671e-2;
            const long double m0 = custom_clock ? .4 : 6.239996;
            const long double m1 = custom_clock ? .002 : 1.99096871e-7;
            const auto mean = m0 + m1 * et;
            const auto rate =
                1 - k * std::cos(mean + eb * std::sin(mean)) * m1 * (1 + eb * std::cos(mean));
            EXPECT_NEAR(provider->tdb_seconds_per_tai_second(core::Epoch{0}),
                        static_cast<double>(1 / rate), 3e-16);
            EXPECT_THROW((void)provider->tdb_seconds_per_tai_second(core::Epoch{3600000000001LL}),
                         core::EphemerisCoverageError);
            constexpr long double light_speed = 299792.458L; // km / TDB second.
            std::array<long double, 3> observer{}, beta{}, ray{};
            long double distance2 = 0;
            for (std::size_t i = 0; i < 3; ++i) {
                observer[i] = earth_origin[i] + earth_velocity[i] * et + position[i] / 1000;
                beta[i] = (earth_velocity[i] + velocity[i] * rate / 1000) / light_speed;
                ray[i] = sun_origin[i] + sun_velocity[i] * et - observer[i];
                distance2 += ray[i] * ray[i];
            }
            // One Newtonian light-time iteration, with an analytic moving Sun.
            const auto emission = et - std::sqrt(distance2) / light_speed;
            distance2 = 0;
            for (std::size_t i = 0; i < 3; ++i) {
                ray[i] = sun_origin[i] + sun_velocity[i] * emission - observer[i];
                distance2 += ray[i] * ray[i];
            }
            const auto distance = std::sqrt(distance2);
            long double parallel = 0, transverse2 = 0;
            for (std::size_t i = 0; i < 3; ++i) {
                ray[i] /= distance;
                parallel += beta[i] * ray[i];
            }
            for (std::size_t i = 0; i < 3; ++i) {
                beta[i] -= parallel * ray[i];
                transverse2 += beta[i] * beta[i];
            }
            for (std::size_t i = 0; i < 3; ++i) {
                const auto expected =
                    distance * (ray[i] * std::sqrt(1 - transverse2) + beta[i]) * 1000;
                // Millimetre bound is ~10^-14 relative: double SPK/rotation roundoff.
                EXPECT_NEAR(actual[i], static_cast<double>(expected), 1e-3);
            }
        }
        const auto centre = provider->apparent_sun_from_spacecraft(core::Epoch{0}, {}, {});
        const auto earth = provider->apparent_sun_from_earth(core::Epoch{0});
        for (std::size_t i = 0; i < 3; ++i)
            EXPECT_NEAR(centre[i], earth[i], 1e-3);
        EXPECT_THROW((void)provider->apparent_sun_from_spacecraft(core::Epoch{0}, {2e15, 0, 0}, {}),
                     core::EphemerisCoverageError);
        EXPECT_THROW((void)provider->apparent_sun_from_spacecraft(
                         core::Epoch{0}, {std::numeric_limits<double>::quiet_NaN(), 0, 0}, {}),
                     std::invalid_argument);
        EXPECT_THROW((void)provider->apparent_sun_from_spacecraft(core::Epoch{0}, {}, {4e8, 0, 0}),
                     std::invalid_argument);
        EXPECT_EQ(provider->apparent_sun_from_spacecraft(core::Epoch{0}, {}, {}), centre);
    }
}
TEST(Spice, MovingSunCertifiesHistoryAndGaps) {
    Fixture fixture;
    linear_sun_kernel(fixture, true);
    {
        const auto provider = fixture.load();
        // Receiving endpoints are covered, but retarded Sun history has a hole.
        provider->validate_earth_gravity_interval(core::Epoch{0}, core::Epoch{100000000000}, true,
                                                  false);
        EXPECT_THROW(provider->validate_spacecraft_apparent_sun_interval(core::Epoch{0},
                                                                         core::Epoch{100000000000}),
                     core::EphemerisCoverageError);
        EXPECT_THROW((void)provider->apparent_sun_from_spacecraft(core::Epoch{0}, {}, {}),
                     core::EphemerisCoverageError);
    }
    linear_sun_kernel(fixture, false, -100);
    const auto provider = fixture.load();
    // Earth is needed at reception only; requiring ancient Earth data would be
    // spurious when the full past Sun chain is available.
    EXPECT_NO_THROW(provider->validate_spacecraft_apparent_sun_interval(core::Epoch{0},
                                                                        core::Epoch{100000000000}));
    EXPECT_NO_THROW((void)provider->apparent_sun_from_spacecraft(core::Epoch{0}, {}, {}));
    EXPECT_THROW(provider->validate_spacecraft_apparent_sun_interval(core::Epoch{-1000000000000},
                                                                     core::Epoch{0}),
                 core::EphemerisCoverageError);
    EXPECT_THROW(provider->validate_spacecraft_apparent_sun_interval(core::Epoch{100000000000},
                                                                     core::Epoch{0}),
                 core::EphemerisCoverageError);
    EXPECT_THROW(provider->validate_spacecraft_apparent_sun_interval(core::Epoch{-3600000000001},
                                                                     core::Epoch{0}),
                 core::EphemerisCoverageError);
}

namespace {
void planetary_test_kernel(const std::filesystem::path &path, int bad_centre = 0,
                           const char *frame = "J2000", bool include_planets = true) {
    std::filesystem::remove(path);
    SpiceInt handle = 0;
    spkopn_c(path.string().c_str(), "Synthetic planetary subset", 0, &handle);
    spice_ok();
    const double coefficients[]{7000, 10, 2, .5, -1200, .2, 3, 0, 500, -.3, 0, .6};
    for (int id : {10, 4, 499, 599, 699, 799, 899, 999}) {
        if (!include_planets && id >= 499)
            continue;
        const int centre = id < 499 ? 0 : (id == 599 && bad_centre ? bad_centre : id / 100);
        spkw02_c(handle, id, centre, frame, -3600, 3600, "Original synthetic segment", 7200, 1, 3,
                 coefficients, -3600);
        spice_ok();
    }
    spkcls_c(handle);
    spice_ok();
}
} // namespace
TEST(Spice, PlanetarySubsetsRetainSixCentresAndMatchIndependentChebyshevValues) {
    Fixture input;
    const auto source = input.root / "planets.bsp";
    planetary_test_kernel(source);
    const auto directory =
        input.root /
        ("subset-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto ids = providers::spice::extract_planetary_centres(source, directory);
    ASSERT_EQ(ids, (std::vector<std::int32_t>{499, 599, 699, 799, 899, 999}));
    input.document["kernels"] = Json::array({input.kernel("naif0012.tls", "LSK")});
    for (auto id : ids) {
        const auto name = "centre-" + std::to_string(id) + ".bsp";
        std::filesystem::copy_file(directory / name, input.root / name,
                                   std::filesystem::copy_options::overwrite_existing);
        input.document["kernels"].push_back(input.kernel(name, "SPK_BODY_CENTRES"));
    }
    input.save();
    const auto provider = input.load();
    for (auto id : ids)
        for (auto ns : {-3000000000000LL, 0LL, 1234567890123LL, 3000000000000LL})
            analytic(provider->geometric_state(id, id / 100, core::Epoch(ns)));
    EXPECT_THROW((void)provider->geometric_state(10, 0, core::Epoch{0}),
                 core::EphemerisCoverageError);
    const auto original = read(directory / "centre-499.bsp");
    EXPECT_THROW((void)providers::spice::extract_planetary_centres(source, directory),
                 core::EphemerisDataError);
    EXPECT_EQ(read(directory / "centre-499.bsp"), original);
    // A role declaration cannot authorize unrelated targets to override the base chain.
    std::filesystem::copy_file(source, input.root / "centre-599.bsp",
                               std::filesystem::copy_options::overwrite_existing);
    input.document["kernels"] = Json::array(
        {input.kernel("naif0012.tls", "LSK"), input.kernel("centre-599.bsp", "SPK_BODY_CENTRES")});
    input.save();
    EXPECT_THROW((void)input.load(), core::EphemerisDataError);
    analytic(provider->geometric_state(599, 5, core::Epoch{0}));
}
TEST(Spice, PlanetaryExtractionRejectsWrongChainsAxesAndCleansOnlyItsCreatedFiles) {
    Fixture input;
    const auto source = input.root / "planet-source.bsp";
    const auto directory =
        input.root /
        ("partial-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "keep.txt") << "caller-owned";
    for (const auto wrong : {1, 2, 3}) {
        planetary_test_kernel(source, wrong == 1 ? 4 : 0, wrong == 2 ? "ECLIPJ2000" : "J2000",
                              wrong != 3);
        EXPECT_THROW((void)providers::spice::extract_planetary_centres(source, directory),
                     core::EphemerisDataError);
        EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                                std::filesystem::directory_iterator{}),
                  1);
        EXPECT_EQ(read(directory / "keep.txt"), "caller-owned");
    }
    planetary_test_kernel(source);
    unsigned checks = 0;
    EXPECT_THROW((void)providers::spice::extract_planetary_centres(
                     source, directory,
                     [&] {
                         if (++checks == 5)
                             throw std::runtime_error("cancel after writing two planets");
                     }),
                 std::runtime_error);
    EXPECT_EQ(checks, 5U);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                            std::filesystem::directory_iterator{}),
              1);
}
