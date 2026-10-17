#include <charconv>
#include <limits>
#include <orbitforge/runtime/data_catalog.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/gravity.hpp>
#include <orbitforge/runtime/ocean_tides.hpp>
#include <orbitforge/runtime/space_weather.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <stdexcept>
#include <string_view>
#ifdef ORBITFORGE_HAS_SPICE
#include <orbitforge/providers/spice/provider.hpp>
#endif

namespace orbitforge::runtime {
namespace {
const char *product_name(EopProduct product) {
    switch (product) {
    case EopProduct::c04:
        return "c04";
    case EopProduct::finals2000a:
        return "finals2000a";
    }
    throw std::invalid_argument("Unknown EOP product");
}
std::string extrapolation_key(EopProduct product) {
    return std::string("extrapolation-") + product_name(product) + "-days";
}
int parse_extrapolation(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return 0;
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (value.starts_with('+'))
        value.remove_prefix(1);
    int days = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), days);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && days >= 0 &&
                   days <= core::max_eop_extrapolation_days
               ? days
               : 0;
}
} // namespace
EopProduct DataCatalog::preferred_eop() const {
    return setting("eop-preferred-product") == "finals2000a" ? EopProduct::finals2000a
                                                             : EopProduct::c04;
}
void DataCatalog::set_preferred_eop(EopProduct product) {
    set_setting("eop-preferred-product", product_name(product));
}
int DataCatalog::extrapolation_days(EopProduct product) const {
    return parse_extrapolation(setting(extrapolation_key(product)));
}
void DataCatalog::set_extrapolation_days(EopProduct product, int days) {
    if (days < 0 || days > core::max_eop_extrapolation_days)
        throw std::invalid_argument("EOP extrapolation limit must be 0..30 days");
    set_setting(extrapolation_key(product), std::to_string(days));
}
} // namespace orbitforge::runtime

#ifdef ORBITFORGE_HAS_DATA_CATALOG
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <locale>
#include <mutex>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <set>
#include <sqlite3.h>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <thread>
#endif

namespace orbitforge::runtime {
namespace {
using Json = nlohmann::json;
void check(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
std::string utf8(const std::filesystem::path &path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}
bool hash_valid(const std::string &value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
           });
}
std::string read(const std::filesystem::path &path, std::size_t limit = 16 * 1024 * 1024) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    check(input.is_open(), "Cannot read catalog file: " + utf8(path));
    const auto size = input.tellg();
    check(size >= 0 && static_cast<std::uint64_t>(size) <= limit,
          "Catalog file exceeds size limit");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(input) && input.peek() == std::char_traits<char>::eof(),
          "Cannot read a stable catalog file");
    return bytes;
}
void write(const std::filesystem::path &path, const std::string &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
}
void publish(const std::filesystem::path &source, const std::filesystem::path &destination) {
    for (unsigned attempt = 0;; ++attempt) {
        std::error_code error;
        std::filesystem::rename(source, destination, error);
        if (!error)
            return;
#ifdef _WIN32
        // A short-lived handle in another process can deny publication on Windows.
        // Retry only sharing/access errors; never replace publication with a partial copy.
        if (attempt < 5 && error.category() == std::system_category() &&
            (error.value() == ERROR_ACCESS_DENIED || error.value() == ERROR_SHARING_VIOLATION ||
             error.value() == ERROR_LOCK_VIOLATION)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20U << attempt));
            continue;
        }
#else
        (void)attempt;
#endif
        throw std::filesystem::filesystem_error("Cannot publish catalog data", source, destination,
                                                error);
    }
}
struct Stage {
    std::filesystem::path path;
    explicit Stage(const std::filesystem::path &root) {
        std::filesystem::create_directories(root);
        for (unsigned i = 0; i < 10000; ++i) {
            auto candidate = root / ("stage-" + std::to_string(i));
            if (std::filesystem::create_directory(candidate)) {
                path = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("Cannot allocate data staging directory");
    }
    ~Stage() {
        if (path.empty())
            return;
        // Only exact direct children created by this operation; never recursive deletion.
        std::error_code error;
        for (auto it = std::filesystem::directory_iterator(path, error);
             !error && it != std::filesystem::directory_iterator(); it.increment(error))
            std::filesystem::remove(it->path(), error);
        std::filesystem::remove(path, error);
    }
};
struct Statement {
    sqlite3 *db;
    sqlite3_stmt *value = nullptr;
    Statement(sqlite3 *connection, const char *sql) : db(connection) {
        check(sqlite3_prepare_v2(db, sql, -1, &value, nullptr) == SQLITE_OK, sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(value); }
    void text(int index, const std::string &text) {
        check(text.size() <= 65536, "Catalog field exceeds limit");
        check(sqlite3_bind_text(value, index, text.data(), static_cast<int>(text.size()),
                                SQLITE_TRANSIENT) == SQLITE_OK,
              sqlite3_errmsg(db));
    }
    void integer(int index, std::int64_t number) {
        check(sqlite3_bind_int64(value, index, number) == SQLITE_OK, sqlite3_errmsg(db));
    }
    bool next() {
        const int code = sqlite3_step(value);
        check(code == SQLITE_ROW || code == SQLITE_DONE, sqlite3_errmsg(db));
        return code == SQLITE_ROW;
    }
    std::string text(int index) const {
        const auto *data = sqlite3_column_text(value, index);
        return data ? reinterpret_cast<const char *>(data) : "";
    }
};
void sql(sqlite3 *db, const char *command) {
    check(sqlite3_exec(db, command, nullptr, nullptr, nullptr) == SQLITE_OK, sqlite3_errmsg(db));
}
struct Transaction {
    sqlite3 *db;
    explicit Transaction(sqlite3 *connection, bool write = true) : db(connection) {
        sql(db, write ? "BEGIN IMMEDIATE" : "BEGIN");
    }
    ~Transaction() { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { sql(db, "COMMIT"); }
};
} // namespace
struct DataCatalog::Impl {
    std::filesystem::path root;
    sqlite3 *db = nullptr;
    mutable std::recursive_mutex mutex;
    ~Impl() {
        if (db)
            sqlite3_close(db);
    }
    std::filesystem::path directory(const std::string &key) const {
        check(hash_valid(key), "Invalid catalog snapshot hash");
        return root / "snapshots" / key;
    }
    CatalogSnapshot describe(const std::string &key, const core::EopDataInfo &info) const {
        CatalogSnapshot result{key,
                               info.id,
                               info.source_url,
                               info.first_epoch,
                               info.last_epoch,
                               directory(key) / "manifest.json"};
        Statement receipt(db, "SELECT kind,acquired_at FROM acquisitions WHERE lock_hash=?");
        receipt.text(1, key);
        if (receipt.next()) {
            result.acquisition = static_cast<DataAcquisition>(sqlite3_column_int(receipt.value, 0));
            if (sqlite3_column_type(receipt.value, 1) != SQLITE_NULL)
                result.acquired_at_unix_seconds = sqlite3_column_int64(receipt.value, 1);
        }
        return result;
    }
    std::unique_ptr<core::IEarthOrientationProvider> verify(const std::string &key) const {
        const auto folder = directory(key);
        check(std::filesystem::canonical(folder).parent_path() == root / "snapshots",
              "Catalog snapshot resolves outside its directory");
        const auto record = Json::parse(read(folder / "record.json", 65536));
        check((record.at("schema") == 1 || record.at("schema") == 2) &&
                  record.at("lock_sha256") == key,
              "Catalog record identity mismatch");
        const auto bytes = read(folder / "manifest.json", 65536);
        check(picosha2::hash256_hex_string(bytes) ==
                  record.at("manifest_sha256").get<std::string>(),
              "Catalog manifest SHA256 mismatch");
        auto clock = make_builtin_time_provider();
        if (record.at("schema") == 2) {
            check(std::filesystem::canonical(folder / "leap-seconds.dat").parent_path() == folder,
                  "Time data resolves outside its snapshot directory");
            clock = load_time_snapshot(folder / "leap-seconds.dat");
            check(clock->data_info().sha256 == record.at("time_sha256").get<std::string>(),
                  "Time data SHA256 mismatch");
        }
        auto provider = load_eop_snapshot_with_time(folder / "manifest.json", *clock);
        check(application::eop_data_lock(provider->data_info()).sha256() == key,
              "Catalog snapshot does not match its pinned data/time identity");
        const auto blob = root / "blobs" / "sha256" / provider->data_info().sha256;
        check(picosha2::hash256_hex_string(read(blob)) == provider->data_info().sha256,
              "Catalog content blob SHA256 mismatch");
        return provider;
    }
    void index(const std::string &key, const core::EopDataInfo &info) {
        Statement row(db, "INSERT OR IGNORE INTO snapshots VALUES(?, ?, ?, ?, ?)");
        row.text(1, key);
        row.text(2, info.id);
        row.text(3, info.source_url);
        row.integer(4, info.first_epoch.tai_nanoseconds());
        row.integer(5, info.last_epoch.tai_nanoseconds());
        row.next();
        const auto lock = application::eop_data_lock(info);
        for (const auto &ref : lock.references()) {
            Statement dep(db, "INSERT OR IGNORE INTO dependencies VALUES(?, ?, ?, ?, ?, ?)");
            dep.text(1, key);
            dep.text(2, ref.role);
            dep.text(3, ref.snapshot_id);
            dep.text(4, ref.content_sha256);
            dep.text(5, ref.interpretation_version);
            dep.text(6, ref.configuration_sha256);
            dep.next();
        }
    }
    void blob(const std::string &bytes, const std::string &hash,
              const std::filesystem::path &stage) {
        check(hash_valid(hash) && picosha2::hash256_hex_string(bytes) == hash,
              "Invalid blob SHA256");
        const auto target = root / "blobs" / "sha256" / hash;
        if (std::filesystem::exists(target)) {
            check(read(target) == bytes, "Existing content blob is damaged; refusing replacement");
            return;
        }
        const auto pending = stage / "blob.pending";
        write(pending, bytes);
        publish(pending, target);
    }
};
bool data_catalog_available() noexcept { return true; }
DataCatalog::DataCatalog(std::filesystem::path root) : impl_(std::make_unique<Impl>()) {
    std::filesystem::create_directories(root);
    impl_->root = std::filesystem::canonical(root);
    for (const auto &part :
         {"snapshots", "blobs/sha256", "staging", "time", "gravity", "ephemerides", "runtime"})
        std::filesystem::create_directories(impl_->root / part);
    check(sqlite3_open_v2(utf8(impl_->root / "catalog.sqlite").c_str(), &impl_->db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                          nullptr) == SQLITE_OK,
          "Cannot open data catalog");
    sqlite3_busy_timeout(impl_->db, 10000);
    sql(impl_->db, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL");
    Transaction transaction(impl_->db);
    Statement version(impl_->db, "PRAGMA user_version");
    version.next();
    const auto schema = sqlite3_column_int(version.value, 0);
    version.next();
    check(schema >= 0 && schema <= 2, "Unsupported data catalog schema");
    sql(impl_->db,
        "CREATE TABLE IF NOT EXISTS snapshots(lock_hash TEXT PRIMARY KEY, id TEXT NOT NULL, "
        "source_url TEXT NOT NULL, first_ns INTEGER NOT NULL, last_ns INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS dependencies(lock_hash TEXT REFERENCES snapshots(lock_hash), "
        "role TEXT, snapshot_id TEXT, content_hash TEXT, interpretation TEXT, configuration_hash "
        "TEXT, PRIMARY KEY(lock_hash,role));"
        "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS acquisitions(lock_hash TEXT PRIMARY KEY REFERENCES "
        "snapshots(lock_hash), kind INTEGER NOT NULL, acquired_at INTEGER); PRAGMA user_version=2");
    // Recover only successfully published directories left by a process that stopped
    // between file publication and the SQLite commit; invalid records stay unindexed.
    for (const auto &entry : std::filesystem::directory_iterator(impl_->root / "snapshots")) {
        const auto key = entry.path().filename().string();
        if (!entry.is_directory() || !hash_valid(key))
            continue;
        Statement exists(impl_->db, "SELECT 1 FROM snapshots WHERE lock_hash=?");
        exists.text(1, key);
        if (exists.next())
            continue;
        try {
            impl_->index(key, impl_->verify(key)->data_info());
        } catch (const std::exception &) {
        }
    }
    transaction.commit();
}
DataCatalog::~DataCatalog() = default;
const std::filesystem::path &DataCatalog::root() const noexcept { return impl_->root; }
CatalogEphemeris DataCatalog::install_de440s(const std::filesystem::path &spk,
                                             DataAcquisition acquisition,
                                             const std::function<void()> &checkpoint) {
    if (!ephemeris_available())
        throw core::EphemerisDataError("This build does not include CSPICE");
    const auto previous = setting("ephemeris/de440s");
    Stage stage(impl_->root / "staging");
    const auto file = stage.path / "de440s.bsp";
    std::ifstream input(spk, std::ios::binary);
    check(input.is_open(), "Cannot open DE440s file");
    std::ofstream output(file, std::ios::binary);
    picosha2::hash256_one_by_one hash;
    std::array<char, 65536> bytes{};
    std::size_t size = 0;
    while (input) {
        if (checkpoint)
            checkpoint();
        input.read(bytes.data(), bytes.size());
        const auto count = input.gcount();
        size += static_cast<std::size_t>(count);
        check(size <= 64U * 1024 * 1024, "DE440s file exceeds 64 MiB");
        hash.process(bytes.begin(), bytes.begin() + count);
        output.write(bytes.data(), count);
        check(static_cast<bool>(output), "Cannot stage DE440s file");
    }
    check(input.eof() && !input.bad(), "Cannot read DE440s file");
    output.close();
    check(static_cast<bool>(output), "Cannot close DE440s file");
    hash.finish();
    check(picosha2::get_hash_hex_string(hash) == de440s_sha256,
          "DE440s checksum mismatch; select the complete official de440s.bsp");
    // NAIF uniform-time model only. UTC leap seconds remain an independent resource.
    const std::string lsk = "KPL/LSK\n\\begindata\nDELTET/DELTA_T_A = 32.184\n"
                            "DELTET/K = 1.657D-3\nDELTET/EB = 1.671D-2\nDELTET/M = ( 6.239996D0 "
                            "1.99096871D-7 )\n\\begintext\n";
    write(stage.path / "uniform-time.tls", lsk);
    const auto tai = [](int year) {
        const auto days = std::chrono::sys_days{std::chrono::year{year} / 1 / 1} -
                          std::chrono::sys_days{std::chrono::year{2000} / 1 / 1};
        return std::chrono::duration_cast<std::chrono::nanoseconds>(days - std::chrono::hours{12})
            .count();
    };
    const auto lsk_hash = picosha2::hash256_hex_string(lsk);
    Json manifest{
        {"schema_version", 1},
        {"snapshot_id", "de440s-1850-2150-v1"},
        {"model", "DE440s; ICRF axes; geometric Earth/Sun/Moon"},
        {"first_tai_ns", tai(1850)},
        {"last_tai_ns", tai(2150)},
        {"kernels",
         Json::array({{{"file", "uniform-time.tls"},
                       {"kind", "LSK"},
                       {"sha256", lsk_hash},
                       {"source_url",
                        "https://naif.jpl.nasa.gov/pub/naif/generic_kernels/lsk/naif0012.tls"},
                       {"source_sha256",
                        "678e32bdb5a744117a467cd9601cd6b373f0e9bc9bbde1371d5eee39600a039b"},
                       {"producer", "OrbitForge; NAIF uniform-time constants only"}},
                      {{"file", "de440s.bsp"},
                       {"kind", "SPK"},
                       {"sha256", de440s_sha256},
                       {"source_url", de440s_url},
                       {"source_sha256", de440s_sha256},
                       {"producer", "JPL / NAIF"}}})}};
    // Refreshing the base must retain already imported physical-centre kernels.
    if (!previous.empty()) {
        const auto entries = ephemerides();
        (void)load_ephemeris("de440s");
        const auto old = Json::parse(read(entries.at(0).manifest, 65536));
        for (const auto &kernel : old.at("kernels")) {
            if (kernel.at("kind") != "SPK_BODY_CENTRES")
                continue;
            const auto name = kernel.at("file").get<std::string>();
            const auto supplement =
                read(entries.at(0).manifest.parent_path() / name, 64 * 1024 * 1024);
            check(picosha2::hash256_hex_string(supplement) ==
                      kernel.at("sha256").get<std::string>(),
                  "Installed planet-centre kernel changed");
            write(stage.path / name, supplement);
            manifest["kernels"].push_back(kernel);
        }
        if (manifest["kernels"].size() > 2) {
            manifest["snapshot_id"] = "de440s-with-planetary-centres-v1";
            manifest["model"] = "DE440s + imported planetary centres; ICRF/J2000";
        }
    }
    write(stage.path / "manifest.json", manifest.dump(2));
    return install_ephemeris_snapshot_checked(stage.path / "manifest.json", acquisition, checkpoint,
                                              previous);
}
CatalogEphemeris DataCatalog::install_ephemeris_snapshot(const std::filesystem::path &manifest,
                                                         DataAcquisition acquisition,
                                                         const std::function<void()> &checkpoint) {
    return install_ephemeris_snapshot_checked(manifest, acquisition, checkpoint,
                                              setting("ephemeris/de440s"));
}
CatalogEphemeris DataCatalog::install_ephemeris_snapshot_checked(
    const std::filesystem::path &source, DataAcquisition acquisition,
    const std::function<void()> &checkpoint, const std::string &expected_receipt) {
    if (!ephemeris_available())
        throw core::EphemerisDataError("This build does not include CSPICE");
    Stage stage(impl_->root / "staging");
    const auto bytes = read(source, 65536);
    const auto manifest = Json::parse(bytes);
    const auto &kernels = manifest.at("kernels");
    check(kernels.is_array() && !kernels.empty() && kernels.size() <= 16,
          "Invalid ephemeris kernel list");
    const auto parent = std::filesystem::canonical(source).parent_path();
    std::size_t total = 0;
    std::set<std::filesystem::path> seen;
    for (const auto &kernel : kernels) {
        if (checkpoint)
            checkpoint();
        const std::filesystem::path name(kernel.at("file").get<std::string>());
        check(!name.empty() && !name.has_parent_path() && name != "manifest.json" && name != "." &&
                  name != ".." && !name.has_root_path(),
              "Invalid ephemeris kernel filename");
        const auto path = std::filesystem::canonical(parent / name);
        check(path.parent_path() == parent && seen.insert(path).second,
              "Ephemeris kernel escapes the snapshot or is duplicated");
        const auto content = read(path, 64 * 1024 * 1024 - total);
        total += content.size();
        check(picosha2::hash256_hex_string(content) == kernel.at("sha256").get<std::string>(),
              "Ephemeris kernel SHA256 mismatch");
        write(stage.path / name, content);
    }
    write(stage.path / "manifest.json", bytes);
    const auto resource = make_third_body_ephemeris(
        load_spice_snapshot(stage.path / "manifest.json", impl_->root / "runtime"));
    const auto info = resource->data_info();
    resource->validate_interval(info.first, info.last, core::third_body_list);
    const auto acquired = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    const auto target = impl_->root / "ephemerides" / info.content_sha256;
    const Json record{{"schema", 1},
                      {"model", info.model},
                      {"sha256", info.content_sha256},
                      {"interpretation", info.interpretation_version},
                      {"first", info.first.tai_nanoseconds()},
                      {"last", info.last.tai_nanoseconds()},
                      {"sun_mu", info.sun_mu},
                      {"moon_mu", info.moon_mu},
                      {"acquisition", static_cast<int>(acquisition)},
                      {"acquired_at", acquired}};
    std::lock_guard guard(impl_->mutex);
    Transaction transaction(impl_->db);
    check(setting("ephemeris/de440s") == expected_receipt,
          "Ephemeris changed during import; retry with the current snapshot");
    if (checkpoint)
        checkpoint();
    if (std::filesystem::exists(target)) {
        const auto existing =
            load_spice_snapshot(target / "manifest.json", impl_->root / "runtime");
        check(existing->data_info().manifest_sha256 == info.content_sha256,
              "Installed ephemeris is damaged");
    } else
        publish(stage.path, target);
    set_setting("ephemeris/de440s", record.dump());
    transaction.commit();
    return {"de440s", info, target / "manifest.json", acquisition, acquired};
}
CatalogEphemeris DataCatalog::install_planetary_centres(const std::filesystem::path &source,
                                                        const std::string &url,
                                                        DataAcquisition acquisition,
                                                        const std::function<void()> &checkpoint) {
#ifndef ORBITFORGE_HAS_SPICE
    (void)source;
    (void)url;
    (void)acquisition;
    (void)checkpoint;
    throw core::EphemerisDataError("This build does not include CSPICE");
#else
    const auto previous = setting("ephemeris/de440s");
    check(!previous.empty(), "Install DE440s before importing planet-centre ephemerides");
    const auto entry = ephemerides().at(0);
    (void)load_ephemeris("de440s");
    Stage stage(impl_->root / "staging");
    const auto staged_source = stage.path / "source.bsp";
    std::ifstream input(source, std::ios::binary);
    check(input.is_open(), "Cannot open supplemental planetary SPK");
    std::ofstream output(staged_source, std::ios::binary);
    picosha2::hash256_one_by_one hash;
    std::array<char, 65536> buffer{};
    std::uint64_t size = 0;
    while (input) {
        if (checkpoint)
            checkpoint();
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();
        size += static_cast<std::uint64_t>(count);
        check(size <= 2ULL * 1024 * 1024 * 1024, "Supplemental SPK exceeds 2 GiB");
        hash.process(buffer.begin(), buffer.begin() + count);
        output.write(buffer.data(), count);
        check(static_cast<bool>(output), "Cannot stage supplemental SPK");
    }
    check(input.eof() && !input.bad(), "Cannot read supplemental SPK");
    output.close();
    check(static_cast<bool>(output), "Cannot close staged supplemental SPK");
    hash.finish();
    const auto source_hash = picosha2::get_hash_hex_string(hash);
    const auto bodies =
        providers::spice::extract_planetary_centres(staged_source, stage.path, checkpoint);
    auto manifest = Json::parse(read(entry.manifest, 65536));
    Json kernels = Json::array();
    std::set<std::string> replacements;
    for (auto id : bodies)
        replacements.insert("centre-" + std::to_string(id) + ".bsp");
    for (const auto &kernel : manifest.at("kernels")) {
        const auto name = kernel.at("file").get<std::string>();
        if (kernel.at("kind") == "SPK_BODY_CENTRES" && replacements.contains(name))
            continue;
        const auto content = read(entry.manifest.parent_path() / name, 64 * 1024 * 1024);
        check(picosha2::hash256_hex_string(content) == kernel.at("sha256").get<std::string>(),
              "Installed ephemeris changed during import");
        write(stage.path / name, content);
        kernels.push_back(kernel);
    }
    for (auto id : bodies) {
        const auto name = "centre-" + std::to_string(id) + ".bsp";
        const auto content = read(stage.path / name, 64 * 1024 * 1024);
        kernels.push_back({{"file", name},
                           {"kind", "SPK_BODY_CENTRES"},
                           {"sha256", picosha2::hash256_hex_string(content)},
                           {"source_url", url.empty() ? "urn:sha256:" + source_hash : url},
                           {"source_sha256", source_hash},
                           {"producer", "OrbitForge; CSPICE spksub physical-centre subset"}});
    }
    std::stable_sort(kernels.begin(), kernels.end(), [](const auto &a, const auto &b) {
        const bool ca = a.at("kind") == "SPK_BODY_CENTRES";
        const bool cb = b.at("kind") == "SPK_BODY_CENTRES";
        if (ca != cb)
            return !ca;
        return ca &&
               a.at("file").template get<std::string>() < b.at("file").template get<std::string>();
    });
    manifest["kernels"] = std::move(kernels);
    manifest["snapshot_id"] = "de440s-with-planetary-centres-v1";
    manifest["model"] = "DE440s + imported planetary centres; ICRF/J2000";
    write(stage.path / "manifest.json", manifest.dump(2));
    return install_ephemeris_snapshot_checked(stage.path / "manifest.json", acquisition, checkpoint,
                                              previous);
#endif
}
std::vector<CatalogEphemeris> DataCatalog::ephemerides() const {
    const auto value = setting("ephemeris/de440s");
    if (value.empty())
        return {};
    const auto r = Json::parse(value);
    check(r.at("schema") == 1 && hash_valid(r.at("sha256")), "Invalid ephemeris receipt");
    core::ThirdBodyDataInfo info{r.at("model"),
                                 r.at("sha256"),
                                 r.at("interpretation"),
                                 core::Epoch{r.at("first").get<std::int64_t>()},
                                 core::Epoch{r.at("last").get<std::int64_t>()},
                                 r.at("sun_mu"),
                                 r.at("moon_mu"),
                                 de440_planet_mu};
    return {{"de440s", info, impl_->root / "ephemerides" / info.content_sha256 / "manifest.json",
             static_cast<DataAcquisition>(r.at("acquisition").get<int>()), r.at("acquired_at")}};
}
std::shared_ptr<const core::IThirdBodyEphemeris>
DataCatalog::load_ephemeris(const std::string &selection) const {
    const auto entries = ephemerides();
    const auto found = std::ranges::find(entries, selection, &CatalogEphemeris::selection);
    check(found != entries.end(), "Ephemeris is missing; download DE440s in Data Center");
    check(std::filesystem::canonical(found->manifest).parent_path().parent_path() ==
              impl_->root / "ephemerides",
          "Ephemeris resolves outside its catalog directory");
    const auto provider =
        make_third_body_ephemeris(load_spice_snapshot(found->manifest, impl_->root / "runtime"));
    check(provider->data_info().content_sha256 == found->info.content_sha256,
          "Ephemeris manifest SHA256 mismatch");
    return provider;
}
CatalogGravity DataCatalog::install_gravity(const std::filesystem::path &source,
                                            const std::string &selection, const std::string &url,
                                            DataAcquisition acquisition,
                                            const std::function<void()> &checkpoint) {
    check(!selection.empty() && selection.size() <= 64 &&
              std::all_of(selection.begin(), selection.end(),
                          [](char c) {
                              return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
                          }),
          "Invalid gravity selection ID");
    Stage stage(impl_->root / "staging");
    const auto pending = stage.path / "gravity.dat";
    // Own a stable copy before validation; never parse a file that can be replaced by import.
    std::ifstream input(source, std::ios::binary);
    check(input.is_open(), "Cannot open gravity source");
    std::ofstream output(pending, std::ios::binary);
    std::array<char, 65536> buffer{};
    std::size_t total = 0;
    while (input) {
        if (checkpoint)
            checkpoint();
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        total += static_cast<std::size_t>(input.gcount());
        check(total <= 512U * 1024 * 1024, "Gravity file exceeds 512 MiB");
        output.write(buffer.data(), input.gcount());
        check(static_cast<bool>(output), "Cannot stage gravity file");
    }
    check(input.eof() && !input.bad(), "Cannot read gravity source");
    output.close();
    check(static_cast<bool>(output), "Cannot close staged gravity file");
    const auto model = runtime::load_gravity(pending, url, checkpoint);
    const auto &info = model->info();
    if (selection == "egm96" || selection == "egm2008") {
        check(info.model == (selection == "egm96" ? "EGM96" : "EGM2008"),
              "Gravity model does not match the selected product");
        // Official static products are pinned to their full original bytes. Local/custom
        // variants belong under a separate ID, not under the official product's identity.
        check(info.sha256 ==
                  (selection == "egm96"
                       ? "5247a9e9c316dd2c8f8fd491d53be0e163cb5cb3676b021754240cc9e44cb43b"
                       : "ab5b524da073e63b5bdceb7ca47a0de07a26dd44a1c5798f39fc98dc80af70fd"),
              "Official gravity file checksum mismatch; import modified models as custom data");
    }
    const auto target = impl_->root / "gravity" / (info.sha256 + ".dat");
    const auto acquired = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    Json secular = nullptr;
    if (info.secular_variations) {
        Json rates = Json::array();
        for (const auto &v : info.secular_variations->rates_per_year)
            rates.push_back({v.degree, v.order, v.c, v.s});
        secular = {{"epoch_tai_ns", info.secular_variations->reference_epoch.tai_nanoseconds()},
                   {"rates_per_julian_year", std::move(rates)}};
    }
    const Json record{{"schema", 3},
                      {"secular_variations", std::move(secular)},
                      {"solid_tide_k20", info.solid_tide_k20},
                      {"model", info.model},
                      {"sha256", info.sha256},
                      {"source_url", url},
                      {"tide_system", info.tide_system},
                      {"mu", info.mu},
                      {"radius_m", info.radius_m},
                      {"degree", info.degree},
                      {"order", info.order},
                      {"acquisition", static_cast<int>(acquisition)},
                      {"acquired_at", acquired}};
    std::lock_guard guard(impl_->mutex);
    Transaction transaction(impl_->db);
    if (checkpoint)
        checkpoint();
    if (std::filesystem::exists(target))
        check(runtime::load_gravity(target, url, checkpoint)->info().sha256 == info.sha256,
              "Existing gravity resource is damaged");
    else
        publish(pending, target);
    set_setting("gravity/" + selection, record.dump());
    transaction.commit();
    return {selection, info, target, acquisition, acquired};
}
std::vector<CatalogGravity> DataCatalog::gravity_models() const {
    std::lock_guard guard(impl_->mutex);
    Statement rows(impl_->db,
                   "SELECT key,value FROM settings WHERE key LIKE 'gravity/%' ORDER BY key");
    std::vector<CatalogGravity> result;
    while (rows.next()) {
        const auto key = reinterpret_cast<const char *>(sqlite3_column_text(rows.value, 0));
        const auto r =
            Json::parse(reinterpret_cast<const char *>(sqlite3_column_text(rows.value, 1)));
        check(r.at("schema") == 3 && hash_valid(r.at("sha256")),
              "Unsupported gravity receipt; reimport or download gravity data");
        core::GravityInfo info{r.at("model"),       r.at("sha256"), r.at("source_url"),
                               r.at("tide_system"), r.at("mu"),     r.at("radius_m"),
                               r.at("degree"),      r.at("order"),  r.at("solid_tide_k20")};
        check(std::isfinite(info.solid_tide_k20), "Invalid gravity Love number receipt");
        const auto &secular = r.at("secular_variations");
        if (!secular.is_null()) {
            const auto &epoch = secular.at("epoch_tai_ns");
            check(epoch.is_number_integer() &&
                      (!epoch.is_number_unsigned() ||
                       epoch.get<std::uint64_t>() <=
                           static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())),
                  "Invalid gravity secular epoch receipt");
            const auto &rates = secular.at("rates_per_julian_year");
            check(info.degree >= 2 && info.degree <= core::gravity_max_degree && info.order >= 0 &&
                      info.order <= info.degree && rates.is_array() && !rates.empty() &&
                      rates.size() <=
                          static_cast<std::size_t>((info.degree + 1) * (info.degree + 2) / 2),
                  "Invalid gravity secular rate receipt");
            info.secular_variations =
                core::GravitySecularVariations{core::Epoch{epoch.get<std::int64_t>()}, {}};
            auto &values = info.secular_variations->rates_per_year;
            for (const auto &v : rates) {
                check(v.is_array() && v.size() == 4 && v[0].is_number_integer() &&
                          v[1].is_number_integer() && v[0] >= 2 && v[0] <= info.degree &&
                          v[1] >= 0 && v[1] <= v[0] && v[1] <= info.order,
                      "Invalid gravity secular rate indices");
                core::GravityCoefficient term{v[0], v[1], v[2], v[3]};
                check(
                    std::isfinite(term.c) && std::isfinite(term.s) &&
                        (term.order != 0 || term.s == 0) &&
                        (values.empty() || values.back().degree < term.degree ||
                         (values.back().degree == term.degree && values.back().order < term.order)),
                    "Invalid or duplicate gravity secular rate receipt");
                values.push_back(term);
            }
        }
        result.push_back(
            {std::string(key).substr(8), info, impl_->root / "gravity" / (info.sha256 + ".dat"),
             static_cast<DataAcquisition>(r.at("acquisition").get<int>()), r.at("acquired_at")});
    }
    return result;
}
std::shared_ptr<const core::GravityModel>
DataCatalog::load_gravity(const std::string &selection) const {
    const auto models = gravity_models();
    const auto it = std::find_if(models.begin(), models.end(),
                                 [&](const auto &v) { return v.selection == selection; });
    check(it != models.end(),
          "Gravity model is not installed; download or import it in Data Center");
    const auto model = runtime::load_gravity(it->file, it->info.source_url);
    check(gravity_reference(selection, model->info()) == gravity_reference(selection, it->info),
          "Gravity content or physical metadata differs from its receipt; reimport the data");
    return model;
}
CatalogSpaceWeather DataCatalog::install_space_weather(const std::filesystem::path &source,
                                                       const std::string &selection,
                                                       const std::string &url,
                                                       DataAcquisition acquisition,
                                                       const std::function<void()> &checkpoint) {
    check(!selection.empty() && selection.size() <= 64 &&
              std::all_of(selection.begin(), selection.end(),
                          [](char c) {
                              return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
                          }),
          "Invalid space-weather selection ID");
    Stage stage(impl_->root / "staging");
    const auto pending = stage.path / "space-weather.dat";
    // Own a stable copy before validation; never parse a file that can be replaced by import.
    std::ifstream input(source, std::ios::binary);
    check(input.is_open(), "Cannot open space-weather source");
    std::ofstream output(pending, std::ios::binary);
    std::array<char, 65536> buffer{};
    std::size_t total = 0;
    while (input) {
        if (checkpoint)
            checkpoint();
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        total += static_cast<std::size_t>(input.gcount());
        check(total <= 32U * 1024 * 1024, "Space weather file exceeds 32 MiB");
        output.write(buffer.data(), input.gcount());
        check(static_cast<bool>(output), "Cannot stage space-weather file");
    }
    check(input.eof() && !input.bad(), "Cannot read space-weather source");
    output.close();
    check(static_cast<bool>(output), "Cannot close staged space-weather file");
    const auto model = runtime::load_space_weather(pending, url, checkpoint);
    const auto &info = model->info();
    const auto target = impl_->root / "space-weather" / (info.sha256 + ".dat");
    std::filesystem::create_directories(target.parent_path());
    const auto acquired = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    const Json record{{"schema", 1},
                      {"sha256", info.sha256},
                      {"source_url", url},
                      {"format", info.format},
                      {"first", {info.first.year, info.first.month, info.first.day}},
                      {"last", {info.last.year, info.last.month, info.last.day}},
                      {"acquisition", static_cast<int>(acquisition)},
                      {"acquired_at", acquired}};
    std::lock_guard guard(impl_->mutex);
    Transaction transaction(impl_->db);
    if (checkpoint)
        checkpoint();
    if (std::filesystem::exists(target))
        check(runtime::load_space_weather(target, url, checkpoint)->info().sha256 == info.sha256,
              "Existing space-weather resource is damaged");
    else
        publish(pending, target);
    set_setting("space-weather/" + selection, record.dump());
    transaction.commit();
    return {selection, info, target, acquisition, acquired};
}
std::vector<CatalogSpaceWeather> DataCatalog::weather_sources() const {
    std::lock_guard guard(impl_->mutex);
    Statement rows(impl_->db,
                   "SELECT key,value FROM settings WHERE key LIKE 'space-weather/%' ORDER BY key");
    std::vector<CatalogSpaceWeather> result;
    while (rows.next()) {
        const auto key = reinterpret_cast<const char *>(sqlite3_column_text(rows.value, 0));
        const auto r =
            Json::parse(reinterpret_cast<const char *>(sqlite3_column_text(rows.value, 1)));
        check(r.at("schema") == 1 && hash_valid(r.at("sha256")), "Invalid space-weather receipt");
        const auto date = [&](const char *field) {
            const auto &d = r.at(field);
            return core::UtcDateTime{d.at(0), d.at(1), d.at(2), 0, 0, 0, 0};
        };
        core::SpaceWeatherInfo info{r.at("sha256"), r.at("source_url"), r.at("format"),
                                    date("first"), date("last")};
        // The file format determines the flux convention; keep receipt metadata
        // consistent with load_space_weather without a redundant persisted flag.
        info.solar_flux_adjusted = info.format == "stk-flux-geomag";
        info.cssi_predictions = info.format == "cssi-predicts";
        result.push_back({std::string(key).substr(14), info,
                          impl_->root / "space-weather" / (info.sha256 + ".dat"),
                          static_cast<DataAcquisition>(r.at("acquisition").get<int>()),
                          r.at("acquired_at")});
    }
    return result;
}
std::shared_ptr<const core::SpaceWeather>
DataCatalog::load_space_weather(const std::string &selection) const {
    const auto models = weather_sources();
    const auto it = std::find_if(models.begin(), models.end(),
                                 [&](const auto &v) { return v.selection == selection; });
    check(it != models.end(),
          "Space weather model is not installed; download or import it in Data Center");
    const auto model = runtime::load_space_weather(it->file, it->info.source_url);
    check(model->info().sha256 == it->info.sha256, "Space weather content SHA256 mismatch");
    return model;
}
CatalogOceanTides DataCatalog::install_ocean_tides(const std::filesystem::path &source,
                                                   const std::string &selection,
                                                   const std::string &url,
                                                   DataAcquisition acquisition,
                                                   const std::function<void()> &checkpoint) {
    check(!selection.empty() && selection.size() <= 64 &&
              std::all_of(selection.begin(), selection.end(),
                          [](char c) {
                              return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
                          }),
          "Invalid ocean-tides selection ID");
    Stage stage(impl_->root / "staging");
    const auto pending = stage.path / "ocean-tides.dat";
    // Own a stable copy before validation; never parse a file that can be replaced by import.
    std::ifstream input(source, std::ios::binary);
    check(input.is_open(), "Cannot open ocean-tides source");
    std::ofstream output(pending, std::ios::binary);
    std::array<char, 65536> buffer{};
    std::size_t total = 0;
    while (input) {
        if (checkpoint)
            checkpoint();
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        total += static_cast<std::size_t>(input.gcount());
        check(total <= 128U * 1024 * 1024, "Ocean tide file exceeds 128 MiB");
        output.write(buffer.data(), input.gcount());
        check(static_cast<bool>(output), "Cannot stage ocean-tides file");
    }
    check(input.eof() && !input.bad(), "Cannot read ocean-tides source");
    output.close();
    check(static_cast<bool>(output), "Cannot close staged ocean-tides file");
    const auto model = runtime::load_ocean_tides(pending, url, checkpoint);
    const auto &info = model->info();
    if (selection == "fes2014bv1")
        check(info.sha256 == fes2014bv1_sha256,
              "Official FES2014bv1 checksum mismatch; import modified models as custom data");
    const auto target = impl_->root / "ocean-tides" / (info.sha256 + ".dat");
    std::filesystem::create_directories(target.parent_path());
    const auto acquired = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    const Json record{{"schema", 1},
                      {"sha256", info.sha256},
                      {"source_url", url},
                      {"model", info.model},
                      {"degree", info.degree},
                      {"order", info.order},
                      {"constituents", info.constituents},
                      {"mu", info.mu},
                      {"radius_m", info.radius_m},
                      {"acquisition", static_cast<int>(acquisition)},
                      {"acquired_at", acquired}};
    std::lock_guard guard(impl_->mutex);
    Transaction transaction(impl_->db);
    if (checkpoint)
        checkpoint();
    if (std::filesystem::exists(target))
        check(runtime::load_ocean_tides(target, url, checkpoint)->info().sha256 == info.sha256,
              "Existing ocean-tides resource is damaged");
    else
        publish(pending, target);
    set_setting("ocean-tides/" + selection, record.dump());
    transaction.commit();
    return {selection, info, target, acquisition, acquired};
}
std::vector<CatalogOceanTides> DataCatalog::ocean_tide_models() const {
    std::lock_guard guard(impl_->mutex);
    Statement rows(impl_->db,
                   "SELECT key,value FROM settings WHERE key LIKE 'ocean-tides/%' ORDER BY key");
    std::vector<CatalogOceanTides> result;
    while (rows.next()) {
        const auto key = reinterpret_cast<const char *>(sqlite3_column_text(rows.value, 0));
        const auto r =
            Json::parse(reinterpret_cast<const char *>(sqlite3_column_text(rows.value, 1)));
        check(r.at("schema") == 1 && hash_valid(r.at("sha256")), "Invalid ocean-tides receipt");
        core::OceanTideInfo info{r.at("model"),  r.at("sha256"),  r.at("source_url"),
                                 r.at("degree"), r.at("order"),   r.at("constituents"),
                                 r.at("mu"),     r.at("radius_m")};
        core::OceanTideOptions options;
        options.model = std::string(key).substr(12);
        options.degree = info.degree;
        options.order = info.order;
        core::validate_ocean_tide_options(options);
        check(!info.model.empty() && info.model.size() <= 512 && info.degree >= 2 &&
                  info.constituents >= 1 && info.constituents <= 4096 && std::isfinite(info.mu) &&
                  info.mu > 0 && std::isfinite(info.radius_m) && info.radius_m > 0 &&
                  r.at("acquisition") >= 0 && r.at("acquisition") <= 3,
              "Invalid ocean tide receipt metadata");
        check(options.model != "fes2014bv1" || info.sha256 == fes2014bv1_sha256,
              "Official FES2014bv1 receipt checksum mismatch");
        result.push_back({std::string(key).substr(12), info,
                          impl_->root / "ocean-tides" / (info.sha256 + ".dat"),
                          static_cast<DataAcquisition>(r.at("acquisition").get<int>()),
                          r.at("acquired_at")});
    }
    return result;
}
std::shared_ptr<const core::OceanTideModel>
DataCatalog::load_ocean_tides(const std::string &selection) const {
    const auto models = ocean_tide_models();
    const auto it = std::find_if(models.begin(), models.end(),
                                 [&](const auto &v) { return v.selection == selection; });
    check(it != models.end(),
          "Ocean tide model is not installed; download or import it in Data Center");
    const auto model = runtime::load_ocean_tides(it->file, it->info.source_url);
    check(model->info().sha256 == it->info.sha256, "Ocean tide content SHA256 mismatch");
    const auto &actual = model->info();
    check(actual.model == it->info.model && actual.degree == it->info.degree &&
              actual.order == it->info.order && actual.constituents == it->info.constituents &&
              actual.mu == it->info.mu && actual.radius_m == it->info.radius_m,
          "Ocean tide receipt metadata differs from its file; reimport the model");
    return model;
}
void DataCatalog::activate_eop(const std::string &key) {
    std::lock_guard guard(impl_->mutex);
    Transaction transaction(impl_->db);
    const auto provider = load_eop(key);
    const auto utc = setting("current-utc");
    const auto time =
        utc.empty() ? make_builtin_time_provider() : load_time_snapshot(time_file(utc));
    check(application::time_data_lock(provider->data_info().time_data) ==
              application::time_data_lock(time->data_info()),
          "Cannot activate EOP with a different UTC dependency; install it with the current UTC "
          "file");
    const auto product = provider->data_info().interpolation.starts_with("finals2000a")
                             ? EopProduct::finals2000a
                             : EopProduct::c04;
    set_setting(std::string("current-") + product_name(product), key);
    transaction.commit();
}
DataSnapshot DataCatalog::snapshot(core::Epoch first, core::Epoch last,
                                   bool earth_orientation) const {
    if (first.tai_nanoseconds() > last.tai_nanoseconds())
        throw std::invalid_argument("Data selection requires first <= last");
    std::lock_guard guard(impl_->mutex);
    std::map<std::string, std::string> settings;
    std::vector<CatalogSnapshot> entries;
    {
        // One SQLite read snapshot prevents mixing pointers from different UTC/EOP updates.
        // Release it before parsing immutable files so other processes can publish updates.
        Transaction transaction(impl_->db, false);
        Statement rows(impl_->db, "SELECT key,value FROM settings");
        while (rows.next())
            settings.emplace(rows.text(0), rows.text(1));
        if (earth_orientation)
            entries = snapshots();
        transaction.commit();
    }
    const auto utc = settings["current-utc"];
    std::shared_ptr<const core::ITimeProvider> time =
        utc.empty() ? make_builtin_time_provider() : load_time_snapshot(time_file(utc));
    check(utc.empty() || time->data_info().sha256 == utc, "Current UTC content identity mismatch");
    (void)time->to_utc(first);
    (void)time->to_utc(last);
    if (!earth_orientation)
        return DataSnapshot{std::move(time)};
    std::vector<std::string> notes;
    std::map<EopProduct, std::shared_ptr<const core::IEarthOrientationProvider>> current;
    const auto load = [&](const std::string &key) {
        std::shared_ptr<const core::IEarthOrientationProvider> provider = load_eop(key);
        check(application::time_data_lock(provider->data_info().time_data) ==
                  application::time_data_lock(time->data_info()),
              "Current EOP uses a different UTC dependency");
        return provider;
    };
    const auto product_of = [](const core::IEarthOrientationProvider &provider) {
        return provider.data_info().interpolation.starts_with("finals2000a")
                   ? EopProduct::finals2000a
                   : EopProduct::c04;
    };
    for (const auto product : {EopProduct::c04, EopProduct::finals2000a}) {
        const auto key = settings[std::string("current-") + product_name(product)];
        if (key.empty())
            continue;
        try {
            auto provider = load(key);
            check(product_of(*provider) == product,
                  "Current EOP pointer refers to a different product");
            current[product] = std::move(provider);
        } catch (const std::exception &error) {
            notes.push_back(std::string(product_name(product)) + ": " + error.what());
        }
    }
    // Legacy catalogs without current pointers use the latest valid receipt/insertion per product.
    // A broken explicit pointer never causes an implicit rollback to that product's older file.
    if (settings["current-c04"].empty() || settings["current-finals2000a"].empty())
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            try {
                auto provider = load(it->lock_sha256);
                const auto product = product_of(*provider);
                if (settings[std::string("current-") + product_name(product)].empty() &&
                    !current.contains(product))
                    current[product] = std::move(provider);
            } catch (const std::exception &error) {
                notes.push_back(it->snapshot_id + ": " + error.what());
            }
        }
    std::vector<EopCandidate> candidates;
    const auto preferred = settings["eop-preferred-product"] == "finals2000a"
                               ? EopProduct::finals2000a
                               : EopProduct::c04;
    for (const auto product :
         {preferred, preferred == EopProduct::c04 ? EopProduct::finals2000a : EopProduct::c04}) {
        if (const auto found = current.find(product); found != current.end())
            candidates.push_back(
                {found->second, parse_extrapolation(settings[extrapolation_key(product)])});
        else
            notes.push_back(std::string(product_name(product)) + ": no usable current data");
    }
    try {
        auto eop = select_eop_for_interval(candidates, first, last, time);
        DataSnapshot result{std::move(time), make_frame_provider(), std::move(eop)};
        result.selection_notes_ = std::move(notes);
        return result;
    } catch (const core::EopCoverageError &error) {
        std::string message = error.what();
        for (const auto &note : notes)
            message += "\n" + note;
        throw core::EopCoverageError(message);
    }
}
CatalogSnapshot DataCatalog::install_eop(const std::filesystem::path &manifest_path,
                                         const std::filesystem::path &time_file) {
    std::lock_guard guard(impl_->mutex);
    const auto clock =
        time_file.empty() ? make_builtin_time_provider() : load_time_snapshot(time_file);
    // Validate copied bytes before publication, even if the source changes after the first load.
    (void)load_eop_snapshot_with_time(manifest_path, *clock);
    const auto manifest_bytes = read(manifest_path, 65536);
    const auto manifest = Json::parse(manifest_bytes);
    const auto filename = std::filesystem::path(manifest.at("data_file").get<std::string>());
    check(!filename.empty() && !filename.has_parent_path() && filename != "manifest.json" &&
              filename != "record.json" && filename != "blob.pending" &&
              filename != "leap-seconds.dat",
          "Invalid snapshot data filename");
    const auto source = std::filesystem::canonical(manifest_path).parent_path();
    const auto data = std::filesystem::canonical(source / filename);
    check(data.parent_path() == source, "EOP data resolves outside its source directory");
    const auto data_bytes = read(data);
    Stage stage(impl_->root / "staging");
    write(stage.path / "manifest.json", manifest_bytes);
    write(stage.path / filename, data_bytes);
    if (!time_file.empty()) {
        write(stage.path / "leap-seconds.dat", read(time_file, 65536));
        check(load_time_snapshot(stage.path / "leap-seconds.dat")->data_info().sha256 ==
                  clock->data_info().sha256,
              "Time data changed during installation");
    }
    const auto provider = load_eop_snapshot_with_time(stage.path / "manifest.json", *clock);
    const auto &info = provider->data_info();
    const auto key = application::eop_data_lock(info).sha256();
    Transaction transaction(impl_->db);
    const auto destination = impl_->directory(key);
    if (std::filesystem::exists(destination)) {
        const auto existing = impl_->verify(key);
        impl_->index(key, existing->data_info());
        transaction.commit();
        return impl_->describe(key, existing->data_info());
    }
    impl_->blob(data_bytes, info.sha256, stage.path);
    std::filesystem::remove(stage.path / filename);
    std::error_code link_error;
    std::filesystem::create_hard_link(impl_->root / "blobs" / "sha256" / info.sha256,
                                      stage.path / filename, link_error);
    if (link_error)
        write(stage.path / filename, data_bytes);
    write(stage.path / "record.json",
          Json{{"schema", time_file.empty() ? 1 : 2},
               {"kind", "earth-orientation"},
               {"lock_sha256", key},
               {"manifest_sha256", picosha2::hash256_hex_string(manifest_bytes)},
               {"time_sha256", clock->data_info().sha256}}
              .dump(2));
    publish(stage.path, destination);
    stage.path.clear();
    impl_->index(key, info);
    transaction.commit();
    return impl_->describe(key, info);
}
std::vector<CatalogSnapshot> DataCatalog::snapshots() const {
    std::lock_guard guard(impl_->mutex);
    Statement rows(impl_->db,
                   "SELECT s.lock_hash,id,source_url,first_ns,last_ns,a.kind,a.acquired_at "
                   "FROM snapshots s LEFT JOIN acquisitions a ON s.lock_hash=a.lock_hash "
                   "ORDER BY COALESCE(a.acquired_at,0),s.rowid");
    std::vector<CatalogSnapshot> result;
    while (rows.next()) {
        const auto key = rows.text(0);
        result.push_back({key, rows.text(1), rows.text(2),
                          core::Epoch(sqlite3_column_int64(rows.value, 3)),
                          core::Epoch(sqlite3_column_int64(rows.value, 4)),
                          impl_->directory(key) / "manifest.json",
                          static_cast<DataAcquisition>(sqlite3_column_int(rows.value, 5)),
                          sqlite3_column_type(rows.value, 6) == SQLITE_NULL
                              ? std::nullopt
                              : std::optional<std::int64_t>{sqlite3_column_int64(rows.value, 6)}});
    }
    return result;
}
void DataCatalog::record_acquisition(const std::string &key, DataAcquisition kind,
                                     std::optional<std::int64_t> acquired_at) {
    std::lock_guard guard(impl_->mutex);
    check(hash_valid(key) && kind >= DataAcquisition::bundled &&
              kind <= DataAcquisition::downloaded,
          "Invalid data acquisition receipt");
    // Re-registering bundled resources on launch must never rewrite a real receipt.
    Statement row(impl_->db,
                  "INSERT INTO acquisitions VALUES(?,?,?) ON CONFLICT(lock_hash) DO UPDATE SET "
                  "kind=excluded.kind,acquired_at=excluded.acquired_at WHERE excluded.kind<>1");
    row.text(1, key);
    row.integer(2, static_cast<int>(kind));
    if (kind != DataAcquisition::bundled)
        row.integer(3, acquired_at.value_or(std::chrono::duration_cast<std::chrono::seconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count()));
    row.next();
}
std::unique_ptr<core::IEarthOrientationProvider>
DataCatalog::load_eop(const std::string &key) const {
    std::lock_guard guard(impl_->mutex);
    Statement exists(impl_->db, "SELECT 1 FROM snapshots WHERE lock_hash=?");
    exists.text(1, key);
    check(exists.next(), "Pinned EOP data is not installed: " + key);
    return impl_->verify(key);
}
std::unique_ptr<core::IEarthOrientationProvider>
DataCatalog::resolve_eop(const application::DataLock &lock) const {
    auto provider = load_eop(lock.sha256());
    check(application::eop_data_lock(provider->data_info()) == lock,
          "Data dependency lock mismatch");
    return provider;
}
std::string DataCatalog::setting(const std::string &key) const {
    std::lock_guard guard(impl_->mutex);
    Statement row(impl_->db, "SELECT value FROM settings WHERE key=?");
    row.text(1, key);
    return row.next() ? row.text(0) : "";
}
void DataCatalog::set_setting(const std::string &key, const std::string &value) {
    std::lock_guard guard(impl_->mutex);
    Statement row(
        impl_->db,
        "INSERT INTO settings VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    row.text(1, key);
    row.text(2, value);
    row.next();
}
void DataCatalog::set_settings(const std::map<std::string, std::string> &values) {
    std::lock_guard guard(impl_->mutex);
    Transaction transaction(impl_->db);
    for (const auto &[key, value] : values)
        set_setting(key, value);
    transaction.commit();
}
std::filesystem::path DataCatalog::time_file(const std::string &hash) const {
    std::lock_guard guard(impl_->mutex);
    check(hash_valid(hash), "Invalid UTC content hash");
    const auto file = impl_->root / "time" / (hash + ".dat");
    check(std::filesystem::canonical(file).parent_path() == impl_->root / "time",
          "UTC data resolves outside its catalog directory");
    check(load_time_snapshot(file)->data_info().sha256 == hash, "UTC content SHA256 mismatch");
    return file;
}
std::filesystem::path DataCatalog::install_time(const std::filesystem::path &source) {
    std::lock_guard guard(impl_->mutex);
    const auto expected = load_time_snapshot(source)->data_info().sha256;
    Stage stage(impl_->root / "staging");
    write(stage.path / "time.dat", read(source, 65536));
    const auto info = load_time_snapshot(stage.path / "time.dat")->data_info();
    check(info.sha256 == expected, "UTC file changed during installation");
    const auto target = impl_->root / "time" / (info.sha256 + ".dat");
    if (std::filesystem::exists(target))
        return time_file(info.sha256);
    publish(stage.path / "time.dat", target);
    return target;
}
CatalogSnapshot DataCatalog::import_c04(const std::filesystem::path &source, const std::string &url,
                                        const std::string &retrieved,
                                        const std::filesystem::path &time_file) {
    std::lock_guard guard(impl_->mutex);
    check(!url.empty() && !retrieved.empty(), "Source URL and retrieval date are required");
    const auto bytes = read(source);
    std::istringstream lines(bytes);
    std::string line;
    int first = -1, last = -1;
    while (std::getline(lines, line)) {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos || line[start] == '#')
            continue;
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        int year = 0, month = 0, day = 0, hour = 0;
        double mjd = 0;
        check(static_cast<bool>(row >> year >> month >> day >> hour >> mjd) && std::isfinite(mjd) &&
                  mjd >= 37665 && mjd <= 100000 && mjd == std::floor(mjd),
              "Invalid C04 date fields");
        if (first < 0)
            first = static_cast<int>(mjd);
        last = static_cast<int>(mjd);
    }
    const auto clock =
        time_file.empty() ? make_builtin_time_provider() : load_time_snapshot(time_file);
    // Earlier records are retained in the full original file, but do not supply
    // the measured polar rates required by our C04 state interpolation.
    first = std::max({first, 45700, clock->data_info().utc_valid_from_mjd});
    last = std::min(last, clock->data_info().utc_valid_until_mjd - 1);
    check(last > first, "C04 lacks two complete daily records within supported coverage");
    const auto hash = picosha2::hash256_hex_string(bytes);
    const Json manifest{{"schema_version", 1},
                        {"snapshot_id", "iers-c04-all-" + hash.substr(0, 16)},
                        {"format", "iers-20-c04-0h-dxdy"},
                        {"data_file", "eop.c04"},
                        {"sha256", hash},
                        {"source_url", url},
                        {"source_sha256", hash},
                        {"retrieved_on", retrieved},
                        {"first_mjd", first},
                        {"last_mjd", last},
                        {"reference_frame", "ITRF2020"},
                        {"interpolation", "c04-hermite-tai-v1"}};
    Stage stage(impl_->root / "staging");
    write(stage.path / "manifest.json", manifest.dump(2));
    write(stage.path / "eop.c04", bytes);
    return install_eop(stage.path / "manifest.json", time_file);
}
CatalogSnapshot DataCatalog::import_finals(const std::filesystem::path &source,
                                           const std::string &url, const std::string &retrieved,
                                           const std::filesystem::path &time_file) {
    std::lock_guard guard(impl_->mutex);
    check(!url.empty() && !retrieved.empty(), "Source URL and retrieval date are required");
    const auto bytes = read(source);
    std::istringstream lines(bytes);
    std::string line;
    int first = -1, last = -1, previous = -1;
    bool incomplete_tail = false;
    const auto mjd_zero = std::chrono::sys_days(std::chrono::year(1858) / 11 / 17);
    while (std::getline(lines, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        check(line.size() >= 134, "Truncated finals2000A source row");
        const auto number = [&](std::size_t offset, std::size_t count) {
            std::istringstream field(line.substr(offset, count));
            field.imbue(std::locale::classic());
            double value = 0;
            check(static_cast<bool>(field >> value) && std::isfinite(value),
                  "Invalid finals2000A numeric field");
            field >> std::ws;
            check(field.eof(), "Invalid finals2000A numeric field");
            return value;
        };
        const auto day_number = number(7, 8);
        check(day_number >= 41317 && day_number <= 100000 && day_number == std::floor(day_number),
              "Invalid finals2000A MJD");
        const auto mjd = static_cast<int>(day_number);
        check(previous < 0 || mjd == previous + 1,
              "Missing, duplicate or unordered finals2000A row");
        previous = mjd;
        const auto calendar_field = [&](std::size_t offset) {
            const double value = number(offset, 2);
            check(value >= 0 && value <= 99 && value == std::floor(value),
                  "Invalid finals2000A date");
            return static_cast<int>(value);
        };
        const auto date = std::chrono::year(calendar_field(0) + (mjd <= 51543 ? 1900 : 2000)) /
                          static_cast<unsigned>(calendar_field(2)) /
                          static_cast<unsigned>(calendar_field(4));
        check(date.ok() && (std::chrono::sys_days(date) - mjd_zero).count() == mjd,
              "finals2000A date/MJD mismatch");
        // A component is either entirely absent in the prediction tail or fully
        // specified. A damaged field inside an otherwise present group is an error.
        const auto group = [&](std::size_t flag, std::size_t end,
                               std::initializer_list<std::pair<std::size_t, std::size_t>> fields) {
            if (line.substr(flag, end - flag).find_first_not_of(" \t\r") == std::string::npos)
                return false;
            check(line[flag] == 'I' || line[flag] == 'P', "Invalid finals2000A source flag");
            std::size_t index = 0;
            for (const auto &[offset, count] : fields) {
                const auto value = number(offset, count);
                check(index++ % 2 == 0 || value >= 0, "Negative finals2000A uncertainty");
            }
            return true;
        };
        const bool polar = group(16, 55, {{18, 9}, {27, 9}, {37, 9}, {46, 9}});
        const bool ut1 = group(57, 78, {{58, 10}, {68, 10}});
        const bool pole = group(95, 134, {{97, 9}, {106, 9}, {116, 9}, {125, 9}});
        if (!(polar && ut1 && pole)) {
            incomplete_tail = true;
            continue;
        }
        check(!incomplete_tail, "Incomplete finals2000A record before complete daily records");
        if (first < 0)
            first = mjd;
        last = mjd;
    }
    const auto clock =
        time_file.empty() ? make_builtin_time_provider() : load_time_snapshot(time_file);
    first = std::max(first, clock->data_info().utc_valid_from_mjd);
    last = std::min(last, clock->data_info().utc_valid_until_mjd - 1);
    check(last > first, "finals2000A lacks two complete daily records within time-data coverage");
    const auto hash = picosha2::hash256_hex_string(bytes);
    const Json manifest{{"schema_version", 1},
                        {"snapshot_id", "iers-finals2000a-all-" + hash.substr(0, 16)},
                        {"format", "iers-finals2000a"},
                        {"data_file", "finals2000A.all"},
                        {"sha256", hash},
                        {"source_url", url},
                        {"source_sha256", hash},
                        {"retrieved_on", retrieved},
                        {"first_mjd", first},
                        {"last_mjd", last},
                        {"reference_frame", "ITRS"},
                        {"interpolation", "finals2000a-hermite-tai-v1"}};
    Stage stage(impl_->root / "staging");
    write(stage.path / "manifest.json", manifest.dump(2));
    write(stage.path / "finals2000A.all", bytes);
    return install_eop(stage.path / "manifest.json", time_file);
}
} // namespace orbitforge::runtime
#else
namespace orbitforge::runtime {
struct DataCatalog::Impl {
    std::filesystem::path root;
};
namespace {
[[noreturn]] void unavailable() { throw std::runtime_error("Data catalog support is not enabled"); }
} // namespace
bool data_catalog_available() noexcept { return false; }
CatalogEphemeris DataCatalog::install_de440s(const std::filesystem::path &, DataAcquisition,
                                             const std::function<void()> &) {
    unavailable();
}
CatalogEphemeris DataCatalog::install_ephemeris_snapshot(const std::filesystem::path &,
                                                         DataAcquisition,
                                                         const std::function<void()> &) {
    unavailable();
}
CatalogEphemeris DataCatalog::install_planetary_centres(const std::filesystem::path &,
                                                        const std::string &, DataAcquisition,
                                                        const std::function<void()> &) {
    unavailable();
}
std::vector<CatalogEphemeris> DataCatalog::ephemerides() const { unavailable(); }
std::shared_ptr<const core::IThirdBodyEphemeris>
DataCatalog::load_ephemeris(const std::string &) const {
    unavailable();
}
CatalogSpaceWeather DataCatalog::install_space_weather(const std::filesystem::path &,
                                                       const std::string &, const std::string &,
                                                       DataAcquisition,
                                                       const std::function<void()> &) {
    unavailable();
}
std::vector<CatalogSpaceWeather> DataCatalog::weather_sources() const { unavailable(); }
std::shared_ptr<const core::SpaceWeather>
DataCatalog::load_space_weather(const std::string &) const {
    unavailable();
}
CatalogOceanTides DataCatalog::install_ocean_tides(const std::filesystem::path &,
                                                   const std::string &, const std::string &,
                                                   DataAcquisition, const std::function<void()> &) {
    unavailable();
}
std::vector<CatalogOceanTides> DataCatalog::ocean_tide_models() const { unavailable(); }
std::shared_ptr<const core::OceanTideModel>
DataCatalog::load_ocean_tides(const std::string &) const {
    unavailable();
}
CatalogGravity DataCatalog::install_gravity(const std::filesystem::path &, const std::string &,
                                            const std::string &, DataAcquisition,
                                            const std::function<void()> &) {
    unavailable();
}
std::vector<CatalogGravity> DataCatalog::gravity_models() const { unavailable(); }
std::shared_ptr<const core::GravityModel> DataCatalog::load_gravity(const std::string &) const {
    unavailable();
}
DataCatalog::DataCatalog(std::filesystem::path) { unavailable(); }
DataCatalog::~DataCatalog() = default;
DataSnapshot DataCatalog::snapshot(core::Epoch, core::Epoch, bool) const { unavailable(); }
void DataCatalog::activate_eop(const std::string &) { unavailable(); }
const std::filesystem::path &DataCatalog::root() const noexcept { return impl_->root; }
CatalogSnapshot DataCatalog::install_eop(const std::filesystem::path &,
                                         const std::filesystem::path &) {
    unavailable();
}
CatalogSnapshot DataCatalog::import_finals(const std::filesystem::path &, const std::string &,
                                           const std::string &, const std::filesystem::path &) {
    unavailable();
}
CatalogSnapshot DataCatalog::import_c04(const std::filesystem::path &, const std::string &,
                                        const std::string &, const std::filesystem::path &) {
    unavailable();
}
std::filesystem::path DataCatalog::install_time(const std::filesystem::path &) { unavailable(); }
std::filesystem::path DataCatalog::time_file(const std::string &) const { unavailable(); }
std::vector<CatalogSnapshot> DataCatalog::snapshots() const { unavailable(); }
void DataCatalog::record_acquisition(const std::string &, DataAcquisition,
                                     std::optional<std::int64_t>) {
    unavailable();
}
std::unique_ptr<core::IEarthOrientationProvider> DataCatalog::load_eop(const std::string &) const {
    unavailable();
}
std::unique_ptr<core::IEarthOrientationProvider>
DataCatalog::resolve_eop(const application::DataLock &) const {
    unavailable();
}
std::string DataCatalog::setting(const std::string &) const { unavailable(); }
void DataCatalog::set_setting(const std::string &, const std::string &) { unavailable(); }
void DataCatalog::set_settings(const std::map<std::string, std::string> &) { unavailable(); }
} // namespace orbitforge::runtime
#endif
