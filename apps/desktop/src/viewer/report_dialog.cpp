#include "report_dialog.hpp"
#include "epoch_editor.hpp"
#include "object_selection.hpp"
#include "report_plot.hpp"
#include "report_selection.hpp"
#include "responsive_layout.hpp"
#include "workbench_style.hpp"
#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QTabBar>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <future>
#include <map>
#include <orbitforge/project.hpp>

namespace orbitforge::desktop {
namespace {
using namespace application;
bool same_time(const std::shared_ptr<const core::ITimeProvider> &a,
               const std::shared_ptr<const core::ITimeProvider> &b) {
    return a && b ? time_data_lock(a->data_info()) == time_data_lock(b->data_info())
                  : bool(a) == bool(b);
}
constexpr auto builtin_count = static_cast<int>(report_kinds.size());
class ReportSelectionButton final : public QPushButton {
  public:
    using QPushButton::QPushButton;
    QSize sizeHint() const override { return {240, QPushButton::sizeHint().height()}; }
    QSize minimumSizeHint() const override {
        return {120, QPushButton::minimumSizeHint().height()};
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QStyleOptionButton option;
        initStyleOption(&option);
        option.text = fontMetrics().elidedText(text(), Qt::ElideRight, std::max(0, width() - 70));
        QStylePainter painter(this);
        painter.drawControl(QStyle::CE_PushButton, option);
        painter.drawText(QRect(width() - 28, 0, 20, height()), Qt::AlignCenter,
                         QStringLiteral("…"));
    }
};
class ReportTitle final : public QLabel {
  public:
    using QLabel::QLabel;

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};
class StepSpinBox final : public QDoubleSpinBox {
  public:
    using QDoubleSpinBox::QDoubleSpinBox;

  protected:
    QString textFromValue(double value) const override {
        auto display = locale();
        display.setNumberOptions(QLocale::OmitGroupSeparator);
        auto value_text = display.toString(value, 'f', 9);
        while (value_text.endsWith(display.zeroDigit()))
            value_text.chop(display.zeroDigit().size());
        if (value_text.endsWith(display.decimalPoint()))
            value_text.chop(display.decimalPoint().size());
        return value_text;
    }
};
QString text(const std::string &value) { return QString::fromStdString(value); }
QString kind_name(ReportKind kind) {
    switch (kind) {
    case ReportKind::scenario_summary:
        return QStringLiteral("场景摘要");
    case ReportKind::object_summary:
        return QStringLiteral("对象摘要");
    case ReportKind::cartesian_state:
        return QStringLiteral("位置与速度");
    case ReportKind::ground_stations:
        return QStringLiteral("地面站坐标");
    case ReportKind::classical_elements:
        return QStringLiteral("经典轨道根数");
    case ReportKind::geodetic_position:
        return QStringLiteral("经纬高");
    case ReportKind::orbital_motion:
        return QStringLiteral("轨道距离与速率");
    case ReportKind::access_intervals:
        return QStringLiteral("可见时段 · Access");
    case ReportKind::aer:
        return QStringLiteral("AER · 方位 / 仰角 / 距离");
    }
    return {};
}
QString localized(QString value) {
    static const std::map<QString, QString> labels{
        {"Property", QStringLiteral("属性")},
        {"Value", QStringLiteral("值")},
        {"Object ID", QStringLiteral("对象 ID")},
        {"Object", QStringLiteral("对象")},
        {"Type", QStringLiteral("类型")},
        {"Model", QStringLiteral("模型")},
        {"Time", QStringLiteral("时间")},
        {"Frame", QStringLiteral("坐标系")},
        {"Longitude", QStringLiteral("经度")},
        {"Geodetic latitude", QStringLiteral("大地纬度")},
        {"Ellipsoid height", QStringLiteral("椭球高")},
        {"Semi-major axis", QStringLiteral("半长轴")},
        {"Eccentricity", QStringLiteral("偏心率")},
        {"Inclination", QStringLiteral("轨道倾角")},
        {"RAAN", QStringLiteral("升交点赤经")},
        {"Argument of periapsis", QStringLiteral("近地点幅角")},
        {"True anomaly", QStringLiteral("真近点角")},
        {"Geocentric distance", QStringLiteral("地心距离")},
        {"Speed", QStringLiteral("速率")},
        {"Radial speed", QStringLiteral("径向速率")},
        {"Transverse speed", QStringLiteral("横向速率")},
        {"Flight path angle", QStringLiteral("航迹倾角")},
        {"Scenario", QStringLiteral("场景")},
        {"Start", QStringLiteral("开始")},
        {"Stop", QStringLiteral("结束")},
        {"Time scale", QStringLiteral("时间系统")},
        {"Satellites", QStringLiteral("卫星数量")},
        {"Ground stations", QStringLiteral("地面站数量")},
        {"Central body", QStringLiteral("中心天体")},
        {"Earth", QStringLiteral("地球")},
        {"Satellite", QStringLiteral("卫星")},
        {"Ground station", QStringLiteral("地面站")},
        {"Duration", QStringLiteral("持续时间")},
        {"Start clipped", QStringLiteral("开始于分析边界")},
        {"Stop clipped", QStringLiteral("结束于分析边界")},
        {"Azimuth", QStringLiteral("方位角")},
        {"Elevation", QStringLiteral("仰角")},
        {"Range", QStringLiteral("斜距")},
        {"Range rate", QStringLiteral("距离变化率")},
        {"Visible", QStringLiteral("可见（1 / 0）")},
        {"Off boresight", QStringLiteral("离轴角")},
        {"Two body", QStringLiteral("二体")},
        {"J2 secular", QStringLiteral("J2 长期项")},
        {"WGS84 fixed", QStringLiteral("WGS84 固定站")}};
    const auto match = labels.find(value);
    return match == labels.end() ? value : match->second;
}
class ReportModel final : public QAbstractTableModel {
  public:
    explicit ReportModel(QObject *parent) : QAbstractTableModel(parent) {}
    std::shared_ptr<const runtime::ReportResult> result;
    void publish(std::shared_ptr<const runtime::ReportResult> next) {
        beginResetModel();
        result = std::move(next);
        endResetModel();
    }
    int rowCount(const QModelIndex &parent = {}) const override {
        return !parent.isValid() && result ? static_cast<int>(result->table.rows().size()) : 0;
    }
    int columnCount(const QModelIndex &parent = {}) const override {
        return !parent.isValid() && result ? static_cast<int>(result->table.columns().size()) : 0;
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!result || !index.isValid())
            return {};
        const auto &cell = result->table.rows().at(index.row()).at(index.column());
        if (role == Qt::TextAlignmentRole &&
            (std::holds_alternative<double>(cell) || std::holds_alternative<std::int64_t>(cell)))
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
            return {};
        auto value = text(result->cell_text(index.row(), index.column()));
        if (role == Qt::DisplayRole && std::holds_alternative<double>(cell))
            return QString::number(value.toDouble(), 'g', 12);
        if (role == Qt::DisplayRole && std::holds_alternative<core::Epoch>(cell)) {
            if (value.endsWith('Z'))
                value.chop(1);
            while (value.endsWith('0'))
                value.chop(1);
            if (value.endsWith('.'))
                value.chop(1);
            return value.replace('T', ' ');
        }
        const auto &id = result->table.columns().at(index.column()).id;
        if (id == "object.type" || id == "object.model" || id == "summary.field" ||
            (id == "summary.value" && index.row() == 6))
            return localized(value);
        return value;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (!result || role != Qt::DisplayRole)
            return {};
        if (orientation == Qt::Vertical)
            return section + 1;
        const auto title = text(result->table.columns().at(section).title);
        return localized(title) + text(result->column_title(section)).mid(title.size());
    }
};
QLabel *note(const QString &value, QWidget *parent) {
    auto *label = new QLabel(value, parent);
    label->setWordWrap(true);
    label->setObjectName("hint");
    return label;
}
} // namespace

struct ReportDialog::Impl {
    ReportDialog *owner;
    application::Project project;
    Prepare prepare;
    Save save;
    std::function<void(core::Epoch)> seek;
    std::shared_ptr<const core::ITimeProvider> time;
    QPushButton *library;
    QPointer<ReportSelectionDialog> report_chooser;
    QWidget *settings, *sampling, *custom_times, *settings_panel, *result_panel;
    QWidget *units_field, *scale_field, *frame_field;
    QGridLayout *plot_tools;
    QToolButton *reset_plot;
    QSplitter *split;
    QStackedWidget *result_stack;
    QCheckBox *follow;
    QComboBox *frame, *units, *scale;
    ObjectSelectionField *access_station, *access_target;
    QWidget *access_pair;
    EpochEditor *first, *last;
    QDoubleSpinBox *step;
    QLabel *status, *estimate, *result_label, *result_title, *scope_label, *scope_detail;
    QLabel *interval_label, *template_description, *format_title, *step_label;
    QPushButton *generate, *export_csv, *cancel, *scope_button, *view_button;
    QToolButton *refresh_button, *config_menu, *result_more;
    QAction *source_button, *copy_button, *store, *update_template, *remove;
    QProgressBar *progress;
    QTableView *table;
    QTabBar *views;
    QWidget *plot_page;
    ReportPlot *plot;
    QComboBox *plot_field;
    ObjectSelectionField *plot_object;
    QLabel *plot_note;
    ReportModel *model;
    ReportDefinition draft;
    std::vector<std::string> scope_ids;
    bool all_objects = true;
    struct GeneratedReport {
        runtime::Analysis analysis;
        runtime::ReportResult report;
    };
    std::future<GeneratedReport> job;
    std::optional<runtime::Analysis> result_analysis;
    std::uint64_t input_generation = 0, job_generation = 0;
    std::future<void> export_job;
    std::shared_ptr<std::atomic_bool> cancellation;
    std::shared_ptr<std::atomic_size_t> completed;
    bool waiting = false, loading = false, changed = false;
    bool compact = false, wide_settings = true, narrow_settings = true;
    WidthMode panel_mode;
    QList<int> wide_sizes{330, 790};
    std::size_t expected = 0;
    std::uint64_t request = 0;

    Impl(ReportDialog *parent, application::Project input,
         std::shared_ptr<const core::ITimeProvider> clock, Prepare preflight, Save persist,
         std::function<void(core::Epoch)> jump)
        : owner(parent), project(std::move(input)), prepare(std::move(preflight)),
          save(std::move(persist)), seek(std::move(jump)), time(std::move(clock)) {
        owner->setObjectName("reportDialog");
        owner->setWindowTitle(QStringLiteral("报告"));
        owner->setWindowIcon(workbench_icon(WorkbenchIcon::properties));
        owner->setMinimumSize(320, 320);
        owner->resize(1120, 740);
        auto *root = new QVBoxLayout(owner);
        root->setSizeConstraint(QLayout::SetNoConstraint);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        auto *heading = new QWidget(owner);
        heading->setObjectName("reportHeading");
        auto *heading_layout = new QHBoxLayout(heading);
        heading_layout->setContentsMargins(24, 12, 24, 12);
        auto *title = new QLabel(QStringLiteral("报告与图表"), heading);
        title->setObjectName("reportHeadingTitle");
        heading_layout->addWidget(title);
        heading_layout->addStretch();
        view_button = new QPushButton(QStringLiteral("收起设置"), heading);
        view_button->setObjectName("reportToggleSettings");
        heading_layout->addWidget(view_button);
        root->addWidget(heading);
        split = new QSplitter(owner);
        split->setObjectName("reportSplit");
        split->setChildrenCollapsible(false);
        split->setHandleWidth(4);
        root->addWidget(split, 1);
        settings_panel = new QWidget(split);
        settings_panel->setObjectName("reportSettingsPanel");
        auto *panel_layout = new QVBoxLayout(settings_panel);
        panel_layout->setContentsMargins(0, 0, 0, 0);
        panel_layout->setSpacing(0);
        auto *scroll = new QScrollArea(settings_panel);
        scroll->setObjectName("reportSettingsScroll");
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFrameShape(QFrame::NoFrame);
        panel_layout->addWidget(scroll, 1);
        settings = new QWidget(scroll);
        settings->setObjectName("reportSettings");
        scroll->setWidget(settings);
        auto *config = new QVBoxLayout(settings);
        config->setContentsMargins(theme::space::panel);
        config->setSpacing(10);
        const auto section = [&](const QString &label) {
            if (config->count())
                config->addSpacing(10);
            auto *caption = new QLabel(label, settings);
            caption->setObjectName("reportSectionTitle");
            config->addWidget(caption);
        };
        section(QStringLiteral("对象范围"));
        auto *scope_row = new QHBoxLayout;
        scope_label = new QLabel(settings);
        scope_label->setObjectName("reportScopeLabel");
        scope_label->setWordWrap(true);
        scope_label->setTextFormat(Qt::PlainText);
        scope_row->addWidget(scope_label, 1);
        scope_button = new QPushButton(QStringLiteral("选择对象…"), settings);
        scope_button->setObjectName("reportChangeScope");
        scope_row->addWidget(scope_button);
        config->addLayout(scope_row);
        scope_detail = note({}, settings);
        scope_detail->setObjectName("reportScopeDetail");
        config->addWidget(scope_detail);
        access_pair = new QWidget(settings);
        auto *pair_form = new QFormLayout(access_pair);
        pair_form->setContentsMargins(0, 0, 0, 0);
        pair_form->setVerticalSpacing(12);
        pair_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        access_station = new ObjectSelectionField(access_pair);
        access_station->set_options({QStringLiteral("选择可见性分析的地面站"), false, {}});
        access_station->setAccessibleName(QStringLiteral("可见性分析地面站"));
        access_station->setObjectName("accessStation");
        access_target = new ObjectSelectionField(access_pair);
        access_target->set_options({QStringLiteral("选择可见性分析的卫星或传感器"), false, {}});
        access_target->setAccessibleName(QStringLiteral("可见性分析目标"));
        access_target->setObjectName("accessTarget");
        pair_form->addRow(QStringLiteral("地面站"), access_station);
        pair_form->addRow(QStringLiteral("卫星 / 传感器"), access_target);
        config->addWidget(access_pair);
        auto *template_heading = new QHBoxLayout;
        auto *template_title = new QLabel(QStringLiteral("报告类型"), settings);
        template_title->setObjectName("reportSectionTitle");
        template_heading->addWidget(template_title);
        template_heading->addStretch();
        config_menu = new QToolButton(settings);
        config_menu->setObjectName("reportConfigMenu");
        config_menu->setText(QStringLiteral("模板"));
        config_menu->setToolTip(QStringLiteral("将当前对象、类型和采样设置保存为项目模板"));
        config_menu->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(config_menu);
        store = menu->addAction(QStringLiteral("保存为模板…"));
        store->setObjectName("saveReport");
        update_template = menu->addAction(QStringLiteral("更新当前模板"));
        update_template->setObjectName("updateReport");
        menu->addSeparator();
        remove = menu->addAction(QStringLiteral("删除当前模板"));
        remove->setObjectName("deleteReport");
        config_menu->setMenu(menu);
        template_heading->addWidget(config_menu);
        config->addSpacing(14);
        config->addLayout(template_heading);
        library = new ReportSelectionButton(settings);
        library->setObjectName("reportLibrary");
        library->setProperty("objectSelector", true);
        library->setAutoDefault(false);
        library->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        library->setAccessibleName(QStringLiteral("选择报告"));
        config->addWidget(library);
        template_description = note({}, settings);
        template_description->setObjectName("reportTemplateDescription");
        config->addWidget(template_description);
        sampling = new QWidget(settings);
        auto *sampling_layout = new QVBoxLayout(sampling);
        sampling_layout->setContentsMargins(0, 8, 0, 0);
        sampling_layout->setSpacing(10);
        auto *sampling_title = new QLabel(QStringLiteral("时间与采样"), sampling);
        sampling_title->setObjectName("reportSectionTitle");
        sampling_layout->addWidget(sampling_title);
        follow = new QCheckBox(QStringLiteral("使用场景时段"), sampling);
        follow->setObjectName("reportFollowScenario");
        sampling_layout->addWidget(follow);
        interval_label = note({}, sampling);
        interval_label->setObjectName("reportInterval");
        sampling_layout->addWidget(interval_label);
        custom_times = new QWidget(sampling);
        auto *time_layout = new QVBoxLayout(custom_times);
        time_layout->setContentsMargins(0, 0, 0, 0);
        time_layout->setSpacing(6);
        first = new EpochEditor(custom_times, time);
        first->setObjectName("reportFirst");
        last = new EpochEditor(custom_times, time);
        last->setObjectName("reportLast");
        time_layout->addWidget(note(QStringLiteral("开始"), custom_times));
        time_layout->addWidget(first);
        time_layout->addWidget(note(QStringLiteral("结束"), custom_times));
        time_layout->addWidget(last);
        for (auto *editor : {first, last})
            editor->findChild<QLineEdit *>("epochValue")->setMinimumWidth(180);
        sampling_layout->addWidget(custom_times);
        auto *sampling_form = new QFormLayout;
        sampling_form->setHorizontalSpacing(12);
        sampling_form->setVerticalSpacing(10);
        sampling_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        step = new StepSpinBox(sampling);
        step->setObjectName("reportStep");
        step->setDecimals(9);
        step->setRange(0.000000001, 86400);
        step->setValue(60);
        step->setSuffix(QStringLiteral(" 秒"));
        step->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        step->setToolTip(QStringLiteral("报告独立采样，始终包含起止时刻。"));
        step_label = new QLabel(QStringLiteral("采样间隔"));
        sampling_form->addRow(step_label, step);
        frame = new QComboBox(sampling);
        frame->setObjectName("reportFrame");
        frame->addItems({"GCRF", "ITRF"});
        frame_field = new QWidget(sampling);
        auto *frame_form = new QFormLayout(frame_field);
        frame_form->setContentsMargins(0, 0, 0, 0);
        frame_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        frame_form->addRow(QStringLiteral("坐标系"), frame);
        sampling_layout->addLayout(sampling_form);
        sampling_layout->addWidget(frame_field);
        config->addWidget(sampling);
        section(QStringLiteral("显示格式"));
        format_title = qobject_cast<QLabel *>(config->itemAt(config->count() - 1)->widget());
        units_field = new QWidget(settings);
        auto *units_form = new QFormLayout(units_field);
        units_form->setContentsMargins(0, 0, 0, 0);
        units_form->setHorizontalSpacing(12);
        units_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        units = new QComboBox(units_field);
        units->setObjectName("reportUnits");
        units->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        units->setMinimumContentsLength(8);
        units->addItems({QStringLiteral("km / km·s⁻¹ / °"), QStringLiteral("m / m·s⁻¹ / rad")});
        units_form->addRow(QStringLiteral("数值单位"), units);
        config->addWidget(units_field);
        scale_field = new QWidget(settings);
        auto *scale_form = new QFormLayout(scale_field);
        scale_form->setContentsMargins(0, 0, 0, 0);
        scale_form->setHorizontalSpacing(12);
        scale_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        scale = new QComboBox(scale_field);
        scale->setObjectName("reportTimeScale");
        scale->addItems({"UTC", "TAI", "TT"});
        scale_form->addRow(QStringLiteral("时间系统"), scale);
        for (auto *form : {sampling_form, frame_form, units_form, scale_form}) {
            form->setHorizontalSpacing(12);
            if (auto *label = form->itemAt(0, QFormLayout::LabelRole)->widget())
                label->setFixedWidth(
                    settings->fontMetrics().horizontalAdvance(QStringLiteral("采样间隔")));
        }
        config->addWidget(scale_field);
        config->addStretch();
        auto *actions = new QWidget(settings_panel);
        actions->setObjectName("reportGenerateActions");
        auto *actions_layout = new QVBoxLayout(actions);
        actions_layout->setContentsMargins(16, 12, 16, 16);
        actions_layout->setSpacing(10);
        estimate = note({}, actions);
        estimate->setObjectName("reportEstimate");
        actions_layout->addWidget(estimate);
        generate = new QPushButton(QStringLiteral("生成报告"), actions);
        generate->setObjectName("generateReport");
        generate->setProperty("primary", true);
        generate->setDefault(true);
        actions_layout->addWidget(generate);
        cancel = new QPushButton(QStringLiteral("取消生成"), actions);
        cancel->setObjectName("cancelReport");
        cancel->hide();
        actions_layout->addWidget(cancel);
        panel_layout->addWidget(actions);
        result_panel = new QWidget(split);
        result_panel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        result_panel->setObjectName("reportResultPanel");
        auto *result_layout = new QVBoxLayout(result_panel);
        result_layout->setContentsMargins(16, 16, 16, 12);
        result_layout->setSpacing(10);
        auto *result_heading = new QHBoxLayout;
        result_title = new ReportTitle(QStringLiteral("报告结果"), result_panel);
        result_title->setObjectName("reportResultTitle");
        result_title->setTextFormat(Qt::PlainText);
        result_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        result_heading->addWidget(result_title, 1);
        refresh_button = new QToolButton(result_panel);
        refresh_button->setObjectName("refreshReport");
        refresh_button->setText(QStringLiteral("↻"));
        refresh_button->setAccessibleName(QStringLiteral("刷新报告"));
        refresh_button->setToolTip(QStringLiteral("按当前设置重新生成（F5）"));
        refresh_button->setShortcut(QKeySequence(Qt::Key_F5));
        refresh_button->setEnabled(false);
        result_heading->addWidget(refresh_button);
        export_csv = new QPushButton(QStringLiteral("导出 CSV…"), result_panel);
        export_csv->setObjectName("exportReportCsv");
        export_csv->setEnabled(false);
        result_heading->addWidget(export_csv);
        result_more = new QToolButton(result_panel);
        result_more->setObjectName("reportResultMenu");
        result_more->setText(QStringLiteral("···"));
        result_more->setToolTip(QStringLiteral("复制与数据来源"));
        result_more->setPopupMode(QToolButton::InstantPopup);
        result_more->setEnabled(false);
        auto *result_menu = new QMenu(result_more);
        copy_button = result_menu->addAction(QStringLiteral("复制选中行"));
        copy_button->setObjectName("reportCopy");
        copy_button->setEnabled(false);
        source_button = result_menu->addAction(QStringLiteral("查看数据来源…"));
        source_button->setObjectName("reportShowSource");
        source_button->setEnabled(false);
        result_more->setMenu(result_menu);
        result_heading->addWidget(result_more);
        result_layout->addLayout(result_heading);
        result_label = note(QStringLiteral("等待生成"), result_panel);
        result_label->setTextFormat(Qt::PlainText);
        result_label->setObjectName("reportResultLabel");
        auto *result_meta = new QHBoxLayout;
        result_meta->addWidget(result_label, 1);
        views = new QTabBar(result_panel);
        views->setObjectName("reportViews");
        views->setExpanding(false);
        views->setDrawBase(false);
        views->addTab(QStringLiteral("表格"));
        views->addTab(QStringLiteral("曲线"));
        views->setTabEnabled(1, false);
        views->setTabToolTip(1, QStringLiteral("生成按时间采样的报告后，可查看曲线。"));
        result_meta->addWidget(views);
        result_layout->addLayout(result_meta);
        result_stack = new QStackedWidget(result_panel);
        result_layout->addWidget(result_stack, 1);
        auto *empty = new QWidget(result_stack);
        empty->setObjectName("reportEmptyState");
        auto *empty_layout = new QVBoxLayout(empty);
        empty_layout->setContentsMargins(20, 20, 20, 20);
        empty_layout->addStretch();
        empty_layout->setSpacing(6);
        auto *empty_icon = new QLabel(empty);
        empty_icon->setObjectName("reportEmptyIcon");
        empty_icon->setFixedSize(52, 52);
        empty_icon->setAlignment(Qt::AlignCenter);
        empty_icon->setPixmap(
            workbench_icon(WorkbenchIcon::properties, theme::color(theme::accent)).pixmap(24, 24));
        empty_layout->addWidget(empty_icon, 0, Qt::AlignHCenter);
        empty_layout->addSpacing(8);
        auto *empty_title = new QLabel(QStringLiteral("还没有报告结果"), empty);
        empty_title->setObjectName("reportEmptyTitle");
        empty_title->setAlignment(Qt::AlignCenter);
        empty_layout->addWidget(empty_title);
        auto *empty_note = note(QStringLiteral("选择对象和报告类型，点击“生成报告”。"), empty);
        empty_note->setAlignment(Qt::AlignCenter);
        empty_layout->addWidget(empty_note);
        empty_layout->addStretch();
        result_stack->addWidget(empty);
        table = new QTableView(result_stack);
        table->setObjectName("reportTable");
        table->setMinimumSize(0, 0);
        model = new ReportModel(table);
        table->setModel(model);
        table->setAlternatingRowColors(true);
        table->setShowGrid(false);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->verticalHeader()->setDefaultSectionSize(28);
        table->horizontalHeader()->setMinimumSectionSize(70);
        table->horizontalHeader()->setResizeContentsPrecision(50);
        auto *copy = new QAction(QStringLiteral("复制所选行"), table);
        copy->setObjectName("copyReportRows");
        copy->setShortcut(QKeySequence::Copy);
        copy->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        table->addAction(copy);
        table->setContextMenuPolicy(Qt::ActionsContextMenu);
        QObject::connect(copy, &QAction::triggered, owner, [this] { copy_rows(); });
        QObject::connect(copy_button, &QAction::triggered, owner, [this] {
            if (views->currentIndex() == 1) {
                QApplication::clipboard()->setImage(plot->image());
                status->setText(QStringLiteral("曲线图已复制。"));
            } else
                copy_rows();
        });
        QObject::connect(source_button, &QAction::triggered, owner, [this] { show_source(); });
        QObject::connect(refresh_button, &QToolButton::clicked, owner, [this] { start(); });
        QObject::connect(table->selectionModel(), &QItemSelectionModel::selectionChanged, owner,
                         [this] {
                             copy_button->setEnabled(views->currentIndex() == 1 ||
                                                     table->selectionModel()->hasSelection());
                         });
        result_stack->addWidget(table);
        plot_page = new QWidget(result_stack);
        auto *plot_layout = new QVBoxLayout(plot_page);
        plot_layout->setContentsMargins(0, 0, 0, 0);
        plot_layout->setSpacing(8);

        plot_field = new QComboBox(plot_page);
        plot_field->setObjectName("reportPlotField");
        plot_field->setAccessibleName(QStringLiteral("曲线字段"));
        plot_object = new ObjectSelectionField(plot_page);
        plot_object->setObjectName("reportPlotObject");
        plot_object->setAccessibleName(QStringLiteral("曲线对象"));
        plot_field->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        plot_field->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        plot_field->setMinimumContentsLength(6);
        plot_field->setToolTip(QStringLiteral("选择绘制的数值字段"));
        plot_object->setToolTip(QStringLiteral("选择比较全部对象或只看单个对象"));
        plot_tools = new QGridLayout;
        plot_tools->setContentsMargins(0, 0, 0, 0);
        plot_tools->setHorizontalSpacing(10);
        plot_tools->setColumnStretch(0, 1);
        plot_tools->setColumnStretch(1, 1);
        plot_tools->addWidget(plot_field, 0, 0);
        plot_tools->addWidget(plot_object, 0, 1);
        reset_plot = new QToolButton(plot_page);
        reset_plot->setObjectName("reportPlotReset");
        reset_plot->setIcon(workbench_icon(WorkbenchIcon::fit));
        reset_plot->setToolTip(QStringLiteral("还原完整范围（Home）"));
        reset_plot->setAccessibleName(QStringLiteral("还原曲线范围"));
        plot_tools->addWidget(reset_plot, 0, 2);
        new WidthObserver(
            plot_page, [this, mode = WidthMode{}, previous = false](int width) mutable {
                const bool small = mode.narrow(width, 500);
                if (small == previous)
                    return;
                previous = small;
                plot_tools->removeWidget(plot_field);
                plot_tools->removeWidget(plot_object);
                plot_tools->removeWidget(reset_plot);
                plot_tools->addWidget(plot_field, 0, 0, 1, small ? 2 : 1);
                plot_tools->addWidget(plot_object, small ? 1 : 0, small ? 0 : 1, 1, small ? 2 : 1);
                plot_tools->addWidget(reset_plot, 0, 2);
            });
        plot_layout->addLayout(plot_tools);
        plot = new ReportPlot(plot_page);
        plot->seek = [this](core::Epoch epoch) {
            if (epoch.tai_nanoseconds() >= project.first.tai_nanoseconds() &&
                epoch.tai_nanoseconds() <= project.last.tai_nanoseconds() && seek)
                seek(epoch);
        };
        plot_layout->addWidget(plot, 1);
        plot_note =
            note(QStringLiteral("滚轮 / 框选缩放 · 右键还原 · 双击采样点定位场景时刻"), plot_page);
        plot_layout->addWidget(plot_note);
        result_stack->addWidget(plot_page);
        QObject::connect(reset_plot, &QToolButton::clicked, owner, [this] { plot->reset_view(); });
        QObject::connect(plot_field, &QComboBox::currentIndexChanged, owner, [this] {
            if (plot_field->currentIndex() >= 0)
                plot->set_field(static_cast<std::size_t>(plot_field->currentData().toInt()),
                                plot_field->currentText());
        });
        QObject::connect(plot_object, &ObjectSelectionField::selectionChanged, owner,
                         [this] { plot->set_object(plot_object->id()); });
        QObject::connect(views, &QTabBar::currentChanged, owner, [this] { select_view(); });
        split->setStretchFactor(0, 0);
        split->setStretchFactor(1, 1);
        split->setSizes({330, 790});
        QObject::connect(split, &QSplitter::splitterMoved, owner, [this] {
            if (!compact && wide_settings)
                wide_sizes = split->sizes();
        });
        progress = new QProgressBar(owner);
        progress->setObjectName("reportProgress");
        progress->setMaximumHeight(4);
        progress->setTextVisible(false);
        progress->hide();
        root->addWidget(progress);
        status = note(QStringLiteral("就绪"), owner);
        status->setObjectName("reportStatus");
        status->setContentsMargins(24, 6, 24, 6);
        root->addWidget(status);
        QObject::connect(library, &QPushButton::clicked, owner, [this] { choose_report(); });
        QObject::connect(scope_button, &QPushButton::clicked, owner, [this] { choose_scope(); });
        QObject::connect(view_button, &QPushButton::clicked, owner, [this] {
            if (compact)
                narrow_settings = !narrow_settings;
            else
                wide_settings = !wide_settings;
            adapt(owner->width());
        });
        QObject::connect(follow, &QCheckBox::toggled, owner, [this] {
            update_interval();
            edit();
        });
        for (auto *combo : {frame, units, scale})
            QObject::connect(combo, &QComboBox::currentIndexChanged, owner, [this] { edit(); });
        for (auto *field : {access_station, access_target})
            QObject::connect(field, &ObjectSelectionField::selectionChanged, owner, [this] {
                if (!loading && report_is_access(draft.query.kind)) {
                    scope_ids = {access_station->id(), access_target->id()};
                    all_objects = false;
                    refresh_scope();
                    edit();
                }
            });
        QObject::connect(scale, &QComboBox::currentIndexChanged, owner, [this](int index) {
            first->set_scale(static_cast<CalendarScale>(index));
            last->set_scale(static_cast<CalendarScale>(index));
            update_interval();
        });
        QObject::connect(step, &QDoubleSpinBox::valueChanged, owner, [this] { edit(); });
        for (auto *editor : {first, last})
            for (auto *line : editor->findChildren<QLineEdit *>())
                QObject::connect(line, &QLineEdit::editingFinished, owner, [this] { edit(); });
        QObject::connect(generate, &QPushButton::clicked, owner, [this] { start(); });
        QObject::connect(store, &QAction::triggered, owner, [this] { this->persist(true); });
        QObject::connect(update_template, &QAction::triggered, owner, [this] { this->persist(); });
        QObject::connect(remove, &QAction::triggered, owner, [this] { erase(); });
        QObject::connect(cancel, &QPushButton::clicked, owner, [this] {
            if (cancellation)
                cancellation->store(true);
            if (waiting) {
                waiting = false;
                ++request;
                set_busy(false);
                status->setText(QStringLiteral("已取消报告生成。"));
            }
        });
        QObject::connect(export_csv, &QPushButton::clicked, owner, [this] { export_file(); });
        QObject::connect(
            table, &QTableView::doubleClicked, owner, [this](const QModelIndex &index) {
                if (!model->result)
                    return;
                for (const auto &cell : model->result->table.rows().at(index.row()))
                    if (const auto *epoch = std::get_if<core::Epoch>(&cell)) {
                        if (epoch->tai_nanoseconds() < project.first.tai_nanoseconds() ||
                            epoch->tai_nanoseconds() > project.last.tai_nanoseconds())
                            status->setText(
                                QStringLiteral("该结果时间已超出当前场景，请刷新报告。"));
                        else
                            seek(*epoch);
                        break;
                    }
            });
        auto *timer = new QTimer(owner);
        timer->setInterval(80);
        QObject::connect(timer, &QTimer::timeout, owner, [this] { poll(); });
        timer->start();
        fill_library();
        choose(0);
        adapt(owner->width());
    }
    ~Impl() {
        if (cancellation)
            cancellation->store(true);
    }
    void fill_library() {
        const bool saved = std::ranges::any_of(
            project.reports, [this](const auto &definition) { return definition.id == draft.id; });
        library->setText(saved ? text(draft.name) : kind_name(draft.query.kind));
        library->setIcon(workbench_icon(saved ? WorkbenchIcon::save : WorkbenchIcon::properties));
        library->setToolTip(library->text() + QStringLiteral("\n选择报告…"));
        library->setAccessibleDescription(library->toolTip());
        remove->setEnabled(saved);
        update_template->setEnabled(saved);
        store->setText(saved ? QStringLiteral("另存为模板…") : QStringLiteral("保存为模板…"));
        refresh_scope();
    }
    QString report_scope(const std::vector<std::string> &ids, bool all) const {
        if (all)
            return QStringLiteral("整个场景中的适用对象");
        QStringList names;
        const auto objects = project_object_choices(project);
        for (const auto &id : ids) {
            const auto found = std::ranges::find(objects, id, &ObjectChoice::id);
            names << (found == objects.end() ? text(id) + QStringLiteral("（已删除）")
                                             : found->path);
        }
        return names.join(QStringLiteral("、"));
    }
    std::vector<ReportChoice> report_choices() const {
        const auto description = [](ReportKind kind) -> QString {
            switch (kind) {
            case ReportKind::scenario_summary:
                return QStringLiteral("场景时段、对象数量与中心天体。");
            case ReportKind::object_summary:
                return QStringLiteral("对象名称、类型与计算模型。");
            case ReportKind::cartesian_state:
                return QStringLiteral("按指定间隔输出位置与速度，可选 GCRF 或 ITRF。");
            case ReportKind::ground_stations:
                return QStringLiteral("WGS84 经度、大地纬度、椭球高与 ITRF 坐标。");
            case ReportKind::classical_elements:
                return QStringLiteral("由 GCRF 状态换算瞬时六根数；J2 / J4 "
                                      "结果不是平均根数。");
            case ReportKind::geodetic_position:
                return QStringLiteral("ITRF / WGS84 "
                                      "经度、大地纬度和椭球高。高度不表示离地距离。");
            case ReportKind::orbital_motion:
                return QStringLiteral("GCRF 地心距离、总速率、径向与横向速率、航迹倾角。");
            case ReportKind::access_intervals:
                return QStringLiteral("地面站与卫星 / "
                                      "传感器的可见时段，考虑地球遮挡、最低仰角与传感器视场。");
            case ReportKind::aer:
                return QStringLiteral("地面站观测目标的方位角、仰角、斜距与可见标记。");
            }
            return {};
        };
        const auto output = [](ReportKind kind) -> QString {
            switch (kind) {
            case ReportKind::scenario_summary:
                return QStringLiteral("属性表");
            case ReportKind::object_summary:
                return QStringLiteral("对象表");
            case ReportKind::ground_stations:
                return QStringLiteral("站点坐标表");
            case ReportKind::access_intervals:
                return QStringLiteral("可见时间区间表");
            default:
                return QStringLiteral("时间序列 · 表格与曲线");
            }
        };
        const auto icon = [](ReportKind kind) {
            switch (kind) {
            case ReportKind::scenario_summary:
                return WorkbenchIcon::earth;
            case ReportKind::object_summary:
                return WorkbenchIcon::satellite;
            case ReportKind::ground_stations:
                return WorkbenchIcon::ground_station;
            case ReportKind::geodetic_position:
                return WorkbenchIcon::map;
            case ReportKind::access_intervals:
                return WorkbenchIcon::clock;
            case ReportKind::aer:
                return WorkbenchIcon::focus;
            default:
                return WorkbenchIcon::orbit;
            }
        };
        std::vector<ReportChoice> choices;
        for (auto kind : report_kinds)
            choices.push_back(
                {QStringLiteral("builtin:%1").arg(static_cast<int>(kind)),
                 kind_name(kind),
                 kind_name(kind),
                 description(kind),
                 report_is_access(kind)
                     ? QStringLiteral("地面站 → 卫星 / 传感器；保留当前对象，随后选择配对对象")
                     : report_scope(scope_ids, all_objects),
                 false,
                 compatible(kind) ? QString{} : QStringLiteral("不适用于当前对象范围"),
                 output(kind),
                 {},
                 icon(kind)});
        for (const auto &definition : project.reports) {
            QString warning;
            const auto objects = project_object_choices(project);
            for (const auto &id : definition.query.object_ids)
                if (std::ranges::find(objects, id, &ObjectChoice::id) == objects.end())
                    warning =
                        QStringLiteral("\n绑定的对象已删除；载入后需重新选择对象，才能生成报告。");
            choices.push_back(
                {"template:" + text(definition.id),
                 text(definition.name),
                 kind_name(definition.query.kind),
                 description(definition.query.kind),
                 report_scope(definition.query.object_ids,
                              definition.query.scope == ReportScope::all_compatible),
                 true,
                 {},
                 output(definition.query.kind),
                 QStringLiteral("载入模板将恢复已保存的对象、时间和格式设置。") + warning,
                 icon(definition.query.kind)});
        }
        return choices;
    }
    void choose_report() {
        const auto choices = report_choices();
        const auto saved_key = "template:" + text(draft.id);
        const bool saved =
            std::ranges::find(choices, saved_key, &ReportChoice::key) != choices.end();
        ReportSelectionDialog chooser(
            choices,
            saved ? saved_key
                  : QStringLiteral("builtin:%1").arg(static_cast<int>(draft.query.kind)),
            owner);
        report_chooser = &chooser;
        const auto accepted = chooser.exec() == QDialog::Accepted;
        report_chooser = nullptr;
        if (!accepted)
            return;
        // Resolve against the current project: it may have changed while the window was open.
        const auto current = report_choices();
        const auto found = std::ranges::find(current, chooser.selection(), &ReportChoice::key);
        if (found == current.end() || !found->unavailable_reason.isEmpty())
            return;
        const int row = static_cast<int>(std::distance(current.begin(), found));
        if ((saved && found->key == saved_key) ||
            (!saved && row == static_cast<int>(draft.query.kind)))
            return;
        choose(row);
    }
    bool compatible(ReportKind kind) const {
        if (report_is_access(kind))
            return !project.ground_stations.empty() && !project.satellites.empty();
        if (std::ranges::any_of(project.sensors, [&](const auto &s) {
                return std::ranges::find(scope_ids, s.object_id) != scope_ids.end();
            }))
            return false;
        if (kind == ReportKind::scenario_summary)
            return all_objects;
        if (report_satellites_only(kind) && !all_objects)
            return std::all_of(scope_ids.begin(), scope_ids.end(), [&](const auto &id) {
                return std::any_of(
                    project.satellites.begin(), project.satellites.end(),
                    [&](const auto &satellite) { return satellite.orbit.object_id == id; });
            });
        if (kind != ReportKind::ground_stations || all_objects)
            return true;
        return std::all_of(scope_ids.begin(), scope_ids.end(), [&](const auto &id) {
            return std::any_of(project.ground_stations.begin(), project.ground_stations.end(),
                               [&](const auto &station) { return station.object_id == id; });
        });
    }
    void refresh_scope() {
        QStringList labels;
        bool single_station = false;
        for (const auto &id : scope_ids) {
            QString label = text(id) + QStringLiteral("（已删除）");
            for (const auto &sensor : project.sensors)
                if (sensor.object_id == id)
                    label = text(sensor.name());
            for (const auto &satellite : project.satellites)
                if (satellite.orbit.object_id == id)
                    label = text(satellite.name());
            for (const auto &station : project.ground_stations)
                if (station.object_id == id) {
                    label = text(station.name());
                    single_station = true;
                }
            labels << label;
        }
        scope_label->setText(all_objects ? QStringLiteral("整个场景")
                             : labels.size() == 1
                                 ? labels.front()
                                 : QStringLiteral("已选 %1 个对象").arg(labels.size()));
        scope_label->setToolTip(labels.join('\n'));
        scope_detail->setText(
            all_objects ? QStringLiteral("包含当前和以后添加的适用对象")
            : labels.size() == 1
                ? (single_station ? QStringLiteral("仅此地面站") : QStringLiteral("仅此对象"))
                : QStringLiteral("仅使用指定对象，不自动添加新对象"));
        owner->setWindowTitle(scope_ids.size() == 1
                                  ? QStringLiteral("报告 · %1").arg(labels.front())
                                  : QStringLiteral("报告"));
        if (report_chooser)
            report_chooser->set_choices(report_choices());
    }
    void adapt(int width) {
        const bool was_compact = compact;
        const bool settings_hidden = settings_panel->isHidden();
        compact = panel_mode.narrow(width, 800);
        settings_panel->setMinimumWidth(compact ? 0 : 290);
        settings_panel->setMaximumWidth(compact ? QWIDGETSIZE_MAX : 380);
        settings_panel->setVisible(compact ? narrow_settings : wide_settings);
        result_panel->setVisible(!compact || !narrow_settings);
        if (!compact && wide_settings && (was_compact || settings_hidden))
            split->setSizes(wide_sizes);
        view_button->setText(
            compact ? (narrow_settings ? QStringLiteral("查看结果") : QStringLiteral("编辑设置"))
                    : (wide_settings ? QStringLiteral("收起设置") : QStringLiteral("展开设置")));
    }
    void update_interval() {
        for (auto *editor : {first, last})
            editor->findChild<QLineEdit *>("epochValue")->setMinimumWidth(180);
        const bool scenario = follow->isChecked();
        custom_times->setVisible(!scenario);
        interval_label->setVisible(scenario);
        if (scenario) {
            first->set_epoch(project.first);
            last->set_epoch(project.last);
        }
        interval_label->setText(first->findChild<QLineEdit *>("epochValue")->text() + "\n" +
                                last->findChild<QLineEdit *>("epochValue")->text());
    }
    void choose_scope() {
        auto choices =
            project_object_choices(project, {ObjectKind::satellite, ObjectKind::ground_station});
        if (draft.query.kind == ReportKind::ground_stations)
            choices = project_object_choices(project, {ObjectKind::ground_station});
        else if (report_satellites_only(draft.query.kind))
            choices = project_object_choices(project, {ObjectKind::satellite});
        ObjectSelectionDialog chooser(std::move(choices),
                                      {QStringLiteral("选择报告适用对象"), true,
                                       QStringLiteral("全部适用对象（含以后新增）")},
                                      {scope_ids, all_objects}, owner);
        if (chooser.exec() != QDialog::Accepted)
            return;
        scope_ids = chooser.selection().ids;
        all_objects = chooser.selection().all;
        refresh_scope();
        if (!compatible(draft.query.kind)) {
            const auto next = draft.query.kind == ReportKind::scenario_summary
                                  ? ReportKind::object_summary
                                  : ReportKind::cartesian_state;
            choose(static_cast<int>(next));
        } else
            edit();
    }
    void show_source() {
        if (!model->result)
            return;
        const auto &result = *model->result;
        QDialog dialog(owner);
        dialog.setWindowTitle(QStringLiteral("报告来源"));
        dialog.setObjectName("reportSourceDialog");
        dialog.resize(640, 400);
        auto *layout = new QVBoxLayout(&dialog);
        auto *source = new QPlainTextEdit(&dialog);
        source->setObjectName("reportSource");
        source->setReadOnly(true);
        source->setPlainText(QStringLiteral("报告：%1\n算法：%2\n\n输入 SHA-256\n%3\n\n场景数据 "
                                            "SHA-256\n%4\n\nUTC 数据 SHA-256\n%5")
                                 .arg(text(result.definition.name), text(result.algorithm),
                                      text(result.input_sha256), text(result.data_sha256),
                                      text(result.time_sha256)));
        layout->addWidget(source, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog.exec();
    }
    void fill_access_pair(bool preserve_missing) {
        const QSignalBlocker station_guard(access_station), target_guard(access_target);
        access_station->set_objects(project_object_choices(project, {ObjectKind::ground_station}));
        access_target->set_objects(
            project_object_choices(project, {ObjectKind::satellite, ObjectKind::sensor}));
        if (preserve_missing && scope_ids.size() == 2) {
            access_station->set_id(scope_ids[0]);
            access_target->set_id(scope_ids[1]);
        } else {
            for (auto *field : {access_station, access_target}) {
                field->set_id(field->objects().empty() ? "" : field->objects().front().id);
                for (const auto &id : scope_ids)
                    if (field->contains(id))
                        field->set_id(id);
            }
        }
        scope_ids = {access_station->id(), access_target->id()};
        all_objects = false;
    }
    void choose(int row) {
        if (row < 0 || loading)
            return;
        if (row < builtin_count && !compatible(static_cast<ReportKind>(row)))
            return;
        loading = true;
        if (row < builtin_count) {
            draft = {};
            draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            draft.query.kind = static_cast<ReportKind>(row);
            draft.name = kind_name(draft.query.kind).toStdString();
            draft.query.first = project.first;
            draft.query.last = project.last;
            draft.query.object_ids = scope_ids;
            if (report_is_access(draft.query.kind))
                draft.query.step_nanoseconds = 1'000'000'000;
        } else {
            draft = project.reports.at(static_cast<std::size_t>(row - builtin_count));
            scope_ids = draft.query.object_ids;
            all_objects = draft.query.scope == ReportScope::all_compatible;
        }
        const bool access = report_is_access(draft.query.kind);
        step_label->setText(draft.query.kind == ReportKind::access_intervals
                                ? QStringLiteral("搜索步长")
                                : QStringLiteral("采样间隔"));
        step->setMinimum(access ? .001 : .000000001);
        access_pair->setVisible(access);
        scope_label->setVisible(!access);
        scope_button->setVisible(!access);
        scope_detail->setVisible(!access);
        if (access)
            fill_access_pair(row >= builtin_count);
        follow->setChecked(draft.query.follow_scenario);
        first->set_epoch(draft.query.follow_scenario ? project.first : draft.query.first);
        last->set_epoch(draft.query.follow_scenario ? project.last : draft.query.last);
        step->setValue(static_cast<double>(draft.query.step_nanoseconds) / 1e9);
        frame->setCurrentIndex(draft.query.frame == core::FrameId::gcrf ? 0 : 1);
        if (draft.query.kind == ReportKind::geodetic_position)
            frame->setCurrentIndex(1);
        if (report_satellites_only(draft.query.kind))
            frame->setCurrentIndex(0);
        frame_field->setVisible(draft.query.kind == ReportKind::cartesian_state);
        units->setCurrentIndex(draft.format.units == ReportUnits::engineering ? 0 : 1);
        scale->setCurrentIndex(static_cast<int>(draft.format.time_scale));
        update_interval();
        refresh_scope();
        sampling->setVisible(report_is_sampled(draft.query.kind));
        units_field->setVisible(report_is_sampled(draft.query.kind) ||
                                draft.query.kind == ReportKind::ground_stations);
        scale_field->setVisible(report_is_sampled(draft.query.kind) ||
                                draft.query.kind == ReportKind::scenario_summary);
        format_title->setVisible(draft.query.kind != ReportKind::object_summary);
        template_description->setText(
            draft.query.kind == ReportKind::scenario_summary
                ? QStringLiteral("场景时段、对象数量与中心天体")
            : draft.query.kind == ReportKind::object_summary
                ? QStringLiteral("对象名称、类型与计算模型")
            : draft.query.kind == ReportKind::cartesian_state
                ? QStringLiteral("按指定间隔输出位置与速度")
            : draft.query.kind == ReportKind::classical_elements
                ? QStringLiteral("GCRF 状态换算的瞬时根数；圆轨道 / 赤道轨道的奇异角采用约定值。J2 "
                                 "结果不是平均根数。")
            : draft.query.kind == ReportKind::geodetic_position
                ? QStringLiteral("ITRF / WGS84 经度、大地纬度和椭球高；高度不表示离地距离。")
            : draft.query.kind == ReportKind::orbital_motion
                ? QStringLiteral("GCRF 地心距离、总速率、径向 / 横向速率与航迹倾角；径向向外为正。")
                : QStringLiteral("WGS84 经纬度、高度与 ITRF 坐标"));
        template_description->setToolTip(template_description->text());
        if (draft.query.kind == ReportKind::classical_elements)
            template_description->setText(QStringLiteral("瞬时六根数 · GCRF"));
        if (draft.query.kind == ReportKind::orbital_motion)
            template_description->setText(QStringLiteral("地心距离、速率与航迹倾角 · GCRF"));
        if (draft.query.kind == ReportKind::geodetic_position)
            template_description->setText(QStringLiteral("经度、大地纬度与椭球高 · WGS84"));
        if (access)
            template_description->setText(
                draft.query.kind == ReportKind::access_intervals
                    ? QStringLiteral("地球遮挡 · 最低仰角 · 传感器视场\n边界细化至 1 "
                                     "ms；短窗口请减小搜索步长。")
                    : QStringLiteral(
                          "当地北向起算方位角，水平线起算仰角。包含不可见时刻，以 0 / 1 标记。"));
        remove->setEnabled(row >= builtin_count);
        update_template->setEnabled(row >= builtin_count);
        store->setText(row >= builtin_count ? QStringLiteral("另存为模板…")
                                            : QStringLiteral("保存为模板…"));
        fill_library();
        loading = false;
        edit();
    }
    ReportDefinition definition() const {
        auto d = draft;
        d.query.object_ids =
            d.query.kind == ReportKind::scenario_summary ? std::vector<std::string>{} : scope_ids;
        d.query.scope = all_objects ? ReportScope::all_compatible : ReportScope::objects;
        if (report_is_access(d.query.kind))
            d.query.object_ids = {access_station->id(), access_target->id()};
        d.query.follow_scenario = follow->isChecked();
        if (report_is_sampled(d.query.kind) && !d.query.follow_scenario) {
            d.query.first = first->epoch();
            d.query.last = last->epoch();
        }
        d.query.step_nanoseconds = static_cast<std::int64_t>(std::llround(step->value() * 1e9));
        d.query.frame = frame->currentIndex() == 0 ? core::FrameId::gcrf : core::FrameId::itrf;
        d.format.units = units->currentIndex() == 0 ? ReportUnits::engineering : ReportUnits::si;
        d.format.time_scale = static_cast<ReportTimeScale>(scale->currentIndex());
        validate_report_definition(d);
        return d;
    }
    void edit() {
        if (loading)
            return;
        changed = true;
        update_estimate();
        if (model->result && !job.valid() && !waiting) {
            try {
                const auto d = definition();
                if (d.query == model->result->definition.query &&
                    d.id == model->result->definition.id && result_is_current()) {
                    auto result = std::make_shared<runtime::ReportResult>(
                        model->result->with_template(d.format));
                    result->definition = d;
                    publish(result, false);
                    status->setText(QStringLiteral("显示格式已更新。"));
                }
            } catch (const std::exception &error) {
                status->setText(
                    QStringLiteral("无法应用显示格式：%1").arg(QString::fromUtf8(error.what())));
            }
        }
        result_status();
    }
    void publish(std::shared_ptr<runtime::ReportResult> result, bool new_calculation = true) {
        model->publish(result);
        if (new_calculation && owner->result_published && result_is_current())
            owner->result_published(result, *result_analysis);
        changed = false;
        table->resizeColumnsToContents();
        for (std::size_t c = 0; c < result->table.columns().size(); ++c)
            table->setColumnHidden(static_cast<int>(c),
                                   result->definition.format.columns.empty() &&
                                       result->table.columns()[c].id == "object.id");
        result_status();
        prepare_plot();
    }
    void update_estimate() {
        try {
            const auto d = definition();
            const auto rows = runtime::estimate_report_rows(project, d);
            estimate->setText(
                (d.query.kind == ReportKind::access_intervals ? QStringLiteral("搜索 %1 个时刻%2")
                                                              : QStringLiteral("预计 %1 行%2"))
                    .arg(static_cast<qulonglong>(rows))
                    .arg(report_is_sampled(d.query.kind) ? QStringLiteral(" · 含起止时刻")
                                                         : QString{}));
        } catch (const std::exception &error) {
            estimate->setText(QString::fromUtf8(error.what()));
        }
    }
    bool result_is_current() const {
        // The report hash also records its actual resources, including a newly selected
        // time table. Compare the captured document and providers with the live inputs.
        return result_analysis &&
               runtime::report_input_sha256(*result_analysis->snapshot().project) ==
                   runtime::report_input_sha256(project) &&
               same_time(result_analysis->data().time(), time);
    }
    void result_status() {
        if (!model->result)
            return;
        const auto &r = *model->result;
        const bool stale = !result_is_current();
        result_title->setText(text(r.definition.name));
        result_title->setToolTip(text(r.definition.name));
        result_label->setText(QStringLiteral("%1 行%2")
                                  .arg(static_cast<qulonglong>(r.table.rows().size()))
                                  .arg(stale ? QStringLiteral(" · 场景已变化，请重新生成")
                                       : changed
                                           ? QStringLiteral(" · 设置已更改 · 当前显示上次结果")
                                           : QStringLiteral(" · 与当前场景一致")));
    }
    void set_busy(bool value) {
        settings->setEnabled(!value);
        library->setEnabled(!value);
        generate->setEnabled(!value);
        store->setEnabled(!value);
        update_template->setEnabled(
            !value && std::any_of(project.reports.begin(), project.reports.end(),
                                  [this](const auto &r) { return r.id == draft.id; }));
        remove->setEnabled(!value &&
                           std::any_of(project.reports.begin(), project.reports.end(),
                                       [this](const auto &r) { return r.id == draft.id; }));
        export_csv->setEnabled(!value && bool(model->result));
        refresh_button->setEnabled(!value && bool(model->result));
        source_button->setEnabled(bool(model->result));
        result_more->setEnabled(bool(model->result));
        config_menu->setEnabled(!value);
        cancel->setVisible(value);
        progress->setVisible(value);
    }
    void start() {
        try {
            const auto d = definition();
            expected = runtime::estimate_report_rows(project, d);
            waiting = true;
            const auto ticket = ++request;
            set_busy(true);
            progress->setRange(0, 0);
            status->setText(QStringLiteral("正在检查场景数据…"));
            const QPointer<ReportDialog> guard(owner);
            prepare([guard, ticket, d](std::optional<ReportInput> context, QString error) {
                if (!guard)
                    return;
                auto &self = *guard->impl_;
                if (ticket != self.request || !self.waiting)
                    return;
                self.waiting = false;
                if (!context) {
                    self.set_busy(false);
                    self.status->setText(error);
                    return;
                }
                try {
                    if (runtime::report_input_sha256(context->project) !=
                            runtime::report_input_sha256(self.project) ||
                        !same_time(context->data.time(), self.time)) {
                        self.set_busy(false);
                        self.status->setText(
                            QStringLiteral("场景在准备期间已变化，请重新生成；上次结果保留。"));
                        return;
                    }
                    self.expected = runtime::estimate_report_rows(context->project, d);
                    self.job_generation = self.input_generation;
                    self.cancellation = std::make_shared<std::atomic_bool>(false);
                    self.completed = std::make_shared<std::atomic_size_t>(0);
                    self.progress->setRange(0, static_cast<int>(self.expected));
                    self.progress->setValue(0);
                    self.status->setText(QStringLiteral("正在生成报告…"));
                    self.job = std::async(std::launch::async, [snapshot = std::move(*context), d,
                                                               cancel = self.cancellation,
                                                               count = self.completed]() mutable {
                        if (cancel->load())
                            throw runtime::AnalysisCancelled{};
                        const auto captured_project =
                            orbitforge::Project::from_data(std::move(snapshot.project));
                        auto analysis = captured_project.prepare(snapshot.data);
                        auto result = analysis.report(
                            d, [cancel] { return cancel->load(); },
                            [count](std::size_t current, std::size_t) { count->store(current); });
                        return GeneratedReport{std::move(analysis), std::move(result)};
                    });
                } catch (const std::exception &failure) {
                    self.set_busy(false);
                    self.status->setText(
                        QStringLiteral("无法启动报告：%1").arg(QString::fromUtf8(failure.what())));
                }
            });
        } catch (const std::exception &error) {
            waiting = false;
            set_busy(false);
            status->setText(QString::fromUtf8(error.what()));
        }
    }
    void persist(bool as_copy = false) {
        try {
            auto d = definition();
            if (as_copy) {
                QInputDialog naming(owner);
                naming.setObjectName("reportTemplateDialog");
                naming.setWindowTitle(QStringLiteral("保存报告模板"));
                naming.setLabelText(QStringLiteral("模板名称"));
                naming.setTextValue(text(d.name));
                naming.setOkButtonText(QStringLiteral("保存模板"));
                naming.setCancelButtonText(QStringLiteral("取消"));
                if (naming.exec() != QDialog::Accepted)
                    return;
                d.name = naming.textValue().trimmed().toStdString();
                d.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                validate_report_definition(d);
            }
            auto reports = project.reports;
            auto found = std::find_if(reports.begin(), reports.end(),
                                      [&](const auto &r) { return r.id == d.id; });
            if (found == reports.end())
                reports.push_back(d);
            else
                *found = d;
            save(reports);
            project.reports = std::move(reports);
            draft = d;
            if (as_copy)
                changed = true;
            fill_library();
            remove->setEnabled(true);
            update_template->setEnabled(true);
            store->setText(QStringLiteral("另存为模板…"));
            result_status();
            status->setText(QStringLiteral("模板已加入项目；保存项目后写入磁盘。"));
        } catch (const std::exception &error) {
            status->setText(QString::fromUtf8(error.what()));
        }
    }
    void erase() {
        try {
            auto reports = project.reports;
            std::erase_if(reports, [this](const auto &r) { return r.id == draft.id; });
            save(reports);
            project.reports = std::move(reports);
            fill_library();
            choose(static_cast<int>(compatible(draft.query.kind) ? draft.query.kind
                                                                 : ReportKind::object_summary));
            status->setText(QStringLiteral("已删除配置；保存项目后写入磁盘。"));
        } catch (const std::exception &error) {
            status->setText(QString::fromUtf8(error.what()));
        }
    }
    void select_view() {
        const bool chart = views->currentIndex() == 1 && model->result;
        if (model->result)
            result_stack->setCurrentWidget(chart ? plot_page : table);
        export_csv->setText(chart ? QStringLiteral("导出 PNG…") : QStringLiteral("导出 CSV…"));
        copy_button->setText(chart ? QStringLiteral("复制曲线图") : QStringLiteral("复制选中行"));
        copy_button->setToolTip(chart ? QStringLiteral("复制当前曲线图")
                                      : QStringLiteral("复制所选行（Ctrl+C）"));
        copy_button->setEnabled(chart || table->selectionModel()->hasSelection());
    }
    void prepare_plot() {
        const auto &r = *model->result;
        const auto previous_field = plot_field->currentData(Qt::UserRole + 1).toString();
        const auto previous_object = plot_object->id();
        const QSignalBlocker fields_guard(plot_field), objects_guard(plot_object);
        plot_field->clear();
        plot_object->set_objects({});
        plot_object->set_id({});
        const auto &columns = r.table.columns();
        const auto id_column = std::ranges::find(columns, "object.id", &results::Column::id);
        const auto name_column = std::ranges::find(columns, "object.name", &results::Column::id);
        const bool sampled =
            report_is_sampled(r.definition.query.kind) &&
            r.definition.query.kind != ReportKind::access_intervals && id_column != columns.end() &&
            name_column != columns.end() &&
            std::ranges::find(columns, "time.epoch", &results::Column::id) != columns.end() &&
            !r.table.rows().empty();
        if (sampled) {
            for (std::size_t c = 0; c < r.table.columns().size(); ++c) {
                bool numeric = false;
                for (std::size_t row = 0; row < r.table.rows().size() && !numeric; ++row)
                    numeric = r.numeric_value(row, c).has_value();
                if (!numeric)
                    continue;
                const auto &column = r.table.columns()[c];
                const auto suffix = r.column_unit(c);
                plot_field->addItem(localized(text(column.title)) +
                                        (suffix.empty() ? QString{} : " (" + text(suffix) + ")"),
                                    static_cast<int>(c));
                plot_field->setItemData(plot_field->count() - 1, text(column.id), Qt::UserRole + 1);
            }
            // A compact legend remains legible for up to twelve objects. Larger scopes keep
            // their complete table/CSV; the chart explicitly asks the user to choose an object.
            std::map<std::string, std::string> objects;
            for (const auto &row : r.table.rows())
                objects.try_emplace(std::get<std::string>(row[id_column - columns.begin()]),
                                    std::get<std::string>(row[name_column - columns.begin()]));
            std::map<std::string, ObjectKind> kinds;
            for (const auto &choice : project_object_choices(project))
                kinds.emplace(choice.id, choice.kind);
            std::vector<ObjectChoice> choices;
            // Result names and IDs belong to the immutable table, even after project edits.
            for (const auto &[id, object_name] : objects)
                choices.push_back(
                    {id, text(object_name), kinds.contains(id) ? kinds.at(id) : ObjectKind::other});
            plot_object->set_options({QStringLiteral("选择曲线显示对象"), false,
                                      objects.size() <= 12
                                          ? QStringLiteral("全部结果对象（%1）").arg(objects.size())
                                          : QString{}});
            plot_object->set_objects(std::move(choices));
            if (plot_object->contains(previous_object))
                plot_object->set_id(previous_object);
            else if (objects.size() <= 12)
                plot_object->set_selection({{}, true});
            else
                plot_object->set_id(objects.begin()->first);
            plot_note->setText(
                objects.size() > 12
                    ? QStringLiteral(
                          "对象超过 12 个，选择单个查看或在左侧缩小范围。滚轮缩放，双击定位时刻。")
                    : QStringLiteral("滚轮 / 框选缩放 · 右键还原 · 双击采样点定位场景时刻"));
            auto index = plot_field->findData(previous_field, Qt::UserRole + 1);
            if (index < 0 && r.definition.query.kind == ReportKind::classical_elements)
                index = plot_field->findData("elements.nu", Qt::UserRole + 1);
            if (index < 0 && r.definition.query.kind == ReportKind::geodetic_position)
                index = plot_field->findData("geodetic.height", Qt::UserRole + 1);
            if (index < 0 && r.definition.query.kind == ReportKind::aer)
                index = plot_field->findData("aer.elevation", Qt::UserRole + 1);
            plot_field->setCurrentIndex(std::max(0, index));
        }
        plot->set_result(model->result, static_cast<std::size_t>(plot_field->currentData().toInt()),
                         plot_field->currentText(), plot_object->id());
        views->setTabEnabled(1, sampled && plot_field->count() > 0);
        if (!views->isTabEnabled(1))
            views->setCurrentIndex(0);
        select_view();
    }
    void export_file() {
        if (!model->result)
            return;
        if (views->currentIndex() == 1) {
            QFileDialog chooser(owner, QStringLiteral("导出曲线图"));
            chooser.setAcceptMode(QFileDialog::AcceptSave);
            chooser.setFileMode(QFileDialog::AnyFile);
            chooser.setNameFilter("PNG (*.png)");
            chooser.setDefaultSuffix("png");
            chooser.selectFile("report.png");
            if (chooser.exec() != QDialog::Accepted)
                return;
            QSaveFile output(chooser.selectedFiles().value(0));
            const bool ok = output.open(QIODevice::WriteOnly) &&
                            plot->image().save(&output, "PNG") && output.commit();
            status->setText(ok ? QStringLiteral("曲线图已导出（2 倍分辨率）。")
                               : QStringLiteral("图片导出失败：%1").arg(output.errorString()));
            return;
        }
        QFileDialog chooser(owner, QStringLiteral("导出报告"));
        chooser.setAcceptMode(QFileDialog::AcceptSave);
        chooser.setFileMode(QFileDialog::AnyFile);
        chooser.setNameFilter("CSV (*.csv)");
        chooser.setDefaultSuffix("csv");
        chooser.selectFile("report.csv");
        if (chooser.exec() != QDialog::Accepted)
            return;
        const auto path = std::filesystem::path(chooser.selectedFiles().value(0).toStdWString());
        cancellation = std::make_shared<std::atomic_bool>(false);
        set_busy(true);
        progress->setRange(0, 0);
        status->setText(QStringLiteral("正在导出上方结果…"));
        try {
            export_job = std::async(
                std::launch::async, [result = model->result, path, cancel = cancellation] {
                    runtime::export_report_csv(*result, path, true, cancel.get());
                });
        } catch (const std::exception &error) {
            set_busy(false);
            status->setText(
                QStringLiteral("无法启动导出：%1").arg(QString::fromUtf8(error.what())));
        }
    }
    void copy_rows() {
        if (!model->result)
            return;
        auto rows = table->selectionModel()->selectedRows();
        if (rows.isEmpty()) {
            status->setText(QStringLiteral("请先选择要复制的行。"));
            return;
        }
        std::sort(rows.begin(), rows.end(),
                  [](const auto &a, const auto &b) { return a.row() < b.row(); });
        QString value;
        const auto append = [&](QStringList fields) {
            for (auto &field : fields) {
                if (field.contains('\t') || field.contains('\n') || field.contains('\r') ||
                    field.contains('"')) {
                    field.replace('"', "\"\"");
                    field = '"' + field + '"';
                }
            }
            value += fields.join('\t') + "\r\n";
        };
        QStringList headers;
        for (int c = 0; c < model->columnCount(); ++c)
            if (!table->isColumnHidden(c))
                headers << model->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
        append(headers);
        for (const auto &row : rows) {
            QStringList cells;
            for (int c = 0; c < model->columnCount(); ++c)
                if (!table->isColumnHidden(c))
                    cells << text(model->result->cell_text(row.row(), c));
            append(cells);
        }
        QApplication::clipboard()->setText(value);
        status->setText(
            QStringLiteral("已复制 %1 行（含列标题，保留完整精度）。").arg(rows.size()));
    }
    void poll() {
        if (job.valid()) {
            progress->setValue(static_cast<int>(completed->load()));
            if (job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            try {
                auto generated = job.get();
                if (cancellation->load())
                    throw runtime::AnalysisCancelled{};
                if (job_generation == input_generation) {
                    result_analysis = std::move(generated.analysis);
                    publish(std::make_shared<runtime::ReportResult>(std::move(generated.report)));
                    narrow_settings = false;
                    adapt(owner->width());
                    status->setText(QStringLiteral("报告已生成。"));
                }
            } catch (const runtime::AnalysisCancelled &) {
                status->setText(QStringLiteral("已取消；上次结果保留。"));
            } catch (const runtime::ReportCancelled &) {
                status->setText(QStringLiteral("已取消；上次结果保留。"));
            } catch (const std::exception &error) {
                status->setText(QStringLiteral("生成失败；上次结果保留。%1")
                                    .arg(QString::fromUtf8(error.what())));
            }
            if (job_generation != input_generation)
                status->setText(
                    QStringLiteral("场景已变化，未发布过期报告；上次结果保留。请重新生成。"));
            set_busy(false);
        }
        if (export_job.valid() &&
            export_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                export_job.get();
                status->setText(QStringLiteral("CSV 已导出。"));
            } catch (const runtime::ReportCancelled &) {
                status->setText(QStringLiteral("已取消导出；原文件保留。"));
            } catch (const std::exception &error) {
                status->setText(
                    QStringLiteral("导出失败：%1").arg(QString::fromUtf8(error.what())));
            }
            set_busy(false);
        }
    }
    void update(application::Project next, std::shared_ptr<const core::ITimeProvider> clock) {
        try {
            draft = definition();
        } catch (const std::exception &) {
        }
        if (runtime::report_input_sha256(project) != runtime::report_input_sha256(next) ||
            !same_time(time, clock)) {
            ++input_generation;
            if (job.valid() && cancellation)
                cancellation->store(true);
        }
        project = std::move(next);
        time = std::move(clock);
        first->set_time_provider(time);
        last->set_time_provider(time);
        if (follow->isChecked()) {
            first->set_epoch(project.first);
            last->set_epoch(project.last);
        }
        fill_library();
        refresh_scope();
        if (report_is_access(draft.query.kind))
            fill_access_pair(true);
        update_interval();
        update_estimate();
        result_status();
    }
};
ReportDialog::ReportDialog(application::Project project,
                           std::shared_ptr<const core::ITimeProvider> time, Prepare prepare,
                           Save save, std::function<void(core::Epoch)> seek, QWidget *parent)
    : QDialog(parent),
      impl_(std::make_unique<Impl>(this, std::move(project), std::move(time), std::move(prepare),
                                   std::move(save), std::move(seek))) {}
ReportDialog::~ReportDialog() = default;
void ReportDialog::select_object(const std::string &id, application::ReportKind kind) {
    auto &self = *impl_;
    if (!self.generate->isEnabled()) {
        self.status->setText(QStringLiteral("报告任务正在运行；完成或取消后可切换对象。"));
        return;
    }
    self.scope_ids = {id};
    self.all_objects = false;
    self.refresh_scope();
    self.choose(static_cast<int>(self.compatible(kind) ? kind : ReportKind::object_summary));
    self.narrow_settings = true;
    self.wide_settings = true;
    self.adapt(width());
}
void ReportDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (impl_)
        impl_->adapt(event->size().width());
}
void ReportDialog::update_project(application::Project project,
                                  std::shared_ptr<const core::ITimeProvider> time) {
    impl_->update(std::move(project), std::move(time));
}
} // namespace orbitforge::desktop
