#include "hpop_settings.hpp"
#include "eop_catalog.hpp"
#include "form_kit.hpp"
#include "quantity_editor.hpp"
#include "responsive_layout.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <orbitforge/runtime/ephemeris.hpp>

namespace orbitforge::desktop {
namespace {
constexpr int summary_role = Qt::UserRole + 1;
constexpr int heading_role = Qt::UserRole + 2;
class NavigationDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        return {160, index.data(heading_role).toBool() ? option.fontMetrics.height() + 14
                                                       : option.fontMetrics.height() * 2 + 18};
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        const QRect row = option.rect.adjusted(2, 1, -2, -1);
        auto font = option.font;
        painter->save();
        if (index.data(heading_role).toBool()) {
            font.setPixelSize(std::max(10, font.pixelSize() - 1));
            font.setLetterSpacing(QFont::AbsoluteSpacing, 1);
            painter->setFont(font);
            painter->setPen(theme::color(theme::text_muted));
            painter->drawText(row.adjusted(10, 6, -6, 0), Qt::AlignLeft | Qt::AlignVCenter,
                              index.data().toString());
            painter->restore();
            return;
        }
        const bool selected = option.state & QStyle::State_Selected;
        paint_list_row(painter, option, QRectF(row), theme::color(theme::accent));
        const QFontMetrics metrics(font);
        painter->setFont(font);
        painter->setPen(theme::color(selected ? theme::accent_bright : theme::text));
        painter->drawText(QRect(row.left() + 12, row.top() + 7, row.width() - 20, metrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, index.data().toString());
        font.setPixelSize(std::max(10, font.pixelSize() - 1));
        painter->setFont(font);
        const QFontMetrics small(font);
        const auto summary = index.data(summary_role).toString();
        painter->setPen(theme::color(summary.startsWith(QStringLiteral("未")) ||
                                             summary.startsWith(QStringLiteral("尚未"))
                                         ? theme::text_faint
                                         : theme::text_muted));
        painter->drawText(QRect(row.left() + 12, row.top() + 9 + metrics.height(), row.width() - 20,
                                small.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          small.elidedText(summary, Qt::ElideRight, row.width() - 20));
        painter->restore();
    }
};
} // namespace
bool edit_hpop_settings(
    core::HpopOptions &options, QWidget *parent, const std::vector<runtime::CatalogGravity> &models,
    const std::vector<runtime::CatalogEphemeris> &ephemerides,
    const std::function<std::vector<runtime::CatalogEphemeris>()> &manage_ephemerides,
    EopCatalog *catalog) {
    QPointer<EopCatalog> data = catalog;
    auto installed_models = data ? data->data().gravity_models() : models;
    if (data)
        std::erase_if(installed_models, [](const auto &entry) {
            std::error_code error;
            return !std::filesystem::is_regular_file(entry.file, error);
        });
    auto installed_ephemerides = data ? data->data().ephemerides() : ephemerides;
    QDialog dialog(parent);
    dialog.setObjectName("hpopSettingsDialog");
    dialog.setWindowTitle(QStringLiteral("HPOP · 数值传播"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(theme::space::page);
    layout->setSpacing(16);
    auto *heading = new QLabel(QStringLiteral("HPOP 轨道传播"));
    heading->setObjectName("panelTitle");
    layout->addWidget(heading);
    auto *subtitle = new QLabel(QStringLiteral("力模型、积分器与协方差设置"));
    subtitle->setObjectName("hint");
    layout->addWidget(subtitle);
    // A navigation list with live summaries replaces the two levels of tabs; the tab widgets
    // keep the pages (and their indices) and show their bars only in narrow windows.
    auto *columns = new QWidget;
    auto *body_layout = new QHBoxLayout(columns);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(16);
    auto *navigation = new QListWidget;
    navigation->setObjectName("hpopNavigation");
    navigation->setAccessibleName(QStringLiteral("HPOP 设置页面"));
    navigation->setItemDelegate(new NavigationDelegate(navigation));
    navigation->setFixedWidth(196);
    navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body_layout->addWidget(navigation);
    auto *tabs = new QTabWidget;
    tabs->setObjectName("hpopSettingsTabs");
    body_layout->addWidget(tabs, 1);
    layout->addWidget(columns, 1);
    auto *force_page = new QWidget;
    force_page->setObjectName("hpopForceModelsPage");
    auto *force_layout = new QVBoxLayout(force_page);
    force_layout->setContentsMargins(0, 0, 0, 0);
    auto *force_tabs = new QTabWidget;
    force_tabs->setObjectName("hpopForceModelTabs");
    force_layout->addWidget(force_tabs);
    tabs->addTab(force_page, QStringLiteral("力模型"));
    const auto page = [&](QTabWidget *owner, const QString &title, const char *name) {
        auto *scroll = new QScrollArea;
        scroll->setObjectName(name);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *content = new QWidget;
        auto *form = new QFormLayout(content);
        form->setContentsMargins(0, 4, 12, 8);
        form->setHorizontalSpacing(12);
        form->setVerticalSpacing(10);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        scroll->setWidget(content);
        owner->addTab(scroll, title);
        return form;
    };
    const auto note = [](QFormLayout *form, const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName("hint");
        label->setWordWrap(true);
        form->addRow(label);
    };
    const auto section = [](QFormLayout *parent_form, const QString &title, const char *name) {
        auto *group = new FormGroup(title, name);
        parent_form->addRow(group);
        return group->form();
    };
    const auto group_of = [](QFormLayout *form) -> FormGroup * {
        return form && form->parentWidget()
                   ? dynamic_cast<FormGroup *>(form->parentWidget()->parentWidget())
                   : nullptr;
    };
    // Explanations sit behind the group's info marker; limitations stay on the page.
    const auto help = [&](QFormLayout *form, const QString &text) {
        if (auto *group = group_of(form))
            group->add_help(text);
        else
            note(form, text);
    };
    const auto caution = [&](QFormLayout *form, const QString &text) {
        if (auto *group = group_of(form))
            group->add_caution(text);
        else
            form->addRow(caution_label(text));
    };
    auto *gravity_page = page(force_tabs, QStringLiteral("引力"), "hpopGravityPage");
    gravity_page->setVerticalSpacing(12);
    gravity_page->setContentsMargins(0, 4, 12, 8);
    auto *gravity =
        section(gravity_page, QStringLiteral("中心天体引力"), "hpopCentralGravitySection");
    auto *model = new QComboBox;
    model->setObjectName("hpopGravityModel");
    model->addItems({QStringLiteral("EGM96 · 内置示例 20 × 20"),
                     QStringLiteral("点质量 · 对照计算"), QStringLiteral("EGM2008"),
                     QStringLiteral("EGM96")});
    model->setItemData(0, QString{});
    model->setItemData(1, QString{});
    model->setItemData(2, QStringLiteral("egm2008"));
    model->setItemData(3, QStringLiteral("egm96"));
    const auto refresh_models = [&] {
        for (const auto &entry : installed_models) {
            const auto id = QString::fromStdString(entry.selection);
            const auto title = QString::fromStdString(entry.info.model) +
                               (entry.selection == "egm2008" || entry.selection == "egm96"
                                    ? QString{}
                                    : QStringLiteral(" · 外部文件"));
            const auto index = model->findData(id);
            if (index < 0)
                model->addItem(title, id);
            else
                model->setItemText(index, title);
        }
    };
    refresh_models();
    model->setCurrentIndex(options.gravity_degree == 0 ? 1 : 0);
    if (!options.gravity_model.empty()) {
        const auto id = QString::fromStdString(options.gravity_model);
        if (model->findData(id) < 0)
            model->addItem(id, id);
        model->setCurrentIndex(model->findData(id));
    }
    gravity->addRow(QStringLiteral("地球引力"), model);
    auto *degree = new QSpinBox;
    degree->setObjectName("hpopGravityDegree");
    degree->setRange(2, core::gravity_max_degree);
    degree->setValue(std::max(2, options.gravity_degree));
    auto *order = new QSpinBox;
    order->setObjectName("hpopGravityOrder");
    order->setRange(0, degree->value());
    order->setValue(options.gravity_order);
    // Truncation is read as one quantity ("8 × 8"), so degree and order share a row.
    degree->setAccessibleName(QStringLiteral("最大阶 Degree"));
    order->setAccessibleName(QStringLiteral("最大次 Order"));
    degree->setToolTip(QStringLiteral("最大阶 Degree"));
    order->setToolTip(QStringLiteral("最大次 Order"));
    auto *truncation = new QWidget;
    auto *truncation_layout = new QHBoxLayout(truncation);
    truncation_layout->setContentsMargins(0, 0, 0, 0);
    truncation_layout->setSpacing(8);
    truncation_layout->addWidget(degree, 1);
    auto *times = new QLabel(QStringLiteral("×"));
    times->setObjectName("muted");
    truncation_layout->addWidget(times);
    truncation_layout->addWidget(order, 1);
    truncation->setMaximumWidth(276);
    gravity->addRow(QStringLiteral("阶 × 次"), truncation);
    const auto gravity_limits = [&] {
        const auto id = model->currentData().toString();
        int max_degree = id.isEmpty() ? core::egm96_max_degree : core::gravity_max_degree;
        int max_order = id == QStringLiteral("egm2008") ? 2159 : max_degree;
        for (const auto &entry : installed_models)
            if (entry.selection == id.toStdString()) {
                max_degree = std::min(entry.info.degree, core::gravity_max_degree);
                max_order = std::min(entry.info.order, max_degree);
            }
        return std::pair{max_degree, max_order};
    };
    QObject::connect(degree, &QSpinBox::valueChanged, &dialog, [&](int value) {
        order->setMaximum(std::min(value, gravity_limits().second));
    });
    const auto update_gravity = [&] {
        degree->setEnabled(model->currentIndex() != 1);
        order->setEnabled(model->currentIndex() != 1);
        const auto [max_degree, max_order] = gravity_limits();
        degree->setMaximum(max_degree);
        order->setMaximum(std::min(degree->value(), max_order));
    };
    QObject::connect(model, &QComboBox::currentIndexChanged, &dialog, update_gravity);
    update_gravity();
    auto *gravity_status = new QLabel;
    gravity_status->setObjectName("hpopGravityStatus");
    gravity_status->setWordWrap(true);
    gravity->addRow(QString{}, gravity_status);
    auto *gravity_progress = new QProgressBar;
    gravity_progress->setObjectName("hpopGravityProgress");
    gravity_progress->hide();
    gravity->addRow(QString{}, gravity_progress);
    auto *gravity_actions = new QWidget;
    auto *gravity_action_layout = new QHBoxLayout(gravity_actions);
    gravity_action_layout->setContentsMargins(0, 0, 0, 0);
    auto *download_gravity = new QPushButton(QStringLiteral("下载重力场"));
    download_gravity->setObjectName("hpopDownloadGravity");
    auto *cancel_gravity = new QPushButton(QStringLiteral("取消下载"));
    cancel_gravity->setObjectName("hpopCancelGravityDownload");
    gravity_action_layout->addWidget(download_gravity);
    gravity_action_layout->addWidget(cancel_gravity);
    gravity_action_layout->addStretch();
    gravity->addRow(QString{}, gravity_actions);
    help(gravity, QStringLiteral(
                      "按文件实际范围选择截断阶 Degree × 次 Order；支持 EGM2008 完整 2190 × 2159。"
                      "次为 0 时只保留带谐项。外部重力场可在数据中心导入。"));
    const auto quantity = [](QFormLayout *form, const QString &label, const char *id, Unit unit,
                             double value, double low, double high) {
        auto *field = new QuantityEditor(unit);
        field->setObjectName(id);
        field->set_label(label);
        field->set_range(low, high, unit);
        field->set_value(value, unit);
        form->addRow(label, field);
        return field;
    };
    auto *secular_use = new QCheckBox(QStringLiteral("包含引力系数长期变化"));
    secular_use->setObjectName("hpopSecularVariations");
    secular_use->setChecked(options.include_secular_variations);
    secular_use->setToolTip(
        QStringLiteral("使用重力文件的参考历元与线性变化率，在各积分阶段求值。"));
    gravity->addRow(secular_use);
    auto *secular_status = new QLabel;
    secular_status->setObjectName("hpopSecularStatus");
    secular_status->setWordWrap(true);
    gravity->addRow(QString{}, secular_status);
    auto *solid = section(gravity_page, QStringLiteral("固体潮"), "hpopSolidTidesSection");
    auto *solid_mode = new QComboBox;
    solid_mode->setObjectName("hpopSolidTideMode");
    solid_mode->addItems(
        {QStringLiteral("无"), QStringLiteral("仅永久潮"), QStringLiteral("完整固体潮")});
    solid_mode->setCurrentIndex(static_cast<int>(options.solid_tides.mode));
    solid_mode->setItemData(
        2,
        QStringLiteral("Full：日月引起的 2、3 阶形变，以及诱导 4 阶项；可另启用频率修正与极潮。"),
        Qt::ToolTipRole);
    solid->addRow(QStringLiteral("模型"), solid_mode);
    auto *solid_k20 = new QLabel;
    solid_k20->setObjectName("hpopSolidTideK20");
    solid_k20->setWordWrap(true);
    solid_k20->setToolTip(
        QStringLiteral("读取所选重力文件中的实部 k20；未指定时采用 IERS 2003 默认值 0.30190。"));
    solid->addRow(QStringLiteral("Love 数 k20"), solid_k20);
    auto *solid_truncate = new QCheckBox(QStringLiteral("截断至重力场阶次"));
    solid_truncate->setObjectName("hpopSolidTideTruncate");
    solid_truncate->setChecked(options.solid_tides.truncate_to_gravity);
    solid->addRow(solid_truncate);
    auto *solid_time = new QCheckBox(QStringLiteral("包含附加时间项"));
    solid_time->setObjectName("hpopSolidTideTimeDependent");
    solid_time->setChecked(options.solid_tides.include_time_dependent_terms);
    solid_time->setToolTip(
        QStringLiteral("同时计算 71 个频率修正项和固体极潮，需要覆盖传播时段的 EOP 与时间数据。"));
    solid->addRow(solid_time);
    const auto refresh_solid = [solid_mode, solid_truncate, solid_time] {
        solid_truncate->setEnabled(solid_mode->currentIndex() != 0);
        solid_time->setEnabled(solid_mode->currentIndex() == 2);
    };
    QObject::connect(solid_mode, &QComboBox::currentIndexChanged, &dialog, refresh_solid);
    refresh_solid();
    help(solid,
         QStringLiteral("采用 IERS 2003 非弹性地球模型；永久潮按重力文件的潮汐系统计入一次。"
                        "完整固体潮独立使用所选星历中的太阳、月球位置与 GM，无需勾选第三体引力。"));
    caution(solid, QStringLiteral("最小振幅筛选待实现，当前包含全部潮汐分量。"));
    auto installed_oceans =
        data ? data->data().ocean_tide_models() : std::vector<runtime::CatalogOceanTides>{};
    auto *ocean = section(gravity_page, QStringLiteral("海潮"), "hpopOceanTidesSection");
    auto *ocean_use = new QCheckBox(QStringLiteral("使用海潮 Use Ocean Tides"));
    ocean_use->setObjectName("hpopUseOceanTides");
    ocean_use->setChecked(options.ocean_tides.enabled);
    ocean->addRow(ocean_use);
    auto *ocean_model = new QComboBox;
    ocean_model->setObjectName("hpopOceanTideModel");
    ocean_model->addItem(QStringLiteral("FES2014bv1"), QStringLiteral("fes2014bv1"));
    const auto refresh_ocean_models = [&] {
        const QSignalBlocker blocker(ocean_model);
        for (const auto &entry : installed_oceans) {
            const auto id = QString::fromStdString(entry.selection);
            if (entry.selection == "fes2014bv1")
                continue;
            const auto title =
                QString::fromStdString(entry.info.model) + QStringLiteral(" · 外部文件");
            const int index = ocean_model->findData(id);
            if (index < 0)
                ocean_model->addItem(title, id);
            else
                ocean_model->setItemText(index, title);
        }
    };
    refresh_ocean_models();
    const auto ocean_id = QString::fromStdString(options.ocean_tides.model);
    if (ocean_model->findData(ocean_id) < 0)
        ocean_model->addItem(ocean_id, ocean_id);
    ocean_model->setCurrentIndex(ocean_model->findData(ocean_id));
    ocean->addRow(QStringLiteral("模型"), ocean_model);
    auto *ocean_degree = new QSpinBox;
    ocean_degree->setObjectName("hpopOceanTideDegree");
    ocean_degree->setRange(0, core::gravity_max_degree);
    ocean_degree->setValue(options.ocean_tides.degree);
    auto *ocean_order = new QSpinBox;
    ocean_order->setObjectName("hpopOceanTideOrder");
    ocean_order->setRange(0, options.ocean_tides.degree);
    ocean_order->setValue(options.ocean_tides.order);
    ocean->addRow(QStringLiteral("最大阶 Degree"), ocean_degree);
    ocean->addRow(QStringLiteral("最大次 Order"), ocean_order);
    QObject::connect(ocean_degree, &QSpinBox::valueChanged, ocean_order, &QSpinBox::setMaximum);
    auto *ocean_status = new QLabel;
    ocean_status->setObjectName("hpopOceanTideStatus");
    ocean_status->setWordWrap(true);
    ocean->addRow(ocean_status);
    auto *ocean_progress = new QProgressBar;
    ocean_progress->setObjectName("hpopOceanTideProgress");
    ocean_progress->hide();
    ocean->addRow(ocean_progress);
    auto *ocean_download = new QPushButton(QStringLiteral("下载 / 重试"));
    ocean_download->setObjectName("hpopDownloadOceanTides");
    ocean->addRow(ocean_download);
    auto *ocean_cancel = new QPushButton(QStringLiteral("取消下载"));
    ocean_cancel->setObjectName("hpopCancelOceanDownload");
    ocean->addRow(ocean_cancel);
    auto *ocean_manage = new QPushButton(QStringLiteral("在数据中心管理海潮"));
    ocean_manage->setObjectName("hpopManageOceanTides");
    ocean_manage->setVisible(data != nullptr);
    ocean->addRow(ocean_manage);
    help(ocean, QStringLiteral("阶数和次数独立于中心引力设置；2 阶 1 次及以上自动包含海洋极潮。"
                               "0、1 阶不产生地心海潮引力。计算需要覆盖传播时段的 EOP。"));
    caution(ocean, QStringLiteral("当前使用文件中的显式分潮；次要分潮推算与最小振幅筛选待实现。"));
    auto *third = section(gravity_page, QStringLiteral("第三体引力"), "hpopThirdBodySection");
    third->setVerticalSpacing(8);
    auto *resource = new QFrame;
    resource->setObjectName("hpopResource");
    auto *resource_layout = new QVBoxLayout(resource);
    resource_layout->setContentsMargins(theme::space::card);
    resource_layout->setSpacing(6);
    auto *resource_header = new QHBoxLayout;
    auto *resource_title = new QLabel(options.ephemeris_model == "de440s"
                                          ? QStringLiteral("JPL DE440s")
                                          : QString::fromStdString(options.ephemeris_model));
    resource_title->setObjectName("hpopResourceTitle");
    auto *resource_badge = new QLabel;
    resource_badge->setObjectName("hpopResourceBadge");
    resource_header->addWidget(resource_title);
    resource_header->addStretch();
    resource_header->addWidget(resource_badge);
    resource_layout->addLayout(resource_header);
    auto *ephemeris_status = new QLabel;
    ephemeris_status->setObjectName("hpopEphemerisStatus");
    ephemeris_status->setWordWrap(true);
    resource_layout->addWidget(ephemeris_status);
    auto *manage = new QToolButton;
    manage->setObjectName("hpopManageEphemerides");
    manage->setText(QStringLiteral("在数据中心管理"));
    manage->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    manage->setIcon(workbench_icon(WorkbenchIcon::database));
    manage->setVisible(static_cast<bool>(manage_ephemerides));
    resource_layout->addWidget(manage, 0, Qt::AlignLeft);
    const auto refresh_ephemeris = [&] {
        const auto found =
            std::find_if(installed_ephemerides.begin(), installed_ephemerides.end(),
                         [&](const auto &v) { return v.selection == options.ephemeris_model; });
        const bool ready = found != installed_ephemerides.end();
        resource_badge->setText(ready ? QStringLiteral("已就绪") : QStringLiteral("未安装"));
        const auto date = [](core::Epoch epoch) {
            return QDateTime(QDate(2000, 1, 1), QTime(12, 0), QTimeZone::UTC)
                .addSecs(epoch.tai_nanoseconds() / 1000000000LL)
                .toString("yyyy-MM-dd");
        };
        ephemeris_status->setText(
            ready ? QStringLiteral("%1 — %2 TAI\n日月与行星共用此星历")
                        .arg(date(found->info.first), date(found->info.last))
                  : QStringLiteral("完整星历约 32 MiB，支持 1850—2150 "
                                   "年。\n启用第三体引力前，请在数据中心下载。"));
    };
    QObject::connect(manage, &QToolButton::clicked, &dialog, [&] {
        try {
            installed_ephemerides = manage_ephemerides();
            refresh_ephemeris();
        } catch (const std::exception &e) {
            ephemeris_status->setText(QString::fromUtf8(e.what()));
        }
    });
    refresh_ephemeris();
    third->addRow(resource);
    struct ManualDraft {
        QString text;
        Unit unit;
        std::optional<double> si;
    };
    struct BodyFields {
        QCheckBox *enabled;
        QComboBox *source;
        QuantityEditor *mu;
        std::shared_ptr<ManualDraft> manual;
        std::shared_ptr<std::optional<core::CbGravity>> cb;
        QString default_manual;
        std::function<void()> update;
    };
    auto *search = new QLineEdit;
    search->setObjectName("hpopThirdBodySearch");
    search->setPlaceholderText(QStringLiteral("搜索天体名称…"));
    third->addRow(search);
    auto *table = new QTableWidget(static_cast<int>(core::third_body_count), 4);
    table->setObjectName("hpopThirdBodyTable");
    table->setHorizontalHeaderLabels({QStringLiteral("天体 Name"), QStringLiteral("使用"),
                                      QStringLiteral("来源 Source"),
                                      QStringLiteral("引力参数 GM")});
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(36);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setMinimumHeight(200);
    table->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table->horizontalHeader()->setMinimumSectionSize(36);
    table->setColumnWidth(0, 110);
    table->setColumnWidth(1, 48);
    table->setColumnWidth(2, 140);
    table->setColumnWidth(3, 220);
    table->horizontalHeader()->setStretchLastSection(true);
    // Table scrolls internally at narrow widths; fields stay alive across resizing.
    third->addRow(table);
    constexpr std::array names{"太阳", "月球", "木星",   "金星",   "土星",
                               "火星", "水星", "天王星", "海王星", "冥王星"};
    core::ThirdBodyDataInfo constants;
    constants.sun_mu = runtime::de440_sun_mu;
    constants.moon_mu = runtime::de440_moon_mu;
    constants.planet_mu = runtime::de440_planet_mu;
    std::array<BodyFields, core::third_body_count> fields;
    for (auto body : core::third_body_list) {
        const auto row = static_cast<int>(body);
        const auto english = QString::fromUtf8(core::third_body_name(body).data());
        const auto title = QString::fromUtf8(names[row]);
        auto *name = new QTableWidgetItem(title + " " + english);
        name->setData(Qt::UserRole, title + " " + english);
        name->setToolTip(core::third_body_naif_id(body) < 10
                             ? QStringLiteral("系统质心位置与系统 GM，包含卫星质量")
                             : QStringLiteral("天体中心位置与天体 GM"));
        table->setItem(row, 0, name);
        const auto &settings = options.third_body(body);
        auto *check = new QCheckBox;
        check->setObjectName("hpop" + english + "Gravity");
        check->setAccessibleName(title + QStringLiteral("引力"));
        check->setChecked(settings.enabled);
        auto *check_cell = new QWidget;
        auto *check_layout = new QHBoxLayout(check_cell);
        check_layout->setContentsMargins(0, 0, 0, 0);
        check_layout->addWidget(check, 0, Qt::AlignCenter);
        table->setCellWidget(row, 1, check_cell);
        auto *source = new QComboBox;
        source->setObjectName("hpop" + english + "Source");
        source->addItems({QStringLiteral("Cb file"), QStringLiteral("JPL DE"),
                          QStringLiteral("User Specified")});
        source->setMinimumWidth(0);
        source->setStyleSheet(QStringLiteral(
            "QComboBox { padding: 4px 5px; } QComboBox::drop-down { width: 18px; }"));
        source->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        source->setMinimumContentsLength(3);
        source->setCurrentIndex(static_cast<int>(settings.source));
        table->setCellWidget(row, 2, source);
        auto *mu = new QuantityEditor(Unit::kilometres_cubed_per_second_squared);
        mu->setObjectName("hpop" + english + "Mu");
        mu->set_label(title + QStringLiteral(" GM"));
        mu->setMaximumWidth(QWIDGETSIZE_MAX);
        mu->set_range(std::numeric_limits<double>::min(), std::numeric_limits<double>::max(),
                      Unit::metres_cubed_per_second_squared);
        mu->set_value(settings.user_mu.value_or(constants.mu(body)),
                      Unit::metres_cubed_per_second_squared);
        auto *input = mu->findChild<QLineEdit *>("quantityValue");
        auto manual = std::make_shared<ManualDraft>(
            ManualDraft{input->text(), mu->unit(), settings.user_mu.value_or(constants.mu(body))});
        auto cb = std::make_shared<std::optional<core::CbGravity>>(settings.cb);
        const auto update = [=] {
            source->setEnabled(check->isChecked());
            mu->setEnabled(check->isChecked());
            mu->set_read_only(source->currentIndex() != 2);
            if (source->currentIndex() == 1)
                mu->set_value(constants.mu(body), Unit::metres_cubed_per_second_squared);
            else if (source->currentIndex() == 0) {
                if (*cb)
                    mu->set_value((*cb)->mu, Unit::metres_cubed_per_second_squared);
                else
                    input->setText(QStringLiteral("待导入"));
            }
            if (source->currentIndex() != 2)
                input->setCursorPosition(0);
            mu->setToolTip(
                source->currentIndex() == 0 && *cb
                    ? QString::fromStdString((*cb)->name + "\nSHA-256: " + (*cb)->content_sha256)
                    : QStringLiteral("JPL DE 使用 DE440 配套常数；自定义值必须大于零"));
        };
        auto previous = std::make_shared<int>(source->currentIndex());
        QObject::connect(source, &QComboBox::currentIndexChanged, &dialog, [=](int index) {
            if (*previous == 2) {
                manual->text = input->text();
                manual->unit = mu->unit();
                try {
                    manual->si = mu->value(Unit::metres_cubed_per_second_squared);
                } catch (const std::exception &) {
                    manual->si.reset();
                }
            }
            if (index == 2) {
                // Reset an unavailable Cb display before converting units. Retain both
                // the exact SI value and literal text, including unfinished manual input.
                mu->set_value(manual->si.value_or(constants.mu(body)),
                              Unit::metres_cubed_per_second_squared);
                mu->set_unit(manual->unit);
                if (manual->si) {
                    const QSignalBlocker blocked(input);
                    input->setText(manual->text);
                } else
                    input->setText(manual->text);
            }
            *previous = index;
            update();
        });
        QObject::connect(check, &QCheckBox::toggled, &dialog, [=] { update(); });
        table->setCellWidget(row, 3, mu);
        fields[row] = {check, source, mu, manual, cb, manual->text, update};
        update();
    }
    const auto resize_columns = [=] {
        const bool compact = table->viewport()->width() < 520;
        const QSignalBlocker blocked(table->horizontalHeader());
        table->setColumnWidth(0, compact ? 54 : 110);
        table->setColumnWidth(1, 40);
        table->setColumnWidth(2, compact ? 92 : 140);
        table->setColumnWidth(3, compact ? 182 : 220);
        table->horizontalHeaderItem(0)->setText(compact ? QStringLiteral("天体")
                                                        : QStringLiteral("天体 Name"));
        table->horizontalHeaderItem(2)->setText(compact ? QStringLiteral("来源")
                                                        : QStringLiteral("来源 Source"));
        for (int row = 0; row < table->rowCount(); ++row) {
            table->item(row, 0)->setText(compact
                                             ? QString::fromUtf8(names[row])
                                             : table->item(row, 0)->data(Qt::UserRole).toString());
            fields[row].source->setItemText(2, compact ? QStringLiteral("自定义")
                                                       : QStringLiteral("User Specified"));
            fields[row].source->setToolTip(QStringLiteral("Cb file / JPL DE / User Specified"));
        }
    };
    QObject::connect(table->horizontalHeader(), &QHeaderView::geometriesChanged, &dialog,
                     resize_columns);
    resize_columns();
    QObject::connect(search, &QLineEdit::textChanged, &dialog, [=](const QString &text) {
        for (int row = 0; row < table->rowCount(); ++row)
            table->setRowHidden(row, !table->item(row, 0)
                                          ->data(Qt::UserRole)
                                          .toString()
                                          .contains(text.trimmed(), Qt::CaseInsensitive));
    });
    auto *cb_controls = new QWidget;
    auto *cb_layout = new QFormLayout(cb_controls);
    cb_layout->setContentsMargins(0, 0, 0, 0);
    cb_layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto *import_cb = new QToolButton;
    import_cb->setObjectName("hpopImportCb");
    import_cb->setText(QStringLiteral("导入所选天体 Cb 文件…"));
    auto *cb_units = new QComboBox;
    cb_units->setObjectName("hpopCbUnits");
    cb_units->addItems({QStringLiteral("文件 GM：m³/s²"), QStringLiteral("文件 GM：km³/s²")});
    cb_layout->addRow(import_cb, cb_units);
    third->addRow(cb_controls);
    auto *cb_status = new QLabel;
    cb_status->setObjectName("hpopCbStatus");
    cb_status->setWordWrap(true);
    third->addRow(cb_status);
    QObject::connect(import_cb, &QToolButton::clicked, &dialog, [&] {
        const auto row = table->currentRow();
        if (row < 0) {
            cb_status->setText(QStringLiteral("请先选择天体行。"));
            return;
        }
        const auto path = QFileDialog::getOpenFileName(&dialog, QStringLiteral("导入天体引力常数"),
                                                       {}, QStringLiteral("Central body (*.cb)"));
        if (path.isEmpty())
            return;
        try {
            auto value = runtime::load_cb_gravity(std::filesystem::path(path.toStdWString()),
                                                  core::third_body_list[row],
                                                  cb_units->currentIndex() == 0 ? 1.0 : 1e9);
            *fields[row].cb = std::move(value);
            fields[row].source->setCurrentIndex(0);
            fields[row].update();
            cb_status->setText(
                QStringLiteral("已导入 %1").arg(QString::fromStdString((*fields[row].cb)->name)));
        } catch (const std::exception &e) {
            cb_status->setText(QString::fromUtf8(e.what()));
        }
    });
    table->selectRow(0);
    help(third,
         QStringLiteral(
             "位置星历：DE440s。Cb file 读取文件中的 Gm / SystemGm；火星与外行星使用系统质心。"));
    const auto pending = [&](QTabWidget *owner, const QString &title, const char *name,
                             const QString &description, const QString &scope) {
        auto *form = page(owner, title, name);
        auto *status = section(form, title, (std::string(name) + "Status").c_str());
        auto *badge = new QLabel(QStringLiteral("尚未实现"));
        badge->setObjectName("hpopPendingBadge");
        status->addRow(badge);
        note(status, description);
        note(status, scope);
        note(form, QStringLiteral("当前传播不包含此项，暂不可启用。"));
    };
    auto *drag_page = page(force_tabs, QStringLiteral("阻力"), "hpopDragPage");
    auto *drag_use = new QCheckBox(QStringLiteral("使用大气阻力 Use Drag"));
    drag_use->setObjectName("hpopUseDrag");
    drag_use->setChecked(options.drag.enabled);
    make_switch(drag_use);
    drag_page->addRow(drag_use);
    auto *drag_off =
        new QLabel(QStringLiteral("未启用。开启后设置阻力系数、大气密度、太阳通量与地磁。"));
    drag_off->setObjectName("hint");
    drag_off->setWordWrap(true);
    drag_page->addRow(drag_off);
    auto *drag_model = section(drag_page, QStringLiteral("球形阻力模型"), "hpopDragModelSection");
    auto *drag_type = new QComboBox;
    drag_type->setObjectName("hpopDragType");
    drag_type->addItem(QStringLiteral("Spherical · 球形"));
    drag_model->addRow(QStringLiteral("类型"), drag_type);
    auto *drag_cd = quantity(drag_model, QStringLiteral("阻力系数 Cd"), "hpopDragCd", Unit::one,
                             options.drag.cd, -100, 100);
    auto *drag_area =
        quantity(drag_model, QStringLiteral("面积 / 质量"), "hpopDragAreaMassRatio",
                 Unit::square_metres_per_kilogram, options.drag.area_mass_ratio_m2_per_kg, 0, 9999);
    auto *drag_compatible_area = new QLabel;
    drag_compatible_area->setObjectName("hpopDragCompatibleArea");
    drag_compatible_area->setWordWrap(true);
    drag_model->addRow(QStringLiteral("对应面积"), drag_compatible_area);
    auto *drag_density = section(drag_page, QStringLiteral("大气密度"), "hpopDragDensitySection");
    auto *density_model = new QComboBox;
    density_model->setObjectName("hpopDragDensityModel");
    density_model->addItems({QStringLiteral("Jacchia–Roberts"), QStringLiteral("NRLMSISE-00")});
    density_model->setCurrentIndex(static_cast<int>(options.drag.atmosphere));
    drag_density->addRow(QStringLiteral("密度模型"), density_model);
    auto *low_density = new QComboBox;
    low_density->setObjectName("hpopDragLowAltitudeModel");
    low_density->addItems({QStringLiteral("None · 关闭"), QStringLiteral("NRLMSISE-00")});
    low_density->setCurrentIndex(static_cast<int>(options.drag.low_altitude_model));
    drag_density->addRow(QStringLiteral("低空密度模型"), low_density);
    auto *drag_blend = quantity(drag_density, QStringLiteral("拼接范围"), "hpopDragBlendingRange",
                                Unit::metre, options.drag.blending_range_m, 0, 1000000);
    drag_blend->set_unit(Unit::kilometre);
    help(drag_density, QStringLiteral("Jacchia–Roberts：90–2500 km；NRLMSISE-00：0–1000 km。"
                                      "低空模型在 90 km 以上的指定范围内平滑拼接。"));
    auto *drag_weather =
        section(drag_page, QStringLiteral("太阳通量 / 地磁"), "hpopDragWeatherSection");
    auto *drag_weather_source = new QComboBox;
    drag_weather_source->setObjectName("hpopDragWeatherSource");
    drag_weather_source->addItem(QStringLiteral("Enter Manually · 手动常量"));
    drag_weather_source->addItem(QStringLiteral("Use File · 数据中心文件"));
    drag_weather_source->setCurrentIndex(options.drag.weather_model.empty() ? 0 : 1);
    drag_weather->addRow(QStringLiteral("输入来源"), drag_weather_source);
    auto *drag_f107 = quantity(drag_weather, QStringLiteral("日 F10.7 (sfu)"), "hpopDragF107",
                               Unit::one, options.drag.f107, 1e-9, 1000);
    auto *drag_average =
        quantity(drag_weather, QStringLiteral("平均 F10.7 (sfu)"), "hpopDragF107Average", Unit::one,
                 options.drag.f107_average, 1e-9, 1000);
    auto *drag_kp = quantity(drag_weather, QStringLiteral("地磁指数 Kp"), "hpopDragKp", Unit::one,
                             options.drag.kp, 0, 9);
    auto *weather_file = new QComboBox;
    weather_file->setObjectName("hpopDragWeatherFile");
    std::function<void()> refresh_weather_policy;
    const auto refresh_weather_files = [&] {
        const auto selected = weather_file->currentData().toString();
        const auto id = selected.isEmpty()
                            ? QString::fromStdString(options.drag.weather_model.empty()
                                                         ? "celestrak"
                                                         : options.drag.weather_model)
                            : selected;
        const QSignalBlocker blocker(weather_file);
        weather_file->clear();
        if (data)
            for (const auto &entry : data->data().weather_sources()) {
                weather_file->addItem(QString::fromStdString(entry.selection),
                                      QString::fromStdString(entry.selection));
                weather_file->setItemData(weather_file->count() - 1, entry.info.cssi_predictions,
                                          Qt::UserRole + 1);
            }
        if (weather_file->findData(id) < 0)
            weather_file->addItem(id + QStringLiteral(" · 数据未就绪"), id);
        weather_file->setCurrentIndex(weather_file->findData(id));
        if (refresh_weather_policy)
            refresh_weather_policy();
    };
    refresh_weather_files();
    drag_weather->addRow(QStringLiteral("天气文件"), weather_file);
    auto *weather_update = new QComboBox;
    weather_update->setObjectName("hpopDragGeomagneticUpdate");
    weather_update->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    weather_update->setMinimumContentsLength(18);
    weather_update->addItems({QStringLiteral("Daily · 每日"), QStringLiteral("3-Hourly · 三小时"),
                              QStringLiteral("3-Hourly Interp · 保持均值"),
                              QStringLiteral("3-Hourly Cubic Spline · 自然三次样条")});
    weather_update->setToolTip(QStringLiteral(
        "3-Hourly Interp：插值保持每个三小时窗口的 Ap/Kp 均值及有效范围。"
        "3-Hourly Cubic Spline：自然三次样条以三小时窗口中心为节点；缺测不外推，越界时报错。"
        "NRLMSISE 仍使用未插值的三小时 Ap 历史。"));
    weather_update->setCurrentIndex(static_cast<int>(options.drag.geomagnetic_update));
    drag_weather->addRow(QStringLiteral("地磁更新率"), weather_update);
    auto *weather_source = new QComboBox;
    weather_source->setObjectName("hpopDragGeomagneticSource");
    weather_source->addItems({QStringLiteral("Ap"), QStringLiteral("Kp")});
    weather_source->setCurrentIndex(static_cast<int>(options.drag.geomagnetic_source));
    drag_weather->addRow(QStringLiteral("地磁数据来源"), weather_source);
    refresh_weather_policy = [&] {
        const bool enabled = drag_use->isChecked() && drag_weather_source->currentIndex() == 1;
        const bool ap_only = enabled && weather_file->currentData(Qt::UserRole + 1).toBool();
        if (ap_only)
            weather_source->setCurrentIndex(0);
        weather_source->setEnabled(enabled && !ap_only);
        weather_source->setToolTip(ap_only ? QStringLiteral("CSSI Predicts 使用名义月预报和 Ap。")
                                           : QString{});
    };
    auto *weather_manage = new QPushButton(QStringLiteral("管理空间天气…"));
    weather_manage->setObjectName("hpopDragManageWeather");
    weather_manage->setVisible(data != nullptr);
    drag_weather->addRow(QString{}, weather_manage);
    QObject::connect(weather_manage, &QPushButton::clicked, &dialog, [&] {
        if (data)
            show_data_center(&dialog, *data, DataProduct::space_weather);
        refresh_weather_files();
    });
    help(drag_weather,
         QStringLiteral("手动模式使用常量。文件模式使用观测 F10.7、居中 81 日均值，"
                        "按 UTC 和模型滞后取样；月预报按节点插值，CSSI Predicts 使用名义值和 Ap。"
                        "NRLMSISE 三小时模式包含历史 Ap。缺值或覆盖不足时停止计算。"));
    auto *drag_sun =
        section(drag_page, QStringLiteral("高度、太阳位置与计算数据"), "hpopDragSunSection");
    auto *drag_approximate = new QCheckBox(QStringLiteral("使用近似高度"));
    drag_approximate->setObjectName("hpopDragApproximateAltitude");
    drag_approximate->setChecked(options.drag.use_approximate_altitude);
    drag_approximate->setToolTip(
        QStringLiteral("密度和低空拼接使用相对 WGS84 椭球的径向高度；关闭时使用精确大地高。"));
    drag_sun->addRow(QString{}, drag_approximate);
    auto *drag_apparent = new QCheckBox(QStringLiteral("使用地心视太阳位置 (LT+S)"));
    drag_apparent->setObjectName("hpopDragApparentSun");
    drag_apparent->setChecked(options.drag.use_apparent_sun_position);
    drag_sun->addRow(QString{}, drag_apparent);
    auto *drag_resource = new QLabel;
    drag_resource->setObjectName("hpopDragEphemerisStatus");
    drag_resource->setWordWrap(true);
    drag_sun->addRow(QString{}, drag_resource);
    const auto refresh_drag_resource = [&] {
        const bool available = std::any_of(
            installed_ephemerides.begin(), installed_ephemerides.end(),
            [&](const auto &entry) { return entry.selection == options.ephemeris_model; });
        if (auto *group = group_of(drag_sun)) {
            group->set_summary(available ? QStringLiteral("星历就绪")
                                         : QStringLiteral("星历未就绪"));
            if (!available)
                group->set_expanded(true);
        }
        drag_resource->setText(available
                                   ? QStringLiteral("位置星历：%1")
                                         .arg(QString::fromStdString(options.ephemeris_model))
                                   : QStringLiteral("所选太阳位置星历未就绪，请在数据中心准备。"));
    };
    refresh_drag_resource();
    auto *drag_manage = new QPushButton(QStringLiteral("管理太阳系星历…"));
    drag_manage->setObjectName("hpopDragManageEphemerides");
    drag_manage->setVisible(static_cast<bool>(manage_ephemerides));
    drag_sun->addRow(QString{}, drag_manage);
    std::function<void()> refresh_srp_resource;
    QObject::connect(drag_manage, &QPushButton::clicked, &dialog, [&] {
        if (manage_ephemerides) {
            installed_ephemerides = manage_ephemerides();
            refresh_ephemeris();
            refresh_drag_resource();
            if (refresh_srp_resource)
                refresh_srp_resource();
        }
    });
    help(drag_sun, QStringLiteral("阻力需要覆盖传播区间的 UTC、EOP 和太阳星历。启用视太阳后，星历"
                                  "还须覆盖起点之前 600 秒；关闭时使用几何太阳。"));
    if (auto *group = group_of(drag_sun))
        group->set_collapsible(true);
    const auto update_drag = [&] {
        const bool enabled = drag_use->isChecked();
        // A switched-off model folds away to one line instead of a page of inactive fields.
        for (auto *form : {drag_model, drag_density, drag_weather, drag_sun}) {
            form->parentWidget()->setEnabled(enabled);
            group_of(form)->setVisible(enabled);
        }
        drag_off->setVisible(!enabled);
        const bool file_weather = drag_weather_source->currentIndex() == 1;
        for (auto *input : {drag_f107, drag_average, drag_kp}) {
            input->setEnabled(enabled && !file_weather);
            drag_weather->setRowVisible(input, !file_weather);
        }
        for (auto *input : {weather_file, weather_update, weather_source}) {
            input->setEnabled(enabled && file_weather);
            drag_weather->setRowVisible(input, file_weather);
        }
        refresh_weather_policy();
        weather_manage->setEnabled(enabled && file_weather && data);
        drag_weather->setRowVisible(weather_manage, file_weather && data);
        const bool jr = density_model->currentIndex() == 0;
        low_density->setEnabled(enabled && jr);
        drag_blend->setEnabled(enabled && jr && low_density->currentIndex() == 1);
        refresh_drag_resource();
    };
    QObject::connect(drag_use, &QCheckBox::toggled, &dialog, update_drag);
    QObject::connect(density_model, &QComboBox::currentIndexChanged, &dialog, update_drag);
    QObject::connect(low_density, &QComboBox::currentIndexChanged, &dialog, update_drag);
    QObject::connect(drag_weather_source, &QComboBox::currentIndexChanged, &dialog, update_drag);
    QObject::connect(weather_file, &QComboBox::currentIndexChanged, &dialog, update_drag);
    update_drag();
    auto *srp_page = page(force_tabs, QStringLiteral("太阳辐射压"), "hpopSrpPage");
    auto *srp_use = new QCheckBox(QStringLiteral("使用太阳辐射压 Use SRP"));
    srp_use->setObjectName("hpopUseSrp");
    srp_use->setChecked(options.srp.enabled);
    make_switch(srp_use);
    srp_page->addRow(srp_use);
    auto *srp_off = new QLabel(QStringLiteral("未启用。开启后设置光压系数、阴影模型与太阳常数。"));
    srp_off->setObjectName("hint");
    srp_off->setWordWrap(true);
    srp_page->addRow(srp_off);
    auto *srp_model = section(srp_page, QStringLiteral("球形光压模型"), "hpopSrpModelSection");
    auto *srp_type = new QComboBox;
    srp_type->setObjectName("hpopSrpType");
    srp_type->addItem(QStringLiteral("Spherical · 球形"));
    srp_model->addRow(QStringLiteral("类型"), srp_type);
    auto *srp_cr = quantity(srp_model, QStringLiteral("光压系数 Cr"), "hpopSrpCr", Unit::one,
                            options.srp.cr, -100, 100);
    auto *srp_area =
        quantity(srp_model, QStringLiteral("面积 / 质量"), "hpopSrpAreaMassRatio",
                 Unit::square_metres_per_kilogram, options.srp.area_mass_ratio_m2_per_kg, 0, 9999);
    auto *srp_compatible_area = new QLabel;
    srp_compatible_area->setObjectName("hpopSrpCompatibleArea");
    srp_compatible_area->setWordWrap(true);
    srp_model->addRow(QStringLiteral("对应面积"), srp_compatible_area);
    auto *srp_shadow_section = section(srp_page, QStringLiteral("阴影"), "hpopSrpShadowSection");
    auto *srp_shadow = new QComboBox;
    srp_shadow->setObjectName("hpopSrpShadowModel");
    srp_shadow->addItems({QStringLiteral("None · 无阴影"), QStringLiteral("Cylindrical · 圆柱"),
                          QStringLiteral("Dual Cone · 双锥")});
    srp_shadow->setCurrentIndex(static_cast<int>(options.srp.shadow_model));
    srp_shadow_section->addRow(QStringLiteral("阴影模型"), srp_shadow);
    auto *srp_boundary = new QCheckBox(QStringLiteral("阴影边界修正"));
    srp_boundary->setObjectName("hpopSrpBoundaryMitigation");
    srp_boundary->setChecked(options.srp.boundary_mitigation);
    srp_boundary->setToolTip(QStringLiteral("Use Boundary Mitigation：在入影、出影处切分积分步，"
                                            "降低跨越阴影边界的计算误差。"));
    srp_shadow_section->addRow(QString{}, srp_boundary);
    auto srp_assignments = options.srp;
    auto *srp_assigned_summary = new QLabel;
    srp_assigned_summary->setObjectName("hpopSrpEclipsingSummary");
    srp_assigned_summary->setWordWrap(true);
    srp_shadow_section->addRow(QStringLiteral("遮挡天体"), srp_assigned_summary);
    auto *srp_select_bodies = new QPushButton(QStringLiteral("选择遮挡天体…"));
    srp_select_bodies->setObjectName("hpopSrpSelectBodies");
    srp_shadow_section->addRow(QString{}, srp_select_bodies);
    auto *srp_altitude = quantity(
        srp_shadow_section, QStringLiteral("地球大气等效高度"), "hpopSrpAtmosphericAltitude",
        Unit::metre, options.srp.atmospheric_altitude_m, 0, std::numeric_limits<double>::max());
    help(srp_shadow_section, QStringLiteral("双锥按日面可见比例缩放光压，包含半影、本影与环食。"
                                            "多体遮挡按并集计算。当前采用球形轮廓；大气高度增加"
                                            "地球遮挡半径，不计算折射或衰减。"));
    auto *srp_sun = section(srp_page, QStringLiteral("太阳位置"), "hpopSrpSunSection");
    auto *srp_sun_method = new QComboBox;
    srp_sun_method->setObjectName("hpopSrpSunMethod");
    srp_sun_method->addItems(
        {QStringLiteral("几何太阳"), QStringLiteral("地心视太阳"), QStringLiteral("卫星视太阳")});
    srp_sun_method->setItemData(0, QStringLiteral("True"), Qt::ToolTipRole);
    srp_sun_method->setItemData(1, QStringLiteral("Apparent to True CB"), Qt::ToolTipRole);
    srp_sun_method->setItemData(2, QStringLiteral("Apparent"), Qt::ToolTipRole);
    srp_sun_method->setMinimumContentsLength(12);
    srp_sun_method->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    srp_sun_method->setCurrentIndex(static_cast<int>(options.srp.sun_position_method));
    srp_sun_method->setToolTip(QStringLiteral(
        "几何太阳（True）使用同历元位置；地心视太阳（Apparent to True CB）以地心为观测者；"
        "卫星视太阳（Apparent）使用积分阶段的卫星位置与速度。两种视位置包含光行时和光行差。"
        "遮挡天体中心仍使用几何位置。"));
    srp_sun->addRow(QStringLiteral("太阳位置方法"), srp_sun_method);
    auto *srp_resource = new QLabel;
    srp_resource->setObjectName("hpopSrpEphemerisStatus");
    srp_resource->setWordWrap(true);
    srp_sun->addRow(QString{}, srp_resource);
    refresh_srp_resource = [&] {
        const bool ready = std::any_of(
            installed_ephemerides.begin(), installed_ephemerides.end(),
            [&](const auto &entry) { return entry.selection == options.ephemeris_model; });
        srp_resource->setText(
            ready ? QStringLiteral("位置星历：%1")
                        .arg(QString::fromStdString(options.ephemeris_model))
                  : QStringLiteral("缺少所选位置星历：%1，请在数据中心准备所选天体的位置数据。")
                        .arg(QString::fromStdString(options.ephemeris_model)));
    };
    auto *srp_manage = new QPushButton(QStringLiteral("在数据中心管理星历"));
    srp_manage->setObjectName("hpopSrpManageEphemerides");
    srp_manage->setVisible(static_cast<bool>(manage_ephemerides));
    srp_sun->addRow(QString{}, srp_manage);
    QObject::connect(srp_manage, &QPushButton::clicked, manage, &QToolButton::click);
    QObject::connect(manage, &QToolButton::clicked, &dialog, refresh_srp_resource);
    QObject::connect(manage, &QToolButton::clicked, &dialog, refresh_drag_resource);
    refresh_srp_resource();
    help(srp_sun, QStringLiteral("视太阳需要更早的太阳星历；实际发光时刻超出所选覆盖时会报错。"));
    auto *srp_constants = section(srp_page, QStringLiteral("模型常数"), "hpopSrpConstantsSection");
    auto *srp_pressure =
        quantity(srp_constants, QStringLiteral("参考距离处光压"), "hpopSrpPressure", Unit::pascal,
                 options.srp.pressure_at_reference_pa, std::numeric_limits<double>::min(),
                 std::numeric_limits<double>::max());
    auto *srp_distance =
        quantity(srp_constants, QStringLiteral("光压参考距离"), "hpopSrpReferenceDistance",
                 Unit::metre, options.srp.reference_distance_m, std::numeric_limits<double>::min(),
                 std::numeric_limits<double>::max());
    auto *srp_sun_radius =
        quantity(srp_constants, QStringLiteral("太阳半径"), "hpopSrpSunRadius", Unit::metre,
                 options.srp.sun_radius_m, std::numeric_limits<double>::min(),
                 std::numeric_limits<double>::max());
    auto *srp_earth_radius =
        quantity(srp_constants, QStringLiteral("地球球面半径"), "hpopSrpEarthRadius", Unit::metre,
                 options.srp.earth_radius_m, std::numeric_limits<double>::min(),
                 std::numeric_limits<double>::max());
    auto *srp_moon_radius =
        quantity(srp_constants, QStringLiteral("月球球面半径"), "hpopSrpMoonRadius", Unit::metre,
                 options.srp.moon_radius_m, std::numeric_limits<double>::min(),
                 std::numeric_limits<double>::max());
    for (auto *field : {srp_distance, srp_sun_radius, srp_earth_radius, srp_moon_radius})
        field->set_unit(Unit::kilometre);
    // Physical constants rarely change: the group starts folded behind a summary.
    if (auto *group = group_of(srp_constants)) {
        group->set_summary(QStringLiteral("标准太阳与地月常数"));
        group->set_collapsible(false);
    }
    caution(srp_page, QStringLiteral("当前遮挡轮廓采用球面，椭球轮廓尚未实现。"));
    const auto update_srp = [&] {
        const bool enabled = srp_use->isChecked(), shadows = srp_shadow->currentIndex() != 0;
        for (auto *form : {srp_model, srp_shadow_section, srp_sun, srp_constants}) {
            form->parentWidget()->setEnabled(enabled);
            group_of(form)->setVisible(enabled);
        }
        srp_off->setVisible(!enabled);
        srp_select_bodies->setEnabled(shadows);
        srp_boundary->setEnabled(shadows &&
                                 (srp_assignments.earth_shadow || srp_assignments.moon_shadow ||
                                  !srp_assignments.additional_eclipsing_bodies.empty()));
        srp_altitude->setEnabled(shadows && srp_assignments.earth_shadow);
        srp_earth_radius->setEnabled(shadows && srp_assignments.earth_shadow);
        srp_moon_radius->setEnabled(shadows && srp_assignments.moon_shadow);
        QStringList names;
        if (srp_assignments.earth_shadow)
            names << QStringLiteral("Earth");
        if (srp_assignments.moon_shadow)
            names << QStringLiteral("Moon");
        for (const auto &body : srp_assignments.additional_eclipsing_bodies)
            names << QString::fromUtf8(core::third_body_name(body.body).data());
        srp_assigned_summary->setText(names.isEmpty() ? QStringLiteral("无")
                                                      : names.join(QStringLiteral("、")));
    };
    QObject::connect(srp_use, &QCheckBox::toggled, &dialog, update_srp);
    QObject::connect(srp_shadow, &QComboBox::currentIndexChanged, &dialog, update_srp);
    QObject::connect(srp_select_bodies, &QPushButton::clicked, &dialog, [&] {
        QDialog selection(&dialog);
        selection.setObjectName("hpopSrpBodiesDialog");
        selection.setWindowTitle(QStringLiteral("遮挡天体 · Eclipsing Central Bodies"));
        selection.resize(430, 470);
        auto *box = new QVBoxLayout(&selection);
        auto *filter = new QLineEdit;
        filter->setObjectName("hpopSrpBodySearch");
        filter->setPlaceholderText(QStringLiteral("搜索可用天体"));
        box->addWidget(filter);
        auto *lists = new QHBoxLayout;
        auto *available = new QListWidget;
        available->setObjectName("hpopSrpAvailableBodies");
        available->setAccessibleName(QStringLiteral("可用天体"));
        auto *assigned = new QListWidget;
        assigned->setObjectName("hpopSrpAssignedBodies");
        assigned->setAccessibleName(QStringLiteral("已分配天体"));
        for (auto *list : {available, assigned}) {
            list->setMinimumWidth(100);
            list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        }
        auto *available_column = new QVBoxLayout;
        available_column->addWidget(new QLabel(QStringLiteral("可用 Available")));
        available_column->addWidget(available);
        auto *assigned_column = new QVBoxLayout;
        assigned_column->addWidget(new QLabel(QStringLiteral("已分配 Assigned")));
        assigned_column->addWidget(assigned);
        lists->addLayout(available_column, 1);
        auto *actions = new QVBoxLayout;
        auto *add = new QToolButton;
        add->setObjectName("hpopSrpAddBody");
        add->setText(QStringLiteral("→"));
        add->setAccessibleName(QStringLiteral("分配所选天体"));
        auto *remove = new QToolButton;
        remove->setObjectName("hpopSrpRemoveBody");
        remove->setText(QStringLiteral("←"));
        remove->setAccessibleName(QStringLiteral("移除所选天体"));
        actions->addStretch();
        actions->addWidget(add);
        actions->addWidget(remove);
        actions->addStretch();
        lists->addLayout(actions);
        lists->addLayout(assigned_column, 1);
        box->addLayout(lists, 1);
        auto *radii = new QStackedWidget;
        auto *hint = new QLabel(QStringLiteral("地球与月球半径在光压模型常数中设置。"));
        hint->setWordWrap(true);
        radii->addWidget(hint);
        std::map<int, QuantityEditor *> editors;
        std::map<int, int> radius_pages;
        const auto append = [&](int id, const QString &name, bool selected) {
            auto *item = new QListWidgetItem(name, selected ? assigned : available);
            item->setData(Qt::UserRole, id);
        };
        append(-1, QStringLiteral("地球 Earth"), srp_assignments.earth_shadow);
        append(1, QStringLiteral("月球 Moon"), srp_assignments.moon_shadow);
        for (auto body : core::third_body_list) {
            if (body == core::ThirdBody::sun || body == core::ThirdBody::moon)
                continue;
            const auto found = std::ranges::find(srp_assignments.additional_eclipsing_bodies, body,
                                                 &core::SrpEclipsingBody::body);
            const auto value = found == srp_assignments.additional_eclipsing_bodies.end()
                                   ? core::default_srp_eclipsing_body(body)
                                   : *found;
            const auto id = static_cast<int>(body);
            append(id, QString::fromUtf8(core::third_body_name(body).data()),
                   found != srp_assignments.additional_eclipsing_bodies.end());
            auto *radius_page = new QWidget;
            auto *form = new QFormLayout(radius_page);
            form->setContentsMargins(0, 0, 0, 0);
            form->setRowWrapPolicy(QFormLayout::WrapLongRows);
            const auto object_name = "hpopSrpRadius" + std::to_string(id);
            auto *editor = quantity(form, QStringLiteral("所选行星球面半径"), object_name.c_str(),
                                    Unit::metre, value.radius_m, std::numeric_limits<double>::min(),
                                    std::numeric_limits<double>::max());
            editor->set_unit(Unit::kilometre);
            editors[id] = editor;
            radius_pages[id] = radii->addWidget(radius_page);
        }
        box->addWidget(radii);
        auto *coverage = new QLabel(
            QStringLiteral("DE440s 直接包含地月、水星和金星。火星及外行星的本体星历可在数据中心"
                           "导入相应 SPK 文件；覆盖不足时停止计算。"));
        coverage->setObjectName("hint");
        coverage->setWordWrap(true);
        box->addWidget(coverage);
        auto *error = new QLabel;
        error->setObjectName("hpopSrpBodiesError");
        error->setWordWrap(true);
        box->addWidget(error);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        box->addWidget(buttons);
        const auto filter_available = [&] {
            for (int i = 0; i < available->count(); ++i) {
                auto *item = available->item(i);
                const bool hidden = !item->text().contains(filter->text(), Qt::CaseInsensitive);
                item->setHidden(hidden);
                if (hidden)
                    item->setSelected(false);
            }
        };
        const auto move = [&](QListWidget *from, QListWidget *to) {
            for (auto *item : from->selectedItems()) {
                from->takeItem(from->row(item));
                to->addItem(item);
                item->setHidden(false);
                to->setCurrentItem(item);
            }
            filter_available();
        };
        QObject::connect(filter, &QLineEdit::textChanged, &selection, filter_available);
        QObject::connect(add, &QToolButton::clicked, &selection,
                         [&] { move(available, assigned); });
        QObject::connect(remove, &QToolButton::clicked, &selection,
                         [&] { move(assigned, available); });
        QObject::connect(available, &QListWidget::itemDoubleClicked, &selection,
                         [&](QListWidgetItem *) { move(available, assigned); });
        QObject::connect(assigned, &QListWidget::itemDoubleClicked, &selection,
                         [&](QListWidgetItem *) { move(assigned, available); });
        QObject::connect(
            assigned, &QListWidget::currentItemChanged, &selection, [&](QListWidgetItem *item) {
                const auto id = item ? item->data(Qt::UserRole).toInt() : -1;
                radii->setCurrentIndex(radius_pages.contains(id) ? radius_pages.at(id) : 0);
            });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &selection, &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &selection, [&] {
            try {
                auto candidate = srp_assignments;
                candidate.earth_shadow = candidate.moon_shadow = false;
                candidate.additional_eclipsing_bodies.clear();
                for (int i = 0; i < assigned->count(); ++i) {
                    auto *item = assigned->item(i);
                    const auto id = item->data(Qt::UserRole).toInt();
                    if (id == -1)
                        candidate.earth_shadow = true;
                    else if (id == 1)
                        candidate.moon_shadow = true;
                    else {
                        assigned->setCurrentItem(item);
                        candidate.additional_eclipsing_bodies.push_back(
                            {static_cast<core::ThirdBody>(id), editors.at(id)->value(Unit::metre)});
                    }
                }
                core::validate_srp_options(candidate);
                srp_assignments = std::move(candidate);
                selection.accept();
            } catch (const std::exception &e) {
                error->setText(QString::fromUtf8(e.what()));
            }
        });
        if (selection.exec() == QDialog::Accepted)
            update_srp();
    });
    update_srp();
    auto *additional_page = page(force_tabs, QStringLiteral("其他"), "hpopAdditionalPage");
    auto *mass_section = section(additional_page, QStringLiteral("卫星质量"), "hpopMassSection");
    auto *satellite_mass =
        quantity(mass_section, QStringLiteral("质量"), "hpopSatelliteMass", Unit::kilogram,
                 options.satellite_mass_kg, std::numeric_limits<double>::denorm_min(),
                 std::numeric_limits<double>::max());
    help(mass_section, QStringLiteral("球形阻力与光压按各自的面积质量比计算。修改质量时，"
                                      "对应面积随之换算，面积质量比和加速度保持不变。"));
    const auto refresh_compatible_areas = [&] {
        const auto refresh = [&](QuantityEditor *ratio, QLabel *label) {
            try {
                core::HpopOptions value;
                value.satellite_mass_kg = satellite_mass->value(Unit::kilogram);
                value.drag.area_mass_ratio_m2_per_kg =
                    ratio->value(Unit::square_metres_per_kilogram);
                label->setText(QStringLiteral("%1 m²").arg(value.drag_area_m2(), 0, 'g', 12));
            } catch (const std::exception &) {
                label->setText(QStringLiteral("请输入有效的质量和面积质量比"));
            }
        };
        refresh(drag_area, drag_compatible_area);
        refresh(srp_area, srp_compatible_area);
    };
    for (auto *field : {satellite_mass, drag_area, srp_area}) {
        QObject::connect(field->findChild<QLineEdit *>("quantityValue"), &QLineEdit::textChanged,
                         &dialog, refresh_compatible_areas);
        QObject::connect(field, &QuantityEditor::unitChanged, &dialog, refresh_compatible_areas);
    }
    refresh_compatible_areas();
    auto *relativity_section =
        section(additional_page, QStringLiteral("相对论"), "hpopRelativitySection");
    auto *relativity = new QCheckBox(QStringLiteral("包含相对论加速度"));
    relativity->setObjectName("hpopRelativisticAccelerations");
    relativity->setChecked(options.include_relativistic_accelerations);
    relativity_section->addRow(relativity);
    help(relativity_section,
         QStringLiteral(
             "IERS 2003：包含地球 Schwarzschild、Lense–Thirring 和太阳 de Sitter "
             "三项修正。需要覆盖传播时段的地球定向参数和太阳星历；无需启用太阳第三体引力。"));
    auto *additional_pending =
        section(additional_page, QStringLiteral("其他附加模型"), "hpopAdditionalPendingSection");
    caution(additional_pending, QStringLiteral("地球反照与热辐射及自定义力模型尚未实现。"));
    auto *integration_page = page(tabs, QStringLiteral("积分器"), "hpopIntegratorPage");
    auto *integration =
        section(integration_page, QStringLiteral("积分方法"), "hpopIntegratorMethodSection");
    auto *method = new QComboBox;
    method->setObjectName("hpopIntegrator");
    method->addItems({QStringLiteral("RKF 7(8)"), QStringLiteral("RK4"),
                      QStringLiteral("RKV 8(9) Efficient"), QStringLiteral("Bulirsch–Stoer"),
                      QStringLiteral("Gauss–Jackson 12")});
    method->setCurrentIndex(static_cast<int>(options.integrator));
    integration->addRow(QStringLiteral("积分器"), method);
    auto *vop = new QCheckBox(QStringLiteral("VOP · 通用变量变参数法"));
    vop->setObjectName("hpopVop");
    vop->setChecked(options.vop);
    vop->setToolTip(
        QStringLiteral("适用于 RKF 7(8) 和 Bulirsch–Stoer，使用引力模型的 GM。星历插值单独设置。"));
    integration->addRow(vop);
    auto *step_control = new QComboBox;
    step_control->setObjectName("hpopStepControl");
    step_control->addItems(
        {QStringLiteral("Relative Error · 相对误差"), QStringLiteral("Fixed Step · 固定步长")});
    int step_control_draft = static_cast<int>(options.step_control);
    integration->addRow(QStringLiteral("步长控制"), step_control);

    auto *adaptive_widget = new QWidget;
    auto *adaptive = new QFormLayout(adaptive_widget);
    adaptive->setContentsMargins(0, 0, 0, 0);
    adaptive->setVerticalSpacing(10);
    adaptive->setHorizontalSpacing(12);
    adaptive->setRowWrapPolicy(QFormLayout::WrapLongRows);
    adaptive->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *minimum = quantity(adaptive, QStringLiteral("最小步长"), "hpopMinStep", Unit::second,
                             options.min_step_seconds, 1e-6, core::hpop_max_step_seconds);
    auto *maximum = quantity(adaptive, QStringLiteral("最大步长"), "hpopMaxStep", Unit::second,
                             options.max_step_seconds, 1e-6, core::hpop_max_step_seconds);
    auto *relative = quantity(adaptive, QStringLiteral("相对容差"), "hpopRelativeTolerance",
                              Unit::one, options.relative_tolerance, 1e-14, 1e-3);
    auto *position =
        quantity(adaptive, QStringLiteral("位置绝对容差"), "hpopPositionTolerance", Unit::metre,
                 options.position_tolerance_m, std::numeric_limits<double>::min(),
                 std::numeric_limits<double>::max());
    auto *velocity =
        quantity(adaptive, QStringLiteral("速度绝对容差"), "hpopVelocityTolerance",
                 Unit::metres_per_second, options.velocity_tolerance_mps,
                 std::numeric_limits<double>::min(), std::numeric_limits<double>::max());
    integration->addRow(adaptive_widget);
    auto *fixed = quantity(integration, QStringLiteral("固定步长"), "hpopFixedStep", Unit::second,
                           options.fixed_step_seconds, 1e-6, core::hpop_max_step_seconds);
    auto *correction = new QComboBox;
    correction->setObjectName("hpopCorrection");
    correction->setToolTip(
        QStringLiteral("完整校正在收敛后更新全部加速度；伪校正只更新二体引力，计算较快。"));
    correction->addItems({QStringLiteral("Full Correction · 完整校正"),
                          QStringLiteral("Pseudo Correction · 伪校正")});
    correction->setCurrentIndex(static_cast<int>(options.correction));
    integration->addRow(QStringLiteral("预测校正方式"), correction);
    auto *regularization_form =
        section(integration_page, QStringLiteral("时间正则化"), "hpopRegularizationSection");
    auto *regularization = new QCheckBox(QStringLiteral("启用时间正则化"));
    regularization->setObjectName("hpopTimeRegularization");
    regularization->setChecked(options.time_regularization);
    regularization_form->addRow(regularization);
    auto *exponent = quantity(
        regularization_form, QStringLiteral("指数"), "hpopRegularizationExponent", Unit::one,
        options.regularization_exponent, std::numeric_limits<double>::min(), 10);
    auto *steps_per_orbit = new QSpinBox;
    steps_per_orbit->setObjectName("hpopStepsPerOrbit");
    steps_per_orbit->setRange(1, std::numeric_limits<int>::max());
    steps_per_orbit->setValue(options.steps_per_orbit);
    regularization_form->addRow(QStringLiteral("每圈步数"), steps_per_orbit);
    help(regularization_form,
         QStringLiteral("需要椭圆初轨道。指数 1 对应偏近点角，2 对应真近点角；每圈步数设置"
                        "正则化时间步长。相对误差模式的最小、最大步长仍以秒计。"));
    const auto update_step_control = [=] {
        const bool is_fixed = step_control->currentIndex() == 1;
        adaptive_widget->setVisible(!is_fixed);
        integration->setRowVisible(fixed, is_fixed && !regularization->isChecked());
        exponent->setEnabled(regularization->isChecked());
        steps_per_orbit->setEnabled(regularization->isChecked());
        integration->setRowVisible(correction, method->currentIndex() == 4);
    };
    const auto update_method = [=, &step_control_draft] {
        vop->setEnabled(method->currentIndex() == 0 || method->currentIndex() == 3);
        const bool forced = method->currentIndex() == 1 || method->currentIndex() == 4;
        const QSignalBlocker blocker(step_control);
        step_control->setCurrentIndex(forced ? 1 : step_control_draft);
        step_control->setEnabled(!forced);
        update_step_control();
    };
    QObject::connect(method, &QComboBox::currentIndexChanged, &dialog, update_method);
    QObject::connect(regularization, &QCheckBox::toggled, &dialog, update_step_control);
    QObject::connect(step_control, &QComboBox::currentIndexChanged, &dialog,
                     [&, update_step_control](int index) {
                         step_control_draft = index;
                         update_step_control();
                     });
    update_method();
    help(integration, QStringLiteral("输出采样间隔独立设置。容差约束单步数值误差，不代表轨道预报的"
                                     "实际精度。固定步长不进行误差控制；RK4 和 Gauss–Jackson "
                                     "仅支持固定步长，Bulirsch–Stoer 固定模式使用 18 阶外推。"));
    auto *interpolation_form =
        section(integration_page, QStringLiteral("星历插值"), "hpopInterpolationSection");
    auto *interpolation = new QComboBox;
    interpolation->setObjectName("hpopInterpolation");
    interpolation->addItems({QStringLiteral("Lagrange · 拉格朗日"),
                             QStringLiteral("Hermitian · 埃尔米特"),
                             QStringLiteral("VOP · 参数变分")});
    interpolation->setCurrentIndex(static_cast<int>(options.interpolation.method));
    interpolation_form->addRow(QStringLiteral("方法"), interpolation);
    auto *interpolation_order = new QSpinBox;
    interpolation_order->setObjectName("hpopInterpolationOrder");
    interpolation_form->addRow(QStringLiteral("阶数"), interpolation_order);
    auto *interpolation_mu =
        quantity(interpolation_form, QStringLiteral("VOP 引力常数 μ"), "hpopInterpolationMu",
                 Unit::metres_cubed_per_second_squared, options.interpolation.vop_mu,
                 std::numeric_limits<double>::min(), std::numeric_limits<double>::max());
    interpolation_mu->set_unit(Unit::kilometres_cubed_per_second_squared);
    std::array<int, 3> interpolation_drafts{7, 7, 7};
    interpolation_drafts[static_cast<std::size_t>(interpolation->currentIndex())] =
        options.interpolation.order;
    const auto update_interpolation = [=, &interpolation_drafts] {
        const QSignalBlocker blocker(interpolation_order);
        const bool hermite = interpolation->currentIndex() == 1;
        interpolation_mu->setEnabled(interpolation->currentIndex() == 2);
        interpolation_order->setRange(hermite ? 3 : 0, 29);
        interpolation_order->setSingleStep(hermite ? 2 : 1);
        interpolation_order->setValue(
            interpolation_drafts[static_cast<std::size_t>(interpolation->currentIndex())]);
    };
    QObject::connect(interpolation, &QComboBox::currentIndexChanged, &dialog, update_interpolation);
    QObject::connect(interpolation_order, &QSpinBox::valueChanged, &dialog, [&](int value) {
        interpolation_drafts[static_cast<std::size_t>(interpolation->currentIndex())] = value;
    });
    update_interpolation();
    auto *fixed_ephemeris = new QCheckBox(QStringLiteral("按轨道步长固定输出星历"));
    fixed_ephemeris->setObjectName("hpopFixedEphemeris");
    fixed_ephemeris->setChecked(options.report_ephemeris_on_fixed_step);
    interpolation_form->addRow(fixed_ephemeris);
    help(interpolation_form,
         QStringLiteral("Lagrange 分别插值位置和速度；Hermitian "
                        "联合使用位置与速度，阶数必须为奇数。VOP 使用独立 μ 处理"
                        "以二体运动为主的稀疏星历。固定输出使用轨道页"
                        "的步长；关闭时使用实际积分端点。"));
    auto *stop = section(integration_page, QStringLiteral("停止条件"), "hpopStopSection");
    auto *altitude =
        quantity(stop, QStringLiteral("最低地心高度"), "hpopMinimumAltitude", Unit::metre,
                 options.minimum_altitude_m, 0, std::numeric_limits<double>::max());
    help(stop, QStringLiteral("高度相对所选重力场参考球面；积分中低于此高度会停止并报错。"));
    pending(
        tabs, QStringLiteral("协方差"), "hpopCovariancePage",
        QStringLiteral("协方差传播尚未实现，当前只计算确定性轨迹。"),
        QStringLiteral("初始 6 × 6 矩阵、参考系、独立重力截断与 consider 参数将集中在此配置。"));
    // Navigation: force models and numerics, each with a live summary of the current draft so
    // the whole configuration reads at a glance.
    struct Destination {
        QListWidgetItem *item;
        int outer, inner;
    };
    std::vector<Destination> destinations;
    const auto heading_item = [&](const QString &text) {
        auto *item = new QListWidgetItem(text, navigation);
        item->setData(heading_role, true);
        item->setFlags(Qt::NoItemFlags);
    };
    const auto entry = [&](const QString &text, int outer, int inner) {
        auto *item = new QListWidgetItem(text, navigation);
        destinations.push_back({item, outer, inner});
        return item;
    };
    heading_item(QStringLiteral("力模型"));
    auto *gravity_item = entry(QStringLiteral("引力"), 0, 0);
    auto *drag_item = entry(QStringLiteral("阻力"), 0, 1);
    auto *srp_item = entry(QStringLiteral("太阳辐射压"), 0, 2);
    auto *other_item = entry(QStringLiteral("其他"), 0, 3);
    heading_item(QStringLiteral("数值"));
    auto *integrator_item = entry(QStringLiteral("积分器"), 1, -1);
    auto *covariance_item = entry(QStringLiteral("协方差"), 2, -1);
    const auto value_text = [](QuantityEditor *field) {
        const auto *input = field->findChild<QLineEdit *>("quantityValue");
        return input ? input->text() : QString{};
    };
    const auto part = [](const QComboBox *box, int index) {
        const auto parts = box->currentText().split(QStringLiteral(" · "));
        return parts.value(std::min<int>(index, static_cast<int>(parts.size()) - 1));
    };
    const auto refresh_summaries = [&] {
        auto gravity_text =
            model->currentIndex() == 1
                ? QStringLiteral("点质量")
                : part(model, 0) +
                      QStringLiteral(" · %1 × %2").arg(degree->value()).arg(order->value());
        const auto bodies = std::count_if(fields.begin(), fields.end(),
                                          [](const auto &row) { return row.enabled->isChecked(); });
        if (bodies > 0)
            gravity_text += QStringLiteral(" · 第三体 %1").arg(bodies);
        if (secular_use->isChecked() && model->currentIndex() != 1)
            gravity_text += QStringLiteral(" · 长期变化");
        if (solid_mode->currentIndex() != 0 &&
            (model->currentIndex() != 1 || !solid_truncate->isChecked()))
            gravity_text += QStringLiteral(" · ") + solid_mode->currentText();
        gravity_item->setData(summary_role, gravity_text);
        drag_item->setData(summary_role, drag_use->isChecked()
                                             ? density_model->currentText() +
                                                   QStringLiteral(" · Cd ") + value_text(drag_cd)
                                             : QStringLiteral("未启用"));
        srp_item->setData(summary_role,
                          srp_use->isChecked()
                              ? part(srp_shadow, 1) + QStringLiteral(" · Cr ") + value_text(srp_cr)
                              : QStringLiteral("未启用"));
        other_item->setData(
            summary_role, value_text(satellite_mass) + QStringLiteral(" ") +
                              unit_symbol(satellite_mass->unit()) +
                              (relativity->isChecked() ? QStringLiteral(" · 相对论") : QString{}));
        const QString integration_name =
            part(method, 0) +
            (vop->isEnabled() && vop->isChecked() ? QStringLiteral(" · VOP") : QString{});
        integrator_item->setData(
            summary_role,
            regularization->isChecked()
                ? integration_name +
                      QStringLiteral(" · 正则化 · %1 步/圈").arg(steps_per_orbit->value())
                : (step_control->currentIndex() == 0
                       ? integration_name + QStringLiteral(" · 相对容差 ") + value_text(relative)
                       : integration_name + QStringLiteral(" · 步长 ") + value_text(fixed) +
                             QStringLiteral(" s")));
        covariance_item->setData(summary_role, QStringLiteral("尚未实现"));
    };
    QObject::connect(secular_use, &QCheckBox::toggled, &dialog, refresh_summaries);
    for (auto *box : {model, density_model, srp_shadow, method, step_control, solid_mode})
        QObject::connect(box, &QComboBox::currentIndexChanged, &dialog, refresh_summaries);
    for (auto *spin : {degree, order, steps_per_orbit})
        QObject::connect(spin, &QSpinBox::valueChanged, &dialog, refresh_summaries);
    for (auto *check : {drag_use, srp_use, solid_truncate, relativity, regularization, vop})
        QObject::connect(check, &QCheckBox::toggled, &dialog, refresh_summaries);
    for (auto &row : fields)
        QObject::connect(row.enabled, &QCheckBox::toggled, &dialog, refresh_summaries);
    for (auto *field : {drag_cd, srp_cr, relative, fixed, satellite_mass})
        QObject::connect(field->findChild<QLineEdit *>("quantityValue"), &QLineEdit::textChanged,
                         &dialog, refresh_summaries);
    QObject::connect(satellite_mass, &QuantityEditor::unitChanged, &dialog, refresh_summaries);
    refresh_summaries();
    QObject::connect(navigation, &QListWidget::currentItemChanged, &dialog,
                     [&](QListWidgetItem *item) {
                         for (const auto &destination : destinations)
                             if (destination.item == item) {
                                 tabs->setCurrentIndex(destination.outer);
                                 if (destination.inner >= 0)
                                     force_tabs->setCurrentIndex(destination.inner);
                             }
                     });
    const auto sync_navigation = [&] {
        // Values set programmatically do not emit edits; refresh whenever the page changes.
        refresh_summaries();
        for (const auto &destination : destinations)
            if (destination.outer == tabs->currentIndex() &&
                (destination.inner < 0 || destination.inner == force_tabs->currentIndex())) {
                const QSignalBlocker block(navigation);
                navigation->setCurrentItem(destination.item);
            }
    };
    QObject::connect(tabs, &QTabWidget::currentChanged, &dialog, sync_navigation);
    QObject::connect(force_tabs, &QTabWidget::currentChanged, &dialog, sync_navigation);
    sync_navigation();
    // Narrow windows give the width to the fields and fall back to the tab bars.
    new WidthObserver(&dialog,
                      [navigation, tabs, force_tabs, mode = WidthMode{}](int width) mutable {
                          const bool narrow = mode.narrow(width, 620);
                          navigation->setVisible(!narrow);
                          tabs->tabBar()->setVisible(narrow);
                          force_tabs->tabBar()->setVisible(narrow);
                      });
    align_form_labels(&dialog);
    auto *error = new QLabel;
    error->setObjectName("errorText");
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);
    std::optional<DataProduct> queued_gravity, owned_gravity;
    bool queued_ocean = false, owned_ocean = false;
    QString ocean_message;
    QString gravity_message;
    auto catalog_revision = data ? data->revision() : 0;
    const auto selected_product = [&]() -> std::optional<DataProduct> {
        const auto id = model->currentData().toString();
        if (id == "egm2008")
            return DataProduct::egm2008;
        if (id == "egm96")
            return DataProduct::egm96;
        return {};
    };
    const auto selected_model = [&] {
        return std::find_if(installed_models.begin(), installed_models.end(), [&](const auto &v) {
            return v.selection == model->currentData().toString().toStdString();
        });
    };
    const auto refresh_gravity_status = [&] {
        const auto product = selected_product();
        const auto found = selected_model();
        const bool secular_ready =
            found != installed_models.end() && found->info.secular_variations.has_value();
        secular_use->setEnabled(model->currentIndex() != 1 &&
                                (secular_ready || secular_use->isChecked()));
        if (model->currentIndex() == 1) {
            secular_status->setText(QStringLiteral("点质量不使用长期变化；保留当前草稿。"));
        } else if (secular_ready) {
            const auto &variation = *found->info.secular_variations;
            const double jd =
                2451545.0 +
                (variation.reference_epoch.seconds_since(core::Epoch{0}) + 32.184) / 86400;
            secular_status->setText(QStringLiteral("参考历元 JD %1 TT · %2 项变化率 / 儒略年")
                                        .arg(jd, 0, 'f', 9)
                                        .arg(variation.rates_per_year.size()));
        } else {
            secular_status->setText(QStringLiteral("所选文件未提供长期变化的参考历元与变化率。"));
        }
        solid_k20->setText(
            found != installed_models.end() ? QString::number(found->info.solid_tide_k20, 'g', 15)
            : model->currentData().toString().isEmpty() ? QStringLiteral("0.30190")
                                                        : QStringLiteral("待读取重力文件"));
        const bool ready =
            model->currentData().toString().isEmpty() || found != installed_models.end();
        const bool running =
            data && product && data->downloading() && data->active_product() == *product;
        const bool waiting = product && queued_gravity == product;
        gravity_progress->setVisible(running);
        if (running) {
            const auto received = data->received_bytes(), total = data->total_bytes();
            // Once all bytes arrive, validation still has to finish before publication.
            if (total > 0 && received < total) {
                gravity_progress->setRange(0, 1000);
                gravity_progress->setValue(static_cast<int>(1000.0 * received / total));
            } else {
                gravity_progress->setRange(0, 0);
            }
            gravity_status->setText(
                QStringLiteral("%1\n已接收 %2 MiB%3")
                    .arg(data->status())
                    .arg(received / (1024.0 * 1024), 0, 'f', 1)
                    .arg(total > 0
                             ? QStringLiteral(" / %1 MiB").arg(total / (1024.0 * 1024), 0, 'f', 1)
                             : QString{}));
        } else if (waiting) {
            gravity_status->setText(QStringLiteral("等待数据中心当前任务完成后下载所选重力场…"));
        } else if (found != installed_models.end()) {
            gravity_status->setText(QStringLiteral("可用阶 × 次：%1 × %2")
                                        .arg(found->info.degree)
                                        .arg(found->info.order));
        } else if (model->currentIndex() == 0) {
            gravity_status->setText(QStringLiteral("内置 EGM96 示例，最高 20 × 20。"));
        } else if (model->currentIndex() == 1) {
            gravity_status->setText(QStringLiteral("使用内置地球引力常数进行点质量对照计算。"));
        } else {
            const auto message = !gravity_message.isEmpty() ? gravity_message
                                 : data && product          ? data->update_status(*product).message
                                                            : QString{};
            gravity_status->setText(
                message.isEmpty()
                    ? (product
                           ? QStringLiteral("未安装。首次打开自动下载完整文件：EGM2008 约 240 MiB，"
                                            "EGM96 约 5.4 MiB。")
                           : QStringLiteral("未安装。请在数据中心导入所选外部重力场。"))
                    : message + QStringLiteral("\n所选模型仍未安装，可重试或在数据中心导入。"));
        }
        download_gravity->setVisible(data && product && !ready && !running && !waiting);
        download_gravity->setText(QStringLiteral("下载 / 重试"));
        cancel_gravity->setVisible(queued_gravity.has_value() ||
                                   (data && owned_gravity && data->downloading() &&
                                    data->active_product() == *owned_gravity));
        gravity_actions->setVisible(!download_gravity->isHidden() || !cancel_gravity->isHidden());
        const bool ocean_missing =
            std::none_of(installed_oceans.begin(), installed_oceans.end(), [&](const auto &entry) {
                return QString::fromStdString(entry.selection) == ocean_model->currentData();
            });
        const bool ocean_waiting =
            ocean_use->isChecked() && ocean_degree->value() >= 2 && ocean_missing &&
            ocean_model->currentData().toString() == "fes2014bv1" &&
            (queued_ocean ||
             (data && data->downloading() && data->active_product() == DataProduct::fes2014bv1));
        buttons->button(QDialogButtonBox::Ok)
            ->setEnabled((ready || (!running && !waiting)) && !ocean_waiting);
    };
    const auto request_gravity = [&] {
        const auto product = selected_product();
        queued_gravity.reset();
        gravity_message.clear();
        if (!data || !product || selected_model() != installed_models.end()) {
            refresh_gravity_status();
            return;
        }
        if (data->downloading()) {
            if (data->active_product() != *product)
                queued_gravity = product;
        } else {
            data->start_download({}, *product);
            if (data->downloading())
                owned_gravity = product;
        }
        refresh_gravity_status();
    };
    QObject::connect(model, &QComboBox::currentIndexChanged, &dialog, request_gravity);
    QObject::connect(download_gravity, &QPushButton::clicked, &dialog, request_gravity);
    QObject::connect(secular_use, &QCheckBox::toggled, &dialog, refresh_gravity_status);
    QObject::connect(cancel_gravity, &QPushButton::clicked, &dialog, [&] {
        queued_gravity.reset();
        gravity_message = QStringLiteral("已取消下载。");
        if (data && owned_gravity && data->downloading() &&
            data->active_product() == *owned_gravity)
            data->cancel_download();
        refresh_gravity_status();
    });
    const auto selected_ocean = [&] {
        return std::find_if(installed_oceans.begin(), installed_oceans.end(), [&](const auto &v) {
            return v.selection == ocean_model->currentData().toString().toStdString();
        });
    };
    const auto refresh_ocean_status = [&] {
        const bool enabled = ocean_use->isChecked(), active = enabled && ocean_degree->value() >= 2;
        ocean_model->setEnabled(enabled);
        ocean_degree->setEnabled(enabled);
        ocean_order->setEnabled(enabled);
        const bool running = ocean_model->currentData().toString() == "fes2014bv1" && data &&
                             data->downloading() &&
                             data->active_product() == DataProduct::fes2014bv1;
        const auto found = selected_ocean();
        const bool ready = found != installed_oceans.end();
        ocean_status->setVisible(enabled);
        ocean_status->setText(!active ? QStringLiteral("当前阶数不产生海潮引力。")
                              : ready ? QStringLiteral("%1 × %2 · %3 个显式分潮")
                                            .arg(found->info.degree)
                                            .arg(found->info.order)
                                            .arg(found->info.constituents)
                              : queued_ocean ? QStringLiteral("等待当前数据任务完成后下载海潮文件…")
                              : running      ? data->status()
                              : !ocean_message.isEmpty()
                                  ? ocean_message
                                  : QStringLiteral("缺少所选海潮文件，请下载或在数据中心导入。"));
        ocean_progress->setVisible(active && running);
        if (running && data->total_bytes() > 0) {
            ocean_progress->setRange(0, 1000);
            ocean_progress->setValue(static_cast<int>(
                std::min<qint64>(1000, data->received_bytes() * 1000 / data->total_bytes())));
        } else {
            ocean_progress->setRange(0, 0);
        }
        ocean_download->setVisible(data && active && !ready && !running && !queued_ocean &&
                                   ocean_model->currentData().toString() == "fes2014bv1");
        ocean_cancel->setVisible(queued_ocean || (owned_ocean && running));
        refresh_gravity_status();
    };
    const auto request_ocean = [&] {
        queued_ocean = false;
        ocean_message.clear();
        const bool needs_download = ocean_use->isChecked() && ocean_degree->value() >= 2 &&
                                    ocean_model->currentData().toString() == "fes2014bv1" &&
                                    selected_ocean() == installed_oceans.end();
        if (!needs_download && data && owned_ocean && data->downloading() &&
            data->active_product() == DataProduct::fes2014bv1)
            data->cancel_download();
        if (needs_download && data) {
            if (data->downloading()) {
                queued_ocean = data->active_product() != DataProduct::fes2014bv1;
            } else {
                data->start_download({}, DataProduct::fes2014bv1);
                owned_ocean = data->downloading();
            }
        }
        refresh_ocean_status();
    };
    QObject::connect(ocean_use, &QCheckBox::toggled, &dialog, request_ocean);
    QObject::connect(ocean_model, &QComboBox::currentIndexChanged, &dialog, request_ocean);
    QObject::connect(ocean_degree, &QSpinBox::valueChanged, &dialog, request_ocean);
    QObject::connect(ocean_download, &QPushButton::clicked, &dialog, request_ocean);
    QObject::connect(ocean_cancel, &QPushButton::clicked, &dialog, [&] {
        queued_ocean = false;
        ocean_message = QStringLiteral("已取消下载，可重试或在数据中心导入。");
        if (data && owned_ocean && data->downloading() &&
            data->active_product() == DataProduct::fes2014bv1)
            data->cancel_download();
        refresh_ocean_status();
    });
    QObject::connect(ocean_manage, &QPushButton::clicked, &dialog, [&] {
        if (data)
            show_data_center(&dialog, *data,
                             ocean_model->currentData().toString() == "fes2014bv1"
                                 ? DataProduct::fes2014bv1
                                 : DataProduct::custom_ocean_tides);
    });
    QTimer gravity_poll;
    QObject::connect(&gravity_poll, &QTimer::timeout, &dialog, [&] {
        if (data && data->revision() != catalog_revision) {
            catalog_revision = data->revision();
            installed_models = data->data().gravity_models();
            installed_ephemerides = data->data().ephemerides();
            installed_oceans = data->data().ocean_tide_models();
            refresh_ocean_models();
            refresh_ephemeris();
            refresh_srp_resource();
            refresh_drag_resource();
            refresh_models();
            update_gravity();
        }
        if (data && !data->downloading()) {
            owned_gravity.reset();
            if (owned_ocean) {
                owned_ocean = false;
                if (selected_ocean() == installed_oceans.end())
                    ocean_message = data->update_status(DataProduct::fes2014bv1).message;
            }
            if (queued_gravity)
                request_gravity();
            if (queued_ocean && !data->downloading())
                request_ocean();
        }
        refresh_ocean_status();
    });
    if (data)
        gravity_poll.start(100);
    refresh_gravity_status();
    QTimer::singleShot(0, &dialog, request_gravity);
    refresh_ocean_status();
    QTimer::singleShot(0, &dialog, request_ocean);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        int validation_page = 0;
        int validation_force_page = 0;
        std::optional<core::ThirdBody> validation_body;
        QWidget *validation_field = model;
        const auto read = [&](QuantityEditor *field, Unit unit) {
            validation_field = field;
            return field->value(unit);
        };
        try {
            auto candidate = options;
            for (auto body : core::third_body_list) {
                const auto &field = fields[static_cast<std::size_t>(body)];
                validation_body = body;
                validation_field = field.source;
                auto &setting = candidate.third_body(body);
                setting.enabled = field.enabled->isChecked();
                setting.source =
                    static_cast<core::ThirdBodyGravitySource>(field.source->currentIndex());
                setting.cb = *field.cb;
                if (setting.enabled && setting.source == core::ThirdBodyGravitySource::cb_file &&
                    !setting.cb)
                    throw std::runtime_error(std::string(core::third_body_name(body)) +
                                             "：请先导入此天体的 Cb 引力常数文件。");
                if (setting.source == core::ThirdBodyGravitySource::user_specified)
                    setting.user_mu = read(field.mu, Unit::metres_cubed_per_second_squared);
                else if (field.manual->si &&
                         (setting.user_mu || field.manual->text != field.default_manual))
                    setting.user_mu = field.manual->si;
            }
            validation_body.reset();
            validation_field = model;
            candidate.gravity_model = model->currentData().toString().toStdString();
            candidate.gravity_mu = core::egm96_mu;
            if (!candidate.gravity_model.empty()) {
                const auto found = std::find_if(
                    installed_models.begin(), installed_models.end(),
                    [&](const auto &entry) { return entry.selection == candidate.gravity_model; });
                if (found == installed_models.end())
                    throw std::runtime_error("请先在数据中心下载或导入所选重力场。");
                candidate.gravity_mu = found->info.mu;
                if (order->value() > found->info.order) {
                    validation_field = order;
                    throw std::runtime_error("所选 Order 超出重力文件的可用范围。");
                }
            }
            candidate.gravity_degree = model->currentIndex() != 1 ? degree->value() : 0;
            candidate.gravity_order = model->currentIndex() != 1 ? order->value() : 0;
            candidate.include_secular_variations = secular_use->isChecked();
            validation_field = secular_use;
            if (candidate.uses_secular_gravity()) {
                const auto found = selected_model();
                if (found == installed_models.end() || !found->info.secular_variations)
                    throw std::runtime_error(
                        "所选重力场缺少长期变化的参考历元与变化率；请导入完整文件或关闭此项。");
            }
            candidate.solid_tides.mode =
                static_cast<core::SolidTideMode>(solid_mode->currentIndex());
            candidate.solid_tides.truncate_to_gravity = solid_truncate->isChecked();
            candidate.solid_tides.include_time_dependent_terms = solid_time->isChecked();
            candidate.ocean_tides = {ocean_use->isChecked(),
                                     ocean_model->currentData().toString().toStdString(),
                                     ocean_degree->value(), ocean_order->value()};
            validation_field = ocean_model;
            if (candidate.ocean_tides.active()) {
                const auto found = selected_ocean();
                if (found == installed_oceans.end())
                    throw std::runtime_error("请先下载或在数据中心导入所选海潮文件。");
                validation_field = ocean_degree;
                if (candidate.ocean_tides.degree > found->info.degree ||
                    candidate.ocean_tides.order > found->info.order)
                    throw std::runtime_error("海潮阶数或次数超出所选文件的可用范围。");
            }
            validation_field = solid_mode;
            if (candidate.uses_full_solid_tides() &&
                std::none_of(installed_ephemerides.begin(), installed_ephemerides.end(),
                             [&](const auto &entry) {
                                 return entry.selection == candidate.ephemeris_model;
                             }))
                throw std::runtime_error("完整固体潮需要太阳和月球星历，请先在数据中心准备。");
            if (candidate.uses_earth_fixed_gravity()) {
                for (const auto &entry : installed_models)
                    if (entry.selection == candidate.gravity_model &&
                        entry.info.tide_system == "mean_tide")
                        throw std::runtime_error("固体潮暂不支持 mean_tide 重力场，请选择 "
                                                 "tide_free 或 zero_tide 文件。");
            }
            validation_force_page = 1;
            auto &drag = candidate.drag;
            drag.enabled = drag_use->isChecked();
            drag.atmosphere = static_cast<core::AtmosphereModel>(density_model->currentIndex());
            drag.low_altitude_model =
                static_cast<core::LowAltitudeDensityModel>(low_density->currentIndex());
            drag.use_apparent_sun_position = drag_apparent->isChecked();
            drag.use_approximate_altitude = drag_approximate->isChecked();
            drag.weather_model = drag_weather_source->currentIndex() == 1
                                     ? weather_file->currentData().toString().toStdString()
                                     : std::string{};
            drag.geomagnetic_update =
                static_cast<core::GeomagneticUpdate>(weather_update->currentIndex());
            drag.geomagnetic_source =
                static_cast<core::GeomagneticSource>(weather_source->currentIndex());
            const auto drag_value = [&](QuantityEditor *field, Unit unit, double &value,
                                        bool used) {
                if (used)
                    value = read(field, unit);
                else {
                    try {
                        value = field->value(unit);
                    } catch (const std::exception &) {
                    }
                }
            };
            drag_value(drag_cd, Unit::one, drag.cd, drag.enabled);
            drag_value(drag_area, Unit::square_metres_per_kilogram, drag.area_mass_ratio_m2_per_kg,
                       drag.enabled);
            drag_value(drag_f107, Unit::one, drag.f107, drag.enabled && drag.weather_model.empty());
            drag_value(drag_average, Unit::one, drag.f107_average,
                       drag.enabled && drag.weather_model.empty());
            drag_value(drag_kp, Unit::one, drag.kp, drag.enabled && drag.weather_model.empty());
            const bool blend = drag.enabled &&
                               drag.atmosphere == core::AtmosphereModel::jacchia_roberts &&
                               drag.low_altitude_model == core::LowAltitudeDensityModel::nrlmsise00;
            drag_value(drag_blend, Unit::metre, drag.blending_range_m, blend);
            validation_field = blend ? static_cast<QWidget *>(drag_blend) : drag_use;
            core::validate_drag_options(drag);
            if (drag.enabled && !std::any_of(installed_ephemerides.begin(),
                                             installed_ephemerides.end(), [&](const auto &entry) {
                                                 return entry.selection ==
                                                        candidate.ephemeris_model;
                                             }))
                throw std::runtime_error("大气阻力需要太阳位置星历，请先在数据中心准备。");
            if (drag.enabled && !drag.weather_model.empty()) {
                validation_field = weather_file;
                const auto available = data ? data->data().weather_sources()
                                            : std::vector<runtime::CatalogSpaceWeather>{};
                if (std::none_of(available.begin(), available.end(), [&](const auto &entry) {
                        return entry.selection == drag.weather_model;
                    }))
                    throw std::runtime_error("缺少空间天气文件，请在数据中心下载或导入。");
            }
            validation_force_page = 2;
            auto &srp = candidate.srp;
            srp.enabled = srp_use->isChecked();
            srp.boundary_mitigation = srp_boundary->isChecked();
            srp.shadow_model = static_cast<core::SrpShadowModel>(srp_shadow->currentIndex());
            srp.sun_position_method =
                static_cast<core::SrpSunPositionMethod>(srp_sun_method->currentIndex());
            srp.earth_shadow = srp_assignments.earth_shadow;
            srp.moon_shadow = srp_assignments.moon_shadow;
            srp.additional_eclipsing_bodies = srp_assignments.additional_eclipsing_bodies;
            const auto srp_value = [&](QuantityEditor *field, Unit unit, double &value, bool used) {
                if (used)
                    value = read(field, unit);
                else {
                    try {
                        value = field->value(unit);
                    } catch (const std::exception &) {
                    }
                }
            };
            srp_value(srp_cr, Unit::one, srp.cr, srp.enabled);
            srp_value(srp_area, Unit::square_metres_per_kilogram, srp.area_mass_ratio_m2_per_kg,
                      srp.enabled);
            srp_value(srp_pressure, Unit::pascal, srp.pressure_at_reference_pa, srp.enabled);
            srp_value(srp_distance, Unit::metre, srp.reference_distance_m, srp.enabled);
            srp_value(srp_sun_radius, Unit::metre, srp.sun_radius_m, srp.enabled);
            const bool shadows = srp.enabled && srp.shadow_model != core::SrpShadowModel::none;
            srp_value(srp_altitude, Unit::metre, srp.atmospheric_altitude_m,
                      shadows && srp.earth_shadow);
            srp_value(srp_earth_radius, Unit::metre, srp.earth_radius_m,
                      shadows && srp.earth_shadow);
            srp_value(srp_moon_radius, Unit::metre, srp.moon_radius_m, shadows && srp.moon_shadow);
            validation_field = srp_use;
            core::validate_srp_options(srp);
            if (srp.enabled && !std::any_of(installed_ephemerides.begin(),
                                            installed_ephemerides.end(), [&](const auto &entry) {
                                                return entry.selection == candidate.ephemeris_model;
                                            }))
                throw std::runtime_error("太阳辐射压需要所选位置星历，请先在数据中心准备。");
            validation_force_page = 3;
            validation_field = satellite_mass;
            candidate.satellite_mass_kg = read(satellite_mass, Unit::kilogram);
            (void)candidate.drag_area_m2();
            (void)candidate.srp_area_m2();
            validation_field = relativity;
            candidate.include_relativistic_accelerations = relativity->isChecked();
            if (candidate.include_relativistic_accelerations &&
                std::none_of(installed_ephemerides.begin(), installed_ephemerides.end(),
                             [&](const auto &entry) {
                                 return entry.selection == candidate.ephemeris_model;
                             }))
                throw std::runtime_error(
                    "相对论加速度需要太阳位置与速度星历，请先在数据中心准备。");
            candidate.integrator = static_cast<core::HpopIntegrator>(method->currentIndex());
            candidate.vop = vop->isChecked();
            candidate.correction = static_cast<core::HpopCorrection>(correction->currentIndex());
            candidate.step_control = static_cast<core::HpopStepControl>(step_control_draft);
            validation_page = 1;
            candidate.time_regularization = regularization->isChecked();
            if (candidate.time_regularization) {
                candidate.regularization_exponent = read(exponent, Unit::one);
                candidate.steps_per_orbit = steps_per_orbit->value();
            }
            candidate.minimum_altitude_m = read(altitude, Unit::metre);
            if (!candidate.uses_fixed_step()) {
                candidate.min_step_seconds = read(minimum, Unit::second);
                candidate.max_step_seconds = read(maximum, Unit::second);
                if (candidate.min_step_seconds > candidate.max_step_seconds)
                    throw std::runtime_error("最大步长不能小于最小步长。");
                candidate.relative_tolerance = read(relative, Unit::one);
                candidate.position_tolerance_m = read(position, Unit::metre);
                candidate.velocity_tolerance_mps = read(velocity, Unit::metres_per_second);
            } else if (!candidate.time_regularization) {
                candidate.fixed_step_seconds = read(fixed, Unit::second);
            }
            const auto vop_mu = read(interpolation_mu, Unit::metres_cubed_per_second_squared);
            validation_field = interpolation_order;
            candidate.interpolation = {
                static_cast<core::OrbitInterpolationMethod>(interpolation->currentIndex()),
                interpolation_order->value(), vop_mu};
            candidate.report_ephemeris_on_fixed_step = fixed_ephemeris->isChecked();
            core::validate_hpop_options(candidate);
            options = candidate;
            dialog.accept();
        } catch (const std::exception &e) {
            tabs->setCurrentIndex(validation_page);
            auto *scroll = dialog.findChild<QScrollArea *>(
                validation_page == 0 ? (validation_force_page == 3   ? "hpopAdditionalPage"
                                        : validation_force_page == 2 ? "hpopSrpPage"
                                        : validation_force_page == 1 ? "hpopDragPage"
                                                                     : "hpopGravityPage")
                                     : "hpopIntegratorPage");
            if (validation_page == 0) {
                force_tabs->setCurrentIndex(validation_force_page);
                if (validation_body) {
                    search->clear();
                    const auto row = static_cast<int>(*validation_body);
                    table->selectRow(row);
                    table->scrollToItem(table->item(row, 0));
                }
            }
            error->setText(QString::fromUtf8(e.what()));
            error->show();
            reveal(validation_field);
            if (scroll)
                scroll->ensureWidgetVisible(validation_field);
            validation_field->setFocus(Qt::OtherFocusReason);
        }
    });
    const auto available = dialog.screen()->availableGeometry().size();
    dialog.resize(std::min(720, available.width() - 32), std::min(700, available.height() - 64));
    const bool accepted = dialog.exec() == QDialog::Accepted;
    gravity_poll.stop();
    // Cancel only work this dialog started; shared data-center tasks keep their owner.
    if (data && owned_gravity && data->downloading() && data->active_product() == *owned_gravity)
        data->cancel_download();
    if (data && owned_ocean && data->downloading() &&
        data->active_product() == DataProduct::fes2014bv1)
        data->cancel_download();
    return accepted;
}
} // namespace orbitforge::desktop
