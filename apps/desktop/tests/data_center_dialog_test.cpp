#include "../src/viewer/eop_catalog.hpp"
#include "../src/viewer/initial_data_setup.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "window_show_observer.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <gtest/gtest.h>

namespace orbitforge::desktop {
namespace {
QApplication &test_app() {
    static int argc = 1;
    static char name[] = "data_center_ui_test";
    static char *argv[] = {name, nullptr};
    static QApplication app(argc, argv);
    apply_workbench_style(app);
    return app;
}
class DownloadServer : public QTcpServer {
  public:
    QByteArray body;
    bool hold = true, fail = false;
    QPointer<QTcpSocket> pending;
    explicit DownloadServer(QByteArray bytes) : body(std::move(bytes)) {
        if (!listen(QHostAddress::LocalHost))
            throw std::runtime_error("Cannot open UI loopback server");
        connect(this, &QTcpServer::newConnection, this, [this] {
            auto *socket = nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                const auto request = socket->readAll();
                if (socket->property("answered").toBool())
                    return;
                socket->setProperty("answered", true);
                if (fail) {
                    socket->write("HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: "
                                  "close\r\n\r\n");
                    socket->disconnectFromHost();
                } else if (!hold && request.contains("If-None-Match: \"ui-file\"")) {
                    socket->write("HTTP/1.1 304 Not Modified\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                } else {
                    socket->write("HTTP/1.1 200 OK\r\nETag: \"ui-file\"\r\nContent-Length: " +
                                  QByteArray::number(body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + body.left(256));
                    pending = socket;
                    if (!hold)
                        release();
                }
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
    void release() {
        if (pending) {
            pending->write(body.mid(256));
            pending->disconnectFromHost();
            pending.clear();
        }
    }
    QUrl url() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/finals").arg(serverPort()));
    }
};
TEST(DataCenterDialog, LoadingWarningsStayInsideTheDialog) {
    (void)test_app();
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    ASSERT_TRUE(QDir().mkpath(temporary.path() + "/legacy/" + QString(64, 'a')));
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    // An incomplete legacy snapshot should produce a visible, inline warning.
    EopCatalog catalog(root / "data", {ORBITFORGE_TEST_FINALS}, root / "legacy", false);
    ASSERT_FALSE(catalog.warnings().isEmpty());
    WindowShowObserver windows;
    bool inspected = false;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        bool found = false;
        for (auto *label : dialog->findChildren<QLabel *>("inputError"))
            if (label->text() == catalog.warnings().join('\n')) {
                EXPECT_TRUE(label->isVisible());
                EXPECT_FALSE(label->isWindow());
                found = true;
            }
        EXPECT_TRUE(found);
        inspected = true;
        dialog->reject();
    });
    show_data_center(nullptr, catalog);
    EXPECT_TRUE(inspected);
    EXPECT_EQ(windows.shown.join(", ").toStdString(), "QDialog:eopDataDialog");
}
TEST(DataCenterDialog, GroupedSourcesKeepDownloadStateAndPriorityIndependent) {
    (void)test_app();
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    QFile file(QString::fromStdWString(
        (std::filesystem::path(ORBITFORGE_TEST_FINALS).parent_path() / "finals2000A.data")
            .wstring()));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    DownloadServer server(file.readAll());
    EopCatalog catalog(root / "data", {ORBITFORGE_TEST_FINALS}, {}, false);
    const auto output = qEnvironmentVariable("ORBITFORGE_DATA_UI_CAPTURE");
    if (!output.isEmpty())
        ASSERT_TRUE(QDir().mkpath(output));
    const auto capture = [&](QDialog *dialog, const QString &state) {
        if (!output.isEmpty()) {
            for (int pass = 0; pass < 3; ++pass)
                QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            EXPECT_LE(dialog->height(), dialog->screen()->availableGeometry().height());
            EXPECT_LE(dialog->width(), dialog->screen()->availableGeometry().width());
            for (auto *scroll : dialog->findChildren<QScrollArea *>())
                if (scroll->isVisible())
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0) << state.toStdString();
            EXPECT_TRUE(dialog->grab().save(output + "/" + state + ".png"));
        }
    };
    int stage = 0;
    std::optional<qint64> downloaded_at;
    QElapsedTimer elapsed;
    elapsed.start();
    // This case exercises the side-by-side layout even on Qt's 800px offscreen display.
    QTimer::singleShot(0, [] {
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            dialog->resize(1020, dialog->height());
    });
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        if (elapsed.elapsed() > 10000) {
            ADD_FAILURE() << "Data center UI timeout at stage " << stage
                          << ", received=" << catalog.received_bytes()
                          << ", total=" << catalog.total_bytes();
            timer.stop();
            dialog->reject();
            return;
        }
        auto *list = dialog->findChild<QTreeWidget *>("dataInventory");
        auto *priority = dialog->findChild<QListWidget *>("eopSourcePriority");
        auto *pages = dialog->findChild<QTabWidget *>("dataPages");
        auto *automatic = dialog->findChild<QCheckBox *>("dataAutoUpdate");
        auto *checked = dialog->findChild<QLabel *>("dataLastChecked");
        auto *badge = dialog->findChild<QLabel *>("dataStatus");
        auto *progress = dialog->findChild<QProgressBar *>("dataDownloadProgress");
        auto *cancel = dialog->findChild<QPushButton *>("cancelEopDownload");
        auto *download = dialog->findChild<QPushButton *>("downloadEop");
        auto *status = dialog->findChild<QLabel *>("dataCenterStatus");
        ASSERT_TRUE(list && priority && checked && badge && progress && cancel && download &&
                    status);
        ASSERT_EQ(list->topLevelItemCount(), 6);
        auto *group = list->topLevelItem(0);
        ASSERT_EQ(group->childCount(), 2);
        auto *finals = group->child(0);
        auto *c04 = group->child(1);
        auto *utc = list->topLevelItem(1);
        switch (stage) {
        case 0:
            EXPECT_EQ(list->currentItem(), group);
            EXPECT_TRUE(group->isExpanded());
            EXPECT_TRUE(priority->isVisible());
            EXPECT_EQ(priority->count(), 2);
            EXPECT_EQ(priority->dragDropMode(), QAbstractItemView::InternalMove);
            EXPECT_FALSE(dialog->findChild<QComboBox *>("eopDataSource"));
            EXPECT_FALSE(download->isVisible());
            EXPECT_FALSE(checked->isVisible());
            EXPECT_EQ(priority->item(0)->data(Qt::UserRole).toInt(),
                      static_cast<int>(DataProduct::c04));
            EXPECT_TRUE(priority->model()->moveRow({}, 1, {}, 0));
            EXPECT_EQ(catalog.preferred_product(), DataProduct::finals2000a);
            EXPECT_TRUE(priority->model()->moveRow({}, 1, {}, 0));
            EXPECT_EQ(catalog.preferred_product(), DataProduct::c04);
            capture(dialog, "priority-c04");
            EXPECT_TRUE(priority->model()->moveRow({}, 1, {}, 0));
            EXPECT_EQ(catalog.preferred_product(), DataProduct::finals2000a);
            priority->setCurrentRow(1);
            dialog->findChild<QToolButton *>("eopPriorityUp")->click();
            EXPECT_EQ(catalog.preferred_product(), DataProduct::c04);
            dialog->findChild<QToolButton *>("eopPriorityDown")->click();
            EXPECT_EQ(catalog.preferred_product(), DataProduct::finals2000a);
            capture(dialog, "priority");
            list->setCurrentItem(finals);
            EXPECT_FALSE(priority->isVisible());
            EXPECT_TRUE(badge->text().contains(QStringLiteral("示例")));
            EXPECT_TRUE(checked->text().contains(QStringLiteral("尚未检查")));
            list->setCurrentItem(c04);
            EXPECT_EQ(catalog.preferred_product(), DataProduct::finals2000a);
            EXPECT_TRUE(badge->text().contains(QStringLiteral("未下载")));
            EXPECT_FALSE(dialog->findChild<QPushButton *>("importEopSnapshot")->isVisible());
            EXPECT_TRUE(download->isVisible());
            ASSERT_TRUE(pages);
            EXPECT_EQ(pages->currentIndex(), 0);
            EXPECT_FALSE(automatic->isVisible());
            capture(dialog, "missing-c04");
            pages->setCurrentIndex(1);
            EXPECT_TRUE(automatic->isVisible());
            automatic->setChecked(false);
            EXPECT_FALSE(catalog.automatic_updates(DataProduct::c04));
            EXPECT_TRUE(catalog.automatic_updates());
            capture(dialog, "c04-settings");
            pages->setCurrentIndex(2);
            EXPECT_TRUE(dialog->findChild<QPushButton *>("importEopSnapshot")->isVisible());
            list->setCurrentItem(finals);
            EXPECT_EQ(pages->currentIndex(), 0);
            capture(dialog, "sample");
            catalog.start_download(server.url());
            ++stage;
            break;
        case 1:
            if (!progress->isVisible() || catalog.received_bytes() == 0 || progress->maximum() == 0)
                break;
            EXPECT_EQ(catalog.update_status().state, DataUpdateState::downloading);
            EXPECT_FALSE(download->isEnabled());
            EXPECT_TRUE(cancel->isVisible());
            EXPECT_GT(progress->value(), 0);
            EXPECT_LT(progress->value(), progress->maximum());
            capture(dialog, "downloading");
            list->setCurrentItem(c04);
            EXPECT_FALSE(progress->isVisible());
            EXPECT_FALSE(cancel->isVisible());
            EXPECT_TRUE(finals->text(0).contains(QStringLiteral("正在下载")));
            EXPECT_TRUE(checked->text().contains(QStringLiteral("尚未检查")));
            list->setCurrentItem(utc);
            EXPECT_FALSE(priority->isVisible());
            EXPECT_FALSE(progress->isVisible());
            EXPECT_FALSE(cancel->isVisible());
            EXPECT_TRUE(checked->text().contains(QStringLiteral("尚未检查")));
            list->setCurrentItem(group);
            EXPECT_TRUE(priority->isVisible());
            EXPECT_FALSE(download->isVisible());
            EXPECT_FALSE(progress->isVisible());
            list->setCurrentItem(finals);
            EXPECT_TRUE(progress->isVisible());
            list->setCurrentItem(c04);
            server.release();
            ++stage;
            break;
        case 2: {
            if (catalog.downloading())
                break;
            EXPECT_EQ(list->currentItem(), c04);
            EXPECT_TRUE(badge->text().contains(QStringLiteral("未下载")));
            list->setCurrentItem(finals);
            EXPECT_TRUE(badge->text().contains(QStringLiteral("完整包")));
            EXPECT_FALSE(progress->isVisible());
            EXPECT_FALSE(cancel->isVisible());
            EXPECT_TRUE(checked->text().contains(QStringLiteral("检查成功")));
            EXPECT_TRUE(download->isEnabled());
            downloaded_at = catalog.inventory()[0].acquired_at;
            EXPECT_TRUE(downloaded_at);
            ASSERT_TRUE(pages);
            auto *overview = qobject_cast<QScrollArea *>(pages->widget(0));
            ASSERT_TRUE(overview);
            EXPECT_EQ(overview->horizontalScrollBar()->maximum(), 0);
            overview->ensureWidgetVisible(download);
            EXPECT_TRUE(overview->viewport()->rect().intersects(
                QRect(download->mapTo(overview->viewport(), QPoint()), download->size())));
            capture(dialog, "downloaded");
            pages->setCurrentIndex(2);
            capture(dialog, "finals-file");
            pages->setCurrentIndex(0);
            server.hold = false;
            catalog.start_download(server.url());
            ++stage;
            break;
        }
        case 3:
            if (catalog.downloading() || !checked->text().contains(QStringLiteral("无新内容")))
                break;
            EXPECT_EQ(catalog.received_bytes(), 0);
            EXPECT_EQ(catalog.inventory()[0].acquired_at, downloaded_at);
            capture(dialog, "unchanged");
            server.fail = true;
            catalog.start_download(server.url());
            ++stage;
            break;
        case 4:
            if (catalog.downloading() || !download->text().contains(QStringLiteral("重试")))
                break;
            EXPECT_TRUE(checked->text().contains(QStringLiteral("失败")));
            EXPECT_TRUE(status->isVisible());
            EXPECT_EQ(catalog.inventory()[0].acquired_at, downloaded_at);
            capture(dialog, "failed");
            list->setCurrentItem(c04);
            EXPECT_FALSE(status->isVisible());
            EXPECT_TRUE(finals->text(0).contains(QStringLiteral("检查失败")));
            list->setCurrentItem(utc);
            EXPECT_FALSE(status->isVisible());
            EXPECT_TRUE(download->isVisible());
            EXPECT_TRUE(checked->text().contains(QStringLiteral("尚未检查")));
            capture(dialog, "utc");
            pages->setCurrentIndex(2);
            capture(dialog, "utc-file");
            list->setCurrentItem(finals);
            server.fail = false;
            server.hold = true;
            catalog.start_download(server.url());
            ++stage;
            break;
        case 5:
            if (!cancel->isVisible())
                break;
            cancel->click();
            ++stage;
            break;
        case 6:
            if (catalog.downloading() || !checked->text().contains(QStringLiteral("取消")))
                break;
            EXPECT_EQ(catalog.update_status().state, DataUpdateState::cancelled);
            EXPECT_EQ(catalog.inventory()[0].acquired_at, downloaded_at);
            EXPECT_FALSE(cancel->isVisible());
            EXPECT_TRUE(download->isEnabled());
            capture(dialog, "cancelled");
            ++stage;
            timer.stop();
            dialog->reject();
            break;
        }
    });
    timer.start(25);
    show_data_center(nullptr, catalog);
    EXPECT_EQ(stage, 7);
}
TEST(DataCenterDialog, ResponsiveNavigationPreservesSelectionSettingsAndDownload) {
    (void)test_app();
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    QFile file(QString::fromStdWString(
        (std::filesystem::path(ORBITFORGE_TEST_FINALS).parent_path() / "finals2000A.data")
            .wstring()));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    DownloadServer server(file.readAll());
    EopCatalog catalog(root / "data", {ORBITFORGE_TEST_FINALS}, {}, false);
    QTimer timeout;
    QObject::connect(&timeout, &QTimer::timeout, [] {
        ADD_FAILURE() << "Responsive data center test timed out";
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            dialog->reject();
    });
    timeout.start(5000);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        const auto settle = [] {
            for (int i = 0; i < 12; ++i)
                QCoreApplication::processEvents();
        };
        const auto capture = [&](const QString &name) {
            settle();
            for (auto *scroll : dialog->findChildren<QScrollArea *>())
                if (scroll->isVisible())
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0) << name.toStdString();
            const auto output = qEnvironmentVariable("ORBITFORGE_RESPONSIVE_UI_CAPTURE");
            if (!output.isEmpty()) {
                ASSERT_TRUE(QDir().mkpath(output));
                EXPECT_TRUE(dialog->grab().save(output + "/data-" + name + ".png"));
            }
        };
        auto *list = dialog->findChild<QTreeWidget *>("dataInventory");
        auto *panel = dialog->findChild<QWidget *>("dataDetailPanel");
        auto *pages = dialog->findChild<QTabWidget *>("dataPages");
        auto *back = dialog->findChild<QPushButton *>("dataBackToList");
        ASSERT_TRUE(list && panel && pages && back);
        auto *finals = list->topLevelItem(0)->child(0);
        list->setCurrentItem(finals);
        pages->setCurrentIndex(1);
        auto *automatic = dialog->findChild<QCheckBox *>("dataAutoUpdate");
        auto *days = dialog->findChild<QSpinBox *>("eopExtrapolationDays");
        automatic->setChecked(false);
        dialog->findChild<QCheckBox *>("allowEopExtrapolation")->setChecked(true);
        days->setValue(5);
        dialog->resize(360, 620);
        settle();
        EXPECT_EQ(dialog->width(), 360);
        EXPECT_TRUE(list->isVisible());
        EXPECT_FALSE(panel->isVisible());
        capture("narrow-list");
        // Keyboard activation must offer the same navigation as clicking a row.
        list->itemActivated(finals, 0);
        settle();
        EXPECT_TRUE(panel->isVisible());
        EXPECT_FALSE(list->isVisible());
        EXPECT_TRUE(back->isVisible());
        EXPECT_EQ(pages->currentIndex(), 1);
        EXPECT_FALSE(automatic->isChecked());
        EXPECT_EQ(days->value(), 5);
        capture("narrow-settings");
        pages->setCurrentIndex(2);
        capture("narrow-file");
        back->click();
        EXPECT_TRUE(list->isVisible());
        EXPECT_EQ(list->currentItem(), finals);
        list->itemClicked(finals, 0);
        EXPECT_EQ(pages->currentIndex(), 2);
        for (int width : {1020, 360, 780, 1020}) {
            dialog->resize(width, 620);
            settle();
            EXPECT_EQ(dialog->width(), width);
            EXPECT_EQ(list->currentItem(), finals);
            EXPECT_EQ(pages->currentIndex(), 2);
            EXPECT_TRUE(panel->isVisible());
            EXPECT_EQ(list->isVisible(), width >= 800);
        }
        capture("wide-file");
        pages->setCurrentIndex(0);
        catalog.start_download(server.url());
        QElapsedTimer elapsed;
        elapsed.start();
        while (catalog.received_bytes() == 0 && elapsed.elapsed() < 1500)
            QCoreApplication::processEvents();
        EXPECT_GT(catalog.received_bytes(), 0);
        dialog->resize(360, 620);
        settle();
        // Refresh the download UI through its normal timer before checking the actions.
        auto *cancel = dialog->findChild<QPushButton *>("cancelEopDownload");
        while (!cancel->isVisible() && elapsed.elapsed() < 2500)
            QCoreApplication::processEvents();
        EXPECT_TRUE(catalog.downloading());
        EXPECT_TRUE(cancel->isVisible());
        auto *overview = qobject_cast<QScrollArea *>(pages->widget(0));
        settle();
        overview->ensureWidgetVisible(cancel);
        capture("narrow-download");
        EXPECT_TRUE(overview->viewport()->rect().contains(
            cancel->mapTo(overview->viewport(), cancel->rect().bottomRight())));
        cancel->click();
        while (catalog.downloading() && elapsed.elapsed() < 3000)
            QCoreApplication::processEvents();
        EXPECT_FALSE(catalog.downloading());
        EXPECT_EQ(catalog.update_status().state, DataUpdateState::cancelled);
        dialog->reject();
    });
    show_data_center(nullptr, catalog);
}
TEST(DataCenterDialog, GravityHasItsOwnGroupAndStaticValidityWithoutImplicitDownloads) {
    (void)test_app();
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    bool inspected = false;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        auto *tree = dialog->findChild<QTreeWidget *>("dataInventory");
        ASSERT_EQ(tree->topLevelItemCount(), 6);
        auto *gravity = tree->topLevelItem(2);
        ASSERT_EQ(gravity->childCount(), 3);
        tree->setCurrentItem(gravity->child(0));
        dialog->resize(1020, 720);
        QApplication::processEvents();
        EXPECT_EQ(dialog->findChild<QLabel *>("dataName")->text(), QStringLiteral("EGM2008"));
        EXPECT_FALSE(dialog->findChild<QCheckBox *>("dataAutoUpdate")->isChecked());
        EXPECT_TRUE(dialog->findChild<QPushButton *>("downloadEop")->isVisible());
        bool static_validity = false;
        for (const auto *label : dialog->findChildren<QLabel *>())
            static_validity |= label->text() == QStringLiteral("无固定有效期");
        EXPECT_TRUE(static_validity);
        const auto output = qEnvironmentVariable("ORBITFORGE_DATA_UI_CAPTURE");
        if (!output.isEmpty()) {
            QDir{}.mkpath(output);
            EXPECT_TRUE(dialog->grab().save(output + "/gravity-download.png"));
            dialog->resize(500, 660);
            tree->itemActivated(gravity->child(0), 0);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_TRUE(dialog->findChild<QLabel *>("dataName")->isVisible());
            EXPECT_TRUE(dialog->findChild<QPushButton *>("downloadEop")->isVisible());
            EXPECT_TRUE(dialog->grab().save(output + "/gravity-compact.png"));
        }
        EXPECT_FALSE(catalog.downloading());
        inspected = true;
        dialog->reject();
    });
    show_data_center(nullptr, catalog);
    EXPECT_TRUE(inspected);
}
TEST(DataCenterDialog, EphemerisGroupAndDetailsRemainReadableAtCompactWidth) {
    (void)test_app();
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *tree = dialog->findChild<QTreeWidget *>("dataInventory");
        ASSERT_EQ(tree->topLevelItemCount(), 6);
        auto *group = tree->topLevelItem(3);
        ASSERT_EQ(group->childCount(), 1);
        EXPECT_EQ(tree->currentItem(), group->child(0));
        dialog->resize(1020, 720);
        QApplication::processEvents();
        EXPECT_EQ(dialog->findChild<QLabel *>("dataName")->text(), QStringLiteral("DE440s"));
        EXPECT_TRUE(dialog->findChild<QPushButton *>("downloadEop")->isVisible());
        auto *centres = dialog->findChild<QPushButton *>("importPlanetaryCentres");
        ASSERT_TRUE(centres);
        EXPECT_TRUE(centres->isVisible());
        EXPECT_TRUE(centres->isEnabled());
        auto *tabs = dialog->findChild<QTabWidget *>("dataPages");
        tabs->setCurrentIndex(1);
        EXPECT_FALSE(dialog->findChild<QCheckBox *>("dataAutoUpdate")->isChecked());
        tabs->setCurrentIndex(2);
        EXPECT_TRUE(dialog->findChild<QPushButton *>("importEopSnapshot")->isVisible());
        EXPECT_EQ(dialog->findChild<QPushButton *>("importEopSnapshot")->text(),
                  QStringLiteral("导入星历数据包…"));
        tabs->setCurrentIndex(0);
        const auto output = qEnvironmentVariable("ORBITFORGE_DATA_UI_CAPTURE");
        if (!output.isEmpty()) {
            QDir{}.mkpath(output);
            EXPECT_TRUE(dialog->grab().save(output + "/ephemeris-wide.png"));
        }
        dialog->resize(430, 660);
        tree->itemActivated(group->child(0), 0);
        QEventLoop settled;
        QTimer::singleShot(60, &settled, &QEventLoop::quit);
        settled.exec();
        EXPECT_TRUE(dialog->findChild<QLabel *>("dataName")->isVisible());
        for (auto *scroll : dialog->findChildren<QScrollArea *>())
            if (scroll->isVisible())
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
        if (!output.isEmpty())
            EXPECT_TRUE(dialog->grab().save(output + "/ephemeris-compact.png"));
        EXPECT_TRUE(centres->isVisible());
        EXPECT_GE(centres->width(), centres->sizeHint().width());
        for (auto *ancestor = centres->parentWidget(); ancestor;
             ancestor = ancestor->parentWidget())
            if (auto *scroll = qobject_cast<QScrollArea *>(ancestor)) {
                scroll->ensureWidgetVisible(centres);
                break;
            }
        QApplication::processEvents();
        EXPECT_FALSE(centres->visibleRegion().isEmpty());
        if (!output.isEmpty()) {
            qInfo() << "ephemeris compact logical width" << dialog->width() << "DPR"
                    << dialog->devicePixelRatioF();
            EXPECT_TRUE(dialog->grab().save(output + "/ephemeris-compact-import.png"));
        }
        tabs->setCurrentIndex(2);
        QApplication::processEvents();
        auto *package = dialog->findChild<QPushButton *>("importEopSnapshot");
        for (auto *ancestor = package->parentWidget(); ancestor;
             ancestor = ancestor->parentWidget())
            if (auto *scroll = qobject_cast<QScrollArea *>(ancestor)) {
                scroll->ensureWidgetVisible(package);
                break;
            }
        QApplication::processEvents();
        EXPECT_FALSE(package->visibleRegion().isEmpty());
        if (!output.isEmpty())
            EXPECT_TRUE(dialog->grab().save(output + "/ephemeris-compact-package.png"));
        EXPECT_FALSE(catalog.downloading());
        dialog->reject();
    });
    show_data_center(nullptr, catalog, DataProduct::de440s);
}

QByteArray initial_fixture(const std::filesystem::path &path) {
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing initial data fixture");
    return file.readAll();
}
QByteArray initial_gravity() {
    return "stk.v.6.0\nModel Local\nDegree 21\nOrder 21\nGm 3.986004415e14\nRefDistance 6378136.3\n"
           "Normalized Yes\nIncludesPermTide No\nBEGIN Coefficients\n2 0 -0.000484 0\nEND "
           "Coefficients\n";
}
void finish_initial_cancel(EopCatalog &catalog) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (catalog.downloading() && elapsed.elapsed() < 5000)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_FALSE(catalog.downloading());
}
void capture_initial_setup(QDialog *dialog, const QString &name) {
    const auto output = qEnvironmentVariable("ORBITFORGE_DATA_UI_CAPTURE");
    if (!output.isEmpty()) {
        EXPECT_TRUE(QDir{}.mkpath(output));
        QEventLoop settled;
        QTimer::singleShot(60, &settled, &QEventLoop::quit);
        settled.exec();
        EXPECT_TRUE(dialog->grab().save(output + '/' + name + ".png"));
    }
}
TEST(DataCenterDialog, InitialSetupDownloadsInDependencyOrderSkipsInstalledAndRepairsMissingFiles) {
    (void)test_app();
    const auto expected = initial_data_products();
    ASSERT_GE(expected.size(), 4U);
    EXPECT_EQ(expected[0], DataProduct::utc);
    EXPECT_EQ(expected[1], DataProduct::c04);
    EXPECT_EQ(expected[2], DataProduct::finals2000a);
    EXPECT_EQ(expected[3], DataProduct::egm2008);
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    DownloadServer clock(initial_fixture(ORBITFORGE_TEST_TIME));
    DownloadServer finals(initial_fixture(
        std::filesystem::path(ORBITFORGE_TEST_FINALS).parent_path() / "finals2000A.data"));
    DownloadServer gravity(initial_gravity());
    clock.hold = finals.hold = gravity.hold = false;
    const std::vector required{DataProduct::utc, DataProduct::finals2000a,
                               DataProduct::custom_gravity};
    std::vector<DataProduct> requests;
    const auto download = [&](DataProduct product) {
        requests.push_back(product);
        if (product != DataProduct::utc)
            EXPECT_TRUE(catalog.has_full_download(DataProduct::utc));
        catalog.start_download(product == DataProduct::utc           ? clock.url()
                               : product == DataProduct::finals2000a ? finals.url()
                                                                     : gravity.url(),
                               product);
    };
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, [&] {
        ADD_FAILURE() << "Initial data setup timed out";
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            dialog->reject();
    });
    timeout.start(5000);
    EXPECT_TRUE(prepare_initial_data(nullptr, catalog, required, download));
    timeout.stop();
    EXPECT_EQ(requests, required);
    requests.clear();
    WindowShowObserver windows;
    EXPECT_TRUE(prepare_initial_data(nullptr, catalog, required, download));
    EXPECT_TRUE(windows.shown.empty());
    EXPECT_TRUE(requests.empty());
    const auto file = catalog.data().gravity_models().front().file;
    ASSERT_TRUE(QFile::remove(QString::fromStdWString(file.wstring())));
    EXPECT_FALSE(catalog.has_full_download(DataProduct::custom_gravity));
    timeout.start(5000);
    EXPECT_TRUE(prepare_initial_data(nullptr, catalog, required, download));
    timeout.stop();
    EXPECT_EQ(requests, std::vector{DataProduct::custom_gravity});
    EXPECT_TRUE(std::filesystem::is_regular_file(file));
}
TEST(DataCenterDialog, InitialSetupFailureStopsTheQueueAndRetryKeepsCompletedDownloads) {
    (void)test_app();
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    DownloadServer clock(initial_fixture(ORBITFORGE_TEST_TIME));
    DownloadServer finals(initial_fixture(
        std::filesystem::path(ORBITFORGE_TEST_FINALS).parent_path() / "finals2000A.data"));
    clock.hold = finals.hold = false;
    finals.fail = true;
    std::vector<DataProduct> requests;
    bool retried = false;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer inspect;
    QObject::connect(&inspect, &QTimer::timeout, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        if (elapsed.elapsed() > 5000) {
            ADD_FAILURE() << "Initial setup retry timed out";
            dialog->reject();
            return;
        }
        auto *retry = dialog->findChild<QPushButton *>("initialDataRetry");
        if (!retried && retry && retry->isVisible()) {
            retried = true;
            EXPECT_TRUE(catalog.has_full_download(DataProduct::utc));
            EXPECT_FALSE(catalog.has_full_download(DataProduct::finals2000a));
            EXPECT_EQ(requests.size(), 2U);
            EXPECT_TRUE(dialog->findChild<QLabel *>("initialDataStatus")->text().contains("503"));
            capture_initial_setup(dialog, "initial-data-failure");
            dialog->resize(400, 450);
            capture_initial_setup(dialog, "initial-data-compact");
            finals.fail = false;
            retry->click();
        }
    });
    inspect.start(25);
    const std::vector required{DataProduct::utc, DataProduct::finals2000a};
    EXPECT_TRUE(prepare_initial_data(nullptr, catalog, required, [&](auto product) {
        requests.push_back(product);
        catalog.start_download(product == DataProduct::utc ? clock.url() : finals.url(), product);
    }));
    inspect.stop();
    EXPECT_TRUE(retried);
    EXPECT_EQ(requests,
              (std::vector{DataProduct::utc, DataProduct::finals2000a, DataProduct::finals2000a}));
}
TEST(DataCenterDialog,
     InitialSetupCancelRetainsCompletedFilesAndNextLaunchOnlyDownloadsMissingItems) {
    (void)test_app();
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    DownloadServer clock(initial_fixture(ORBITFORGE_TEST_TIME));
    DownloadServer finals(initial_fixture(
        std::filesystem::path(ORBITFORGE_TEST_FINALS).parent_path() / "finals2000A.data"));
    clock.hold = false;
    std::vector<DataProduct> requests;
    const auto download = [&](DataProduct product) {
        requests.push_back(product);
        catalog.start_download(product == DataProduct::utc ? clock.url() : finals.url(), product);
    };
    QElapsedTimer elapsed;
    elapsed.start();
    bool cancelled = false;
    QTimer inspect;
    QObject::connect(&inspect, &QTimer::timeout, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        if (elapsed.elapsed() > 5000) {
            ADD_FAILURE() << "Initial setup cancel timed out";
            dialog->reject();
        } else if (!cancelled && catalog.has_full_download(DataProduct::utc) && finals.pending) {
            cancelled = true;
            capture_initial_setup(dialog, "initial-data-progress");
            dialog->findChild<QPushButton *>("initialDataCancel")->click();
        }
    });
    inspect.start(25);
    const std::vector required{DataProduct::utc, DataProduct::finals2000a};
    EXPECT_FALSE(prepare_initial_data(nullptr, catalog, required, download));
    inspect.stop();
    EXPECT_TRUE(cancelled);
    finish_initial_cancel(catalog);
    EXPECT_TRUE(catalog.has_full_download(DataProduct::utc));
    EXPECT_FALSE(catalog.has_full_download(DataProduct::finals2000a));
    requests.clear();
    finals.hold = false;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, [&] {
        ADD_FAILURE() << "Initial setup resume timed out";
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            dialog->reject();
    });
    timeout.start(5000);
    EXPECT_TRUE(prepare_initial_data(nullptr, catalog, required, download));
    timeout.stop();
    EXPECT_EQ(requests, std::vector{DataProduct::finals2000a});
}

TEST(DataCenterDialog, InitialSetupDefaultResourcesRemainReadableAtNarrowWidths) {
    (void)test_app();
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    DownloadServer source(initial_fixture(ORBITFORGE_TEST_TIME));
    QTimer::singleShot(120, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        for (auto product : initial_data_products())
            EXPECT_TRUE(dialog->findChild<QLabel *>(
                QStringLiteral("initialDataState%1").arg(static_cast<int>(product))));
        capture_initial_setup(dialog, "initial-data-all-resources");
        dialog->resize(400, 450);
        QEventLoop settled;
        QTimer::singleShot(60, &settled, &QEventLoop::quit);
        settled.exec();
        EXPECT_EQ(dialog->width(), 400);
        for (auto *scroll : dialog->findChildren<QScrollArea *>())
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
        capture_initial_setup(dialog, "initial-data-all-resources-compact");
        dialog->findChild<QPushButton *>("initialDataCancel")->click();
    });
    EXPECT_FALSE(prepare_initial_data(nullptr, catalog, initial_data_products(), [&](auto product) {
        catalog.start_download(source.url(), product);
    }));
    finish_initial_cancel(catalog);
}
TEST(DataCenterDialog, LiveInitialDataSetup) {
    if (qEnvironmentVariableIsEmpty("ORBITFORGE_LIVE_INITIAL_DATA_HOME"))
        GTEST_SKIP();
    (void)test_app();
    EopCatalog catalog(
        std::filesystem::path(
            qEnvironmentVariable("ORBITFORGE_LIVE_INITIAL_DATA_HOME").toStdWString()),
        {}, {}, false);
    const auto required = initial_data_products();
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer inspect;
    QObject::connect(&inspect, &QTimer::timeout, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        const auto active = catalog.active_product();
        if ((!catalog.downloading() &&
             catalog.update_status(active).state == DataUpdateState::failed) ||
            elapsed.elapsed() > 900000) {
            ADD_FAILURE() << catalog.status().toStdString();
            dialog->reject();
        }
    });
    inspect.start(100);
    EXPECT_TRUE(prepare_initial_data(nullptr, catalog));
    inspect.stop();
    for (auto product : required)
        EXPECT_TRUE(catalog.has_full_download(product));
    EXPECT_TRUE(prepare_initial_data(nullptr, catalog));
}
} // namespace

TEST(DataCenterDialog, OceanGroupShowsIndependentExtentAndLocalImportAtCompactWidth) {
    (void)test_app();
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *tree = dialog->findChild<QTreeWidget *>("dataInventory");
        ASSERT_EQ(tree->topLevelItemCount(), 6);
        auto *group = tree->topLevelItem(5);
        ASSERT_EQ(group->childCount(), 2);
        EXPECT_EQ(tree->currentItem(), group->child(0));
        EXPECT_EQ(dialog->findChild<QLabel *>("dataName")->text(),
                  QStringLiteral("FES2014bv1 海潮"));
        EXPECT_FALSE(catalog.automatic_updates(DataProduct::fes2014bv1));
        tree->setCurrentItem(group);
        EXPECT_EQ(dialog->findChild<QLabel *>("dataName")->text(), QStringLiteral("海潮模型"));
        tree->setCurrentItem(group->child(1));
        EXPECT_EQ(dialog->findChild<QLabel *>("dataName")->text(), QStringLiteral("外部海潮模型"));
        EXPECT_FALSE(dialog->findChild<QPushButton *>("downloadEop")->isVisible());
        dialog->resize(500, 660);
        tree->itemActivated(group->child(1), 0);
        QEventLoop settled;
        QTimer::singleShot(60, &settled, &QEventLoop::quit);
        settled.exec();
        for (auto *scroll : dialog->findChildren<QScrollArea *>())
            if (scroll->isVisible()) {
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            }
        const auto output = qEnvironmentVariable("ORBITFORGE_DATA_UI_CAPTURE");
        if (!output.isEmpty()) {
            EXPECT_TRUE(QDir{}.mkpath(output));
            EXPECT_TRUE(dialog->grab().save(output + "/ocean-compact.png"));
        }
        dialog->reject();
    });
    show_data_center(nullptr, catalog, DataProduct::fes2014bv1);
}
} // namespace orbitforge::desktop
