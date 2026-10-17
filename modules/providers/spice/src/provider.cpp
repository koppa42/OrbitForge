#include <algorithm>
#include <cmath>
#include <cspice/SpiceUsr.h>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <orbitforge/providers/spice/provider.hpp>
#include <picosha2.h>
#include <random>
#include <set>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace orbitforge::providers::spice {
namespace {
using Json = nlohmann::json;
struct Engine {
    std::mutex mutex;
    const void *active = nullptr;
    bool initialized = false;
};
Engine &engine() {
    static Engine value;
    return value;
}
// Every caller holds Engine::mutex, including all failure and cleanup paths.
void check_spice() {
    if (!failed_c()) {
        return;
    }
    char short_message[256]{}, long_message[2048]{};
    getmsg_c("SHORT", sizeof(short_message), short_message);
    getmsg_c("LONG", sizeof(long_message), long_message);
    reset_c();
    const std::string message = std::string(short_message) + ": " + long_message;
    if (std::string(short_message) == "SPICE(SPKINSUFFDATA)") {
        throw core::EphemerisCoverageError(message);
    }
    throw core::EphemerisQueryError(message);
}
void initialize_engine() {
    if (!engine().initialized) {
        char action[] = "RETURN", report[] = "NONE";
        erract_c("SET", 0, action);
        errprt_c("SET", 0, report);
        check_spice();
        if (std::string(tkvrsn_c("TOOLKIT")) != "CSPICE_N0067") {
            throw core::EphemerisDataError("This provider requires CSPICE N0067");
        }
        engine().initialized = true;
    }
}

void validate_lsk_pool() {
    // A KPL/LSK header does not stop furnsh from interpreting meta-kernel variables.
    // LSKs are loaded with ldpool; reject indirection before any further kernel loading.
    for (const auto *name : {"KERNELS_TO_LOAD", "PATH_SYMBOLS", "PATH_VALUES"}) {
        SpiceBoolean found = SPICEFALSE;
        SpiceInt count = 0;
        SpiceChar type = 0;
        dtpool_c(name, &found, &count, &type);
        check_spice();
        if (found) {
            throw core::EphemerisDataError(
                "LSK must not contain meta-kernel indirection variables");
        }
    }
    const std::pair<const char *, SpiceInt> required[] = {
        {"DELTET/DELTA_T_A", 1}, {"DELTET/K", 1}, {"DELTET/EB", 1}, {"DELTET/M", 2}};
    for (const auto &[name, expected] : required) {
        SpiceBoolean found = SPICEFALSE;
        SpiceInt count = 0;
        SpiceChar type = 0;
        dtpool_c(name, &found, &count, &type);
        check_spice();
        if (!found || type != 'N' || count != expected) {
            throw core::EphemerisDataError("LSK time constant has the wrong type or cardinality: " +
                                           std::string(name));
        }
        double values[2]{};
        gdpool_c(name, 0, expected, &count, values, &found);
        check_spice();
        if (!found || count != expected || !std::isfinite(values[0]) ||
            (expected == 2 && !std::isfinite(values[1])) ||
            (std::string(name) == "DELTET/DELTA_T_A" && values[0] != 32.184)) {
            throw core::EphemerisDataError(
                "LSK time constants are invalid or TT-TAI is not 32.184 s");
        }
    }
}

std::string read_file(const std::filesystem::path &path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw core::EphemerisDataError("Cannot read SPICE input: " + path.string());
    }
    const auto size = input.tellg();
    if (size <= 0 || static_cast<std::uintmax_t>(size) > limit) {
        throw core::EphemerisDataError("SPICE input is empty or exceeds its size limit");
    }
    std::string data(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(data.data(), static_cast<std::streamsize>(data.size()))) {
        throw core::EphemerisDataError("Incomplete SPICE input read");
    }
    return data;
}
bool hash_string(const std::string &value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
void keys(const Json &value, std::initializer_list<const char *> expected) {
    if (!value.is_object() || value.size() != expected.size()) {
        throw core::EphemerisDataError("SPICE manifest has missing or extra fields");
    }
    for (const auto *key : expected) {
        if (!value.contains(key)) {
            throw core::EphemerisDataError("Missing SPICE manifest field: " + std::string(key));
        }
    }
}
std::string nonempty(const Json &value, const char *key) {
    auto result = value.at(key).get<std::string>();
    if (result.empty()) {
        throw core::EphemerisDataError("Empty SPICE manifest field: " + std::string(key));
    }
    return result;
}
core::Epoch epoch_field(const Json &value, const char *key) {
    const auto &number = value.at(key);
    if (!number.is_number_integer() ||
        (number.is_number_unsigned() &&
         number.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX))) {
        throw core::EphemerisDataError("SPICE coverage requires int64 TAI nanoseconds");
    }
    return core::Epoch(number.get<std::int64_t>());
}

// Own only files created in one new directory. Cleanup never recursively removes a caller path.
struct KernelCopies {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> paths;
#ifdef _WIN32
    std::vector<HANDLE> leases;
#endif
    explicit KernelCopies(const std::filesystem::path &workspace) {
        std::filesystem::create_directories(workspace);
        const auto parent = std::filesystem::canonical(workspace);
        std::random_device random;
        for (int attempt = 0; attempt < 16; ++attempt) {
            auto candidate = parent / ("orbitforge-spice-" + std::to_string(random()) + "-" +
                                       std::to_string(random()));
            if (std::filesystem::create_directory(candidate)) {
                directory = std::move(candidate);
                return;
            }
        }
        throw core::EphemerisDataError("Cannot allocate a private kernel directory");
    }
    KernelCopies(const KernelCopies &) = delete;
    KernelCopies &operator=(const KernelCopies &) = delete;
    ~KernelCopies() {
#ifdef _WIN32
        for (const auto handle : leases) {
            CloseHandle(handle);
        }
#endif
        std::error_code error;
        for (const auto &path : paths) {
            std::filesystem::remove(path, error);
        }
        std::filesystem::remove(directory,
                                error); // Only succeeds when the owned directory is empty.
    }
    void add(const std::string &bytes, const std::string &kind) {
        const auto path = directory / ("kernel-" + std::to_string(paths.size()) +
                                       (kind == "LSK" ? ".tls" : ".bsp"));
        paths.push_back(path);
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            throw core::EphemerisDataError("Cannot write the private kernel copy");
        }
#ifdef _WIN32
        // SPICE reads SPKs lazily. Keep the runtime copy locked against writes/deletion.
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            throw core::EphemerisDataError("Cannot hold the private kernel read lease");
        }
        leases.push_back(handle);
#endif
    }
};

bool planetary_centre(SpiceInt id) { return id >= 499 && id <= 999 && id % 100 == 99; }

template <typename Visitor>
void visit_segments(const std::filesystem::path &path, Visitor visitor) {
    SpiceInt handle = 0;
    dafopr_c(path.string().c_str(), &handle);
    check_spice();
    try {
        SpiceInt nd = 0, ni = 0;
        dafhsf_c(handle, &nd, &ni);
        check_spice();
        if (nd != 2 || ni != 6)
            throw core::EphemerisDataError("Expected an SPK DAF descriptor");
        dafbfs_c(handle);
        check_spice();
        SpiceBoolean found = SPICEFALSE;
        std::size_t count = 0;
        daffna_c(&found);
        check_spice();
        while (found) {
            if (++count > 100000)
                throw core::EphemerisDataError("Too many SPK segments");
            SpiceDouble descriptor[5]{}, bounds[2]{};
            SpiceInt integers[6]{};
            dafgs_c(descriptor);
            dafus_c(descriptor, 2, 6, bounds, integers);
            check_spice();
            visitor(handle, descriptor, bounds, integers);
            dafcs_c(handle);
            check_spice();
            daffna_c(&found);
            check_spice();
        }
        dafcls_c(handle);
        check_spice();
    } catch (...) {
        reset_c();
        dafcls_c(handle);
        reset_c();
        throw;
    }
}
void validate_centre_segment(const double *bounds, const SpiceInt *integers) {
    if (!planetary_centre(integers[0]) || integers[1] != integers[0] / 100 || integers[2] != 1 ||
        !std::isfinite(bounds[0]) || !std::isfinite(bounds[1]) || bounds[0] > bounds[1])
        throw core::EphemerisDataError(
            "Supplemental SPK must contain only planet centres 499..999 relative to "
            "their own system barycentres in J2000");
}

class Snapshot final : public core::IEphemerisProvider {
  public:
    Snapshot(core::EphemerisDataInfo info, std::unique_ptr<KernelCopies> copies)
        : info_(std::move(info)), copies_(std::move(copies)),
          source_("manifest-sha256=" + info_.manifest_sha256 +
                  ";toolkit=CSPICE_N0067;frame=J2000;abcorr=NONE;velocity-scale=TDB") {}
    ~Snapshot() override {
        std::scoped_lock guard(engine().mutex);
        if (engine().active == this) {
            reset_c();
            kclear_c();
            reset_c();
            engine().active = nullptr;
        }
    }
    void validate() const {
        std::scoped_lock guard(engine().mutex);
        activate();
        // Verify required time constants at load time, without calling SPICE UTC conversion.
        (void)tdb_unlocked(info_.first_epoch);
        (void)tdb_unlocked(info_.last_epoch);
        for (std::size_t i = 0; i < info_.kernels.size(); ++i) {
            if (info_.kernels[i].kind != "SPK_BODY_CENTRES")
                continue;
            std::optional<SpiceInt> body;
            visit_segments(copies_->paths[i], [&](SpiceInt, const double *, const double *bounds,
                                                  const SpiceInt *integers) {
                validate_centre_segment(bounds, integers);
                if (body && *body != integers[0])
                    throw core::EphemerisDataError("Each supplemental SPK must contain one planet");
                body = integers[0];
            });
            if (!body || info_.kernels[i].file != "centre-" + std::to_string(*body) + ".bsp")
                throw core::EphemerisDataError(
                    "Supplemental SPK must be named centre-<physical NAIF ID>.bsp");
        }
    }
    core::TdbTime to_tdb(core::Epoch epoch) const override {
        check_epoch(epoch);
        std::scoped_lock guard(engine().mutex);
        activate();
        return tdb_unlocked(epoch);
    }
    core::Epoch from_tdb(core::TdbTime time) const override {
        if (!std::isfinite(time.seconds_past_j2000)) {
            throw std::invalid_argument("TDB seconds must be finite");
        }
        std::scoped_lock guard(engine().mutex);
        activate();
        const auto first = tdb_unlocked(info_.first_epoch).seconds_past_j2000;
        const auto last = tdb_unlocked(info_.last_epoch).seconds_past_j2000;
        if (time.seconds_past_j2000 < first || time.seconds_past_j2000 > last) {
            throw core::EphemerisCoverageError("TDB is outside this snapshot's policy window");
        }
        if (time.seconds_past_j2000 == first) {
            return info_.first_epoch;
        }
        if (time.seconds_past_j2000 == last) {
            return info_.last_epoch;
        }
        const double tai = unitim_c(time.seconds_past_j2000, "TDB", "TAI");
        check_spice();
        double whole = 0;
        const auto fraction = std::modf(tai, &whole);
        constexpr std::int64_t billion = 1000000000;
        if (!std::isfinite(tai) || whole < static_cast<double>(INT64_MIN / billion) ||
            whole > static_cast<double>(INT64_MAX / billion)) {
            throw core::EphemerisCoverageError("Converted TAI does not fit Epoch");
        }
        const auto base = static_cast<std::int64_t>(whole) * billion;
        const auto remainder = static_cast<std::int64_t>(std::llround(fraction * 1e9));
        if ((base >= 0 && remainder > INT64_MAX - base) ||
            (base < 0 && remainder < INT64_MIN - base)) {
            throw core::EphemerisCoverageError("Converted TAI does not fit Epoch");
        }
        // The floating input passed the range check; clamp only its final integer rounding.
        return core::Epoch(std::clamp(base + remainder, info_.first_epoch.tai_nanoseconds(),
                                      info_.last_epoch.tai_nanoseconds()));
    }
    core::EphemerisState geometric_state(std::int32_t target, std::int32_t observer,
                                         core::Epoch epoch) const override {
        check_epoch(epoch);
        std::scoped_lock guard(engine().mutex);
        activate();
        return state_unlocked(target, observer, epoch);
    }
    std::vector<core::EphemerisState>
    geometric_states(std::int32_t target, std::int32_t observer,
                     std::span<const core::Epoch> epochs) const override {
        for (const auto epoch : epochs) {
            check_epoch(epoch);
        }
        std::vector<core::EphemerisState> result;
        result.reserve(epochs.size());
        if (epochs.empty()) {
            return result;
        }
        std::scoped_lock guard(engine().mutex);
        activate();
        for (const auto epoch : epochs) {
            result.push_back(state_unlocked(target, observer, epoch));
        }
        return result;
    }
    const core::EphemerisDataInfo &data_info() const noexcept override { return info_; }
    double tdb_seconds_per_tai_second(core::Epoch epoch) const override {
        check_epoch(epoch);
        std::scoped_lock guard(engine().mutex);
        activate();
        return 1 / tai_per_tdb_unlocked(tdb_unlocked(epoch).seconds_past_j2000);
    }
    std::array<double, 3> apparent_sun_from_earth(core::Epoch epoch) const override {
        check_epoch(epoch);
        constexpr std::int64_t margin = 600000000000;
        if (epoch.tai_nanoseconds() < std::numeric_limits<std::int64_t>::min() + margin)
            throw core::EphemerisCoverageError("Apparent Sun margin exceeds Epoch range");
        check_epoch(core::Epoch{epoch.tai_nanoseconds() - margin});
        std::scoped_lock guard(engine().mutex);
        activate();
        return state_unlocked(10, 399, epoch, "LT+S").position_m;
    }
    std::array<double, 3>
    apparent_sun_from_spacecraft(core::Epoch epoch, const std::array<double, 3> &position,
                                 const std::array<double, 3> &velocity) const override {
        check_epoch(epoch);
        for (std::size_t i = 0; i < 3; ++i)
            if (!std::isfinite(position[i]) || !std::isfinite(velocity[i]))
                throw std::invalid_argument("Apparent Sun observer state must be finite");
        std::scoped_lock guard(engine().mutex);
        activate();
        const auto earth = state_unlocked(399, 0, epoch);
        const double et = earth.tdb.seconds_past_j2000;
        const double rate = tai_per_tdb_unlocked(et);
        double observer[6]{};
        for (std::size_t i = 0; i < 3; ++i) {
            observer[i] = (earth.position_m[i] + position[i]) / 1000;
            observer[i + 3] = (earth.velocity_mps[i] + velocity[i] * rate) / 1000;
            if (!std::isfinite(observer[i]) || !std::isfinite(observer[i + 3]))
                throw std::invalid_argument("Apparent Sun observer state overflows");
        }
        if (std::hypot(observer[3], observer[4], observer[5]) >= clight_c())
            throw std::invalid_argument("Apparent Sun observer speed must be below light speed");
        double sun[3]{}, ignored = 0;
        spkgps_c(10, et, "J2000", 0, sun, &ignored);
        check_spice();
        const double light_time =
            std::hypot(sun[0] - observer[0], sun[1] - observer[1], sun[2] - observer[2]) /
            clight_c();
        const double emission = et - light_time;
        if (!std::isfinite(emission) ||
            emission < tdb_unlocked(info_.first_epoch).seconds_past_j2000 || emission > et)
            throw core::EphemerisCoverageError(
                "Spacecraft apparent Sun emission epoch is outside the selected snapshot; "
                "select earlier ephemeris coverage");
        // Explicit single-iteration reception LT, as in spkapo(..., "LT+S").
        // Check the emission epoch BEFORE querying so hidden SPICE reads cannot
        // escape the snapshot policy window. Stellar aberration uses the full
        // moving observer velocity, not a fixed geocentric offset.
        spkgps_c(10, emission, "J2000", 0, sun, &ignored);
        check_spice();
        for (std::size_t i = 0; i < 3; ++i)
            sun[i] -= observer[i];
        double apparent[3]{};
        stelab_c(sun, observer + 3, apparent);
        check_spice();
        std::array<double, 3> result{};
        for (std::size_t i = 0; i < 3; ++i) {
            result[i] = apparent[i] * 1000;
            if (!std::isfinite(result[i]))
                throw core::EphemerisQueryError("CSPICE returned a nonfinite apparent Sun");
        }
        return result;
    }
    void validate_spacecraft_apparent_sun_interval(core::Epoch first,
                                                   core::Epoch last) const override {
        check_epoch(first);
        check_epoch(last);
        if (first.tai_nanoseconds() > last.tai_nanoseconds())
            throw core::EphemerisCoverageError("Invalid ephemeris interval");
        std::scoped_lock guard(engine().mutex);
        activate();
        const double begin = tdb_unlocked(first).seconds_past_j2000;
        const double end = tdb_unlocked(last).seconds_past_j2000;
        validate_chains_unlocked(begin, end, {{399, 3}, {3, 0}});
        // No assumed spacecraft range. All available past Sun data are certified;
        // the actual emission epoch is checked against that range at each stage.
        validate_chains_unlocked(tdb_unlocked(info_.first_epoch).seconds_past_j2000, end,
                                 {{10, 0}});
    }
    void validate_apparent_sun_interval(core::Epoch first, core::Epoch last) const override {
        // DE440 Earth/Sun distances stay below 1.02 AU: a conservative 600 s
        // reception margin certifies all light-time and observer-state queries.
        constexpr std::int64_t margin = 600000000000;
        if (first.tai_nanoseconds() < std::numeric_limits<std::int64_t>::min() + margin)
            throw core::EphemerisCoverageError("Apparent Sun margin exceeds Epoch range");
        const std::array<std::int32_t, 1> sun{10};
        validate_earth_gravity_interval(core::Epoch{first.tai_nanoseconds() - margin}, last, sun);
    }
    void validate_earth_gravity_interval(core::Epoch first, core::Epoch last,
                                         std::span<const std::int32_t> targets) const override {
        check_epoch(first);
        check_epoch(last);
        if (first.tai_nanoseconds() > last.tai_nanoseconds())
            throw core::EphemerisCoverageError("Invalid ephemeris interval");
        std::scoped_lock guard(engine().mutex);
        activate();
        const double begin = tdb_unlocked(first).seconds_past_j2000;
        const double end = tdb_unlocked(last).seconds_past_j2000;
        // Earth -> EMB -> SSB, then each selected target's full DE chain.
        std::map<std::int32_t, std::int32_t> chains;
        if (!targets.empty()) {
            chains[399] = 3;
            chains[3] = 0;
        }
        for (auto target : targets) {
            if (target == 10 || target == 301)
                chains[target] = target == 301 ? 3 : 0;
            else if (target >= 199 && target <= 999 && target % 100 == 99 && target != 399) {
                chains[target] = target / 100;
                chains[target / 100] = 0;
            } else if (target >= 4 && target <= 9)
                chains[target] = 0;
            else
                throw core::EphemerisDataError("Unsupported DE third-body target");
        }
        validate_chains_unlocked(begin, end, chains);
    }

  private:
    void validate_chains_unlocked(double begin, double end,
                                  const std::map<std::int32_t, std::int32_t> &chains) const {
        std::map<std::int32_t, std::vector<std::pair<double, double>>> coverage;
        for (std::size_t k = 0; k < copies_->paths.size(); ++k) {
            if (info_.kernels[k].kind == "LSK")
                continue;
            SpiceInt handle = 0;
            dafopr_c(copies_->paths[k].string().c_str(), &handle);
            check_spice();
            try {
                dafbfs_c(handle);
                check_spice();
                SpiceBoolean found = SPICEFALSE;
                daffna_c(&found);
                check_spice();
                while (found) {
                    SpiceDouble summary[5]{}, dc[2]{};
                    SpiceInt ic[6]{};
                    dafgs_c(summary);
                    dafus_c(summary, 2, 6, dc, ic);
                    check_spice();
                    if (const auto chain = chains.find(ic[0]); chain != chains.end()) {
                        if (ic[1] != chain->second || ic[2] != 1)
                            throw core::EphemerisDataError(
                                "HPOP requires DE-style ICRF-aligned Earth/third-body chains");
                        coverage[ic[0]].emplace_back(dc[0], dc[1]);
                    }
                    daffna_c(&found);
                    check_spice();
                }
                dafcls_c(handle);
                check_spice();
            } catch (...) {
                reset_c();
                dafcls_c(handle);
                reset_c();
                throw;
            }
        }
        for (const auto &[id, centre] : chains) {
            (void)centre;
            auto &intervals = coverage[id];
            std::sort(intervals.begin(), intervals.end());
            double covered = begin;
            bool matched = false;
            for (const auto &[a, b] : intervals) {
                if (a > covered)
                    break;
                if (b >= covered) {
                    matched = true;
                    covered = b;
                }
                if (matched && covered >= end)
                    break;
            }
            if (!matched || covered < end)
                throw core::EphemerisCoverageError("Third-body ephemeris target " +
                                                   std::to_string(id) +
                                                   " has a gap or does not cover "
                                                   "the complete integration interval");
        }
    }
    // Derivative of NAIF TIME Required Reading equations (1)-(3), using the
    // snapshot's own validated LSK constants: TAI = TDB - 32.184 - K sin(E).
    double tai_per_tdb_unlocked(double et) const {
        const auto constant = [](const char *name, SpiceInt size, double *values) {
            SpiceInt count = 0;
            SpiceBoolean found = SPICEFALSE;
            gdpool_c(name, 0, size, &count, values, &found);
            check_spice();
            if (!found || count != size)
                throw core::EphemerisDataError("Missing LSK time derivative constants");
        };
        double k = 0, eb = 0, m[2]{};
        constant("DELTET/K", 1, &k);
        constant("DELTET/EB", 1, &eb);
        constant("DELTET/M", 2, m);
        const double mean = m[0] + m[1] * et;
        const double eccentric = mean + eb * std::sin(mean);
        const double rate = 1 - k * std::cos(eccentric) * m[1] * (1 + eb * std::cos(mean));
        if (!std::isfinite(rate) || rate <= 0)
            throw core::EphemerisDataError("Invalid LSK TAI/TDB derivative");
        return rate;
    }

    void check_epoch(core::Epoch epoch) const {
        if (epoch.tai_nanoseconds() < info_.first_epoch.tai_nanoseconds() ||
            epoch.tai_nanoseconds() > info_.last_epoch.tai_nanoseconds()) {
            throw core::EphemerisCoverageError(
                "Epoch is outside this snapshot's policy window; extrapolation is disabled");
        }
    }
    void activate() const {
        initialize_engine();
        if (engine().active == this) {
            return;
        }
        engine().active = nullptr;
        kclear_c();
        check_spice();
        try {
            for (std::size_t i = 0; i < copies_->paths.size(); ++i) {
                const auto path = copies_->paths[i].string();
                if (path.size() > 255) {
                    throw core::EphemerisDataError(
                        "CSPICE kernel paths must fit 255 bytes; choose a shorter workspace path");
                }
                char architecture[8]{}, kind[8]{};
                getfat_c(path.c_str(), sizeof(architecture), sizeof(kind), architecture, kind);
                check_spice();
                if (std::string(kind) != (info_.kernels[i].kind == "LSK" ? "LSK" : "SPK") ||
                    std::string(architecture) != (info_.kernels[i].kind == "LSK" ? "KPL" : "DAF")) {
                    throw core::EphemerisDataError(
                        "Kernel content does not match the declared LSK/SPK type");
                }
                if (info_.kernels[i].kind == "LSK") {
                    ldpool_c(path.c_str());
                    check_spice();
                    validate_lsk_pool();
                } else {
                    furnsh_c(path.c_str());
                }
                check_spice();
            }
            engine().active = this;
        } catch (...) {
            reset_c();
            kclear_c();
            reset_c();
            throw;
        }
    }
    core::TdbTime tdb_unlocked(core::Epoch epoch) const {
        const double et = unitim_c(epoch.seconds_since(core::Epoch(0)), "TAI", "TDB");
        check_spice();
        if (!std::isfinite(et)) {
            throw core::EphemerisQueryError("CSPICE returned a nonfinite TDB time");
        }
        return {et};
    }
    core::EphemerisState state_unlocked(std::int32_t target, std::int32_t observer,
                                        core::Epoch epoch, const char *correction = "NONE") const {
        static_assert(sizeof(SpiceInt) == sizeof(std::int32_t));
        const auto tdb = tdb_unlocked(epoch);
        double state[6]{}, light_time = 0;
        spkez_c(target, tdb.seconds_past_j2000, "J2000", correction, observer, state, &light_time);
        check_spice();
        for (auto &value : state) {
            value *= 1000; // CSPICE km and km/s -> public SI units.
            if (!std::isfinite(value)) {
                throw core::EphemerisQueryError("CSPICE returned a nonfinite geometric state");
            }
        }
        return {epoch,
                target,
                observer,
                core::FrameId::spice_j2000,
                {state[0], state[1], state[2]},
                {state[3], state[4], state[5]},
                core::TimeScale::tdb,
                tdb,
                source_};
    }
    core::EphemerisDataInfo info_;
    std::unique_ptr<KernelCopies> copies_;
    std::string source_;
};

class TwoBody final : public core::IStateProvider {
  public:
    TwoBody(core::CartesianState initial, core::GravitationalParameter mu)
        : initial_(std::move(initial)), mu_(mu.metres_cubed_per_second_squared) {
        if (initial_.origin != core::OriginId::earth || initial_.frame != core::FrameId::gcrf) {
            throw std::invalid_argument("Two-body initial state must be Earth-centred GCRF");
        }
        if (!std::isfinite(mu_) || mu_ <= 0) {
            throw std::invalid_argument(
                "Two-body gravitational parameter must be finite and positive");
        }
        for (std::size_t k = 0; k < 3; ++k) {
            if (!std::isfinite(initial_.position_m[k]) ||
                !std::isfinite(initial_.velocity_mps[k])) {
                throw std::invalid_argument("Two-body initial state must be finite");
            }
            input_[k] = initial_.position_m[k];
            input_[k + 3] = initial_.velocity_mps[k];
        }
        const auto r = std::hypot(input_[0], input_[1], input_[2]);
        const auto v = std::hypot(input_[3], input_[4], input_[5]);
        if (!(r > 0) || !(v > 0) || !std::isfinite(r) || !std::isfinite(v)) {
            throw std::invalid_argument(
                "Two-body position and velocity norms must be finite and nonzero");
        }
        // Normalize before the cross product so this validation does not square
        // dimensional numbers or introduce a near-radial eccentricity cutoff.
        const std::array<double, 3> a{input_[0] / r, input_[1] / r, input_[2] / r};
        const std::array<double, 3> b{input_[3] / v, input_[4] / v, input_[5] / v};
        if (std::hypot(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                       a[0] * b[1] - a[1] * b[0]) == 0) {
            throw std::invalid_argument("Radial two-body motion is unsupported");
        }
        // Validate the CSPICE numerical domain before a Session can be published.
        (void)propagate(initial_.epoch);
    }

    core::CartesianState state_at(core::Epoch epoch) const override {
        return epoch == initial_.epoch ? initial_ : propagate(epoch);
    }

  private:
    core::CartesianState propagate(core::Epoch epoch) const {
        std::array<double, 6> values{};
        try {
            std::scoped_lock guard(engine().mutex);
            initialize_engine();
            // prop2b accepts consistent units (including SI) and no kernels.
            // It must not clear or replace engine().active.
            prop2b_c(mu_, input_.data(), epoch.seconds_since(initial_.epoch), values.data());
            check_spice();
        } catch (const core::EphemerisQueryError &error) {
            throw core::PropagationError(error.what());
        } catch (const core::EphemerisDataError &error) {
            throw core::PropagationError(error.what());
        }
        if (!std::all_of(values.begin(), values.end(), [](double x) { return std::isfinite(x); })) {
            throw core::PropagationError("CSPICE two-body propagation returned a non-finite state");
        }
        return {epoch,
                initial_.origin,
                initial_.frame,
                {values[0], values[1], values[2]},
                {values[3], values[4], values[5]}};
    }
    const core::CartesianState initial_;
    const double mu_;
    std::array<double, 6> input_{};
};
} // namespace

std::shared_ptr<const core::IStateProvider> make_two_body(core::CartesianState initial,
                                                          core::GravitationalParameter mu) {
    return std::make_shared<TwoBody>(std::move(initial), mu);
}

namespace {
double anomaly_remainder(double value, bool hyperbolic) {
    if (std::abs(value) >= 0.5)
        return hyperbolic ? std::sinh(value) - value : value - std::sin(value);
    // Evaluate sinh(H)-H or E-sin(E) without subtracting nearly equal values.
    // Only the elementary anomaly conversion uses this series; CSPICE still
    // solves the conic. Twelve terms suffice for |value| < 0.5 in double precision.
    double term = value * value * value / 6, sum = term;
    const double square = (hyperbolic ? 1 : -1) * value * value;
    for (int n = 2; n <= 12; ++n) {
        term *= square / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}
} // namespace

core::CartesianState to_cartesian(core::KeplerianElements elements,
                                  core::GravitationalParameter parameter) {
    const double mu = parameter.metres_cubed_per_second_squared;
    const double a = elements.semi_major_axis_m, e = elements.eccentricity;
    const double pi = std::acos(-1.0);
    for (double value : {a, e, elements.inclination_rad, elements.raan_rad,
                         elements.argument_of_periapsis_rad, elements.true_anomaly_rad, mu})
        if (!std::isfinite(value))
            throw std::invalid_argument("Orbital elements and mu must be finite");
    if (mu <= 0 || e < 0 || e == 1 || a == 0 || (a > 0) != (e < 1) ||
        elements.inclination_rad < 0 || elements.inclination_rad > pi)
        throw std::invalid_argument("Require elliptical a>0, 0<=e<1 or hyperbolic a<0, e>1, and "
                                    "0<=i<=pi; parabolic states use Cartesian input");
    const double nu = std::remainder(elements.true_anomaly_rad, 2 * pi);
    double anomaly = 0;
    if (e < 1) {
        const double eccentric =
            std::atan2(std::sqrt((1 - e) * (1 + e)) * std::sin(nu), e + std::cos(nu));
        anomaly = (1 - e) * eccentric + e * anomaly_remainder(eccentric, false);
    } else {
        const double denominator = 1 + e * std::cos(nu);
        if (denominator <= 0)
            throw std::invalid_argument("True anomaly lies outside the hyperbolic branch");
        const double sinh_h = std::sqrt((e - 1) * (e + 1)) * std::sin(nu) / denominator;
        const double hyperbolic = std::asinh(sinh_h);
        anomaly = (e - 1) * hyperbolic + e * anomaly_remainder(hyperbolic, true);
    }
    // Pure conversion at the same epoch: no TDB/TAI elapsed-time interpretation.
    // Use the documented CSPICE kilometre units at this adapter boundary.
    const std::array<double, 8> conic{a * (1 - e) / 1000,
                                      e,
                                      elements.inclination_rad,
                                      std::remainder(elements.raan_rad, 2 * pi),
                                      std::remainder(elements.argument_of_periapsis_rad, 2 * pi),
                                      anomaly,
                                      0,
                                      mu / 1e9};
    for (double value : conic)
        if (!std::isfinite(value))
            throw std::invalid_argument("Orbital element conversion overflow");
    std::array<double, 6> values{};
    try {
        std::scoped_lock guard(engine().mutex);
        initialize_engine();
        conics_c(conic.data(), 0, values.data());
        check_spice();
    } catch (const core::EphemerisQueryError &error) {
        throw core::PropagationError(error.what());
    } catch (const core::EphemerisDataError &error) {
        throw core::PropagationError(error.what());
    }
    for (double &value : values) {
        value *= 1000;
        if (!std::isfinite(value))
            throw core::PropagationError("Orbital element conversion returned nonfinite values");
    }
    return {elements.epoch,
            core::OriginId::earth,
            core::FrameId::gcrf,
            {values[0], values[1], values[2]},
            {values[3], values[4], values[5]}};
}

core::KeplerianElements to_keplerian(core::CartesianState state, core::GravitationalParameter mu) {
    // Reuse the same finite, origin/frame and non-radial validation as propagation.
    (void)make_two_body(state, mu);
    std::array<double, 6> input{};
    for (std::size_t i = 0; i < 3; ++i) {
        input[i] = state.position_m[i] / 1000;
        input[i + 3] = state.velocity_mps[i] / 1000;
    }
    std::array<double, SPICE_OSCLTX_NELTS> output{};
    try {
        std::scoped_lock guard(engine().mutex);
        initialize_engine();
        oscltx_c(input.data(), 0, mu.metres_cubed_per_second_squared / 1e9, output.data());
        check_spice();
    } catch (const core::EphemerisQueryError &error) {
        throw core::PropagationError(error.what());
    } catch (const core::EphemerisDataError &error) {
        throw core::PropagationError(error.what());
    }
    core::KeplerianElements elements{state.epoch, output[9] * 1000, output[1], output[2],
                                     output[3],   output[4],        output[8]};
    if (elements.semi_major_axis_m == 0 || elements.eccentricity == 1)
        throw std::invalid_argument(
            "Semi-major axis is undefined near a parabolic orbit; retain Cartesian input");
    const auto recovered = to_cartesian(elements, mu);
    // NAIF recommends this check near singular element sets; never silently replace
    // a valid Cartesian draft with a numerically unreliable element representation.
    for (std::size_t i = 0; i < 3; ++i)
        if (std::abs(recovered.position_m[i] - state.position_m[i]) > 0.001 ||
            std::abs(recovered.velocity_mps[i] - state.velocity_mps[i]) > 1e-6)
            throw core::PropagationError(
                "Classical elements are numerically ill-conditioned; retain Cartesian input");
    return elements;
}

std::unique_ptr<core::IEphemerisProvider> load_snapshot(const std::filesystem::path &manifest_path,
                                                        const std::filesystem::path &workspace) {
    try {
        const auto manifest_bytes = read_file(manifest_path, 64 * 1024);
        const auto manifest = Json::parse(manifest_bytes);
        keys(manifest,
             {"schema_version", "snapshot_id", "model", "first_tai_ns", "last_tai_ns", "kernels"});
        if (!manifest.at("schema_version").is_number_integer() ||
            manifest.at("schema_version") != 1) {
            throw core::EphemerisDataError("Unsupported SPICE manifest schema version");
        }
        core::EphemerisDataInfo info{nonempty(manifest, "snapshot_id"),
                                     picosha2::hash256_hex_string(manifest_bytes),
                                     nonempty(manifest, "model"),
                                     "CSPICE_N0067",
                                     epoch_field(manifest, "first_tai_ns"),
                                     epoch_field(manifest, "last_tai_ns"),
                                     {}};
        if (info.first_epoch.tai_nanoseconds() >= info.last_epoch.tai_nanoseconds()) {
            throw core::EphemerisDataError("SPICE snapshot needs a nonempty policy window");
        }
        const auto &kernels = manifest.at("kernels");
        if (!kernels.is_array() || kernels.empty() || kernels.size() > 16) {
            throw core::EphemerisDataError("SPICE snapshot requires 1 to 16 explicit kernels");
        }
        const auto parent =
            std::filesystem::canonical(std::filesystem::absolute(manifest_path).parent_path());
        auto copies = std::make_unique<KernelCopies>(workspace);
        std::set<std::filesystem::path> paths;
        std::size_t total = 0;
        int lsk_count = 0, spk_count = 0;
        for (const auto &kernel : kernels) {
            keys(kernel, {"file", "kind", "sha256", "source_url", "source_sha256", "producer"});
            core::KernelDataInfo item{
                nonempty(kernel, "file"),          nonempty(kernel, "kind"),
                nonempty(kernel, "sha256"),        nonempty(kernel, "source_url"),
                nonempty(kernel, "source_sha256"), nonempty(kernel, "producer")};
            const std::filesystem::path name(item.file);
            if (name.has_parent_path() || name.has_root_path() || name == "." || name == ".." ||
                !hash_string(item.sha256) || !hash_string(item.source_sha256) ||
                (item.kind != "LSK" && item.kind != "SPK" && item.kind != "SPK_BODY_CENTRES")) {
                throw core::EphemerisDataError("Invalid SPICE filename, hash or kernel kind");
            }
            const auto path = std::filesystem::canonical(parent / name);
            if (path.parent_path() != parent || !paths.insert(path).second) {
                throw core::EphemerisDataError(
                    "SPICE kernel escapes the snapshot or is duplicated");
            }
            const auto bytes = read_file(path, 64 * 1024 * 1024 - total);
            total += bytes.size();
            if (picosha2::hash256_hex_string(bytes) != item.sha256) {
                throw core::EphemerisDataError("SPICE kernel SHA256 mismatch: " + item.file);
            }
            copies->add(bytes, item.kind);
            lsk_count += item.kind == "LSK" ? 1 : 0;
            spk_count += item.kind != "LSK" ? 1 : 0;
            info.kernels.push_back(std::move(item));
        }
        if (lsk_count != 1 || spk_count < 1) {
            throw core::EphemerisDataError(
                "SPICE snapshot requires exactly one LSK and at least one SPK");
        }
        auto provider = std::make_unique<Snapshot>(std::move(info), std::move(copies));
        provider->validate();
        return provider;
    } catch (const core::EphemerisDataError &) {
        throw;
    } catch (const std::exception &error) {
        throw core::EphemerisDataError(std::string("Cannot load SPICE snapshot: ") + error.what());
    }
}
std::vector<std::int32_t> extract_planetary_centres(const std::filesystem::path &source,
                                                    const std::filesystem::path &directory,
                                                    const std::function<void()> &checkpoint) {
    std::scoped_lock guard(engine().mutex);
    initialize_engine();
    if (source.string().size() > 255)
        throw core::EphemerisDataError("CSPICE input path exceeds 255 bytes");
    char architecture[8]{}, kind[8]{};
    getfat_c(source.string().c_str(), sizeof(architecture), sizeof(kind), architecture, kind);
    check_spice();
    if (std::string(architecture) != "DAF" || std::string(kind) != "SPK")
        throw core::EphemerisDataError("Planet-centre input must be a binary SPK");
    std::filesystem::create_directories(directory);
    std::map<std::int32_t, SpiceInt> outputs;
    std::vector<std::filesystem::path> created;
    try {
        visit_segments(source, [&](SpiceInt input, double *descriptor, const double *bounds,
                                   const SpiceInt *integers) {
            if (checkpoint)
                checkpoint();
            const auto id = integers[0];
            if (!planetary_centre(id))
                return;
            validate_centre_segment(bounds, integers);
            if (!outputs.contains(id)) {
                const auto path = directory / ("centre-" + std::to_string(id) + ".bsp");
                if (std::filesystem::exists(path) || path.string().size() > 255)
                    throw core::EphemerisDataError(
                        "Planet-centre output already exists or its path is too long");
                SpiceInt handle = 0;
                spkopn_c(path.string().c_str(), "OrbitForge planet-centre subset", 1024, &handle);
                check_spice();
                outputs[id] = handle;
                created.push_back(path);
                const char comments[][112] = {
                    "Producer of this derived kernel: OrbitForge project.",
                    "CSPICE N0067 spksub copied original segments without coefficient refitting.",
                    "Only the physical planet centre relative to its own barycentre is retained.",
                    "The accompanying manifest records original source SHA256 and provenance."};
                dafac_c(handle, 4, 112, comments);
                check_spice();
            }
            char identifier[41]{};
            dafgn_c(sizeof(identifier), identifier);
            check_spice();
            spksub_c(input, descriptor, identifier, bounds[0], bounds[1], outputs.at(id));
            check_spice();
        });
        if (outputs.empty())
            throw core::EphemerisDataError("SPK contains no supported physical planet centres");
        std::vector<std::int32_t> ids;
        for (auto &[id, handle] : outputs) {
            spkcls_c(handle);
            check_spice();
            handle = 0;
            ids.push_back(id);
        }
        if (checkpoint)
            checkpoint();
        return ids;
    } catch (...) {
        reset_c();
        for (const auto &[id, handle] : outputs) {
            (void)id;
            if (handle) {
                spkcls_c(handle);
                reset_c();
            }
        }
        for (const auto &path : created) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
        throw;
    }
}
namespace {
void validate_j2(const core::J2MeanElements &elements, const core::CentralBody &body) {
    for (const double value :
         {elements.semi_major_axis_m, elements.eccentricity, elements.inclination_rad,
          elements.raan_rad, elements.argument_of_periapsis_rad, elements.mean_anomaly_rad,
          body.mu.metres_cubed_per_second_squared, body.equatorial_radius_m, body.j2})
        if (!std::isfinite(value))
            throw std::invalid_argument("J2 elements and central-body constants must be finite");
    if (body.origin != core::OriginId::earth || body.mu.metres_cubed_per_second_squared <= 0 ||
        body.equatorial_radius_m <= 0 || std::abs(body.j2) > 0.01 ||
        elements.semi_major_axis_m <= 0 || elements.eccentricity < 0 ||
        elements.eccentricity >= 1 || elements.inclination_rad < 0 ||
        elements.inclination_rad > std::acos(-1.0) ||
        elements.semi_major_axis_m * (1 - elements.eccentricity) <= body.equatorial_radius_m)
        throw std::invalid_argument("J2 requires an Earth-centred mean ellipse with pericentre "
                                    "above the reference surface");
}
std::array<double, 3> cross(const std::array<double, 3> &a, const std::array<double, 3> &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
class J2Secular final : public core::IStateProvider {
  public:
    J2Secular(core::J2MeanElements initial, core::CentralBody body, core::J2SecularOptions options,
              bool fourth_order = false)
        : initial_(initial), body_(body) {
        validate_j2(initial_, body_);
        const double a = initial_.semi_major_axis_m, e = initial_.eccentricity;
        const double p = a * (1 - e) * (1 + e), c = std::cos(initial_.inclination_rad);
        n_ = std::sqrt(body_.mu.metres_cubed_per_second_squared / a) / a;
        const double factor = body_.j2 * std::pow(body_.equatorial_radius_m / p, 2) * n_;
        node_rate_ = options.node_precession ? -1.5 * factor * c : 0;
        apsis_rate_ = options.apsidal_precession ? 0.75 * factor * (5 * c * c - 1) : 0;
        mean_rate_ = n_ + (options.mean_motion_correction
                               ? 0.75 * factor * std::sqrt((1 - e) * (1 + e)) * (3 * c * c - 1)
                               : 0);
        if (fourth_order) {
            if (!std::isfinite(body.j4) || std::abs(body.j4) > 0.01)
                throw std::invalid_argument("Invalid J4 gravity coefficient");
            // Kozai secular J2, J2^2 and J4 equations. Adapted from the MIT-licensed
            // SatelliteToolboxPropagators.jl; see third_party/SatelliteToolbox-LICENSE.txt.
            // Keep the published node-term sign; do not fit it to a particular STK version.
            const double e2 = e * e, beta2 = (1 - e) * (1 + e), beta = std::sqrt(beta2);
            const double s2 = std::pow(std::sin(initial.inclination_rad), 2), s4 = s2 * s2;
            const double rp2 = std::pow(body.equatorial_radius_m / p, 2), rp4 = rp2 * rp2;
            const double j22 = body.j2 * body.j2;
            mean_rate_ =
                n_ * (1 + 0.75 * body.j2 * rp2 * beta * (2 - 3 * s2) +
                      (3.0 / 128) * j22 * rp4 * beta *
                          (120 + 64 * beta - 40 * beta2 + (-240 - 192 * beta + 40 * beta2) * s2 +
                           (105 + 144 * beta + 25 * beta2) * s4) -
                      (45.0 / 128) * body.j4 * rp4 * beta * e2 * (-8 + 40 * s2 - 35 * s4));
            const double k2 = mean_rate_ * body.j2 * rp2;
            const double k22 = mean_rate_ * j22 * rp4, n22 = n_ * j22 * rp4;
            const double k4 = n_ * body.j4 * rp4;
            node_rate_ =
                -1.5 * k2 * c +
                (3.0 / 32) * k22 * c * (-36 - 4 * e2 + 48 * beta + (40 - 5 * e2 - 72 * beta) * s2) +
                (15.0 / 32) * k4 * c * (8 + 12 * e2 - (14 + 21 * e2) * s2);
            apsis_rate_ =
                0.75 * k2 * (4 - 5 * s2) +
                (3.0 / 128) * k22 *
                    (384 + 96 * e2 - 384 * beta + (-824 - 116 * e2 + 1056 * beta) * s2 +
                     (430 - 5 * e2 - 720 * beta) * s4) -
                (15.0 / 16) * n22 * e2 * std::pow(c, 4) -
                (15.0 / 128) * k4 * (64 + 72 * e2 - (248 + 252 * e2) * s2 + (196 + 189 * e2) * s4);
        }
        if (!(n_ > 0) || !std::isfinite(n_) || !(mean_rate_ > 0))
            throw std::invalid_argument("J2 mean motion is outside the numerical domain");
        (void)state_at(initial_.epoch);
    }
    core::CartesianState state_at(core::Epoch epoch) const override {
        auto elements = initial_;
        const auto dt = epoch.seconds_since(initial_.epoch);
        const auto advance = [dt](double initial, double rate) {
            return std::remainder(std::fma(rate, dt, initial), 2 * std::acos(-1.0));
        };
        elements.epoch = epoch;
        elements.raan_rad = advance(initial_.raan_rad, node_rate_);
        elements.argument_of_periapsis_rad =
            advance(initial_.argument_of_periapsis_rad, apsis_rate_);
        elements.mean_anomaly_rad = advance(initial_.mean_anomaly_rad, mean_rate_);
        auto state = j2_mean_ellipse_state(elements, body_);
        const std::array<double, 3> normal{
            std::sin(elements.inclination_rad) * std::sin(elements.raan_rad),
            -std::sin(elements.inclination_rad) * std::cos(elements.raan_rad),
            std::cos(elements.inclination_rad)};
        const auto node_velocity = cross({0, 0, 1}, state.position_m);
        const auto apsis_velocity = cross(normal, state.position_m);
        // Differentiate the evolving ellipse, including its orientation and anomaly.
        // Keplerian velocity alone is not tangent to this secular trajectory.
        for (std::size_t k = 0; k < 3; ++k) {
            state.velocity_mps[k] = state.velocity_mps[k] * (mean_rate_ / n_) +
                                    node_rate_ * node_velocity[k] + apsis_rate_ * apsis_velocity[k];
            if (!std::isfinite(state.velocity_mps[k]))
                throw core::PropagationError("J2 returned non-finite velocity");
        }
        return state;
    }

  private:
    core::J2MeanElements initial_;
    core::CentralBody body_;
    double n_ = 0, node_rate_ = 0, apsis_rate_ = 0, mean_rate_ = 0;
};
} // namespace
core::CartesianState j2_mean_ellipse_state(core::J2MeanElements elements,
                                           const core::CentralBody &body) {
    validate_j2(elements, body);
    const double turn = 2 * std::acos(-1.0);
    const std::array<double, 8> conic{elements.semi_major_axis_m * (1 - elements.eccentricity) /
                                          1000,
                                      elements.eccentricity,
                                      elements.inclination_rad,
                                      std::remainder(elements.raan_rad, turn),
                                      std::remainder(elements.argument_of_periapsis_rad, turn),
                                      std::remainder(elements.mean_anomaly_rad, turn),
                                      0,
                                      body.mu.metres_cubed_per_second_squared / 1e9};
    std::array<double, 6> values{};
    try {
        std::scoped_lock guard(engine().mutex);
        initialize_engine();
        conics_c(conic.data(), 0, values.data());
        check_spice();
    } catch (const core::EphemerisQueryError &error) {
        throw core::PropagationError(error.what());
    } catch (const core::EphemerisDataError &error) {
        throw core::PropagationError(error.what());
    }
    for (double &value : values) {
        value *= 1000;
        if (!std::isfinite(value))
            throw core::PropagationError("J2 mean ellipse conversion returned non-finite values");
    }
    return {elements.epoch,
            body.origin,
            core::FrameId::gcrf,
            {values[0], values[1], values[2]},
            {values[3], values[4], values[5]}};
}
core::J2MeanElements j2_mean_ellipse_elements(core::CartesianState auxiliary_state,
                                              const core::CentralBody &body) {
    const auto ellipse = to_keplerian(auxiliary_state, body.mu);
    const auto e = ellipse.eccentricity, nu = ellipse.true_anomaly_rad;
    if (e >= 1)
        throw std::invalid_argument("J2 mean propagation requires a closed ellipse");
    const double eccentric =
        std::atan2(std::sqrt((1 - e) * (1 + e)) * std::sin(nu), e + std::cos(nu));
    core::J2MeanElements mean{ellipse.epoch,
                              ellipse.semi_major_axis_m,
                              e,
                              ellipse.inclination_rad,
                              ellipse.raan_rad,
                              ellipse.argument_of_periapsis_rad,
                              (1 - e) * eccentric + e * anomaly_remainder(eccentric, false)};
    validate_j2(mean, body);
    return mean;
}
std::shared_ptr<const core::IStateProvider> make_j2(core::J2MeanElements initial,
                                                    const core::CentralBody &body,
                                                    core::J2SecularOptions options) {
    return std::make_shared<J2Secular>(initial, body, options);
}
std::shared_ptr<const core::IStateProvider> make_j4(core::J4MeanElements initial,
                                                    const core::CentralBody &body) {
    return std::make_shared<J2Secular>(initial, body, core::J2SecularOptions{}, true);
}
} // namespace orbitforge::providers::spice
