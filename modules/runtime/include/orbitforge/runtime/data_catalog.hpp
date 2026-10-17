#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <orbitforge/application/data_lock.hpp>
#include <orbitforge/runtime/analysis.hpp>
#include <string>
#include <vector>

namespace orbitforge::runtime {
enum class DataAcquisition { unknown, bundled, imported, downloaded };
enum class EopProduct { c04, finals2000a };
struct CatalogGravity {
    std::string selection;
    core::GravityInfo info;
    std::filesystem::path file;
    DataAcquisition acquisition = DataAcquisition::unknown;
    std::int64_t acquired_at_unix_seconds = 0;
};
struct CatalogEphemeris {
    std::string selection;
    core::ThirdBodyDataInfo info;
    std::filesystem::path manifest;
    DataAcquisition acquisition = DataAcquisition::unknown;
    std::int64_t acquired_at_unix_seconds = 0;
};
struct CatalogSpaceWeather {
    std::string selection;
    core::SpaceWeatherInfo info;
    std::filesystem::path file;
    DataAcquisition acquisition = DataAcquisition::unknown;
    std::int64_t acquired_at_unix_seconds = 0;
};
struct CatalogOceanTides {
    std::string selection;
    core::OceanTideInfo info;
    std::filesystem::path file;
    DataAcquisition acquisition = DataAcquisition::unknown;
    std::int64_t acquired_at_unix_seconds = 0;
};
struct CatalogSnapshot {
    std::string lock_sha256;
    std::string snapshot_id;
    std::string source_url;
    core::Epoch first{0}, last{0};
    std::filesystem::path manifest;
    DataAcquisition acquisition = DataAcquisition::unknown;
    std::optional<std::int64_t> acquired_at_unix_seconds = std::nullopt;
};
[[nodiscard]] bool data_catalog_available() noexcept;
// Shared on-disk catalog. Methods are serialized per instance and SQLite serializes
// publication across processes. No method changes a running Scenario or downloads.
class DataCatalog {
  public:
    explicit DataCatalog(std::filesystem::path root);
    ~DataCatalog();
    DataCatalog(const DataCatalog &) = delete;
    DataCatalog &operator=(const DataCatalog &) = delete;
    [[nodiscard]] const std::filesystem::path &root() const noexcept;
    // Validate the complete file before atomically replacing this model's current receipt.
    // Static fields have no expiry. Failed/cancelled installs retain the previous selection.
    CatalogGravity install_gravity(const std::filesystem::path &source,
                                   const std::string &selection, const std::string &source_url = {},
                                   DataAcquisition acquisition = DataAcquisition::imported,
                                   const std::function<void()> &checkpoint = {});
    [[nodiscard]] std::vector<CatalogGravity> gravity_models() const;
    [[nodiscard]] std::shared_ptr<const core::GravityModel>
    load_gravity(const std::string &selection) const;
    // Install the complete pinned DE440s SPK with owned uniform-time constants.
    CatalogEphemeris install_de440s(const std::filesystem::path &spk,
                                    DataAcquisition acquisition = DataAcquisition::imported,
                                    const std::function<void()> &checkpoint = {});
    // Import an explicit DE440s-based snapshot (including validated per-planet
    // supplements). Owned copies and atomic publication; no implicit download.
    CatalogEphemeris
    install_ephemeris_snapshot(const std::filesystem::path &manifest,
                               DataAcquisition acquisition = DataAcquisition::imported,
                               const std::function<void()> &checkpoint = {});
    // Augment the installed DE440s snapshot from a binary SPK (up to 2 GiB).
    // Retain only 499..999 physical-centre segments in J2000 relative to each
    // planet's own barycentre; replace only the imported planets. Other segments
    // cannot override Sun/Earth/Moon or planetary-system gravity. Body coverage
    // is checked for the requested propagation interval, independently of base DE.
    CatalogEphemeris
    install_planetary_centres(const std::filesystem::path &source,
                              const std::string &source_url = {},
                              DataAcquisition acquisition = DataAcquisition::imported,
                              const std::function<void()> &checkpoint = {});
    [[nodiscard]] std::vector<CatalogEphemeris> ephemerides() const;
    [[nodiscard]] std::shared_ptr<const core::IThirdBodyEphemeris>
    load_ephemeris(const std::string &selection) const;
    CatalogSpaceWeather install_space_weather(
        const std::filesystem::path &source, const std::string &selection = "celestrak",
        const std::string &source_url = {}, DataAcquisition acquisition = DataAcquisition::imported,
        const std::function<void()> &checkpoint = {});
    [[nodiscard]] std::vector<CatalogSpaceWeather> weather_sources() const;
    [[nodiscard]] std::shared_ptr<const core::SpaceWeather>
    load_space_weather(const std::string &selection) const;
    // FES normalized potential file. The official fes2014bv1 ID requires the pinned
    // original bytes; custom models use another ID. Atomic, cancellable publication.
    CatalogOceanTides install_ocean_tides(const std::filesystem::path &source,
                                          const std::string &selection = "fes2014bv1",
                                          const std::string &source_url = {},
                                          DataAcquisition acquisition = DataAcquisition::imported,
                                          const std::function<void()> &checkpoint = {});
    [[nodiscard]] std::vector<CatalogOceanTides> ocean_tide_models() const;
    [[nodiscard]] std::shared_ptr<const core::OceanTideModel>
    load_ocean_tides(const std::string &selection) const;
    // Reads a coherent local catalog view; never downloads or changes current pointers.
    // Full coverage wins over extrapolation, then preferred_eop() orders the sources.
    [[nodiscard]] DataSnapshot snapshot(core::Epoch first, core::Epoch last,
                                        bool earth_orientation = true) const;
    [[nodiscard]] EopProduct preferred_eop() const;
    void set_preferred_eop(EopProduct product);
    [[nodiscard]] int extrapolation_days(EopProduct product) const;
    void set_extrapolation_days(EopProduct product, int days);
    // Explicit activation is independent of installation and preserves receipt metadata.
    // Rejects an EOP resource whose UTC dependency differs from the catalog's current UTC.
    void activate_eop(const std::string &lock_sha256);
    CatalogSnapshot install_eop(const std::filesystem::path &manifest,
                                const std::filesystem::path &time_file = {});
    // Installs the entire original file. Coverage is the complete daily prefix
    // intersected with the time provider validity; incomplete predictions stay in the blob.
    CatalogSnapshot import_finals(const std::filesystem::path &source,
                                  const std::string &source_url, const std::string &retrieved_on,
                                  const std::filesystem::path &time_file = {});
    CatalogSnapshot import_c04(const std::filesystem::path &source, const std::string &source_url,
                               const std::string &retrieved_on,
                               const std::filesystem::path &time_file = {});
    // Publish a validated immutable UTC file; activation is a separate settings transaction.
    [[nodiscard]] std::filesystem::path install_time(const std::filesystem::path &source);
    [[nodiscard]] std::filesystem::path time_file(const std::string &sha256) const;
    [[nodiscard]] std::vector<CatalogSnapshot> snapshots() const;
    // Local receipt metadata is separate from immutable content and Scenario locks.
    // Call only after successful acquisition; a 304 response is not a download.
    void record_acquisition(const std::string &lock_sha256, DataAcquisition acquisition,
                            std::optional<std::int64_t> acquired_at = std::nullopt);
    [[nodiscard]] std::unique_ptr<core::IEarthOrientationProvider>
    resolve_eop(const application::DataLock &lock) const;
    [[nodiscard]] std::unique_ptr<core::IEarthOrientationProvider>
    load_eop(const std::string &lock_sha256) const;
    [[nodiscard]] std::string setting(const std::string &key) const;
    void set_setting(const std::string &key, const std::string &value);
    void set_settings(const std::map<std::string, std::string> &values);

  private:
    CatalogEphemeris install_ephemeris_snapshot_checked(const std::filesystem::path &manifest,
                                                        DataAcquisition acquisition,
                                                        const std::function<void()> &checkpoint,
                                                        const std::string &expected_receipt);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbitforge::runtime
