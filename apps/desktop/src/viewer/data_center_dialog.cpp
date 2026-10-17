#include "eop_catalog.hpp"
#include "form_kit.hpp"
#include "responsive_layout.hpp"
#include "workbench_spin_box.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTimeZone>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace orbitforge::desktop {
namespace {
QString product_name(DataProduct p) {
    switch (p) {
    case DataProduct::finals2000a:
        return QStringLiteral("finals2000A");
    case DataProduct::c04:
        return QStringLiteral("C04");
    case DataProduct::utc:
        return QStringLiteral("UTC / 闰秒");
    case DataProduct::egm2008:
        return QStringLiteral("EGM2008");
    case DataProduct::egm96:
        return QStringLiteral("EGM96");
    case DataProduct::custom_gravity:
        return QStringLiteral("外部重力场");
    case DataProduct::fes2014bv1:
        return QStringLiteral("FES2014bv1 海潮");
    case DataProduct::custom_ocean_tides:
        return QStringLiteral("外部海潮模型");
    case DataProduct::space_weather:
        return QStringLiteral("空间天气 · CelesTrak");
    case DataProduct::de440s:
        return QStringLiteral("DE440s");
    }
    return {};
}

class DataCenterArtwork final : public QWidget {
  public:
    DataCenterArtwork() : artwork_(":/orbitforge/artwork/data-preparation.png") {
        setObjectName("dataCenterArtwork");
        setMinimumHeight(112);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.fillRect(rect(), QColor("#111a30"));
        if (!artwork_.isNull()) {
            // Keep the full head visible; narrow windows crop only the quiet left side.
            const auto ratio = devicePixelRatioF();
            const auto pixels = qRound(height() * ratio);
            if (scaled_artwork_.height() != pixels || scaled_artwork_.devicePixelRatio() != ratio) {
                scaled_artwork_ = artwork_.scaledToHeight(pixels, Qt::SmoothTransformation);
                scaled_artwork_.setDevicePixelRatio(ratio);
            }
            const auto left = width() - scaled_artwork_.width() / ratio;
            painter.drawPixmap(QPointF(left, 0), scaled_artwork_);
            QLinearGradient blend(left, 0, left + height(), 0);
            blend.setColorAt(0, QColor("#111a30"));
            blend.setColorAt(1, QColor(17, 26, 48, 0));
            painter.fillRect(QRectF(left, 0, height(), height()), blend);
        }
        QLinearGradient shade(0, 0, width(), 0);
        shade.setColorAt(0, QColor(15, 23, 40, 245));
        shade.setColorAt(0.5, QColor(15, 23, 40, width() < 620 ? 210 : 100));
        shade.setColorAt(1, QColor(15, 23, 40, width() < 620 ? 90 : 0));
        painter.fillRect(rect(), shade);
    }

  private:
    QPixmap artwork_;
    QPixmap scaled_artwork_;
};

QString installation_text(const DataInventoryItem &item) {
    switch (item.installation) {
    case DataInstallation::missing:
        return item.online_updates ? QStringLiteral("未下载") : QStringLiteral("未导入");
    case DataInstallation::bundled_sample:
        return QStringLiteral("内置示例");
    case DataInstallation::local_file:
        return QStringLiteral("本地文件");
    case DataInstallation::downloaded:
        return QStringLiteral("完整包已下载");
    case DataInstallation::builtin:
        return QStringLiteral("程序内置");
    }
    return {};
}
QString check_text(DataUpdateState state) {
    switch (state) {
    case DataUpdateState::never_checked:
        return QStringLiteral("尚未检查");
    case DataUpdateState::checking:
        return QStringLiteral("正在检查");
    case DataUpdateState::downloading:
        return QStringLiteral("正在下载");
    case DataUpdateState::updated:
        return QStringLiteral("检查成功 · 已下载更新");
    case DataUpdateState::unchanged:
        return QStringLiteral("检查成功 · 无新内容");
    case DataUpdateState::failed:
        return QStringLiteral("检查失败");
    case DataUpdateState::cancelled:
        return QStringLiteral("已取消");
    case DataUpdateState::interrupted:
        return QStringLiteral("上次下载未完成");
    case DataUpdateState::local_changed:
        return QStringLiteral("本地文件已更换 · 待检查");
    }
    return {};
}
QString local_time(qint64 seconds) {
    return QDateTime::fromSecsSinceEpoch(seconds).toLocalTime().toString("yyyy-MM-dd HH:mm");
}
QString coverage_time(const QDateTime &value) {
    if (!value.isValid())
        return QStringLiteral("—");
    return value.toString(value.time() == QTime(0, 0) ? "yyyy-MM-dd" : "yyyy-MM-dd HH:mm:ss");
}
QString bytes_text(quint64 bytes) {
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KiB").arg(static_cast<double>(bytes) / 1024, 0, 'f', 1);
    return QStringLiteral("%1 MiB").arg(static_cast<double>(bytes) / (1024 * 1024), 0, 'f', 1);
}
class DataTreeWidget final : public QTreeWidget {
  protected:
    void drawBranches(QPainter *painter, const QRect &rect,
                      const QModelIndex &index) const override {
        painter->fillRect(rect, theme::color(theme::panel));
        if (!model()->hasChildren(index))
            return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(
            QPen(theme::color(theme::text_muted), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF center(rect.right() - indentation() / 2.0, rect.center().y());
        const auto points = isExpanded(index)
                                ? QPolygonF{center + QPointF(-3, -2), center + QPointF(0, 1),
                                            center + QPointF(3, -2)}
                                : QPolygonF{center + QPointF(-1, -3), center + QPointF(2, 0),
                                            center + QPointF(-1, 3)};
        painter->drawPolyline(points);
        painter->restore();
    }
};
class DataItemDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        // A desktop list row: transparent at rest, a soft wash on hover, and the selected row
        // marked by an accent wash with a leading stripe.
        const auto bounds = QRectF(option.rect).adjusted(2, 2, -2, -2);
        const bool selected = option.state & QStyle::State_Selected;
        paint_list_row(painter, option, bounds, theme::color(theme::accent));
        const bool child = index.parent().isValid();
        const auto icon = child ? WorkbenchIcon::database
                          : index.data(Qt::UserRole).toInt() == static_cast<int>(DataProduct::utc)
                              ? WorkbenchIcon::clock
                              : WorkbenchIcon::earth;
        const auto top = static_cast<int>(bounds.top());
        workbench_icon(icon, theme::color(selected ? theme::accent_bright
                                          : child  ? theme::text_muted
                                                   : theme::accent))
            .paint(painter, QRect(static_cast<int>(bounds.left()) + 12, top + 11, 16, 16));
        const auto left = static_cast<int>(bounds.left()) + 38;
        const auto width = static_cast<int>(bounds.right()) - left - 8;
        const auto lines = index.data().toString().split('\n');
        auto title_font = option.font;
        title_font.setPixelSize(child ? 13 : 12);
        title_font.setWeight(QFont::Normal);
        painter->setFont(title_font);
        painter->setPen(theme::color(selected ? theme::accent_bright
                                     : child  ? theme::text
                                              : theme::text_soft));
        painter->drawText(QRect(left, top + 6, width, 22), Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(lines.value(0), Qt::ElideRight, width));
        title_font.setPixelSize(11);
        painter->setFont(title_font);
        painter->setPen(theme::color(index.data(Qt::UserRole + 1).toBool() ? theme::warning
                                                                           : theme::text_muted));
        painter->drawText(QRect(left, top + 28, width, 17), Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(lines.value(1), Qt::ElideRight, width));
        painter->restore();
    }
};
class PriorityDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const auto bounds = option.rect.adjusted(1, 4, -1, -4);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        painter->setPen(theme::color(selected ? theme::accent : theme::line));
        painter->setBrush(theme::color(selected ? theme::accent_wash
                                       : hover  ? theme::accent_hover
                                                : theme::panel));
        painter->drawRoundedRect(bounds, 6, 6);
        painter->setPen(Qt::NoPen);
        painter->setBrush(theme::color(theme::text_muted));
        for (int x = 0; x < 2; ++x)
            for (int y = 0; y < 3; ++y)
                painter->drawEllipse(
                    QPointF(bounds.left() + 15 + x * 5, bounds.center().y() - 5 + y * 5), 1.2, 1.2);
        auto font = option.font;
        font.setPixelSize(14);
        font.setWeight(QFont::DemiBold);
        painter->setFont(font);
        painter->setPen(theme::color(theme::accent));
        painter->drawText(QRect(bounds.left() + 33, bounds.top(), 24, bounds.height()),
                          Qt::AlignCenter, QString::number(index.row() + 1));
        const auto lines = index.data().toString().split('\n');
        const auto left = bounds.left() + 72;
        const auto width = bounds.width() - 96;
        painter->setPen(theme::color(theme::text));
        painter->drawText(QRect(left, bounds.top() + 12, width, 22), Qt::AlignVCenter,
                          lines.value(0));
        font.setPixelSize(12);
        font.setWeight(QFont::Normal);
        painter->setFont(font);
        painter->setPen(theme::color(theme::text_muted));
        painter->drawText(QRect(left, bounds.top() + 40, width, 20), Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(lines.value(1), Qt::ElideRight, width));
        painter->restore();
    }
};
} // namespace
void show_data_center(QWidget *parent, EopCatalog &catalog, std::optional<DataProduct> initial) {
    QDialog dialog(parent);
    dialog.setObjectName("eopDataDialog");
    dialog.setWindowTitle(QStringLiteral("数据中心"));
    dialog.setWindowIcon(workbench_icon(WorkbenchIcon::database));
    dialog.resize(
        QSize(1020, 720).boundedTo(dialog.screen()->availableGeometry().size() - QSize(48, 72)));
    dialog.setSizeGripEnabled(true);
    dialog.setMinimumSize(360, 360);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *artwork = new DataCenterArtwork;
    auto *heading = new QVBoxLayout(artwork);
    heading->setContentsMargins(24, 16, 24, 14);
    heading->setSpacing(6);
    auto *heading_title = new QLabel(QStringLiteral("数据中心"));
    heading_title->setObjectName("panelTitle");
    auto *heading_row = new QHBoxLayout;
    heading_row->addWidget(heading_title);
    auto *back = new QPushButton(QStringLiteral("数据列表"), &dialog);
    back->setObjectName("dataBackToList");
    back->setIcon(workbench_icon(WorkbenchIcon::database));
    back->hide();
    heading_row->addWidget(back);
    heading_row->addStretch();
    heading->addLayout(heading_row);
    auto *description =
        new QLabel(QStringLiteral("管理计算数据，查看下载时间、覆盖范围与更新结果。"));
    description->setObjectName("dialogDescription");
    description->setWordWrap(true);
    description->setMaximumWidth(600);
    heading->addWidget(description);
    layout->addWidget(artwork);
    auto *panels = new QHBoxLayout;
    panels->setSpacing(0);
    auto *sidebar = new QWidget;
    sidebar->setObjectName("dataSidebar");
    sidebar->setFixedWidth(std::clamp(dialog.width() / 4, 220, 258));
    auto *sidebar_layout = new QVBoxLayout(sidebar);
    sidebar_layout->setContentsMargins(12, 16, 12, 16);
    sidebar_layout->setSpacing(12);
    auto *inventory_caption = new QLabel(QStringLiteral("数据列表"));
    inventory_caption->setObjectName("dataSidebarCaption");
    sidebar_layout->addWidget(inventory_caption);
    auto *list = new DataTreeWidget;
    list->setObjectName("dataInventory");
    list->setAccessibleName(QStringLiteral("数据及安装状态"));
    list->setItemDelegate(new DataItemDelegate(list));
    list->setMouseTracking(true);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setMinimumHeight(100);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setHeaderHidden(true);
    list->setIndentation(16);
    auto *eop_node = new QTreeWidgetItem(list, {QStringLiteral("地球定向参数\n来源与计算优先级")});
    eop_node->setSizeHint(0, QSize(0, 62));
    const auto add_source = [&](DataProduct product, QTreeWidgetItem *parent) {
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(list);
        item->setData(0, Qt::UserRole, static_cast<int>(product));
        item->setSizeHint(0, QSize(0, 62));
        return item;
    };
    auto *finals_node = add_source(DataProduct::finals2000a, eop_node);
    auto *c04_node = add_source(DataProduct::c04, eop_node);
    auto *utc_node = add_source(DataProduct::utc, nullptr);
    auto *gravity_node =
        new QTreeWidgetItem(list, {QStringLiteral("地球重力场\n模型文件与计算资源")});
    gravity_node->setSizeHint(0, QSize(0, 54));
    auto *egm2008_node = add_source(DataProduct::egm2008, gravity_node);
    auto *egm96_node = add_source(DataProduct::egm96, gravity_node);
    auto *custom_node = add_source(DataProduct::custom_gravity, gravity_node);
    gravity_node->setExpanded(true);
    auto *ephemeris_node =
        new QTreeWidgetItem(list, {QStringLiteral("太阳系星历\n第三体引力数据")});
    ephemeris_node->setSizeHint(0, QSize(0, 54));
    auto *de440s_node = add_source(DataProduct::de440s, ephemeris_node);
    auto *weather_node = add_source(DataProduct::space_weather, nullptr);
    auto *ocean_node = new QTreeWidgetItem(list, {QStringLiteral("海潮模型\n归一化势系数")});
    ocean_node->setSizeHint(0, QSize(0, 54));
    auto *fes_node = add_source(DataProduct::fes2014bv1, ocean_node);
    auto *custom_ocean_node = add_source(DataProduct::custom_ocean_tides, ocean_node);
    ocean_node->setExpanded(true);
    ephemeris_node->setExpanded(true);
    eop_node->setExpanded(true);
    list->setCurrentItem(eop_node);
    sidebar_layout->addWidget(list, 1);
    auto *inventory_note = new QLabel(QStringLiteral("所有场景共用这些数据"));
    inventory_note->setObjectName("dataSidebarCaption");
    sidebar_layout->addWidget(inventory_note);
    panels->addWidget(sidebar);
    auto *panel = new QWidget;
    panel->setObjectName("dataDetailPanel");
    panel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto *detail_layout = new QVBoxLayout(panel);
    detail_layout->setContentsMargins(theme::space::page);
    detail_layout->setSpacing(16);
    panels->addWidget(panel, 1);
    layout->addLayout(panels, 1);
    auto *title = new QLabel;
    title->setObjectName("dataName");
    title->setWordWrap(true);
    auto *data_heading = new QHBoxLayout;
    data_heading->addWidget(title, 1);
    auto *state = new QLabel;
    state->setObjectName("dataStatus");
    state->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    data_heading->addWidget(state, 0, Qt::AlignVCenter);
    detail_layout->addLayout(data_heading);
    auto *data_description = new QLabel;
    data_description->setObjectName("dataDescription");
    data_description->setWordWrap(true);
    detail_layout->addWidget(data_description);
    auto *gravity_summary = new QLabel(QStringLiteral(
        "EGM2008 / EGM96 "
        "是静态重力场，无固定有效期，默认按需下载。\n\n完整文件保留全部系数；支持 EGM2008 "
        "完整 2190 × 2159，按文件范围独立选择阶数和次数。外部文件支持静态 ICGEM .gfc 和 STK .grv，"
        "不支持的时变项会明确报错。"));
    gravity_summary->setWordWrap(true);
    gravity_summary->setObjectName("hint");
    detail_layout->addWidget(gravity_summary);
    auto *priority_scroll = new QScrollArea;
    priority_scroll->setWidgetResizable(true);
    priority_scroll->setFrameShape(QFrame::NoFrame);
    priority_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    priority_scroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto *priority_panel = new QWidget;
    priority_scroll->setWidget(priority_panel);
    auto *priority_layout = new QVBoxLayout(priority_panel);
    priority_layout->setContentsMargins(0, 12, 0, 0);
    priority_layout->setSpacing(12);
    auto *priority_heading = new QHBoxLayout;
    auto *priority_caption = new QLabel(QStringLiteral("计算优先级"));
    priority_caption->setObjectName("sectionTitle");
    priority_heading->addWidget(priority_caption);
    priority_heading->addStretch();
    auto *move_up = new QToolButton;
    move_up->setObjectName("eopPriorityUp");
    move_up->setIcon(workbench_icon(WorkbenchIcon::chevron_up));
    move_up->setToolTip(QStringLiteral("上移（Alt+↑）"));
    move_up->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    auto *move_down = new QToolButton;
    move_down->setObjectName("eopPriorityDown");
    move_down->setIcon(workbench_icon(WorkbenchIcon::chevron_down));
    move_down->setToolTip(QStringLiteral("下移（Alt+↓）"));
    move_down->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Down));
    priority_heading->addWidget(move_up);
    priority_heading->addWidget(move_down);
    priority_layout->addLayout(priority_heading);
    auto *priority = new QListWidget;
    priority->setObjectName("eopSourcePriority");
    priority->setAccessibleName(QStringLiteral("地球定向参数计算优先级"));
    priority->setItemDelegate(new PriorityDelegate(priority));
    priority->setMouseTracking(true);
    priority->setCursor(Qt::OpenHandCursor);
    priority->setDragDropMode(QAbstractItemView::InternalMove);
    priority->setDefaultDropAction(Qt::MoveAction);
    priority->setDragDropOverwriteMode(false);
    priority->setSelectionMode(QAbstractItemView::SingleSelection);
    priority->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    priority->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    priority->setFixedHeight(176);
    for (const auto product : {DataProduct::finals2000a, DataProduct::c04}) {
        auto *item = new QListWidgetItem(priority);
        item->setData(Qt::UserRole, static_cast<int>(product));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
        item->setSizeHint(QSize(0, 84));
    }
    priority->setCurrentRow(0);
    priority_layout->addWidget(priority);
    auto *priority_note = new QLabel(QStringLiteral("先按顺序选择能完整覆盖场景时段的数据。\n两种来"
                                                    "源均不足时，再按此顺序尝试允许的有限外推。"));
    priority_note->setObjectName("dataPriorityNote");
    priority_note->setWordWrap(true);
    priority_layout->addWidget(priority_note);
    auto *priority_error = new QLabel;
    priority_error->setObjectName("inputError");
    priority_error->setWordWrap(true);
    priority_error->hide();
    priority_layout->addWidget(priority_error);
    priority_layout->addStretch();
    detail_layout->addWidget(priority_scroll, 1);
    auto *pages = new QTabWidget;
    pages->setObjectName("dataPages");
    pages->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    const auto page = [&](const QString &label) {
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *widget = new QWidget;
        auto *page_layout = new QVBoxLayout(widget);
        page_layout->setContentsMargins(0, 16, 0, 0);
        page_layout->setSpacing(24);
        scroll->setWidget(widget);
        pages->addTab(scroll, label);
        return page_layout;
    };
    auto *overview_layout = page(QStringLiteral("概览"));
    auto *settings_layout = page(QStringLiteral("更新与计算"));
    auto *file_layout = page(QStringLiteral("文件信息"));
    detail_layout->addWidget(pages, 1);
    auto *facts = new QFrame;
    facts->setObjectName("dataFacts");
    auto *form = new QFormLayout(facts);
    form->setContentsMargins(theme::space::panel);
    form->setHorizontalSpacing(20);
    form->setVerticalSpacing(16);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto field = [&](const QString &caption, const char *name) {
        auto *label = new QLabel;
        label->setObjectName(name);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *field_caption = new QLabel(caption);
        field_caption->setProperty("fieldCaption", true);
        form->addRow(field_caption, label);
        return label;
    };
    auto *acquired = field(QStringLiteral("下载 / 导入时间"), "dataAcquiredAt");
    auto *first = field(QStringLiteral("覆盖起点（UTC）"), "dataValidFrom");
    auto *until = field(QStringLiteral("覆盖截止（UTC）"), "dataValidUntil");
    auto *checked = field(QStringLiteral("最近检查"), "dataLastChecked");
    acquired->setToolTip(QStringLiteral("本地时间"));
    checked->setToolTip(QStringLiteral("本地时间"));
    overview_layout->addWidget(facts);
    responsive_form(form, 380);
    auto *note = new QLabel;
    note->setObjectName("dataNote");
    note->setWordWrap(true);
    auto *acquisition = new QWidget;
    acquisition->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *acquisition_layout = new QVBoxLayout(acquisition);
    acquisition_layout->setContentsMargins(0, 0, 0, 0);
    acquisition_layout->setSpacing(12);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(10);
    auto *download = new QPushButton;
    download->setObjectName("downloadEop");
    download->setProperty("primary", true);
    download->setIcon(workbench_icon(WorkbenchIcon::download, Qt::white));
    auto *raw = new QPushButton(QStringLiteral("导入本地文件…"));
    raw->setObjectName("importRawEop");
    raw->setIcon(workbench_icon(WorkbenchIcon::folder));
    auto *manifest = new QPushButton(QStringLiteral("导入数据…"));
    manifest->setObjectName("importEopSnapshot");
    actions->addWidget(download);
    actions->addWidget(raw);
    file_layout->addWidget(manifest, 0, Qt::AlignLeft);
    actions->addStretch();
    acquisition_layout->addLayout(actions);
    auto *planetary_centres = new QPushButton(QStringLiteral("导入行星本体星历…"));
    planetary_centres->setObjectName("importPlanetaryCentres");
    planetary_centres->setIcon(workbench_icon(WorkbenchIcon::folder));
    acquisition_layout->addWidget(planetary_centres, 0, Qt::AlignLeft);
    // Update and extrapolation settings: two inspector groups, each opened by a switch.
    settings_layout->setSpacing(0);
    auto *automatic_row = new FormGroup(QStringLiteral("自动更新"));
    automatic_row->setProperty("leading", true);
    auto *automatic = new QCheckBox(QStringLiteral("自动下载与更新"));
    automatic->setObjectName("dataAutoUpdate");
    automatic->setChecked(catalog.automatic_updates());
    make_switch(automatic);
    auto *frequency = new QComboBox;
    frequency->setObjectName("dataUpdateFrequency");
    frequency->setAccessibleName(QStringLiteral("自动检查频率"));
    frequency->addItem(QStringLiteral("每 6 小时"), 6);
    frequency->addItem(QStringLiteral("每 24 小时"), 24);
    frequency->addItem(QStringLiteral("每 7 天"), 168);
    frequency->setMaximumWidth(276);
    automatic_row->form()->addRow(automatic);
    automatic_row->form()->addRow(QStringLiteral("检查频率"), frequency);
    settings_layout->addWidget(automatic_row);
    auto *schedule = new QLabel;
    schedule->setObjectName("dataUpdateSchedule");
    schedule->setWordWrap(true);
    automatic_row->form()->addRow(schedule);
    auto *progress = new QProgressBar;
    progress->setObjectName("dataDownloadProgress");
    progress->setTextVisible(false);
    progress->setFixedHeight(5);
    acquisition_layout->addWidget(progress);
    overview_layout->addWidget(acquisition);
    overview_layout->addWidget(note);
    overview_layout->addStretch();
    auto *policy_title = new FormGroup(QStringLiteral("覆盖范围之外"));
    settings_layout->addWidget(policy_title);
    auto *extrapolation = policy_title->body();
    auto *allow = new QCheckBox(QStringLiteral("允许有限外推"));
    allow->setObjectName("allowEopExtrapolation");
    make_switch(allow);
    auto *days = new WorkbenchSpinBox<QSpinBox>;
    days->setObjectName("eopExtrapolationDays");
    days->setAccessibleName(QStringLiteral("EOP 前后各允许外推的天数"));
    days->setProperty("stepButtons", true);
    days->setMinimumWidth(90);
    days->setRange(1, core::max_eop_extrapolation_days);
    days->setValue(3);
    days->setSuffix(QStringLiteral(" 天"));
    days->setMaximumWidth(160);
    policy_title->form()->addRow(allow);
    auto *limit_caption = new QLabel(QStringLiteral("前后各最多"));
    policy_title->form()->addRow(limit_caption, days);
    auto *extrapolation_note = policy_title->add_caution(
        QStringLiteral("端点线性估算，距离覆盖时段越远，误差越大。闰秒有效期不会随外推延长。"));
    align_form_labels(policy_title->parentWidget());
    settings_layout->addStretch();
    auto *technical = new QLabel;
    technical->setObjectName("dataInventoryDetails");
    technical->setTextFormat(Qt::PlainText);
    technical->setWordWrap(true);
    technical->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    technical->setTextInteractionFlags(Qt::TextSelectableByMouse);
    file_layout->addWidget(technical);
    file_layout->addStretch();
    auto *warnings = new QLabel(catalog.warnings().join('\n'), &dialog);
    warnings->setObjectName("inputError");
    warnings->setContentsMargins(24, 8, 24, 8);
    warnings->setTextFormat(Qt::PlainText);
    warnings->setWordWrap(true);
    warnings->setVisible(!warnings->text().isEmpty());
    layout->addWidget(warnings);
    auto *status = new QLabel;
    status->setObjectName("dataCenterStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    acquisition_layout->addWidget(status);
    auto *footer_panel = new QWidget;
    footer_panel->setObjectName("dataFooter");
    auto *footer = new QHBoxLayout(footer_panel);
    footer->setContentsMargins(theme::space::footer);
    auto *usage = new QLabel(QStringLiteral("打开或重算时检查数据，数据变化后需确认。"));
    usage->setWordWrap(true);
    usage->setObjectName("hint");
    footer->addWidget(usage, 1);
    auto *cancel = new QPushButton(QStringLiteral("取消下载"));
    cancel->setObjectName("cancelEopDownload");
    actions->insertWidget(3, cancel);
    auto *close = new QPushButton(QStringLiteral("关闭"));
    close->setMinimumWidth(88);
    footer->addWidget(close);
    layout->addWidget(footer_panel);
    responsive_columns(acquisition, actions, 380);
    bool narrow = false, showing_details = false;
    const auto show_panels = [&] {
        sidebar->setVisible(!narrow || !showing_details);
        panel->setVisible(!narrow || showing_details);
        back->setVisible(narrow && showing_details);
        description->setVisible(!narrow || !showing_details);
    };
    new WidthObserver(&dialog, [&, mode = WidthMode{}](int width) mutable {
        narrow = mode.narrow(width, 760);
        sidebar->setMinimumWidth(narrow ? 0 : std::clamp(width / 4, 220, 258));
        sidebar->setMaximumWidth(narrow ? QWIDGETSIZE_MAX : sidebar->minimumWidth());
        detail_layout->setContentsMargins(narrow ? 18 : 30, narrow ? 18 : 26, narrow ? 18 : 30, 24);
        show_panels();
    });
    QObject::connect(back, &QPushButton::clicked, &dialog, [&] {
        showing_details = false;
        show_panels();
        list->setFocus();
    });
    const auto open_details = [&] {
        showing_details = true;
        show_panels();
        if (narrow)
            back->setFocus();
    };
    QObject::connect(list, &QTreeWidget::itemClicked, &dialog, open_details);
    QObject::connect(list, &QTreeWidget::itemActivated, &dialog, open_details);
    auto entries = catalog.inventory();
    QString operation_error;
    bool syncing_priority = false;
    const auto selected = [&]() -> std::optional<DataProduct> {
        return list->currentItem() == eop_node || list->currentItem() == gravity_node ||
                       list->currentItem() == ephemeris_node || list->currentItem() == ocean_node
                   ? std::nullopt
                   : std::optional{static_cast<DataProduct>(
                         list->currentItem()->data(0, Qt::UserRole).toInt())};
    };
    const auto inventory_item = [&](DataProduct product) -> const DataInventoryItem & {
        return *std::find_if(entries.begin(), entries.end(),
                             [&](const auto &entry) { return entry.product == product; });
    };
    const auto selected_item = [&]() -> const DataInventoryItem & {
        return inventory_item(*selected());
    };
    const auto update_details = [&] {
        const bool group = !selected();
        gravity_summary->setVisible(list->currentItem() == gravity_node ||
                                    list->currentItem() == ephemeris_node ||
                                    list->currentItem() == ocean_node);
        priority_scroll->setVisible(group);
        pages->setVisible(!group);
        state->setVisible(!group);
        facts->setVisible(!group);
        note->setVisible(!group);
        technical->setVisible(!group);
        if (group) {
            if (list->currentItem() == ephemeris_node) {
                priority_scroll->hide();
                title->setText(QStringLiteral("太阳系星历"));
                data_description->setText(
                    QStringLiteral("统一管理 HPOP 使用的太阳、月球位置数据。"));
                gravity_summary->setText(QStringLiteral(
                    "JPL DE440s\n\n完整文件约 32 MiB，安装后支持 1850—2150 年的日月与行星引力计算。"
                    "\n\n选择左侧 DE440s 下载或导入文件，然后在 HPOP 的“力模型 → "
                    "引力”页启用相应第三体。"
                    "\n\n星历覆盖不使用外推。每次计算固定数据内容，更新后重新计算需确认。"));
                return;
            }
            if (list->currentItem() == ocean_node) {
                priority_scroll->hide();
                title->setText(QStringLiteral("海潮模型"));
                data_description->setText(QStringLiteral("管理 HPOP 使用的海潮引力系数。"));
                gravity_summary->setText(QStringLiteral(
                    "FES2014bv1\n\n官方公开势系数文件约 10.7 MiB，包含 36 个显式分潮，最高 100 × "
                    "100。"
                    "\n\n在 HPOP 的“引力 → 海潮”中启用，并独立设置阶数和次数。"
                    "适用时自动包含海洋极潮。\n\n外部模型使用相同的 FES 归一化势系数格式。"));
                return;
            }
            if (list->currentItem() == gravity_node) {
                gravity_summary->setText(QStringLiteral(
                    "EGM2008 / EGM96\n\n下载完整球谐系数文件，或导入 GFC / GRV 重力场。"
                    "\n\n在 HPOP 中选择模型与阶次；每次计算固定所选数据。静态模型无固定有效期。"));
                priority_scroll->hide();
                title->setText(QStringLiteral("地球重力场"));
                data_description->setText(QStringLiteral(
                    "下载完整模型，或导入自己的重力文件。在 HPOP 设置中选择使用的模型和阶次。"));
                return;
            }
            title->setText(QStringLiteral("地球定向参数"));
            data_description->setText(
                QStringLiteral("拖动数据来源调整计算顺序，排在上方的优先使用。"));
            acquisition->hide();
            policy_title->hide();
            extrapolation->hide();
            return;
        }
        const auto &item = selected_item();
        const bool is_time = item.product == DataProduct::utc;
        const bool is_gravity = gravity_product(item.product);
        const bool is_ocean = ocean_product(item.product);
        const bool is_ephemeris = item.product == DataProduct::de440s;
        acquisition->show();
        policy_title->setVisible(item.extrapolation);
        extrapolation->setVisible(item.extrapolation);
        extrapolation_note->setVisible(item.extrapolation);
        download->setVisible(item.online_updates);
        raw->setVisible(item.raw_import);
        manifest->setVisible(item.manifest_import || is_ephemeris);
        manifest->setText(is_ephemeris ? QStringLiteral("导入星历数据包…")
                                       : QStringLiteral("导入带清单的数据包…"));
        planetary_centres->setVisible(is_ephemeris);
        automatic_row->setVisible(item.online_updates);
        schedule->setVisible(item.online_updates);
        const QSignalBlocker auto_block(automatic), frequency_block(frequency), allow_block(allow),
            days_block(days);
        automatic->setChecked(catalog.automatic_updates(item.product));
        schedule->setText(automatic->isChecked()
                              ? QStringLiteral("在应用运行时检查；尚未安装完整文件时会自动下载。")
                              : QStringLiteral("自动更新已关闭，可随时手动下载或检查。"));
        const auto hours = catalog.update_interval_hours(item.product);
        if (frequency->findData(hours) < 0)
            frequency->addItem(QStringLiteral("每 %1 小时").arg(hours), hours);
        frequency->setCurrentIndex(frequency->findData(hours));
        frequency->setEnabled(automatic->isChecked());
        if (item.provider_index) {
            const auto limit = catalog.extrapolation_days(*item.provider_index);
            allow->setChecked(limit > 0);
            days->setValue(limit > 0 ? limit : 3);
            days->setEnabled(limit > 0);
            limit_caption->setEnabled(limit > 0);
        }
        title->setText(product_name(item.product));
        data_description->setText(
            item.product == DataProduct::space_weather
                ? QStringLiteral(
                      "F10.7 / Kp / Ap · 原始记录范围；计算另检查模型滞后、历史窗口和缺值")
            : is_ocean     ? QStringLiteral("海潮势系数 · HPOP / 机动滑行共用")
            : is_gravity   ? QStringLiteral("地球静态球谐重力场 · HPOP / 机动滑行共用")
            : is_ephemeris ? QStringLiteral("JPL 行星星历 · 太阳系第三体引力")
            : is_time      ? QStringLiteral("UTC、TAI 与 TT 之间的时间转换")
            : item.product == DataProduct::finals2000a
                ? QStringLiteral("快速观测与预测 · 适用于近期和未来时段")
                : QStringLiteral("综合参考序列 · 历史时段的默认首选"));
        state->setText(installation_text(item));
        state->setProperty("tone", item.installation == DataInstallation::bundled_sample ||
                                           item.installation == DataInstallation::missing
                                       ? "warning"
                                       : "ready");
        state->style()->unpolish(state);
        state->style()->polish(state);
        acquired->setText(item.acquired_at ? local_time(*item.acquired_at)
                          : item.acquisition == runtime::DataAcquisition::bundled
                              ? QStringLiteral("内置，随程序附带")
                          : item.installation == DataInstallation::missing
                              ? QStringLiteral("尚未下载或导入")
                              : QStringLiteral("未记录（已有本地文件）"));
        qobject_cast<QLabel *>(form->labelForField(first))
            ->setText(is_gravity || is_ocean ? QStringLiteral("可用阶 × 次")
                      : is_ephemeris         ? QStringLiteral("覆盖起点（TAI）")
                                             : QStringLiteral("覆盖起点（UTC）"));
        first->setText(is_ocean     ? (item.ocean_tides ? QStringLiteral("%1 × %2")
                                                          .arg(item.ocean_tides->degree)
                                                          .arg(item.ocean_tides->order)
                                                        : QStringLiteral("下载或导入后显示"))
                       : is_gravity ? (item.gravity ? QStringLiteral("%1 × %2")
                                                          .arg(item.gravity->degree)
                                                          .arg(item.gravity->order)
                                                    : QStringLiteral("下载或导入后显示"))
                                    : coverage_time(item.first));
        until->setText(is_gravity || is_ocean
                           ? QStringLiteral("无固定有效期")
                           : coverage_time(item.last) +
                                 (item.exclusive_end ? QStringLiteral("\n不含当日") : QString{}));
        qobject_cast<QLabel *>(form->labelForField(until))
            ->setText(is_gravity || is_ocean ? QStringLiteral("有效期")
                      : is_ephemeris         ? QStringLiteral("覆盖截止（TAI）")
                      : is_time              ? QStringLiteral("有效截止（UTC）")
                                             : QStringLiteral("覆盖截止（UTC）"));
        if (is_gravity)
            note->setText(
                item.gravity
                    ? QStringLiteral(
                          "%1 · %2\nGM = %3 m³/s² · 参考半径 %4 "
                          "m · 固体潮 k20 = %5\n计算开始时固定文件；更新不会改变正在运行的计算。")
                              .arg(QString::fromStdString(item.gravity->model),
                                   QString::fromStdString(item.gravity->tide_system))
                              .arg(item.gravity->mu, 0, 'g', 15)
                              .arg(item.gravity->radius_m, 0, 'g', 15)
                              .arg(item.gravity->solid_tide_k20, 0, 'g', 15) +
                          (item.gravity->secular_variations
                               ? QStringLiteral(
                                     "\n长期变化：参考历元 JD %1 TT，%2 项变化率 / 儒略年")
                                     .arg(2451545.0 +
                                              (item.gravity->secular_variations->reference_epoch
                                                   .seconds_since(core::Epoch{0}) +
                                               32.184) /
                                                  86400,
                                          0, 'f', 9)
                                     .arg(item.gravity->secular_variations->rates_per_year.size())
                               : QStringLiteral("\n文件未定义长期变化。"))
                : item.product == DataProduct::egm2008
                    ? QStringLiteral(
                          "官方完整文件约 240 MiB，下载后在后台校验。静态模型默认不自动下载。")
                    : QStringLiteral("导入或下载完整重力场后，可在 HPOP 中选择模型与阶次。"));
        else if (is_ocean)
            note->setText(
                item.ocean_tides
                    ? QStringLiteral(
                          "%1 · %2 个显式分潮\n计算开始时固定文件；更新不会改变正在运行的计算。")
                          .arg(QString::fromStdString(item.ocean_tides->model))
                          .arg(item.ocean_tides->constituents)
                    : QStringLiteral("下载官方 FES2014bv1，或导入外部 FES 势系数文件。"));
        else if (is_ephemeris)
            note->setText(QStringLiteral(
                "完整 DE440s 约 32 MiB · NAIF 官方文件\n"
                "HPOP 使用地心几何位置与 DE440 配套 GM；日月、行星和冥王星可独立启用。"
                "覆盖为可计算时段，静态星历不按下载日期到期，也不作外推。"));
        else if (is_time)
            note->setText(QDateTime::currentDateTimeUtc() >= item.last
                              ? QStringLiteral("闰秒数据已到期，请检查更新后再计算后续时段。")
                              : QStringLiteral("有效期来自 IERS 公告；到期后可在此联网更新。"));
        else if (item.installation == DataInstallation::bundled_sample)
            note->setText(
                item.online_updates
                    ? QStringLiteral("当前只有内置示例片段，完整包尚未下载。")
                    : QStringLiteral("当前只有内置示例片段，可导入覆盖所需时段的 C04 数据。"));
        else if (item.installation == DataInstallation::missing)
            note->setText(item.online_updates
                              ? QStringLiteral("下载完整文件后，可查看实际计算覆盖范围。")
                              : QStringLiteral("导入本地 C04 数据后，可查看计算覆盖范围。"));
        else
            note->setText(QStringLiteral("以上为可计算时段，不含外推；历史数据可持续使用。"));
        const auto location = is_time && item.file.empty() ? QStringLiteral("内嵌在程序中")
                              : item.file.empty()          ? QStringLiteral("尚无本地文件")
                                                  : QString::fromStdWString(item.file.wstring());
        technical->setText(
            QStringLiteral("来源：%1\n文件：%2\n大小：%3\nSHA-256：%4%5")
                .arg(item.source_url.isEmpty() ? QStringLiteral("本地导入") : item.source_url,
                     location, item.file_size ? bytes_text(*item.file_size) : QStringLiteral("—"),
                     item.sha256.isEmpty() ? QStringLiteral("—") : item.sha256,
                     item.identity_sha256.isEmpty()
                         ? QString{}
                         : QStringLiteral("\n数据身份：") + item.identity_sha256));
    };
    const auto show_error = [&](const std::exception &error) {
        operation_error = QString::fromUtf8(error.what());
    };
    const auto select_provider = [&](int index) {
        for (const auto &entry : entries)
            if (entry.provider_index == static_cast<std::size_t>(index)) {
                eop_node->setExpanded(true);
                list->setCurrentItem(entry.product == DataProduct::finals2000a ? finals_node
                                                                               : c04_node);
                return;
            }
    };
    const auto rebuild = [&] {
        entries = catalog.inventory();
        update_details();
    };
    const auto selection_changed = [&] {
        operation_error.clear();
        pages->setCurrentIndex(0);
        qobject_cast<QScrollArea *>(pages->widget(0))->verticalScrollBar()->setValue(0);
        update_details();
    };
    QObject::connect(list, &QTreeWidget::currentItemChanged, &dialog, selection_changed);
    QObject::connect(priority->model(), &QAbstractItemModel::rowsMoved, &dialog, [&] {
        move_up->setEnabled(priority->currentRow() > 0);
        move_down->setEnabled(priority->currentRow() >= 0 &&
                              priority->currentRow() < priority->count() - 1);
        if (syncing_priority)
            return;
        operation_error.clear();
        try {
            catalog.set_preferred_product(
                static_cast<DataProduct>(priority->item(0)->data(Qt::UserRole).toInt()));
        } catch (const std::exception &error) {
            show_error(error);
        }
    });
    const auto move_priority = [&](int offset) {
        const auto row = priority->currentRow();
        const auto target = row + offset;
        if (row >= 0 && target >= 0 && target < priority->count())
            priority->model()->moveRow({}, row, {}, target > row ? target + 1 : target);
    };
    QObject::connect(move_up, &QToolButton::clicked, &dialog, [&] { move_priority(-1); });
    QObject::connect(move_down, &QToolButton::clicked, &dialog, [&] { move_priority(1); });
    QObject::connect(download, &QPushButton::clicked, &dialog, [&] {
        operation_error.clear();
        catalog.start_download({}, *selected());
    });
    QObject::connect(manifest, &QPushButton::clicked, &dialog, [&] {
        const auto file = QFileDialog::getOpenFileName(&dialog,
                                                       selected() == DataProduct::de440s
                                                           ? QStringLiteral("导入星历数据包")
                                                           : QStringLiteral("导入 EOP 数据"),
                                                       {}, QStringLiteral("数据清单 (*.json)"));
        if (file.isEmpty())
            return;
        operation_error.clear();
        try {
            if (selected() == DataProduct::de440s) {
                catalog.import_ephemeris_snapshot(std::filesystem::path(file.toStdWString()));
                rebuild();
            } else {
                const auto index =
                    catalog.import_snapshot(std::filesystem::path(file.toStdWString()));
                rebuild();
                select_provider(static_cast<int>(index));
            }
        } catch (const std::exception &error) {
            show_error(error);
        }
    });
    QObject::connect(planetary_centres, &QPushButton::clicked, &dialog, [&] {
        const auto file = QFileDialog::getOpenFileName(&dialog, QStringLiteral("导入行星本体星历"),
                                                       {}, QStringLiteral("SPICE 星历 (*.bsp)"));
        if (file.isEmpty())
            return;
        operation_error.clear();
        try {
            catalog.import_planetary_centres(std::filesystem::path(file.toStdWString()));
            rebuild();
        } catch (const std::exception &error) {
            show_error(error);
        }
    });
    QObject::connect(raw, &QPushButton::clicked, &dialog, [&] {
        const auto file = QFileDialog::getOpenFileName(
            &dialog, QStringLiteral("导入 %1 文件").arg(title->text()), {},
            ocean_product(*selected()) ? QStringLiteral("海潮势系数 (*.txt *.pot);;所有文件 (*)")
            : *selected() == DataProduct::space_weather
                ? QStringLiteral("空间天气 (*.csv *.txt *.fxm *.dat);;所有文件 (*)")
            : *selected() == DataProduct::de440s ? QStringLiteral("DE440s 星历 (de440s.bsp *.bsp)")
            : gravity_product(*selected()) ? QStringLiteral("重力场 (*.gfc *.grv);;所有文件 (*)")
                                           : QStringLiteral("所有文件 (*)"));
        if (file.isEmpty())
            return;
        operation_error.clear();
        try {
            catalog.import_raw(std::filesystem::path(file.toStdWString()), *selected());
            rebuild();
            if (catalog.last_installed())
                select_provider(static_cast<int>(*catalog.last_installed()));
        } catch (const std::exception &error) {
            show_error(error);
        }
    });
    QObject::connect(automatic, &QCheckBox::toggled, &dialog, [&](bool value) {
        try {
            catalog.set_automatic_updates(value, *selected());
        } catch (const std::exception &error) {
            show_error(error);
        }
        update_details();
    });
    QObject::connect(frequency, &QComboBox::currentIndexChanged, &dialog, [&] {
        try {
            catalog.set_update_interval_hours(frequency->currentData().toInt(), *selected());
        } catch (const std::exception &error) {
            show_error(error);
        }
        update_details();
    });
    const auto save_extrapolation = [&] {
        if (!selected() || !selected_item().provider_index)
            return;
        try {
            catalog.set_extrapolation_days(*selected_item().provider_index,
                                           allow->isChecked() ? days->value() : 0);
        } catch (const std::exception &error) {
            show_error(error);
        }
        update_details();
    };
    QObject::connect(allow, &QCheckBox::toggled, &dialog, save_extrapolation);
    QObject::connect(days, &QSpinBox::valueChanged, &dialog, save_extrapolation);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, [&] { catalog.cancel_download(); });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    QTimer timer;
    std::uint64_t seen_revision = 0;
    QDate seen_day;
    const auto refresh = [&] {
        const auto today = QDateTime::currentDateTimeUtc().date();
        if (seen_revision != catalog.revision() || seen_day != today) {
            rebuild();
            seen_revision = catalog.revision();
            seen_day = today;
        }
        const bool busy = catalog.downloading();
        for (auto *node : {finals_node, c04_node, utc_node, egm2008_node, egm96_node, custom_node,
                           de440s_node, weather_node, fes_node, custom_ocean_node}) {
            const auto &entry =
                inventory_item(static_cast<DataProduct>(node->data(0, Qt::UserRole).toInt()));
            const auto name = product_name(entry.product);
            const auto update = catalog.update_status(entry.product);
            const bool failed = update.state == DataUpdateState::failed ||
                                update.state == DataUpdateState::interrupted;
            const auto installation = (busy && catalog.active_product() == entry.product) || failed
                                          ? check_text(update.state)
                                          : installation_text(entry);
            node->setText(
                0, name + "\n" + installation + "\n" +
                       (ocean_product(entry.product)     ? QStringLiteral("海潮系数 · 无固定有效期")
                        : gravity_product(entry.product) ? QStringLiteral("静态模型 · 无固定有效期")
                        : entry.last.isValid()
                            ? (entry.product == DataProduct::utc ? QStringLiteral("有效至 ")
                                                                 : QStringLiteral("覆盖至 ")) +
                                  entry.last.date().toString(Qt::ISODate)
                            : QStringLiteral("尚无覆盖数据")));
            node->setData(0, Qt::UserRole + 1, entry.online_updates && failed);
            node->setToolTip(0, node->text(0));
        }
        if (priority->item(0)->data(Qt::UserRole).toInt() !=
            static_cast<int>(catalog.preferred_product())) {
            syncing_priority = true;
            priority->model()->moveRow({}, 1, {}, 0);
            syncing_priority = false;
        }
        for (int row = 0; row < priority->count(); ++row) {
            auto *entry_item = priority->item(row);
            const auto &entry =
                inventory_item(static_cast<DataProduct>(entry_item->data(Qt::UserRole).toInt()));
            const auto name = entry.product == DataProduct::finals2000a
                                  ? QStringLiteral("finals2000A")
                                  : QStringLiteral("C04");
            entry_item->setText(name + "\n" + installation_text(entry) +
                                (entry.first.isValid() ? QStringLiteral(" · %1 — %2 UTC")
                                                             .arg(coverage_time(entry.first),
                                                                  coverage_time(entry.last))
                                                       : QStringLiteral(" · 尚无覆盖数据")));
        }
        move_up->setEnabled(priority->currentRow() > 0);
        move_down->setEnabled(priority->currentRow() >= 0 &&
                              priority->currentRow() < priority->count() - 1);
        priority_error->setText(operation_error);
        priority_error->setVisible(!operation_error.isEmpty());
        if (!selected())
            return;
        const auto &item = selected_item();
        const auto update = catalog.update_status(item.product);
        const bool downloading_this = busy && catalog.active_product() == item.product;
        checked->setText(
            item.online_updates
                ? (update.attempted_at ? local_time(*update.attempted_at) + "\n" : QString{}) +
                      check_text(update.state)
            : item.product == DataProduct::utc ? QStringLiteral("随程序更新")
                                               : QStringLiteral("本地导入，不联网检查"));
        download->setText(downloading_this ? check_text(update.state)
                          : update.state == DataUpdateState::failed ||
                                  update.state == DataUpdateState::interrupted
                              ? QStringLiteral("重试下载 / 更新")
                          : item.installation == DataInstallation::missing ||
                                  item.installation == DataInstallation::bundled_sample
                              ? QStringLiteral("下载完整数据")
                              : QStringLiteral("检查并更新"));
        download->setEnabled(!busy);
        raw->setEnabled(!busy);
        planetary_centres->setEnabled(!busy);
        manifest->setEnabled(!busy);
        cancel->setVisible(downloading_this);
        progress->setVisible(downloading_this);
        if (busy && catalog.total_bytes() > 0) {
            progress->setRange(0, 1000);
            progress->setValue(static_cast<int>(
                std::min<qint64>(1000, catalog.received_bytes() * 1000 / catalog.total_bytes())));
        } else {
            progress->setRange(0, 0);
        }
        schedule->setText(automatic->isChecked()
                              ? QStringLiteral("在应用运行时检查；尚未安装完整文件时会自动下载。")
                              : QStringLiteral("自动更新已关闭，可随时手动下载或检查。"));
        QString message = operation_error;
        if (message.isEmpty() &&
            (item.online_updates || gravity_product(item.product) || ocean_product(item.product))) {
            if (downloading_this) {
                message = update.message;
                if (catalog.received_bytes() > 0)
                    message +=
                        "  " + bytes_text(static_cast<quint64>(catalog.received_bytes())) +
                        (catalog.total_bytes() > 0
                             ? " / " + bytes_text(static_cast<quint64>(catalog.total_bytes()))
                             : QString{});
            } else if (update.state == DataUpdateState::failed ||
                       update.state == DataUpdateState::interrupted ||
                       update.state == DataUpdateState::cancelled ||
                       update.state == DataUpdateState::local_changed) {
                message = update.message;
                if (update.checked_at)
                    message += QStringLiteral(" 上次成功检查：") + local_time(*update.checked_at);
            }
        }
        status->setText(message);
        const bool warning =
            !operation_error.isEmpty() ||
            (item.online_updates && (update.state == DataUpdateState::failed ||
                                     update.state == DataUpdateState::interrupted));
        if (status->property("warning").toBool() != warning) {
            status->setProperty("warning", warning);
            status->style()->unpolish(status);
            status->style()->polish(status);
        }
        status->setVisible(!status->text().isEmpty());
    };
    QObject::connect(list, &QTreeWidget::currentItemChanged, &dialog, refresh);
    QObject::connect(priority, &QListWidget::currentRowChanged, &dialog, [&] {
        move_up->setEnabled(priority->currentRow() > 0);
        move_down->setEnabled(priority->currentRow() >= 0 &&
                              priority->currentRow() < priority->count() - 1);
    });
    QObject::connect(download, &QPushButton::clicked, &dialog, refresh);
    QObject::connect(raw, &QPushButton::clicked, &dialog, refresh);
    QObject::connect(manifest, &QPushButton::clicked, &dialog, refresh);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, refresh);
    QObject::connect(&timer, &QTimer::timeout, &dialog, refresh);
    timer.start(150);
    refresh();
    if (initial) {
        for (auto *node : {finals_node, c04_node, utc_node, egm2008_node, egm96_node, custom_node,
                           de440s_node, weather_node, fes_node, custom_ocean_node})
            if (node->data(0, Qt::UserRole).toInt() == static_cast<int>(*initial)) {
                list->setCurrentItem(node);
                list->itemActivated(node, 0);
                break;
            }
    }
    dialog.exec();
}
} // namespace orbitforge::desktop
