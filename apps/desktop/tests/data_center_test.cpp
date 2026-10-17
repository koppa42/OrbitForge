#include "../src/viewer/eop_catalog.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <cmath>
#include <gtest/gtest.h>
#include <orbitforge/application/project.hpp>
#include <orbitforge/runtime/ephemeris.hpp>

namespace orbitforge::desktop {
namespace {
QByteArray payload() {
    QFile file(QString::fromStdString(
        (std::filesystem::path(ORBITFORGE_TEST_FINALS).parent_path() / "finals2000A.data")
            .string()));
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing finals fixture");
    return file.readAll();
}
class Server : public QTcpServer {
  public:
    QByteArray bytes, etag;
    QList<QByteArray> requests;
    explicit Server(QByteArray body, int code = 200, bool truncate = false)
        : bytes(std::move(body)) {
        if (!listen(QHostAddress::LocalHost))
            throw std::runtime_error("Cannot open loopback test server");
        QObject::connect(this, &QTcpServer::newConnection, this, [this, code, truncate] {
            auto *socket = nextPendingConnection();
            QObject::connect(
                socket, &QTcpSocket::readyRead, socket, [this, socket, code, truncate] {
                    const auto request = socket->readAll();
                    if (socket->property("answered").toBool())
                        return;
                    socket->setProperty("answered", true);
                    requests.push_back(request);
                    if (!etag.isEmpty() && request.contains("If-None-Match: " + etag)) {
                        socket->write("HTTP/1.1 304 Not Modified\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    socket->write("HTTP/1.1 " + QByteArray::number(code) +
                                  " Test\r\nContent-Length: " +
                                  QByteArray::number(bytes.size() + (truncate ? 30 : 0)) +
                                  (etag.isEmpty() ? QByteArray{} : "\r\nETag: " + etag) +
                                  "\r\nConnection: close\r\n\r\n" + bytes);
                    socket->disconnectFromHost();
                });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
    QUrl url() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/finals").arg(serverPort()));
    }
};
class DataCenterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        static int argc = 1;
        static char name[] = "data_center_test";
        static char *argv[] = {name, nullptr};
        static QCoreApplication app(argc, argv);
        ASSERT_TRUE(temp.isValid());
    }
    std::filesystem::path root() const { return std::filesystem::path(temp.path().toStdWString()); }
    static void complete(EopCatalog &catalog) {
        QElapsedTimer timer;
        timer.start();
        const int limit = qEnvironmentVariableIsEmpty("ORBITFORGE_LIVE_IERS_URL") &&
                                  qEnvironmentVariableIsEmpty("ORBITFORGE_LIVE_IERS")
                              ? 15000
                              : 120000;
        while (catalog.downloading() && timer.elapsed() < limit)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        ASSERT_FALSE(catalog.downloading()) << catalog.status().toStdString();
    }
    QTemporaryDir temp;
};
TEST_F(DataCenterTest, DownloadSelectsCurrentDataWithoutChangingActiveProviders) {
    EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}, {}, false);
    auto original = catalog.providers().front();
    const auto lock = application::eop_data_lock(original->data_info());
    for (const auto &entry : catalog.data().snapshots()) {
        EXPECT_EQ(entry.acquisition, runtime::DataAcquisition::bundled);
        EXPECT_FALSE(entry.acquired_at_unix_seconds);
    }
    const auto before = QDateTime::currentSecsSinceEpoch();
    Server server(payload());
    const auto live = qEnvironmentVariable("ORBITFORGE_LIVE_IERS_URL");
    const auto endpoint = live.isEmpty() ? server.url() : QUrl(live);
    catalog.start_download(endpoint);
    complete(catalog);
    ASSERT_TRUE(catalog.last_installed().has_value()) << catalog.status().toStdString();
    EXPECT_EQ(catalog.providers().size(), 3);
    const auto &installed = catalog.providers().at(*catalog.last_installed())->data_info();
    if (live.isEmpty()) {
        EXPECT_EQ(
            installed.sha256,
            QCryptographicHash::hash(payload(), QCryptographicHash::Sha256).toHex().toStdString());
        EXPECT_EQ(installed.first_epoch, original->data_info().first_epoch);
        EXPECT_EQ(installed.last_epoch, original->data_info().last_epoch);
    } else {
        const auto time = runtime::make_builtin_time_provider();
        EXPECT_EQ(time->to_utc(installed.first_epoch).year, 1973);
    }
    EXPECT_EQ(catalog.preferred_product(), DataProduct::c04);
    EXPECT_EQ(catalog.for_interval(installed.first_epoch, installed.last_epoch),
              catalog.providers().at(*catalog.last_installed()));
    const auto receipt = catalog.data().snapshots().back();
    EXPECT_EQ(receipt.acquisition, runtime::DataAcquisition::downloaded);
    ASSERT_TRUE(receipt.acquired_at_unix_seconds);
    EXPECT_GE(*receipt.acquired_at_unix_seconds, before);
    EXPECT_LE(*receipt.acquired_at_unix_seconds, QDateTime::currentSecsSinceEpoch());
    EXPECT_EQ(catalog.inventory()[0].installation, DataInstallation::downloaded);
    EXPECT_EQ(catalog.update_status().state,
              live.isEmpty() ? DataUpdateState::unchanged : DataUpdateState::updated);
    EXPECT_TRUE(catalog.update_status().checked_at);
    EXPECT_EQ(application::eop_data_lock(original->data_info()), lock);
    EXPECT_EQ(catalog.data().resolve_eop(lock)->data_info().id, original->data_info().id);
    catalog.start_download(endpoint);
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 3);
    EXPECT_TRUE(std::filesystem::is_empty(root() / "data/staging"));
}
TEST_F(DataCenterTest, CancelHttpFailureTruncationAndInvalidDataNeverInstall) {
    EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_C04}, {}, false);
    Server valid(payload());
    catalog.start_download(valid.url());
    catalog.cancel_download();
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 1);
    EXPECT_TRUE(catalog.status().contains(QStringLiteral("取消")));
    EXPECT_EQ(catalog.update_status().state, DataUpdateState::cancelled);
    EXPECT_FALSE(catalog.update_status().checked_at);
    for (int kind = 0; kind < 3; ++kind) {
        Server failure(kind == 0 ? QByteArray("<html>invalid</html>") : payload(),
                       kind == 1 ? 404 : 200, kind == 2);
        catalog.start_download(failure.url());
        complete(catalog);
        EXPECT_EQ(catalog.providers().size(), 1);
        EXPECT_FALSE(catalog.last_installed().has_value());
        EXPECT_EQ(catalog.update_status().state, DataUpdateState::failed);
        EXPECT_FALSE(catalog.update_status().checked_at);
        EXPECT_TRUE(std::filesystem::is_empty(root() / "data/staging"));
    }
}
TEST_F(DataCenterTest, OversizedResponseIsRejectedAndCanRetry) {
    EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_C04}, {}, false);
    Server large(QByteArray(16 * 1024 * 1024 + 1, 'x'));
    catalog.start_download(large.url());
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 1);
    EXPECT_TRUE(catalog.status().contains("16 MiB"));
    Server good(payload());
    catalog.start_download(good.url());
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 2);
}
TEST_F(DataCenterTest, LegacyMigrationAndPreferencesSurviveRestart) {
    const auto old = runtime::import_eop_snapshot(ORBITFORGE_TEST_C04, root() / "eop");
    auto provider = runtime::load_eop_snapshot(old);
    const auto lock = application::eop_data_lock(provider->data_info());
    {
        EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_FINALS}, root() / "eop", false);
        ASSERT_EQ(catalog.providers().size(), 2);
        EXPECT_EQ(application::eop_data_lock(catalog.providers()[1]->data_info()), lock);
        catalog.set_automatic_updates(false);
    }
    EopCatalog reopened(root() / "data", {ORBITFORGE_TEST_FINALS}, root() / "eop", false);
    EXPECT_EQ(reopened.current_indices().size(), 2);
    EXPECT_FALSE(reopened.automatic_updates());
    EXPECT_EQ(reopened.providers().size(), 2);
    EXPECT_TRUE(std::filesystem::exists(old));
    const auto migrated = reopened.data().snapshots().back();
    EXPECT_EQ(migrated.acquisition, runtime::DataAcquisition::unknown);
    EXPECT_FALSE(migrated.acquired_at_unix_seconds);
}
TEST_F(DataCenterTest, ReimportingExistingContentSelectsItAndPersistsAcrossRestart) {
    const auto store = root() / "data";
    std::string selected_key;
    {
        EopCatalog catalog(store, {ORBITFORGE_TEST_C04}, {}, false);
        const auto source = root() / "finals.all";
        QFile file(QString::fromStdWString(source.wstring()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(payload());
        file.close();
        catalog.import_raw(source);
        selected_key = application::eop_data_lock(catalog.providers().back()->data_info()).sha256();
        ASSERT_TRUE(file.open(QIODevice::Append));
        file.write("\n");
        file.close();
        catalog.import_raw(source);
        EXPECT_EQ(catalog.inventory()[0].provider_index, 2);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(payload());
        file.close();
        catalog.import_raw(source);
        EXPECT_EQ(catalog.inventory()[0].provider_index, 1);
        EXPECT_EQ(catalog.current_indices().size(), 2);
        EXPECT_EQ(catalog.providers().size(), 3);
    }
    EopCatalog reopened(store, {ORBITFORGE_TEST_C04}, {}, false);
    EXPECT_EQ(application::eop_data_lock(
                  reopened.providers()[*reopened.inventory()[0].provider_index]->data_info())
                  .sha256(),
              selected_key);
    EXPECT_EQ(reopened.current_indices().size(), 2);
}
TEST_F(DataCenterTest, AutomaticUpdatesAcquireFirstFullFileAndRespectIntervalAndOptOut) {
    EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_FINALS}, {}, false);
    Server server(payload());
    const auto now = QDateTime::currentSecsSinceEpoch();
    EXPECT_TRUE(catalog.automatic_updates());
    catalog.check_updates_if_due(now, server.url());
    EXPECT_TRUE(catalog.downloading());
    EXPECT_EQ(catalog.update_status().state, DataUpdateState::checking);
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 2);
    EXPECT_TRUE(catalog.has_full_download());
    catalog.check_updates_if_due(now, server.url());
    EXPECT_FALSE(catalog.downloading());
    catalog.set_update_interval_hours(6);
    catalog.data().set_setting("finals-last-attempt", std::to_string(now - 5 * 3600));
    catalog.check_updates_if_due(now, server.url());
    EXPECT_FALSE(catalog.downloading());
    catalog.data().set_setting("finals-last-attempt", std::to_string(now - 6 * 3600));
    catalog.check_updates_if_due(now, server.url());
    EXPECT_TRUE(catalog.downloading());
    complete(catalog);
    catalog.set_automatic_updates(false);
    catalog.data().set_setting("finals-last-attempt", "0");
    catalog.check_updates_if_due(now, server.url());
    EXPECT_FALSE(catalog.downloading());
    catalog.set_automatic_updates(true);
    catalog.check_updates_if_due(now, server.url());
    EXPECT_TRUE(catalog.downloading());
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 2);
    EXPECT_EQ(catalog.inventory()[0].provider_index, catalog.providers().size() - 1);
    EXPECT_THROW(catalog.set_update_interval_hours(0), std::invalid_argument);
    EXPECT_THROW(catalog.set_update_interval_hours(169), std::invalid_argument);
}
TEST_F(DataCenterTest, ConditionalUpdatesSkipUnchangedBytesAndPreserveOldVersionsAcrossRestart) {
    Server server(payload());
    server.etag = "\"revision-1\"";
    const auto store = root() / "data";
    std::string first_key;
    std::optional<std::int64_t> downloaded_at;
    {
        EopCatalog catalog(store, {ORBITFORGE_TEST_C04}, {}, false);
        catalog.start_download(server.url());
        complete(catalog);
        ASSERT_TRUE(catalog.last_installed());
        first_key = application::eop_data_lock(catalog.providers().back()->data_info()).sha256();
        downloaded_at = catalog.data().snapshots().back().acquired_at_unix_seconds;
        ASSERT_TRUE(downloaded_at);
        EXPECT_FALSE(server.requests.back().contains("If-None-Match"));
    }
    EopCatalog catalog(store, {ORBITFORGE_TEST_C04}, {}, false);
    const auto revision = catalog.revision();
    catalog.start_download(server.url());
    complete(catalog);
    EXPECT_TRUE(server.requests.back().contains("If-None-Match: \"revision-1\""));
    EXPECT_EQ(catalog.received_bytes(), 0);
    EXPECT_EQ(catalog.update_status().state, DataUpdateState::unchanged);
    const auto checked_at = catalog.update_status().checked_at;
    EXPECT_TRUE(checked_at);
    EXPECT_EQ(catalog.providers().size(), 2);
    EXPECT_TRUE(catalog.status().contains(QStringLiteral("未重复下载")));
    EXPECT_EQ(catalog.data().snapshots().back().acquired_at_unix_seconds, downloaded_at);
    EXPECT_EQ(catalog.revision(), revision);
    server.etag = "\"revision-2\"";
    // A new source revision, even with unchanged useful values, has its own full-file identity.
    server.bytes += '\n';
    catalog.data().set_setting("finals-last-attempt", "0");
    catalog.check_updates_if_due(QDateTime::currentSecsSinceEpoch(), server.url());
    complete(catalog);
    EXPECT_EQ(catalog.providers().size(), 3);
    EXPECT_EQ(catalog.inventory()[0].provider_index, catalog.providers().size() - 1);
    EXPECT_NO_THROW((void)catalog.data().load_eop(first_key));
    EXPECT_NE(application::eop_data_lock(catalog.providers().back()->data_info()).sha256(),
              first_key);
    // A failed newer response must not replace the validator for the installed revision.
    server.etag = "\"broken-revision\"";
    server.bytes = "<html>error</html>";
    const auto current_revision = catalog.revision();
    const auto current_receipt = catalog.data().snapshots().back();
    catalog.start_download(server.url());
    complete(catalog);
    EXPECT_EQ(catalog.data().setting("finals-etag"), "\"revision-2\"");
    EXPECT_EQ(catalog.providers().size(), 3);
    EXPECT_EQ(catalog.current_indices().size(), 2);
    EXPECT_EQ(catalog.revision(), current_revision);
    EXPECT_EQ(catalog.data().snapshots().back().acquired_at_unix_seconds,
              current_receipt.acquired_at_unix_seconds);
    EXPECT_TRUE(std::filesystem::is_empty(store / "staging"));
}
TEST_F(DataCenterTest, InventorySeparatesInstallationCoverageAndUpdateCapabilities) {
    EopCatalog empty(root() / "empty", {}, {}, false);
    const auto missing = empty.inventory();
    ASSERT_EQ(missing.size(), all_data_products.size());
    EXPECT_EQ(missing[0].installation, DataInstallation::missing);
    EXPECT_FALSE(missing[0].last.isValid());
    EXPECT_TRUE(missing[0].online_updates);
    EXPECT_TRUE(missing[1].manifest_import);
    EXPECT_TRUE(missing[1].online_updates);
    EXPECT_EQ(missing[2].product, DataProduct::utc);
    EXPECT_EQ(missing[2].installation, DataInstallation::builtin);
    EXPECT_TRUE(missing[2].exclusive_end);
    EXPECT_TRUE(missing[2].online_updates);
    EXPECT_FALSE(missing[2].extrapolation);
    EXPECT_FALSE(missing[2].acquired_at);

    EopCatalog samples(root() / "samples", {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}, {},
                       false);
    const auto installed = samples.inventory();
    EXPECT_EQ(installed[0].installation, DataInstallation::bundled_sample);
    EXPECT_EQ(installed[1].installation, DataInstallation::bundled_sample);
    EXPECT_FALSE(installed[0].acquired_at);
    EXPECT_TRUE(installed[0].file_size);
    EXPECT_TRUE(std::filesystem::exists(installed[0].file));
    EXPECT_EQ(installed[1].last.date(), QDate(2000, 1, 2));
    EXPECT_EQ(samples.update_status().state, DataUpdateState::never_checked);
    samples.import_snapshot(ORBITFORGE_TEST_C04);
    const auto imported = samples.inventory();
    EXPECT_EQ(imported[1].installation, DataInstallation::local_file);
    EXPECT_TRUE(imported[1].acquired_at);
    EXPECT_EQ(imported[1].last, installed[1].last);
}
TEST_F(DataCenterTest, LocalReplacementMustNotReuseAnOlderFilesOnlineVerification) {
    Server server(payload());
    server.etag = "\"server-file\"";
    EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_FINALS}, {}, false);
    catalog.start_download(server.url());
    complete(catalog);
    const auto verified = catalog.update_status().checked_sha256;
    QFile imported(temp.filePath("different.all"));
    ASSERT_TRUE(imported.open(QIODevice::WriteOnly));
    imported.write(payload() + "\n");
    imported.close();
    catalog.import_raw(std::filesystem::path(imported.fileName().toStdWString()));
    EXPECT_EQ(catalog.update_status().state, DataUpdateState::local_changed);
    EXPECT_EQ(catalog.inventory()[0].installation, DataInstallation::local_file);
    catalog.start_download(server.url());
    complete(catalog);
    ASSERT_EQ(server.requests.size(), 2);
    EXPECT_FALSE(server.requests.back().contains("If-None-Match"));
    EXPECT_EQ(catalog.update_status().state, DataUpdateState::updated);
    EXPECT_EQ(catalog.update_status().checked_sha256, verified);
    EXPECT_EQ(catalog.inventory()[0].sha256, verified);
    catalog.import_snapshot(ORBITFORGE_TEST_FINALS);
    EXPECT_FALSE(catalog.has_full_download());
}
TEST_F(DataCenterTest, UpdateResultsAndPreferencesSurviveFailureCancellationAndRestart) {
    const auto store = root() / "data";
    std::optional<qint64> checked_at, downloaded_at;
    QString verified;
    {
        EopCatalog catalog(store, {}, {}, false);
        Server server(payload());
        catalog.start_download(server.url());
        complete(catalog);
        checked_at = catalog.update_status().checked_at;
        verified = catalog.update_status().checked_sha256;
        downloaded_at = catalog.inventory()[0].acquired_at;
        Server bad("invalid", 500);
        catalog.start_download(bad.url());
        complete(catalog);
        EXPECT_EQ(catalog.update_status().state, DataUpdateState::failed);
        EXPECT_EQ(catalog.update_status().checked_at, checked_at);
        EXPECT_EQ(catalog.inventory()[0].acquired_at, downloaded_at);
        catalog.set_update_interval_hours(168);
        catalog.set_automatic_updates(false);
    }
    {
        EopCatalog catalog(store, {}, {}, false);
        EXPECT_EQ(catalog.update_status().state, DataUpdateState::failed);
        EXPECT_EQ(catalog.update_status().checked_at, checked_at);
        EXPECT_EQ(catalog.update_status().checked_sha256, verified);
        EXPECT_EQ(catalog.update_interval_hours(), 168);
        EXPECT_FALSE(catalog.automatic_updates());
        Server server(payload());
        catalog.start_download(server.url());
        catalog.cancel_download();
        complete(catalog);
        EXPECT_EQ(catalog.update_status().state, DataUpdateState::cancelled);
        EXPECT_EQ(catalog.update_status().checked_at, checked_at);
        EXPECT_EQ(catalog.inventory()[0].acquired_at, downloaded_at);
    }
    EopCatalog reopened(store, {}, {}, false);
    EXPECT_EQ(reopened.update_status().state, DataUpdateState::cancelled);
    EXPECT_EQ(reopened.update_status().checked_at, checked_at);
}
TEST_F(DataCenterTest, InterruptedAndLegacyChecksNeverClaimThatCurrentDataIsLatest) {
    const auto store = root() / "data";
    {
        runtime::DataCatalog data(store);
        data.set_setting("finals-last-attempt", "1234567890");
        data.set_setting("finals-status", "数据已是最新");
    }
    {
        EopCatalog legacy(store, {}, {}, false);
        EXPECT_EQ(legacy.update_status().state, DataUpdateState::never_checked);
        EXPECT_FALSE(legacy.update_status().checked_at);
        legacy.data().set_setting("finals-update-record",
                                  R"({"schema":1,"state":1,"checked_at":"1234567800"})");
    }
    EopCatalog interrupted(store, {}, {}, false);
    EXPECT_EQ(interrupted.update_status().state, DataUpdateState::interrupted);
    EXPECT_EQ(interrupted.update_status().attempted_at, 1234567890);
    EXPECT_EQ(interrupted.update_status().checked_at, 1234567800);
}
TEST_F(DataCenterTest, LiveOfficialProductsAreValidated) {
    if (qEnvironmentVariableIsEmpty("ORBITFORGE_LIVE_IERS"))
        GTEST_SKIP() << "Opt-in official network check";
    EopCatalog catalog(root() / "data", {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}, {}, false);
    for (const auto product : {DataProduct::utc, DataProduct::c04, DataProduct::finals2000a}) {
        catalog.start_download({}, product);
        complete(catalog);
        const auto state = catalog.update_status(product).state;
        ASSERT_TRUE(state == DataUpdateState::updated || state == DataUpdateState::unchanged)
            << catalog.status().toStdString();
        EXPECT_TRUE(catalog.has_full_download(product));
        const auto item = catalog.inventory()[static_cast<std::size_t>(product)];
        EXPECT_EQ(item.installation, DataInstallation::downloaded);
        EXPECT_TRUE(item.acquired_at);
        std::cout << "product=" << static_cast<int>(product)
                  << " bytes=" << item.file_size.value_or(0)
                  << " coverage=" << item.first.toString(Qt::ISODate).toStdString() << " / "
                  << item.last.toString(Qt::ISODate).toStdString()
                  << " sha256=" << item.sha256.toStdString() << '\n';
    }
}
TEST_F(DataCenterTest, C04AndUtcDownloadsAreIndependentAndRebindCalculationTimeAtomically) {
    QFile c04(QString::fromStdWString(
        (std::filesystem::path(ORBITFORGE_TEST_C04).parent_path() / "eop.c04").wstring()));
    ASSERT_TRUE(c04.open(QIODevice::ReadOnly));
    auto c04_bytes = c04.readAll();
    const auto day = QDate(1858, 11, 17).daysTo(QDate(2027, 1, 1));
    c04_bytes.replace("2000   1   1   0  51544.00",
                      "2027   1   1   0  " + QByteArray::number(day) + ".00");
    c04_bytes.replace("2000   1   2   0  51545.00",
                      "2027   1   2   0  " + QByteArray::number(day + 1) + ".00");
    QFile leap(QString::fromStdString(ORBITFORGE_TEST_TIME));
    ASSERT_TRUE(leap.open(QIODevice::ReadOnly));
    auto time_bytes = leap.readAll();
    time_bytes.replace("28 June 2027", "28 June 2028");
    time_bytes += "\n" + QByteArray::number(day) + ".0 1 1 2027 38\n"; // Synthetic positive leap.
    const auto store = root() / "data";
    application::DataLock new_lock;
    std::string new_time_hash;
    {
        EopCatalog catalog(store, {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}, {}, false);
        const auto old_time = catalog.time_provider();
        Server c04_server(c04_bytes);
        c04_server.etag = "\"c04\"";
        catalog.start_download(c04_server.url(), DataProduct::c04);
        complete(catalog);
        ASSERT_EQ(catalog.update_status(DataProduct::c04).state, DataUpdateState::updated)
            << catalog.status().toStdString();
        ASSERT_TRUE(catalog.inventory()[1].acquired_at);
        const auto receipt = catalog.inventory()[1].acquired_at;
        const auto first = old_time->from_utc({2027, 1, 1, 0, 0, 0, 0});
        const auto last = old_time->from_utc({2027, 1, 2, 0, 0, 0, 0});
        const auto active = catalog.for_interval(first, last);
        const auto value = active->at(first).sample.ut1_minus_tai_seconds;
        catalog.start_download(c04_server.url(), DataProduct::c04);
        complete(catalog);
        EXPECT_EQ(catalog.update_status(DataProduct::c04).state, DataUpdateState::unchanged);
        EXPECT_EQ(catalog.update_status().state, DataUpdateState::never_checked);
        EXPECT_EQ(catalog.inventory()[1].acquired_at, receipt);
        catalog.set_automatic_updates(false, DataProduct::c04);
        catalog.set_update_interval_hours(168, DataProduct::utc);
        EXPECT_TRUE(catalog.automatic_updates());
        EXPECT_EQ(catalog.update_interval_hours(), 24);
        Server time_server(time_bytes);
        time_server.etag = "\"utc\"";
        catalog.start_download(time_server.url(), DataProduct::utc);
        complete(catalog);
        ASSERT_EQ(catalog.update_status(DataProduct::utc).state, DataUpdateState::updated)
            << catalog.status().toStdString();
        EXPECT_EQ(catalog.inventory()[2].installation, DataInstallation::downloaded);
        EXPECT_EQ(catalog.inventory()[1].acquired_at, receipt);
        const auto time = catalog.time_provider();
        new_time_hash = time->data_info().sha256;
        EXPECT_NE(new_time_hash, old_time->data_info().sha256);
        const auto next_first = time->from_utc({2027, 1, 1, 0, 0, 0, 0});
        const auto next_last = time->from_utc({2027, 1, 2, 0, 0, 0, 0});
        EXPECT_DOUBLE_EQ(next_first.seconds_since(first), 1);
        EXPECT_NO_THROW((void)time->from_utc({2028, 1, 1, 0, 0, 0, 0}));
        EXPECT_THROW((void)old_time->from_utc({2028, 1, 1, 0, 0, 0, 0}), core::TimeDataRangeError);
        const auto updated = catalog.for_interval(next_first, next_last);
        new_lock = application::eop_data_lock(updated->data_info());
        EXPECT_EQ(updated->data_info().time_data.sha256, new_time_hash);
        EXPECT_DOUBLE_EQ(updated->at(next_first).sample.ut1_minus_tai_seconds, value - 1);
        EXPECT_DOUBLE_EQ(active->at(first).sample.ut1_minus_tai_seconds, value);
        EXPECT_NE(new_lock, application::eop_data_lock(active->data_info()));
        EXPECT_EQ(catalog.data().resolve_eop(new_lock)->data_info().time_data.sha256,
                  new_time_hash);
        catalog.start_download(time_server.url(), DataProduct::utc);
        complete(catalog);
        EXPECT_EQ(catalog.update_status(DataProduct::utc).state, DataUpdateState::unchanged);
        Server invalid("# File expires on tomorrow\n<html>not a leap table</html>");
        catalog.start_download(invalid.url(), DataProduct::utc);
        complete(catalog);
        EXPECT_EQ(catalog.update_status(DataProduct::utc).state, DataUpdateState::failed);
        EXPECT_EQ(catalog.time_provider(), time);
        EXPECT_EQ(catalog.for_interval(next_first, next_last), updated);
    }
    EopCatalog reopened(store, {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}, {}, false);
    EXPECT_TRUE(reopened.warnings().empty()) << reopened.warnings().join('\n').toStdString();
    EXPECT_EQ(reopened.time_provider()->data_info().sha256, new_time_hash);
    EXPECT_EQ(reopened.data().resolve_eop(new_lock)->data_info().time_data.sha256, new_time_hash);
    EXPECT_FALSE(reopened.automatic_updates(DataProduct::c04));
    EXPECT_TRUE(reopened.automatic_updates());
    EXPECT_EQ(reopened.update_interval_hours(DataProduct::utc), 168);
    EXPECT_EQ(reopened.update_status(DataProduct::utc).state, DataUpdateState::failed);
    EXPECT_EQ(reopened.update_status(DataProduct::c04).state, DataUpdateState::unchanged);
}
TEST_F(DataCenterTest, SourcePriorityPersistsAndRealCoveragePrecedesExtrapolation) {
    // Synthetic overlapping dates exercise source selection, not scientific accuracy.
    QFile original(QString::fromStdString(ORBITFORGE_TEST_C04));
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    auto manifest = QJsonDocument::fromJson(original.readAll()).object();
    QFile data(QString::fromStdWString(
        (std::filesystem::path(ORBITFORGE_TEST_C04).parent_path() / "eop.c04").wstring()));
    ASSERT_TRUE(data.open(QIODevice::ReadOnly));
    auto bytes = data.readAll();
    bytes.replace("2000   1   1   0  51544.00", "2026   9   1   0  61284.00");
    bytes.replace("2000   1   2   0  51545.00", "2026   9   2   0  61285.00");
    manifest["snapshot_id"] = "test-c04-overlap";
    manifest["data_file"] = "overlap.c04";
    manifest["sha256"] =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    manifest["first_mjd"] = 61284;
    manifest["last_mjd"] = 61285;
    QFile fixture(temp.path() + "/overlap.c04");
    ASSERT_TRUE(fixture.open(QIODevice::WriteOnly));
    ASSERT_EQ(fixture.write(bytes), bytes.size());
    fixture.close();
    QFile metadata(temp.path() + "/overlap.json");
    ASSERT_TRUE(metadata.open(QIODevice::WriteOnly));
    metadata.write(QJsonDocument(manifest).toJson());
    metadata.close();
    const auto store = root() / "data";
    const std::vector<std::filesystem::path> bundled{ORBITFORGE_TEST_FINALS,
                                                     root() / "overlap.json"};
    {
        EopCatalog catalog(store, bundled, {}, false);
        EXPECT_EQ(catalog.preferred_product(), DataProduct::c04);
        const auto default_info = catalog.providers()[1]->data_info();
        EXPECT_EQ(catalog.for_interval(default_info.first_epoch, default_info.last_epoch),
                  catalog.providers()[1]);
        catalog.set_preferred_product(DataProduct::finals2000a);
        const auto finals = catalog.providers()[0], c04 = catalog.providers()[1];
        const auto first = c04->data_info().first_epoch, last = c04->data_info().last_epoch;
        const auto active = catalog.for_interval(first, last);
        const auto active_lock = application::eop_data_lock(active->data_info());
        EXPECT_EQ(catalog.preferred_product(), DataProduct::finals2000a);
        EXPECT_EQ(active, finals);
        catalog.set_preferred_product(DataProduct::c04);
        EXPECT_EQ(catalog.preferred_index(), 1);
        EXPECT_EQ(catalog.for_interval(first, last), c04);
        EXPECT_EQ(application::eop_data_lock(active->data_info()), active_lock);
        EXPECT_EQ(active, finals);
        catalog.set_extrapolation_days(1, 2);
        const core::Epoch after{last.tai_nanoseconds() + 3'600'000'000'000};
        EXPECT_EQ(catalog.for_interval(last, after), finals);
        const core::Epoch before{first.tai_nanoseconds() - 3'600'000'000'000};
        catalog.set_extrapolation_days(0, 3);
        EXPECT_EQ(catalog.for_interval(before, first)->data_info().sha256, c04->data_info().sha256);
        EXPECT_EQ(catalog.for_interval(before, first)->data_info().extrapolation_days, 2);
        catalog.set_preferred_product(DataProduct::finals2000a);
        EXPECT_EQ(catalog.for_interval(first, last), finals);
        EXPECT_EQ(catalog.for_interval(before, first)->data_info().sha256,
                  finals->data_info().sha256);
        catalog.set_preferred_product(DataProduct::c04);
        catalog.set_extrapolation_days(1, 0);
        EXPECT_EQ(catalog.for_interval(before, first)->data_info().sha256,
                  finals->data_info().sha256);
        catalog.import_snapshot(ORBITFORGE_TEST_FINALS);
        EXPECT_EQ(catalog.preferred_product(), DataProduct::c04);
        EXPECT_EQ(catalog.for_interval(first, last), c04);
        EXPECT_THROW(catalog.set_preferred_product(DataProduct::utc), std::invalid_argument);
        EXPECT_EQ(catalog.preferred_product(), DataProduct::c04);
    }
    EopCatalog reopened(store, bundled, {}, false);
    EXPECT_EQ(reopened.preferred_product(), DataProduct::c04);
    EXPECT_EQ(reopened.preferred_index(), 1);
    const auto &info = reopened.providers()[1]->data_info();
    EXPECT_EQ(reopened.for_interval(info.first_epoch, info.last_epoch), reopened.providers()[1]);
    EopCatalog missing(root() / "missing", {ORBITFORGE_TEST_FINALS}, {}, false);
    missing.set_preferred_product(DataProduct::c04);
    const auto available = missing.providers()[0];
    EXPECT_EQ(
        missing.for_interval(available->data_info().first_epoch, available->data_info().last_epoch),
        available);
}
TEST_F(DataCenterTest, ExtrapolationPoliciesAreIndependentPersistentAndNeverChangeActiveProviders) {
    const auto store = root() / "data";
    EopCatalog::Provider active;
    core::Epoch begin{0}, end{0};
    {
        EopCatalog catalog(store, {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}, {}, false);
        EXPECT_EQ(catalog.extrapolation_days(0), 0);
        EXPECT_EQ(catalog.extrapolation_days(1), 0);
        const auto &info = catalog.providers()[0]->data_info();
        begin = core::Epoch{info.last_epoch.tai_nanoseconds() + 1'000'000'000};
        end = core::Epoch{begin.tai_nanoseconds() + 60'000'000'000};
        EXPECT_THROW((void)catalog.for_interval(begin, end), core::EopCoverageError);
        catalog.set_extrapolation_days(0, 2);
        active = catalog.for_interval(begin, end);
        EXPECT_EQ(active->data_info().extrapolation_days, 2);
        EXPECT_EQ(active->data_info().last_epoch, info.last_epoch);
        EXPECT_EQ(catalog.extrapolation_days(1), 0);
        EXPECT_EQ(
            catalog.for_interval(info.first_epoch, info.last_epoch)->data_info().extrapolation_days,
            0);
        catalog.set_extrapolation_days(0, 0);
        EXPECT_THROW((void)catalog.for_interval(begin, end), core::EopCoverageError);
        EXPECT_NO_THROW((void)active->at(end));
        catalog.set_extrapolation_days(0, 3);
        EXPECT_THROW(catalog.set_extrapolation_days(0, 31), std::invalid_argument);
        EXPECT_THROW(catalog.set_extrapolation_days(1, -1), std::invalid_argument);
    }
    EopCatalog reopened(store, {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}, {}, false);
    EXPECT_EQ(reopened.extrapolation_days(0), 3);
    EXPECT_EQ(reopened.extrapolation_days(1), 0);
    EXPECT_EQ(active->data_info().extrapolation_days, 2);
    EXPECT_EQ(reopened.for_interval(begin, end)->data_info().extrapolation_days, 3);
    reopened.data().set_setting("extrapolation-finals2000a-days", "invalid");
    EXPECT_EQ(reopened.extrapolation_days(0), 0);
}
TEST_F(DataCenterTest, LimitedExtrapolationIsContinuousDifferentiableAndBoundedAtBothEnds) {
    constexpr std::int64_t day = 86'400'000'000'000;
    for (const auto *manifest : {ORBITFORGE_TEST_FINALS, ORBITFORGE_TEST_C04}) {
        const auto strict = runtime::load_eop_snapshot(manifest);
        const auto extended = runtime::load_eop_snapshot(manifest, 2);
        const auto &info = strict->data_info();
        EXPECT_EQ(extended->data_info().sha256, info.sha256);
        EXPECT_EQ(extended->data_info().first_epoch, info.first_epoch);
        EXPECT_EQ(extended->data_info().last_epoch, info.last_epoch);
        for (const auto direction : {-1, 1}) {
            const auto anchor = direction < 0 ? info.first_epoch : info.last_epoch;
            const auto limit = core::Epoch{anchor.tai_nanoseconds() + direction * 2 * day};
            EXPECT_NO_THROW((void)extended->at(limit));
            EXPECT_THROW((void)extended->at(core::Epoch{limit.tai_nanoseconds() + direction}),
                         core::EopCoverageError);
            const auto inside = extended->at(anchor);
            EXPECT_EQ(inside.sample.ut1_minus_tai_seconds,
                      strict->at(anchor).sample.ut1_minus_tai_seconds);
            const auto adjacent = extended->at(core::Epoch{anchor.tai_nanoseconds() + direction});
            EXPECT_NEAR(adjacent.sample.ut1_minus_tai_seconds, inside.sample.ut1_minus_tai_seconds,
                        1e-12);
            const auto earlier =
                extended->at(core::Epoch{anchor.tai_nanoseconds() - 1'000'000'000});
            const auto later = extended->at(core::Epoch{anchor.tai_nanoseconds() + 1'000'000'000});
            EXPECT_NEAR(
                (later.sample.ut1_minus_tai_seconds - earlier.sample.ut1_minus_tai_seconds) / 2,
                inside.rates.ut1_minus_tai_seconds_per_second, 1e-11);
            EXPECT_NEAR((later.sample.polar_motion_x_rad - earlier.sample.polar_motion_x_rad) / 2,
                        inside.rates.polar_motion_x_rad_per_second, 1e-15);
            EXPECT_NEAR((later.sample.polar_motion_y_rad - earlier.sample.polar_motion_y_rad) / 2,
                        inside.rates.polar_motion_y_rad_per_second, 1e-15);
            EXPECT_NEAR(
                (later.sample.celestial_pole_dx_rad - earlier.sample.celestial_pole_dx_rad) / 2,
                inside.rates.celestial_pole_dx_rad_per_second, 1e-15);
            EXPECT_NEAR(
                (later.sample.celestial_pole_dy_rad - earlier.sample.celestial_pole_dy_rad) / 2,
                inside.rates.celestial_pole_dy_rad_per_second, 1e-15);
            EXPECT_NE(
                extended->at(limit).sample.source_id.find("extrapolation=endpoint-linear-tai-v1"),
                std::string::npos);
            EXPECT_THROW((void)strict->at(limit), core::EopCoverageError);
        }
    }
}
TEST_F(DataCenterTest, ExtrapolationNeverExtendsLeapSecondValidity) {
    class EndOfTimeData final : public core::IEarthOrientationProvider {
      public:
        core::EopDataInfo info;
        explicit EndOfTimeData(const core::ITimeProvider &clock)
            : info{"test",
                   std::string(64, 'a'),
                   "test",
                   "ITRS",
                   "test",
                   clock.from_utc({2027, 6, 26, 0, 0, 0, 0}),
                   clock.from_utc({2027, 6, 27, 0, 0, 0, 0}),
                   clock.data_info()} {}
        const core::EopDataInfo &data_info() const noexcept override { return info; }
        core::EarthOrientationState at(core::Epoch t) const override {
            return {{t, -37, 0, 0, 0, 0, "test"}, {0, 0, 0, 0, 0}};
        }
    };
    const auto time = runtime::make_builtin_time_provider();
    auto source = std::make_shared<EndOfTimeData>(*time);
    const auto extended = runtime::with_eop_extrapolation(source, 30);
    const auto expiry = core::Epoch{source->info.last_epoch.tai_nanoseconds() + 86'400'000'000'000};
    EXPECT_NO_THROW((void)extended->at(core::Epoch{expiry.tai_nanoseconds() - 1}));
    EXPECT_THROW((void)extended->at(expiry), core::TimeDataRangeError);
}
TEST_F(DataCenterTest, GravityImportRunsInBackgroundAndRejectsInvalidReplacement) {
    EopCatalog catalog(root() / "data", {}, {}, false);
    EXPECT_FALSE(catalog.automatic_updates(DataProduct::egm2008));
    EXPECT_FALSE(catalog.automatic_updates(DataProduct::egm96));
    const auto file = root() / "custom.grv";
    QFile source(QString::fromStdWString(file.wstring()));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write("stk.v.6.0\nModel Local\nDegree 21\nOrder 21\nGm 3.986004415e14\nRefDistance "
                 "6378136.3\nNormalized Yes\nIncludesPermTide No\nBEGIN Coefficients\n2 0 "
                 "-0.000484 0\nEND Coefficients\n");
    source.close();
    catalog.import_raw(file, DataProduct::custom_gravity);
    ASSERT_TRUE(catalog.downloading());
    complete(catalog);
    const auto item = catalog.inventory().at(static_cast<std::size_t>(DataProduct::custom_gravity));
    ASSERT_TRUE(item.gravity);
    EXPECT_EQ(item.gravity->degree, 21);
    EXPECT_FALSE(item.first.isValid());
    EXPECT_FALSE(item.last.isValid());
    ASSERT_TRUE(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
    source.write("bad gravity data\n");
    source.close();
    catalog.import_raw(file, DataProduct::custom_gravity);
    complete(catalog);
    EXPECT_EQ(catalog.update_status(DataProduct::custom_gravity).state, DataUpdateState::failed);
    EXPECT_EQ(catalog.inventory().at(static_cast<std::size_t>(DataProduct::custom_gravity)).sha256,
              item.sha256);
    Server server("not an official EGM2008 field");
    catalog.start_download(server.url(), DataProduct::egm2008);
    complete(catalog);
    EXPECT_EQ(catalog.update_status(DataProduct::egm2008).state, DataUpdateState::failed);
    EXPECT_FALSE(catalog.has_full_download(DataProduct::egm2008));
}
TEST_F(DataCenterTest, LiveOfficialGravityDownload) {
    if (qEnvironmentVariableIsEmpty("ORBITFORGE_LIVE_GRAVITY"))
        GTEST_SKIP();
    EopCatalog catalog(root() / "data", {}, {}, false);
    const auto product = qEnvironmentVariable("ORBITFORGE_LIVE_GRAVITY") == "egm2008"
                             ? DataProduct::egm2008
                             : DataProduct::egm96;
    catalog.start_download({}, product);
    QElapsedTimer timer;
    timer.start();
    while (catalog.downloading() && timer.elapsed() < 600000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    ASSERT_FALSE(catalog.downloading());
    ASSERT_EQ(catalog.update_status(product).state, DataUpdateState::updated)
        << catalog.status().toStdString();
    EXPECT_TRUE(catalog.has_full_download(product));
    const auto received = catalog.received_bytes();
    EXPECT_GT(received, product == DataProduct::egm2008 ? 200'000'000 : 5'000'000);
    catalog.start_download({}, product);
    timer.restart();
    while (catalog.downloading() && timer.elapsed() < 600000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    ASSERT_FALSE(catalog.downloading());
    EXPECT_EQ(catalog.update_status(product).state, DataUpdateState::unchanged);
}
TEST_F(DataCenterTest, EphemerisDownloadRejectsWrongContentAndCanBeCancelled) {
    EopCatalog catalog(root() / "data", {}, {}, false);
    EXPECT_FALSE(catalog.automatic_updates(DataProduct::de440s));
    EXPECT_FALSE(catalog.has_full_download(DataProduct::de440s));
    EXPECT_TRUE(
        catalog.inventory().at(static_cast<std::size_t>(DataProduct::de440s)).online_updates);
    Server server("invalid SPK data");
    catalog.start_download(server.url(), DataProduct::de440s);
    complete(catalog);
    EXPECT_EQ(catalog.update_status(DataProduct::de440s).state, DataUpdateState::failed);
    EXPECT_TRUE(catalog.data().ephemerides().empty());
    catalog.start_download(server.url(), DataProduct::de440s);
    catalog.cancel_download();
    complete(catalog);
    EXPECT_EQ(catalog.update_status(DataProduct::de440s).state, DataUpdateState::cancelled);
    EXPECT_TRUE(catalog.data().ephemerides().empty());
    catalog.set_automatic_updates(true, DataProduct::de440s);
    catalog.set_update_interval_hours(168, DataProduct::de440s);
    EopCatalog reopened(root() / "data", {}, {}, false);
    EXPECT_TRUE(reopened.automatic_updates(DataProduct::de440s));
    EXPECT_EQ(reopened.update_interval_hours(DataProduct::de440s), 168);
}
TEST_F(DataCenterTest, LiveOfficialEphemerisDownloadAndRecheckRetainCurrentData) {
    const auto local_file = qEnvironmentVariable("ORBITFORGE_FULL_DE440S");
    if (qEnvironmentVariableIsEmpty("ORBITFORGE_LIVE_DE440S") && local_file.isEmpty())
        GTEST_SKIP();
    std::unique_ptr<Server> local_source;
    if (!local_file.isEmpty()) {
        QFile file(local_file);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        ASSERT_LE(file.size(), 64 * 1024 * 1024);
        local_source = std::make_unique<Server>(file.readAll());
        local_source->etag = "\"full-de440s\"";
    }
    const auto endpoint = local_source ? local_source->url() : QUrl{};
    EopCatalog catalog(root() / "data", {}, {}, false);
    catalog.start_download(endpoint, DataProduct::de440s);
    QElapsedTimer timer;
    timer.start();
    while (catalog.downloading() && timer.elapsed() < 300000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    ASSERT_FALSE(catalog.downloading());
    ASSERT_EQ(catalog.update_status(DataProduct::de440s).state, DataUpdateState::updated)
        << catalog.status().toStdString();
    ASSERT_TRUE(catalog.has_full_download(DataProduct::de440s));
    const auto item = catalog.inventory().at(static_cast<std::size_t>(DataProduct::de440s));
    EXPECT_EQ(item.first.date(), QDate(1850, 1, 1));
    EXPECT_EQ(item.last.date(), QDate(2150, 1, 1));
    EXPECT_GT(*item.file_size, 30000000U);
    auto provider = catalog.data().load_ephemeris("de440s");
    const auto position = provider->position(core::ThirdBody::sun, core::Epoch{0});
    const auto sample = std::filesystem::path(ORBITFORGE_TEST_PLANETS).parent_path().parent_path() /
                        "mar099s_2016_leap/mar099s_subset.bsp";
    catalog.import_planetary_centres(sample);
    complete(catalog);
    ASSERT_EQ(catalog.update_status(DataProduct::de440s).state, DataUpdateState::updated);
    const auto augmented = catalog.data().ephemerides().at(0);
    catalog.import_raw(augmented.manifest.parent_path() / "de440s.bsp", DataProduct::de440s);
    complete(catalog);
    ASSERT_EQ(catalog.update_status(DataProduct::de440s).state, DataUpdateState::unchanged);
    const auto refreshed = catalog.data().ephemerides().at(0);
    EXPECT_EQ(refreshed.info.content_sha256, augmented.info.content_sha256);
    const std::array mars{core::ThirdBody::mars};
    const auto supplemented = catalog.data().load_ephemeris("de440s");
    EXPECT_THROW(supplemented->validate_eclipsing_body_interval(refreshed.info.first,
                                                                refreshed.info.last, mars),
                 core::EphemerisCoverageError);
    catalog.start_download(endpoint, DataProduct::de440s);
    timer.restart();
    while (catalog.downloading() && timer.elapsed() < 300000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    ASSERT_FALSE(catalog.downloading());
    EXPECT_EQ(catalog.update_status(DataProduct::de440s).state, DataUpdateState::unchanged);
    if (local_source) {
        catalog.start_download(endpoint, DataProduct::de440s);
        complete(catalog);
        EXPECT_EQ(catalog.update_status(DataProduct::de440s).state, DataUpdateState::unchanged);
        ASSERT_EQ(local_source->requests.size(), 3);
        EXPECT_TRUE(local_source->requests.back().contains("If-None-Match: " + local_source->etag));
    }
    EXPECT_EQ(catalog.data().ephemerides().at(0).info.content_sha256,
              augmented.info.content_sha256);
    EXPECT_TRUE(catalog.has_full_download(DataProduct::de440s));
    EXPECT_EQ(provider->position(core::ThirdBody::sun, core::Epoch{0}), position);
    Server invalid("truncated replacement");
    catalog.start_download(invalid.url(), DataProduct::de440s);
    complete(catalog);
    EXPECT_EQ(catalog.update_status(DataProduct::de440s).state, DataUpdateState::failed);
    EXPECT_EQ(catalog.inventory().at(static_cast<std::size_t>(DataProduct::de440s)).sha256,
              item.sha256);
    EXPECT_EQ(
        catalog.data().load_ephemeris("de440s")->position(core::ThirdBody::sun, core::Epoch{0}),
        position);
}
} // namespace
TEST_F(DataCenterTest, WeatherDownloadImportsRawBytesAndFailedReplacementRetainsSnapshot) {
    EopCatalog catalog(root() / "data", {}, {}, false);
    const QByteArray csv =
        "DATE,BSRN,ND,KP1,KP2,KP3,KP4,KP5,KP6,KP7,KP8,KP_SUM,"
        "AP1,AP2,AP3,AP4,AP5,AP6,AP7,AP8,AP_AVG,CP,C9,ISN,F10.7_OBS,F10.7_ADJ,"
        "F10.7_DATA_TYPE,F10.7_OBS_CENTER81,F10.7_OBS_LAST81,F10.7_ADJ_CENTER81,F10.7_ADJ_LAST81\n"
        "2000-01-01,1,1,30,30,30,30,30,30,30,30,240,15,15,15,15,15,15,15,15,15,0,0,0,150,150,OBS,"
        "150,150,150,150\n";
    Server server(csv);
    catalog.start_download(server.url(), DataProduct::space_weather);
    complete(catalog);
    ASSERT_EQ(catalog.update_status(DataProduct::space_weather).state, DataUpdateState::updated)
        << catalog.status().toStdString();
    EXPECT_TRUE(catalog.has_full_download(DataProduct::space_weather));
    const auto before = catalog.data().load_space_weather("celestrak");
    const auto item = catalog.inventory().at(static_cast<std::size_t>(DataProduct::space_weather));
    EXPECT_EQ(item.first.date(), QDate(2000, 1, 1));
    EXPECT_EQ(item.sha256, QString::fromStdString(before->info().sha256));
    Server broken("bad weather");
    catalog.start_download(broken.url(), DataProduct::space_weather);
    complete(catalog);
    EXPECT_EQ(catalog.update_status(DataProduct::space_weather).state, DataUpdateState::failed);
    EXPECT_EQ(catalog.data().load_space_weather("celestrak")->info().sha256, before->info().sha256);
}

TEST_F(DataCenterTest, OceanImportRunsInBackgroundAndFailedOrCancelledDownloadsPreserveData) {
    EopCatalog catalog(root() / "data", {}, {}, false);
    EXPECT_FALSE(catalog.automatic_updates(DataProduct::fes2014bv1));
    const auto path = root() / "custom.pot";
    QFile file(QString::fromStdWString(path.wstring()));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(
        "Coefficients to compute variations in normalized Stokes coefficients (unit = 10^-11)\n"
        "Ocean tide model: Local up to (2,0)\n(test)\nDoodson Darw l m DelC+ DelS+ DelC- DelS-\n"
        "55.555 Test 2 0 1 2 3 4\n");
    file.close();
    catalog.import_raw(path, DataProduct::custom_ocean_tides);
    EXPECT_TRUE(catalog.downloading());
    complete(catalog);
    const auto item =
        catalog.inventory().at(static_cast<std::size_t>(DataProduct::custom_ocean_tides));
    ASSERT_TRUE(item.ocean_tides);
    EXPECT_EQ(item.ocean_tides->degree, 2);
    EXPECT_EQ(item.ocean_tides->order, 0);
    EXPECT_FALSE(item.first.isValid());
    const auto frozen = catalog.data().load_ocean_tides("custom-ocean");
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("bad ocean data");
    file.close();
    catalog.import_raw(path, DataProduct::custom_ocean_tides);
    complete(catalog);
    EXPECT_EQ(catalog.update_status(DataProduct::custom_ocean_tides).state,
              DataUpdateState::failed);
    EXPECT_EQ(catalog.data().load_ocean_tides("custom-ocean")->info().sha256,
              frozen->info().sha256);
    Server invalid("not an official FES potential model");
    catalog.start_download(invalid.url(), DataProduct::fes2014bv1);
    complete(catalog);
    EXPECT_EQ(catalog.update_status(DataProduct::fes2014bv1).state, DataUpdateState::failed);
    EXPECT_FALSE(catalog.has_full_download(DataProduct::fes2014bv1));
    catalog.start_download(invalid.url(), DataProduct::fes2014bv1);
    catalog.cancel_download();
    complete(catalog);
    EXPECT_EQ(catalog.update_status(DataProduct::fes2014bv1).state, DataUpdateState::cancelled);
    EXPECT_EQ(catalog.data().load_ocean_tides("custom-ocean")->info().sha256,
              frozen->info().sha256);
}
TEST_F(DataCenterTest, FullOceanTransferRecheckAndInvalidReplacementPreserveFrozenModel) {
    const auto path = qEnvironmentVariable("ORBITFORGE_FULL_OCEAN_FILE");
    if (path.isEmpty())
        GTEST_SKIP()
            << "Set ORBITFORGE_FULL_OCEAN_FILE to test the complete pinned download payload";
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    Server server(file.readAll());
    server.etag = "\"ocean-v1\"";
    EopCatalog catalog(root() / "data", {}, {}, false);
    catalog.start_download(server.url(), DataProduct::fes2014bv1);
    complete(catalog);
    ASSERT_EQ(catalog.update_status(DataProduct::fes2014bv1).state, DataUpdateState::updated)
        << catalog.status().toStdString();
    EXPECT_TRUE(catalog.has_full_download(DataProduct::fes2014bv1));
    const auto frozen = catalog.data().load_ocean_tides("fes2014bv1");
    EXPECT_EQ(frozen->info().degree, 100);
    EXPECT_EQ(frozen->info().constituents, 36U);
    catalog.start_download(server.url(), DataProduct::fes2014bv1);
    complete(catalog);
    EXPECT_EQ(catalog.update_status(DataProduct::fes2014bv1).state, DataUpdateState::unchanged);
    ASSERT_EQ(server.requests.size(), 2);
    EXPECT_TRUE(server.requests.back().contains("If-None-Match: " + server.etag));
    Server invalid("truncated replacement");
    catalog.start_download(invalid.url(), DataProduct::fes2014bv1);
    complete(catalog);
    EXPECT_EQ(catalog.update_status(DataProduct::fes2014bv1).state, DataUpdateState::failed);
    EXPECT_EQ(catalog.data().load_ocean_tides("fes2014bv1")->info().sha256, frozen->info().sha256);
}

TEST_F(DataCenterTest,
       PlanetarySpkImportUsesBackgroundPublicationAndRetainsThePreviousSnapshotOnFailure) {
    if (!runtime::ephemeris_available())
        GTEST_SKIP();
    EopCatalog catalog(root() / "planet-data", {}, {}, false);
    catalog.import_ephemeris_snapshot(ORBITFORGE_TEST_PLANETS);
    complete(catalog);
    ASSERT_EQ(catalog.update_status(DataProduct::de440s).state, DataUpdateState::updated)
        << catalog.status().toStdString();
    const auto base = catalog.data().ephemerides().at(0);
    EXPECT_FALSE(catalog.has_full_download(DataProduct::de440s));
    const auto inventory = catalog.inventory().at(static_cast<std::size_t>(DataProduct::de440s));
    EXPECT_TRUE(std::filesystem::is_regular_file(inventory.file));
    EXPECT_EQ(inventory.file.filename(), "orbitforge_de440s_planets_2016_leap.bsp");
    EXPECT_EQ(inventory.sha256,
              QStringLiteral("e4c52121a5de093ea75f9826d5db48aeba25bb0f8bbe542e6fa07c6a0dd824fd"));
    EXPECT_EQ(inventory.identity_sha256, QString::fromStdString(base.info.content_sha256));
    const auto sample = std::filesystem::path(ORBITFORGE_TEST_PLANETS).parent_path().parent_path() /
                        "mar099s_2016_leap/mar099s_subset.bsp";
    catalog.import_planetary_centres(sample);
    complete(catalog);
    ASSERT_EQ(catalog.update_status(DataProduct::de440s).state, DataUpdateState::updated)
        << catalog.status().toStdString();
    const auto imported = catalog.data().ephemerides().at(0);
    EXPECT_NE(imported.info.content_sha256, base.info.content_sha256);
    auto provider = catalog.data().load_ephemeris("de440s");
    const std::array mars{core::ThirdBody::mars};
    EXPECT_NO_THROW(
        provider->validate_eclipsing_body_interval(imported.info.first, imported.info.last, mars));
    const auto revision = catalog.revision();
    catalog.import_planetary_centres(root() / "missing.bsp");
    complete(catalog);
    EXPECT_EQ(catalog.update_status(DataProduct::de440s).state, DataUpdateState::failed);
    EXPECT_EQ(catalog.data().ephemerides().at(0).info.content_sha256, imported.info.content_sha256);
    EXPECT_EQ(catalog.revision(), revision);
    EopCatalog reopened(root() / "planet-data", {}, {}, false);
    EXPECT_EQ(reopened.data().ephemerides().at(0).info.content_sha256,
              imported.info.content_sha256);
}
} // namespace orbitforge::desktop
