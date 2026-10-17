#include "eop_catalog.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QTimeZone>
#include <algorithm>
#include <orbitforge/application/project.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/gravity.hpp>
#include <orbitforge/runtime/ocean_tides.hpp>
#include <orbitforge/runtime/space_weather.hpp>

namespace orbitforge::desktop {
namespace {
QJsonArray ephemeris_kernels(const runtime::CatalogEphemeris &entry) {
    QFile file(QString::fromStdWString(entry.manifest.wstring()));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
        return {};
    const auto bytes = file.readAll();
    if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString() !=
        entry.info.content_sha256)
        return {};
    const auto kernels = QJsonDocument::fromJson(bytes).object().value("kernels").toArray();
    for (const auto &value : kernels) {
        const std::filesystem::path name(value.toObject().value("file").toString().toStdWString());
        if (name.empty() || name.has_parent_path() || name.has_root_path() || name == "." ||
            name == "..")
            return {};
    }
    return kernels;
}
std::string source_url(DataProduct product) {
    switch (product) {
    case DataProduct::finals2000a:
        return "https://datacenter.iers.org/data/9/finals2000A.all";
    case DataProduct::c04:
        return "https://datacenter.iers.org/products/eop/long-term/c04_operational/eopc04.1962-now";
    case DataProduct::utc:
        return "https://hpiers.obspm.fr/iers/bul/bulc/Leap_Second.dat";
    case DataProduct::egm2008:
    case DataProduct::egm96:
        return runtime::gravity_download_url(gravity_selection(product));
    case DataProduct::fes2014bv1:
        return runtime::ocean_tide_download_url();
    case DataProduct::custom_ocean_tides:
    case DataProduct::custom_gravity:
        return {};
    case DataProduct::space_weather:
        return runtime::space_weather_download_url();
    case DataProduct::de440s:
        return std::string(runtime::de440s_url);
    }
    throw std::invalid_argument("Unknown data product");
}
std::string setting_key(DataProduct product, const char *suffix) {
    if (ocean_product(product))
        return "ocean-tides-" + ocean_selection(product) + "-" + suffix;
    if (gravity_product(product))
        return "gravity-" + gravity_selection(product) + "-" + suffix;
    const auto prefix = product == DataProduct::space_weather ? "space-weather"
                        : product == DataProduct::de440s      ? "de440s"
                        : product == DataProduct::finals2000a ? "finals"
                        : product == DataProduct::c04         ? "c04"
                                                              : "utc";
    return std::string(prefix) + "-" + suffix;
}
qint64 download_limit(DataProduct p) {
    if (ocean_product(p))
        return 128LL * 1024 * 1024;
    if (p == DataProduct::space_weather)
        return 32LL * 1024 * 1024;
    if (p == DataProduct::de440s)
        return 64LL * 1024 * 1024;
    return gravity_product(p) ? 512LL * 1024 * 1024 : 16LL * 1024 * 1024;
}
QDateTime utc_datetime(const core::ITimeProvider &clock, core::Epoch epoch) {
    const auto t = clock.to_utc(epoch);
    return {QDate(t.year, t.month, t.day), QTime(t.hour, t.minute, std::min(t.second, 59)),
            QTimeZone::UTC};
}
std::optional<qint64> timestamp(const QString &text) {
    bool valid = false;
    const auto value = text.toLongLong(&valid);
    return valid && value > 0 ? std::optional<qint64>{value} : std::nullopt;
}
QString download_error(const QNetworkReply &reply, int http) {
    if (http >= 400)
        return QStringLiteral("数据源返回 HTTP %1，暂时无法下载，请稍后重试。").arg(http);
    switch (reply.error()) {
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("无法找到数据源，请检查网络连接后重试。");
    case QNetworkReply::TimeoutError:
        return QStringLiteral("连接数据源超时，请重试。");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("无法与数据源建立安全连接，请检查系统时间和网络设置。");
    case QNetworkReply::NoError:
        return QStringLiteral("数据源返回了无法使用的响应（HTTP %1），请重试。").arg(http);
    default:
        return QStringLiteral("下载连接中断，请检查网络后重试。");
    }
}
runtime::EopProduct eop_product(const core::EopDataInfo &info) {
    return info.interpolation.starts_with("finals2000a") ? runtime::EopProduct::finals2000a
                                                         : runtime::EopProduct::c04;
}
} // namespace
EopCatalog::EopCatalog(std::filesystem::path store, std::vector<std::filesystem::path> bundled,
                       std::filesystem::path legacy, bool automatic)
    : store_(std::move(store)), data_(store_) {
    time_ = runtime::make_builtin_time_provider();
    if (const auto key = data_.setting("current-utc"); !key.empty()) {
        try {
            time_file_ = data_.time_file(key);
            time_ = runtime::load_time_snapshot(time_file_);
        } catch (const std::exception &error) {
            warnings_.push_back(QString::fromUtf8(error.what()));
            time_file_.clear();
        }
    }
    for (const auto &manifest : bundled) {
        std::size_t index;
        try {
            const auto entry = data_.install_eop(manifest, time_file_);
            data_.record_acquisition(entry.lock_sha256, runtime::DataAcquisition::bundled);
            index = append(data_.load_eop(entry.lock_sha256));
        } catch (const std::exception &error) {
            warnings_.push_back(QString::fromUtf8(error.what()));
            index = append(runtime::load_eop_snapshot_with_time(manifest, *time_));
        }
        bundled_.emplace_back(application::eop_data_lock(providers_[index]->data_info()).sha256(),
                              manifest);
    }
    if (!legacy.empty() && std::filesystem::exists(legacy)) {
        for (const auto &entry : std::filesystem::directory_iterator(legacy)) {
            const auto name = entry.path().filename().string();
            if (!entry.is_directory() || name.size() != 64 ||
                !std::all_of(name.begin(), name.end(), [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                }))
                continue;
            try {
                const auto installed =
                    data_.install_eop(entry.path() / "manifest.json", time_file_);
                append(data_.load_eop(installed.lock_sha256));
            } catch (const std::exception &error) {
                warnings_.push_back(QString::fromUtf8(error.what()));
            }
        }
    }
    for (const auto &entry : data_.snapshots()) {
        try {
            append(data_.load_eop(entry.lock_sha256));
        } catch (const std::exception &error) {
            warnings_.push_back(QString::fromStdString(entry.snapshot_id) + ": " +
                                QString::fromUtf8(error.what()));
        }
    }
    refresh_current();
    for (const auto product : all_data_products) {
        auto &update_ = update(product);
        const auto saved =
            QJsonDocument::fromJson(
                QByteArray::fromStdString(data_.setting(setting_key(product, "update-record"))))
                .object();
        const auto saved_state = saved.value("state").toInt(-1);
        if (saved.value("schema").toInt() == 1 && saved_state >= 0 &&
            saved_state <= static_cast<int>(DataUpdateState::local_changed)) {
            update_.state = static_cast<DataUpdateState>(saved_state);
            update_.checked_at = timestamp(saved.value("checked_at").toString());
            update_.attempted_at = timestamp(saved.value("attempted_at").toString());
            update_.checked_sha256 = saved.value("checked_sha256").toString();
            update_.message = saved.value("message").toString();
            if (update_.state == DataUpdateState::checking ||
                update_.state == DataUpdateState::downloading) {
                update_.state = DataUpdateState::interrupted;
                update_.message = QStringLiteral("上次下载未完成，可重新检查并下载。");
            }
        }
        if (!update_.attempted_at)
            update_.attempted_at = timestamp(
                QString::fromStdString(data_.setting(setting_key(product, "last-attempt"))));
    }
    deadline_.setSingleShot(true);
    connect(&resource_poll_, &QTimer::timeout, this, [this] {
        if (!resource_job_.valid() ||
            resource_job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        resource_poll_.stop();
        auto &record = update(active_product_);
        try {
            const auto installed = resource_job_.get();
            record.state = record.checked_sha256 == QString::fromStdString(installed.sha256)
                               ? DataUpdateState::unchanged
                               : DataUpdateState::updated;
            record.checked_sha256 = QString::fromStdString(installed.sha256);
            if (installed.acquisition == runtime::DataAcquisition::downloaded) {
                record.checked_at = QDateTime::currentSecsSinceEpoch();
                data_.set_setting(setting_key(active_product_, "installed-source"),
                                  installed.sha256);
                data_.set_settings(resource_response_);
            }
            record.message = status_ = QStringLiteral("数据已校验并安装，可在 HPOP 中选择。");
            ++revision_;
        } catch (const std::exception &e) {
            record.state = resource_cancel_ && *resource_cancel_ ? DataUpdateState::cancelled
                                                                 : DataUpdateState::failed;
            record.message = status_ = QString::fromUtf8(e.what());
        }
        try {
            save_update_status();
        } catch (const std::exception &) {
        }
        download_.reset();
        resource_cancel_.reset();
        resource_response_.clear();
    });
    connect(&deadline_, &QTimer::timeout, this, [this] {
        transfer_error_ = QStringLiteral("下载超时，请重试。");
        if (reply_)
            reply_->abort();
    });
    connect(&schedule_, &QTimer::timeout, this,
            [this] { check_updates_if_due(QDateTime::currentSecsSinceEpoch()); });
    set_update_schedule_enabled(automatic);
}
void EopCatalog::set_update_schedule_enabled(bool enabled) {
    if (enabled) {
        schedule_.start(60000);
        QTimer::singleShot(1500, this, [this] {
            if (schedule_.isActive())
                check_updates_if_due(QDateTime::currentSecsSinceEpoch());
        });
    } else {
        schedule_.stop();
    }
}
EopCatalog::~EopCatalog() {
    if (resource_cancel_)
        *resource_cancel_ = true;
    if (resource_job_.valid())
        resource_job_.wait();
    if (reply_) {
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
    }
}
bool EopCatalog::has_full_download(DataProduct product) const {
    const auto present = [](const std::filesystem::path &path) {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error) &&
               std::filesystem::file_size(path, error) > 0 && !error;
    };
    if (ocean_product(product)) {
        for (const auto &entry : data_.ocean_tide_models())
            if (entry.selection == ocean_selection(product))
                return present(entry.file);
        return false;
    }
    if (product == DataProduct::space_weather) {
        for (const auto &entry : data_.weather_sources())
            if (entry.selection == "celestrak")
                return present(entry.file);
        return false;
    }
    if (product == DataProduct::de440s) {
        for (const auto &entry : data_.ephemerides()) {
            if (entry.selection != "de440s")
                continue;
            bool full_base = false, time_constants = false;
            for (const auto &value : ephemeris_kernels(entry)) {
                const auto kernel = value.toObject();
                const auto path =
                    entry.manifest.parent_path() /
                    std::filesystem::path(kernel.value("file").toString().toStdWString());
                if (!present(path))
                    return false;
                const auto kind = kernel.value("kind").toString();
                full_base |= kind == "SPK" && kernel.value("sha256").toString() ==
                                                  QString::fromUtf8(runtime::de440s_sha256);
                time_constants |= kind == "LSK";
            }
            // A historical package is usable for its own interval, but cannot
            // satisfy first-launch acquisition of the full DE440s policy window.
            const QDateTime origin(QDate(2000, 1, 1), QTime(12, 0), QTimeZone::UTC);
            const auto first =
                origin.secsTo(QDateTime(QDate(1850, 1, 1), QTime(0, 0), QTimeZone::UTC));
            const auto last =
                origin.secsTo(QDateTime(QDate(2150, 1, 1), QTime(0, 0), QTimeZone::UTC));
            return full_base && time_constants &&
                   entry.info.first.tai_nanoseconds() <= first * 1000000000LL &&
                   entry.info.last.tai_nanoseconds() >= last * 1000000000LL;
        }
        return false;
    }
    if (gravity_product(product)) {
        for (const auto &entry : data_.gravity_models())
            if (entry.selection == gravity_selection(product))
                return present(entry.file);
        return false;
    }
    if (product == DataProduct::utc)
        return !time_file_.empty();
    const auto index = current_index(product);
    return index &&
           providers_[*index]->data_info().id.starts_with(
               product == DataProduct::finals2000a ? "iers-finals2000a-all-" : "iers-c04-all-");
}
std::optional<std::size_t> EopCatalog::current_index(DataProduct product) const {
    for (const auto index : current_)
        if ((providers_[index]->data_info().interpolation.starts_with("finals2000a")
                 ? DataProduct::finals2000a
                 : DataProduct::c04) == product)
            return index;
    return std::nullopt;
}
std::string EopCatalog::current_hash(DataProduct product) const {
    if (ocean_product(product)) {
        for (const auto &entry : data_.ocean_tide_models())
            if (entry.selection == ocean_selection(product))
                return entry.info.sha256;
        return {};
    }
    if (product == DataProduct::space_weather) {
        for (const auto &entry : data_.weather_sources())
            if (entry.selection == "celestrak")
                return entry.info.sha256;
        return {};
    }
    if (product == DataProduct::de440s) {
        const auto entries = data_.ephemerides();
        return entries.empty() ? std::string{} : entries.front().info.content_sha256;
    }
    if (gravity_product(product)) {
        for (const auto &model : data_.gravity_models())
            if (model.selection == gravity_selection(product))
                return model.info.sha256;
        return {};
    }
    if (product == DataProduct::utc)
        return time_->data_info().sha256;
    const auto index = current_index(product);
    return index ? providers_[*index]->data_info().sha256 : std::string{};
}
std::vector<DataInventoryItem> EopCatalog::inventory() const {
    const auto &clock = time_;
    const auto entries = data_.snapshots();
    std::vector<DataInventoryItem> items(all_data_products.size());
    items[0].online_updates = true;
    items[0].raw_import = true;
    items[0].manifest_import = true;
    items[0].source_url = QString::fromStdString(source_url(DataProduct::finals2000a));
    items[1].product = DataProduct::c04;
    items[1].online_updates = true;
    items[1].raw_import = true;
    items[1].source_url = QString::fromStdString(source_url(DataProduct::c04));
    items[1].manifest_import = true;
    for (const auto index : current_) {
        const auto &info = providers_[index]->data_info();
        auto &item = items[info.interpolation.starts_with("finals2000a") ? 0 : 1];
        item.provider_index = index;
        item.extrapolation = true;
        item.first = utc_datetime(*clock, info.first_epoch);
        item.last = utc_datetime(*clock, info.last_epoch);
        item.source_url = QString::fromStdString(info.source_url);
        item.sha256 = QString::fromStdString(info.sha256);
        const auto key = application::eop_data_lock(info).sha256();
        item.identity_sha256 = QString::fromStdString(key);
        const auto entry = std::find_if(entries.begin(), entries.end(),
                                        [&](const auto &e) { return e.lock_sha256 == key; });
        const auto bundled = std::find_if(bundled_.begin(), bundled_.end(),
                                          [&](const auto &e) { return e.first == key; });
        std::filesystem::path manifest;
        if (entry != entries.end()) {
            item.acquisition = entry->acquisition;
            item.acquired_at = entry->acquired_at_unix_seconds;
            manifest = entry->manifest;
        } else if (bundled != bundled_.end()) {
            item.acquisition = runtime::DataAcquisition::bundled;
            manifest = bundled->second;
        }
        item.installation = item.acquisition == runtime::DataAcquisition::bundled
                                ? DataInstallation::bundled_sample
                            : item.acquisition == runtime::DataAcquisition::downloaded &&
                                    has_full_download(item.product)
                                ? DataInstallation::downloaded
                                : DataInstallation::local_file;
        if (!manifest.empty()) {
            QFile file(QString::fromStdWString(manifest.wstring()));
            if (file.open(QIODevice::ReadOnly)) {
                const auto filename =
                    QJsonDocument::fromJson(file.readAll()).object().value("data_file").toString();
                if (!filename.isEmpty()) {
                    item.file = manifest.parent_path() / filename.toStdWString();
                    std::error_code error;
                    const auto size = std::filesystem::file_size(item.file, error);
                    if (!error)
                        item.file_size = size;
                }
            }
        }
    }
    auto &utc = items[2];
    const auto &time = clock->data_info();
    utc.product = DataProduct::utc;
    utc.online_updates = true;
    utc.raw_import = true;
    utc.installation = DataInstallation::builtin;
    utc.acquisition = runtime::DataAcquisition::bundled;
    utc.first = {QDate(1858, 11, 17).addDays(time.utc_valid_from_mjd), QTime(0, 0), QTimeZone::UTC};
    utc.last = {QDate(1858, 11, 17).addDays(time.utc_valid_until_mjd), QTime(0, 0), QTimeZone::UTC};
    utc.exclusive_end = true;
    utc.source_url = QString::fromStdString(time.source_url);
    utc.sha256 = QString::fromStdString(time.sha256);
    if (!time_file_.empty()) {
        utc.file = time_file_;
        utc.acquisition = data_.setting("utc-acquisition") == "imported"
                              ? runtime::DataAcquisition::imported
                              : runtime::DataAcquisition::downloaded;
        utc.installation = utc.acquisition == runtime::DataAcquisition::imported
                               ? DataInstallation::local_file
                               : DataInstallation::downloaded;
        utc.acquired_at = timestamp(QString::fromStdString(data_.setting("utc-acquired-at")));
        std::error_code error;
        const auto size = std::filesystem::file_size(time_file_, error);
        if (!error)
            utc.file_size = size;
    }
    const auto gravity = data_.gravity_models();
    for (auto p : {DataProduct::egm2008, DataProduct::egm96, DataProduct::custom_gravity}) {
        auto &item = items[static_cast<std::size_t>(p)];
        item.product = p;
        item.online_updates = p != DataProduct::custom_gravity;
        item.raw_import = true;
        item.source_url = QString::fromStdString(source_url(p));
        for (const auto &entry : gravity) {
            if (entry.selection != gravity_selection(p))
                continue;
            item.gravity = entry.info;
            item.file = entry.file;
            item.acquired_at = entry.acquired_at_unix_seconds;
            item.acquisition = entry.acquisition;
            item.installation = entry.acquisition == runtime::DataAcquisition::downloaded
                                    ? DataInstallation::downloaded
                                    : DataInstallation::local_file;
            item.sha256 = QString::fromStdString(entry.info.sha256);
            item.source_url = QString::fromStdString(entry.info.source_url);
            std::error_code error;
            auto size = std::filesystem::file_size(entry.file, error);
            if (!error)
                item.file_size = size;
        }
    }
    auto &ephemeris = items[static_cast<std::size_t>(DataProduct::de440s)];
    ephemeris.product = DataProduct::de440s;
    ephemeris.online_updates = true;
    ephemeris.raw_import = true;
    ephemeris.source_url = QString::fromStdString(source_url(DataProduct::de440s));
    for (const auto &entry : data_.ephemerides()) {
        if (entry.selection != "de440s")
            continue;
        ephemeris.ephemeris = entry.info;
        ephemeris.file = entry.manifest;
        ephemeris.sha256 = QString::fromStdString(entry.info.content_sha256);
        for (const auto &value : ephemeris_kernels(entry)) {
            const auto kernel = value.toObject();
            if (kernel.value("kind").toString() != "SPK")
                continue;
            ephemeris.file = entry.manifest.parent_path() /
                             std::filesystem::path(kernel.value("file").toString().toStdWString());
            ephemeris.sha256 = kernel.value("sha256").toString();
            ephemeris.source_url = kernel.value("source_url").toString();
            break;
        }
        ephemeris.identity_sha256 = QString::fromStdString(entry.info.content_sha256);
        ephemeris.acquired_at = entry.acquired_at_unix_seconds;
        ephemeris.acquisition = entry.acquisition;
        ephemeris.installation = entry.acquisition == runtime::DataAcquisition::downloaded
                                     ? DataInstallation::downloaded
                                     : DataInstallation::local_file;
        // These are TAI calendar labels, not UTC conversions before UTC existed.
        const auto calendar = [](core::Epoch epoch) {
            return QDateTime(QDate(2000, 1, 1), QTime(12, 0), QTimeZone::UTC)
                .addSecs(epoch.tai_nanoseconds() / 1000000000LL);
        };
        ephemeris.first = calendar(entry.info.first);
        ephemeris.last = calendar(entry.info.last);
        std::error_code error;
        const auto size = std::filesystem::file_size(ephemeris.file, error);
        if (!error)
            ephemeris.file_size = size;
    }
    auto &weather = items[static_cast<std::size_t>(DataProduct::space_weather)];
    weather.product = DataProduct::space_weather;
    weather.online_updates = weather.raw_import = true;
    weather.source_url = QString::fromStdString(runtime::space_weather_download_url());
    for (const auto &entry : data_.weather_sources()) {
        if (entry.selection != "celestrak")
            continue;
        weather.file = entry.file;
        weather.sha256 = weather.identity_sha256 = QString::fromStdString(entry.info.sha256);
        weather.acquired_at = entry.acquired_at_unix_seconds;
        weather.acquisition = entry.acquisition;
        weather.installation = entry.acquisition == runtime::DataAcquisition::downloaded
                                   ? DataInstallation::downloaded
                                   : DataInstallation::local_file;
        const auto date = [](core::UtcDateTime u) {
            return QDateTime(QDate(u.year, u.month, u.day), QTime(0, 0), QTimeZone::UTC);
        };
        weather.first = date(entry.info.first);
        weather.last = date(entry.info.last);
        std::error_code error;
        const auto size = std::filesystem::file_size(entry.file, error);
        if (!error)
            weather.file_size = size;
    }
    const auto oceans = data_.ocean_tide_models();
    for (auto product : {DataProduct::fes2014bv1, DataProduct::custom_ocean_tides}) {
        auto &item = items[static_cast<std::size_t>(product)];
        item.product = product;
        item.raw_import = true;
        item.online_updates = product == DataProduct::fes2014bv1;
        item.source_url = QString::fromStdString(source_url(product));
        for (const auto &entry : oceans) {
            if (entry.selection != ocean_selection(product))
                continue;
            item.ocean_tides = entry.info;
            item.file = entry.file;
            item.sha256 = item.identity_sha256 = QString::fromStdString(entry.info.sha256);
            item.acquired_at = entry.acquired_at_unix_seconds;
            item.acquisition = entry.acquisition;
            item.installation = entry.acquisition == runtime::DataAcquisition::downloaded
                                    ? DataInstallation::downloaded
                                    : DataInstallation::local_file;
            std::error_code error;
            const auto size = std::filesystem::file_size(item.file, error);
            if (error)
                item.installation = DataInstallation::missing;
            else
                item.file_size = size;
        }
    }
    return items;
}
DataUpdateStatus EopCatalog::update_status(DataProduct product) const {
    auto result = updates_.at(static_cast<std::size_t>(product));
    if ((result.state == DataUpdateState::updated || result.state == DataUpdateState::unchanged) &&
        QString::fromStdString(current_hash(product)) != result.checked_sha256) {
        result.state = DataUpdateState::local_changed;
        result.message = QStringLiteral("本地文件已更换，请重新检查更新。");
    }
    return result;
}
void EopCatalog::save_update_status() {
    const auto &update_ = update(active_product_);
    const auto text = [](std::optional<qint64> value) {
        return value ? QString::number(*value) : QString{};
    };
    const QJsonObject record{{"schema", 1},
                             {"state", static_cast<int>(update_.state)},
                             {"attempted_at", text(update_.attempted_at)},
                             {"checked_at", text(update_.checked_at)},
                             {"checked_sha256", update_.checked_sha256},
                             {"message", update_.message}};
    data_.set_setting(setting_key(active_product_, "update-record"),
                      QJsonDocument(record).toJson(QJsonDocument::Compact).toStdString());
}
void EopCatalog::check_updates_if_due(qint64 now, QUrl source) {
    try {
        if (downloading())
            return;
        for (const auto product : {DataProduct::utc, DataProduct::finals2000a, DataProduct::c04,
                                   DataProduct::egm2008, DataProduct::egm96, DataProduct::de440s,
                                   DataProduct::space_weather, DataProduct::fes2014bv1}) {
            if ((!source.isEmpty() && product != DataProduct::finals2000a) ||
                !automatic_updates(product))
                continue;
            const auto last = std::max<qint64>(
                0, QString::fromStdString(data_.setting(setting_key(product, "last-attempt")))
                       .toLongLong());
            if (now < last || now - last < update_interval_hours(product) * 60 * 60)
                continue;
            start_download(std::move(source), product);
            return;
        }
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
}
std::size_t EopCatalog::append(Provider provider) {
    for (std::size_t i = 0; i < providers_.size(); ++i)
        if (application::matching_project_data(providers_[i]->data_info(), provider->data_info()))
            return i;
    providers_.push_back(std::move(provider));
    return providers_.size() - 1;
}
std::size_t EopCatalog::import_snapshot(const std::filesystem::path &manifest) {
    const auto entry = data_.install_eop(manifest, time_file_);
    data_.record_acquisition(entry.lock_sha256, runtime::DataAcquisition::imported);
    const auto index = append(data_.load_eop(entry.lock_sha256));
    refresh_current(index);
    return index;
}
void EopCatalog::refresh_current(std::optional<std::size_t> acquired) {
    std::optional<std::size_t> finals, c04;
    if (acquired) {
        const auto &info = providers_.at(*acquired)->data_info();
        data_.set_setting(info.interpolation.starts_with("finals2000a") ? "current-finals2000a"
                                                                        : "current-c04",
                          application::eop_data_lock(info).sha256());
    }
    const auto choose = [&](std::size_t index) {
        if (providers_[index]->data_info().time_data.sha256 != time_->data_info().sha256)
            return;
        (providers_[index]->data_info().interpolation.starts_with("finals2000a") ? finals : c04) =
            index;
    };
    // Include usable bundled fallbacks even if their catalog copies were damaged.
    for (std::size_t i = 0; i < providers_.size(); ++i)
        choose(i);
    // Existing stores without current pointers fall back to receipt/insertion order.
    for (const auto &entry : data_.snapshots())
        for (std::size_t i = 0; i < providers_.size(); ++i)
            if (application::eop_data_lock(providers_[i]->data_info()).sha256() ==
                entry.lock_sha256) {
                choose(i);
                break;
            }
    // Explicit pointers preserve the last successful acquisition even when timestamps
    // coincide, the local clock changes, or previously installed bytes are imported again.
    for (const auto &name : {"current-finals2000a", "current-c04"}) {
        const auto key = data_.setting(name);
        for (std::size_t i = 0; i < providers_.size(); ++i)
            if (application::eop_data_lock(providers_[i]->data_info()).sha256() == key) {
                choose(i);
                break;
            }
    }
    current_.clear();
    if (finals)
        current_.push_back(*finals);
    if (c04)
        current_.push_back(*c04);
    if (preferred_product() == DataProduct::c04)
        std::reverse(current_.begin(), current_.end());
    ++revision_;
}
DataProduct EopCatalog::preferred_product() const {
    return data_.preferred_eop() == runtime::EopProduct::finals2000a ? DataProduct::finals2000a
                                                                     : DataProduct::c04;
}
void EopCatalog::set_preferred_product(DataProduct product) {
    if (product != DataProduct::finals2000a && product != DataProduct::c04)
        throw std::invalid_argument("EOP priority must be finals2000A or C04");
    if (product == preferred_product())
        return;
    data_.set_preferred_eop(product == DataProduct::c04 ? runtime::EopProduct::c04
                                                        : runtime::EopProduct::finals2000a);
    refresh_current();
}
bool EopCatalog::automatic_updates(DataProduct product) const {
    if (force_product(product))
        return data_.setting(setting_key(product, "auto-update")) == "1";
    return data_.setting(setting_key(product, "auto-update")) != "0";
}
void EopCatalog::set_automatic_updates(bool enabled, DataProduct product) {
    data_.set_setting(setting_key(product, "auto-update"), enabled ? "1" : "0");
    ++revision_;
}
int EopCatalog::update_interval_hours(DataProduct product) const {
    bool valid = false;
    const auto hours =
        QString::fromStdString(data_.setting(setting_key(product, "update-hours"))).toInt(&valid);
    return valid && hours >= 1 && hours <= 168 ? hours : 24;
}
void EopCatalog::set_update_interval_hours(int hours, DataProduct product) {
    if (hours < 1 || hours > 168)
        throw std::invalid_argument("Update interval must be 1..168 hours");
    data_.set_setting(setting_key(product, "update-hours"), std::to_string(hours));
    ++revision_;
}
int EopCatalog::extrapolation_days(std::size_t index) const {
    return data_.extrapolation_days(eop_product(providers_.at(index)->data_info()));
}
void EopCatalog::set_extrapolation_days(std::size_t index, int days) {
    data_.set_extrapolation_days(eop_product(providers_.at(index)->data_info()), days);
    ++revision_;
}
EopCatalog::Provider EopCatalog::for_interval(core::Epoch first, core::Epoch last) const {
    if (first.tai_nanoseconds() >= last.tai_nanoseconds())
        throw std::invalid_argument("结束时间必须晚于开始时间。");
    std::vector<runtime::EopCandidate> candidates;
    for (const auto index : current_)
        candidates.push_back({providers_.at(index), extrapolation_days(index)});
    try {
        return runtime::select_eop_for_interval(candidates, first, last, time_);
    } catch (const core::EopCoverageError &error) {
        throw core::EopCoverageError(
            std::string("所选时段超出当前 EOP "
                        "数据覆盖及允许的外推范围，请在数据中心更新数据或调整外推设置。\n") +
            error.what());
    }
}
void EopCatalog::import_raw(const std::filesystem::path &source, DataProduct product) {
    if (downloading())
        throw std::runtime_error("请等待当前下载完成，或先取消下载。");
    if (force_product(product)) {
        start_resource_install(source, product, {}, runtime::DataAcquisition::imported);
        return;
    }
    install_source(source, product, source_url(product), runtime::DataAcquisition::imported);
    status_ = QStringLiteral("本地文件已导入。");
}
void EopCatalog::import_ephemeris_snapshot(const std::filesystem::path &source) {
    if (downloading())
        throw std::runtime_error("请等待当前操作完成，或先取消。");
    start_resource_install(source, DataProduct::de440s, {}, runtime::DataAcquisition::imported,
                           EphemerisImport::snapshot);
}
void EopCatalog::import_planetary_centres(const std::filesystem::path &source) {
    if (downloading())
        throw std::runtime_error("请等待当前操作完成，或先取消。");
    start_resource_install(source, DataProduct::de440s, {}, runtime::DataAcquisition::imported,
                           EphemerisImport::planetary_centres);
}
void EopCatalog::install_source(const std::filesystem::path &source, DataProduct product,
                                const std::string &url, runtime::DataAcquisition acquisition) {
    const auto retrieved =
        QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate).toStdString();
    if (product != DataProduct::utc) {
        const auto entry = product == DataProduct::finals2000a
                               ? data_.import_finals(source, url, retrieved, time_file_)
                               : data_.import_c04(source, url, retrieved, time_file_);
        data_.record_acquisition(entry.lock_sha256, acquisition);
        last_installed_ = append(data_.load_eop(entry.lock_sha256));
        refresh_current(last_installed_);
        return;
    }
    const auto file = data_.install_time(source);
    std::shared_ptr<const core::ITimeProvider> next_time = runtime::load_time_snapshot(file);
    if (next_time->data_info().utc_valid_until_mjd < time_->data_info().utc_valid_until_mjd)
        throw std::runtime_error("该闰秒文件的有效期早于当前数据，请使用较新的文件。");
    std::map<std::string, std::string> settings{
        {"current-utc", next_time->data_info().sha256},
        {"utc-acquisition",
         acquisition == runtime::DataAcquisition::imported ? "imported" : "downloaded"},
        {"utc-acquired-at", std::to_string(QDateTime::currentSecsSinceEpoch())}};
    std::vector<Provider> rebound;
    if (next_time->data_info().sha256 != time_->data_info().sha256) {
        const auto snapshots = data_.snapshots();
        for (const auto &item : inventory()) {
            if (!item.provider_index)
                continue;
            const auto found =
                std::find_if(snapshots.begin(), snapshots.end(), [&](const auto &entry) {
                    return entry.lock_sha256 == item.identity_sha256.toStdString();
                });
            if (found == snapshots.end())
                throw std::runtime_error("无法找到当前 EOP 文件，请重新导入后更新闰秒。");
            auto entry =
                has_full_download(item.product)
                    ? (item.product == DataProduct::finals2000a
                           ? data_.import_finals(item.file, found->source_url, retrieved, file)
                           : data_.import_c04(item.file, found->source_url, retrieved, file))
                    : data_.install_eop(found->manifest, file);
            if (item.acquisition != runtime::DataAcquisition::unknown)
                data_.record_acquisition(entry.lock_sha256, item.acquisition, item.acquired_at);
            rebound.push_back(data_.load_eop(entry.lock_sha256));
            settings[item.product == DataProduct::finals2000a ? "current-finals2000a"
                                                              : "current-c04"] = entry.lock_sha256;
        }
    }
    // All new providers are validated before atomically switching catalog references.
    // Existing computations retain their shared immutable providers until user confirmation.
    data_.set_settings(settings);
    time_ = std::move(next_time);
    time_file_ = file;
    for (auto &provider : rebound)
        append(std::move(provider));
    refresh_current();
}
void EopCatalog::cancel_download() {
    if (resource_cancel_)
        *resource_cancel_ = true;
    if (reply_) {
        cancelled_ = true;
        reply_->abort();
    }
}
void EopCatalog::start_download(QUrl url, DataProduct product) {
    if (downloading())
        return;
    active_product_ = product;
    auto &update_ = update(product);
    try {
        received_ = 0;
        total_ = -1;
        cancelled_ = false;
        transfer_error_.clear();
        last_installed_.reset();
        update_.attempted_at = QDateTime::currentSecsSinceEpoch();
        data_.set_setting(setting_key(product, "last-attempt"),
                          std::to_string(*update_.attempted_at));
        if (url.isEmpty())
            url = QUrl(QString::fromStdString(source_url(product)));
        if (url.scheme() != "https" && !(url.scheme() == "http" && url.host() == "127.0.0.1"))
            throw std::runtime_error("数据源必须使用 HTTPS。");
        download_ = std::make_unique<QTemporaryFile>(
            QString::fromStdWString((store_ / "staging/download-XXXXXX").wstring()));
        if (!download_->open())
            throw std::runtime_error("无法创建临时下载文件。");
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setMaximumRedirectsAllowed(3);
        request.setTransferTimeout(30000);
        request.setRawHeader("User-Agent", "OrbitForge/0.1 data-center");
        request.setRawHeader("Accept-Encoding", "identity");
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::AlwaysNetwork);
        const auto installed_hash = data_.setting(setting_key(product, "installed-source"));
        const auto current_hash = this->current_hash(product);
        // A validator for an older cached file cannot verify a different current import.
        const bool cached =
            data_.setting(setting_key(product, "source-url")) == url.toString().toStdString() &&
            !current_hash.empty() && current_hash == installed_hash && has_full_download(product);
        bool conditional = false;
        if (cached) {
            for (const auto &[setting, header] :
                 {std::pair{"etag", "If-None-Match"},
                  std::pair{"last-modified", "If-Modified-Since"}}) {
                const auto value = data_.setting(setting_key(product, setting));
                if (!value.empty()) {
                    request.setRawHeader(header, QByteArray::fromStdString(value));
                    conditional = true;
                }
            }
        }
        update_.state = DataUpdateState::checking;
        update_.message = status_ = QStringLiteral("正在连接数据源，检查完整文件…");
        save_update_status();
        reply_ = network_.get(request);
        reply_->setReadBufferSize(64 * 1024);
        deadline_.start(force_product(product) ? 1800000 : 120000);
        connect(reply_, &QNetworkReply::metaDataChanged, this, [this] {
            // Progress notifications may be delayed until the next body chunk arrives.
            // Use an uncompressed Content-Length as soon as headers are available.
            const auto encoding = reply_->rawHeader("Content-Encoding").toLower();
            if (encoding.isEmpty() || encoding == "identity") {
                bool valid = false;
                const auto length =
                    reply_->header(QNetworkRequest::ContentLengthHeader).toLongLong(&valid);
                if (valid && length > 0)
                    total_ = length;
                if (valid && length > download_limit(active_product_)) {
                    transfer_error_ = QStringLiteral("数据源文件超过 %1 MiB 限制。")
                                          .arg(download_limit(active_product_) / (1024 * 1024));
                    reply_->abort();
                }
            }
        });
        connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64, qint64 total) {
            if (total > 0)
                total_ = total;
        });
        connect(reply_, &QNetworkReply::readyRead, this, [this] {
            auto &update_ = update(active_product_);
            const auto bytes = reply_->readAll();
            received_ += bytes.size();
            if (received_ > 0) {
                update_.state = DataUpdateState::downloading;
                update_.message = status_ = QStringLiteral("正在下载完整文件…");
            }
            if (received_ > download_limit(active_product_) ||
                download_->write(bytes) != bytes.size()) {
                transfer_error_ = QStringLiteral("下载超过 %1 MiB 限制，或临时文件写入失败。")
                                      .arg(download_limit(active_product_) / (1024 * 1024));
                reply_->abort();
            }
        });
        connect(
            reply_, &QNetworkReply::finished, this,
            [this, url, conditional, current_hash, product] {
                auto &update_ = update(product);
                deadline_.stop();
                auto *finished = reply_;
                reply_ = nullptr;
                try {
                    if (cancelled_)
                        throw std::runtime_error("下载已取消，已有数据和场景保持不变。");
                    if (!transfer_error_.isEmpty())
                        throw std::runtime_error(transfer_error_.toStdString());
                    const auto http =
                        finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    if (finished->error() != QNetworkReply::NoError ||
                        (http != 200 && !(http == 304 && conditional)))
                        throw std::runtime_error(download_error(*finished, http).toStdString());
                    if (!download_->flush() || !download_->seek(0))
                        throw std::runtime_error("Cannot flush download");
                    if (http == 304) {
                        if (this->current_hash(product) != current_hash)
                            throw std::runtime_error("检查期间本地文件已更换，请重新检查。");
                        update_.state = DataUpdateState::unchanged;
                        update_.checked_sha256 = QString::fromStdString(current_hash);
                        status_ = QStringLiteral("数据已是最新，未重复下载。");
                    } else {
                        if (force_product(product)) {
                            resource_response_ = {
                                {setting_key(product, "source-url"), url.toString().toStdString()},
                                {setting_key(product, "etag"),
                                 finished->rawHeader("ETag").toStdString()},
                                {setting_key(product, "last-modified"),
                                 finished->rawHeader("Last-Modified").toStdString()}};
                            start_resource_install(
                                std::filesystem::path(download_->fileName().toStdWString()),
                                product, url.toString().toStdString(),
                                runtime::DataAcquisition::downloaded);
                            finished->deleteLater();
                            return;
                        }
                        const auto hash = QCryptographicHash::hash(download_->readAll(),
                                                                   QCryptographicHash::Sha256)
                                              .toHex()
                                              .toStdString();
                        install_source(std::filesystem::path(download_->fileName().toStdWString()),
                                       product, url.toString().toStdString(),
                                       runtime::DataAcquisition::downloaded);
                        data_.set_setting(setting_key(product, "installed-source"), hash);
                        data_.set_setting(setting_key(product, "source-url"),
                                          url.toString().toStdString());
                        data_.set_setting(setting_key(product, "etag"),
                                          finished->rawHeader("ETag").toStdString());
                        data_.set_setting(setting_key(product, "last-modified"),
                                          finished->rawHeader("Last-Modified").toStdString());
                        update_.state = hash == current_hash ? DataUpdateState::unchanged
                                                             : DataUpdateState::updated;
                        update_.checked_sha256 = QString::fromStdString(hash);
                        status_ = hash == current_hash
                                      ? QStringLiteral("检查完成，文件内容没有变化。")
                                      : QStringLiteral("完整文件已下载并通过校验。");
                    }
                    update_.checked_at = QDateTime::currentSecsSinceEpoch();
                } catch (const std::exception &error) {
                    update_.state =
                        cancelled_ ? DataUpdateState::cancelled : DataUpdateState::failed;
                    status_ = QString::fromUtf8(error.what());
                }
                update_.message = status_;
                try {
                    save_update_status();
                } catch (const std::exception &) {
                }
                download_.reset();
                finished->deleteLater();
            });
    } catch (const std::exception &error) {
        if (!reply_)
            download_.reset();
        update_.state = DataUpdateState::failed;
        update_.message = status_ = QString::fromUtf8(error.what());
        try {
            save_update_status();
        } catch (const std::exception &) {
        }
    }
}
void EopCatalog::start_resource_install(const std::filesystem::path &file, DataProduct product,
                                        const std::string &url,
                                        runtime::DataAcquisition acquisition,
                                        EphemerisImport ephemeris_import) {
    active_product_ = product;
    last_installed_.reset();
    total_ = -1;
    resource_cancel_ = std::make_shared<std::atomic_bool>(false);
    auto &record = update(product);
    record.attempted_at = QDateTime::currentSecsSinceEpoch();
    record.state = DataUpdateState::checking;
    record.message = status_ = QStringLiteral("正在后台校验完整数据与覆盖范围，可取消…");
    resource_job_ = std::async(std::launch::async, [root = store_, file, product, url, acquisition,
                                                    cancel = resource_cancel_, ephemeris_import] {
        runtime::DataCatalog catalog(root);
        const auto checkpoint = [cancel] {
            if (*cancel)
                throw std::runtime_error("操作已取消，已有数据保持不变。");
        };
        if (ocean_product(product)) {
            const auto installed = catalog.install_ocean_tides(file, ocean_selection(product), url,
                                                               acquisition, checkpoint);
            return InstalledResource{installed.info.sha256, acquisition};
        }
        if (product == DataProduct::space_weather) {
            const auto installed =
                catalog.install_space_weather(file, "celestrak", url, acquisition, checkpoint);
            return InstalledResource{installed.info.sha256, acquisition};
        }
        if (product == DataProduct::de440s) {
            const auto installed =
                ephemeris_import == EphemerisImport::snapshot
                    ? catalog.install_ephemeris_snapshot(file, acquisition, checkpoint)
                : ephemeris_import == EphemerisImport::planetary_centres
                    ? catalog.install_planetary_centres(file, url, acquisition, checkpoint)
                    : catalog.install_de440s(file, acquisition, checkpoint);
            return InstalledResource{installed.info.content_sha256, acquisition};
        }
        const auto installed =
            catalog.install_gravity(file, gravity_selection(product), url, acquisition, checkpoint);
        return InstalledResource{installed.info.sha256, acquisition};
    });
    resource_poll_.start(50);
}
} // namespace orbitforge::desktop
