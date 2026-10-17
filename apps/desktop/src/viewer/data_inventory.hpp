#pragma once
#include <QDateTime>
#include <QString>
#include <filesystem>
#include <optional>
#include <orbitforge/runtime/data_catalog.hpp>

namespace orbitforge::desktop {
enum class DataProduct {
    finals2000a,
    c04,
    utc,
    egm2008,
    egm96,
    custom_gravity,
    de440s,
    space_weather,
    fes2014bv1,
    custom_ocean_tides
};
inline constexpr std::array all_data_products{
    DataProduct::finals2000a, DataProduct::c04,
    DataProduct::utc,         DataProduct::egm2008,
    DataProduct::egm96,       DataProduct::custom_gravity,
    DataProduct::de440s,      DataProduct::space_weather,
    DataProduct::fes2014bv1,  DataProduct::custom_ocean_tides};
inline bool gravity_product(DataProduct p) {
    return p >= DataProduct::egm2008 && p <= DataProduct::custom_gravity;
}
inline bool ocean_product(DataProduct p) {
    return p == DataProduct::fes2014bv1 || p == DataProduct::custom_ocean_tides;
}
inline std::string ocean_selection(DataProduct p) {
    return p == DataProduct::fes2014bv1 ? "fes2014bv1" : "custom-ocean";
}
inline bool force_product(DataProduct p) {
    return gravity_product(p) || ocean_product(p) || p == DataProduct::de440s ||
           p == DataProduct::space_weather;
}
inline std::string gravity_selection(DataProduct p) {
    return p == DataProduct::egm2008 ? "egm2008" : p == DataProduct::egm96 ? "egm96" : "custom";
}
enum class DataInstallation { missing, bundled_sample, local_file, downloaded, builtin };
// Values are persisted by finals-update-record schema 1; append new states.
enum class DataUpdateState {
    never_checked,
    checking,
    downloading,
    updated,
    unchanged,
    failed,
    cancelled,
    interrupted,
    local_changed
};
struct DataUpdateStatus {
    DataUpdateState state = DataUpdateState::never_checked;
    std::optional<qint64> attempted_at;
    std::optional<qint64> checked_at;
    QString checked_sha256;
    QString message;
};
// One current item per product. Installation, coverage and online checks are independent.
struct DataInventoryItem {
    DataProduct product = DataProduct::finals2000a;
    DataInstallation installation = DataInstallation::missing;
    runtime::DataAcquisition acquisition = runtime::DataAcquisition::unknown;
    std::optional<std::size_t> provider_index;
    std::optional<qint64> acquired_at;
    QDateTime first, last;
    bool exclusive_end = false;
    bool online_updates = false;
    bool raw_import = false;
    bool manifest_import = false;
    bool extrapolation = false;
    QString source_url, sha256, identity_sha256;
    std::filesystem::path file;
    std::optional<std::uintmax_t> file_size;
    std::optional<core::GravityInfo> gravity;
    std::optional<core::OceanTideInfo> ocean_tides;
    std::optional<core::ThirdBodyDataInfo> ephemeris;
};
} // namespace orbitforge::desktop
