#include "initial_data_setup.hpp"
#include "eop_catalog.hpp"
#include "responsive_layout.hpp"
#include "workbench_theme.hpp"
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <orbitforge/runtime/ephemeris.hpp>

namespace orbitforge::desktop {
std::vector<DataProduct> initial_data_products() {
    // UTC must be ready before interpreting either EOP product.
    std::vector products{DataProduct::utc, DataProduct::c04, DataProduct::finals2000a,
                         DataProduct::egm2008};
    if (runtime::ephemeris_available())
        products.push_back(DataProduct::de440s);
    return products;
}
namespace {
QString product_title(DataProduct product) {
    switch (product) {
    case DataProduct::utc:
        return QStringLiteral("UTC / 闰秒表");
    case DataProduct::c04:
        return QStringLiteral("IERS C04 · 历史地球定向");
    case DataProduct::finals2000a:
        return QStringLiteral("IERS finals2000A · 近期与预测");
    case DataProduct::egm2008:
        return QStringLiteral("EGM2008 · 地球重力场（约 240 MiB）");
    case DataProduct::de440s:
        return QStringLiteral("DE440s · 太阳系星历（约 32 MiB）");
    case DataProduct::egm96:
        return QStringLiteral("EGM96 · 地球重力场");
    case DataProduct::custom_gravity:
        return QStringLiteral("外部重力场");
    }
    return {};
}
} // namespace
bool prepare_initial_data(QWidget *parent, EopCatalog &catalog,
                          const std::vector<DataProduct> &required,
                          const std::function<void(DataProduct)> &download) {
    const auto complete = [&] {
        return std::all_of(required.begin(), required.end(),
                           [&](auto product) { return catalog.has_full_download(product); });
    };
    if (complete())
        return true;
    QDialog dialog(parent);
    dialog.setObjectName("initialDataSetupDialog");
    dialog.setWindowTitle(QStringLiteral("OrbitForge · 准备基础数据"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(theme::space::page);
    layout->setSpacing(16);
    auto *heading = new QLabel(QStringLiteral("首次使用 · 准备基础数据"));
    heading->setObjectName("panelTitle");
    layout->addWidget(heading);
    auto *description =
        new QLabel(QStringLiteral("进入工作台前需要完整的时间、地球定向、重力与星历数据。"
                                  "程序会自动下载缺失文件并校验安装；已完成的文件下次直接使用。"
                                  "也可导入已下载的官方文件。"));
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget;
    auto *form = new QFormLayout(content);
    form->setContentsMargins(0, 8, 12, 8);
    form->setVerticalSpacing(12);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    std::vector<QLabel *> states;
    for (auto product : required) {
        auto *state = new QLabel;
        state->setObjectName(QStringLiteral("initialDataState%1").arg(static_cast<int>(product)));
        states.push_back(state);
        form->addRow(product_title(product), state);
    }
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    auto *status = new QLabel;
    status->setObjectName("initialDataStatus");
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *progress = new QProgressBar;
    progress->setObjectName("initialDataProgress");
    layout->addWidget(progress);
    auto *actions = new QHBoxLayout;
    auto *retry = new QPushButton(QStringLiteral("重试"));
    retry->setObjectName("initialDataRetry");
    retry->setProperty("primary", true);
    auto *manage = new QPushButton(QStringLiteral("导入已有文件…"));
    manage->setObjectName("initialDataImport");
    auto *cancel = new QPushButton(QStringLiteral("取消并退出"));
    cancel->setObjectName("initialDataCancel");
    actions->addWidget(retry);
    actions->addWidget(manage);
    actions->addStretch();
    actions->addWidget(cancel);
    layout->addLayout(actions);
    responsive_columns(&dialog, actions, 460);
    bool paused = false;
    std::optional<DataProduct> owned;
    const auto refresh = [&] {
        for (std::size_t i = 0; i < required.size(); ++i) {
            const auto product = required[i];
            const bool running = catalog.downloading() && catalog.active_product() == product;
            const auto state = catalog.update_status(product).state;
            states[i]->setText(catalog.has_full_download(product) ? QStringLiteral("已就绪")
                               : running                          ? QStringLiteral("下载 / 校验中")
                               : state == DataUpdateState::failed ? QStringLiteral("失败，待重试")
                                                                  : QStringLiteral("待下载"));
        }
        retry->setVisible(paused);
        manage->setEnabled(!catalog.downloading());
        progress->setVisible(catalog.downloading());
        if (catalog.downloading()) {
            const auto received = catalog.received_bytes(), total = catalog.total_bytes();
            if (total > 0 && received < total) {
                progress->setRange(0, 1000);
                progress->setValue(static_cast<int>(1000.0 * received / total));
            } else {
                progress->setRange(0, 0);
            }
            status->setText(
                QStringLiteral("%1\n%2 · 已接收 %3 MiB%4")
                    .arg(product_title(catalog.active_product()), catalog.status())
                    .arg(received / (1024.0 * 1024), 0, 'f', 1)
                    .arg(total > 0
                             ? QStringLiteral(" / %1 MiB").arg(total / (1024.0 * 1024), 0, 'f', 1)
                             : QString{}));
        } else if (paused) {
            status->setText(catalog.status() +
                            QStringLiteral("\n可重试或导入已有文件。已安装的数据会保留。"));
        } else {
            status->setText(QStringLiteral("正在准备下一项…"));
        }
    };
    const auto advance = [&] {
        if (owned && !catalog.downloading()) {
            paused = !catalog.has_full_download(*owned);
            owned.reset();
        }
        if (!paused && !catalog.downloading()) {
            const auto missing = std::find_if(required.begin(), required.end(), [&](auto product) {
                return !catalog.has_full_download(product);
            });
            if (missing == required.end()) {
                dialog.accept();
                return;
            }
            owned = *missing;
            if (download)
                download(*missing);
            else
                catalog.start_download({}, *missing);
        }
        refresh();
    };
    QObject::connect(retry, &QPushButton::clicked, &dialog, [&] {
        paused = false;
        advance();
    });
    QObject::connect(manage, &QPushButton::clicked, &dialog, [&] {
        show_data_center(&dialog, catalog);
        paused = false;
        advance();
    });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &dialog, advance);
    poll.start(100);
    refresh();
    QTimer::singleShot(0, &dialog, advance);
    const auto available = dialog.screen()->availableGeometry().size();
    dialog.resize(std::min(640, available.width() - 32), std::min(540, available.height() - 64));
    const bool accepted = dialog.exec() == QDialog::Accepted;
    poll.stop();
    if (owned && catalog.downloading() && catalog.active_product() == *owned)
        catalog.cancel_download();
    return accepted && complete();
}
} // namespace orbitforge::desktop
