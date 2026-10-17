#include "mission_editor.hpp"
#include "hpop_settings.hpp"
#include "quantity_editor.hpp"
#include "responsive_layout.hpp"
#include "workbench_style.hpp"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <algorithm>
#include <cmath>

namespace orbitforge::desktop {
namespace {
constexpr int timing_role = Qt::UserRole + 1;
constexpr int detail_role = Qt::UserRole + 2;
constexpr int impulse_role = Qt::UserRole + 3;
constexpr std::int64_t day_ns = 86'400'000'000'000;

QString elapsed_text(std::int64_t ns) {
    const auto seconds = ns / 1'000'000'000;
    auto text = QStringLiteral("T+%1:%2:%3")
                    .arg(seconds / 3600, 2, 10, QChar('0'))
                    .arg(seconds / 60 % 60, 2, 10, QChar('0'))
                    .arg(seconds % 60, 2, 10, QChar('0'));
    if (const auto fraction = ns % 1'000'000'000; fraction != 0) {
        auto digits = QStringLiteral("%1").arg(fraction, 9, 10, QChar('0'));
        while (digits.endsWith('0'))
            digits.chop(1);
        text += '.' + digits;
    }
    return text;
}
QString duration_text(double seconds) {
    QStringList parts;
    for (const auto &[length, symbol] :
         {std::pair{86400.0, "d"}, std::pair{3600.0, "h"}, std::pair{60.0, "min"}}) {
        const auto count = std::floor(seconds / length);
        if (count >= 1) {
            parts.push_back(QStringLiteral("%1 %2").arg(count, 0, 'f', 0).arg(symbol));
            seconds -= count * length;
        }
    }
    if (seconds > 0 || parts.isEmpty())
        parts.push_back(QStringLiteral("%1 s").arg(seconds, 0, 'g', 8));
    return parts.join(' ');
}
class SegmentDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        return {160, option.fontMetrics.height() * 3 + 28};
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect row = option.rect.adjusted(1, 2, -1, -2);
        const bool selected = option.state & QStyle::State_Selected;
        const auto accent = index.data(impulse_role).toBool() ? theme::color(theme::sakura)
                                                              : theme::color(theme::accent);
        paint_list_row(painter, option, QRectF(row), accent);
        auto number_font = option.font;
        number_font.setFamilies({"Consolas", "Microsoft YaHei UI"});
        painter->setFont(number_font);
        painter->setPen(accent);
        painter->drawText(QRect(row.left() + 10, row.top() + 11, 26, option.fontMetrics.height()),
                          Qt::AlignCenter,
                          QStringLiteral("%1").arg(index.row() + 1, 2, 10, QChar('0')));
        const int x = row.left() + 46, width = std::max(0, row.width() - 58);
        auto font = option.font;
        painter->setFont(font);
        painter->setPen(theme::color(selected ? theme::accent_bright : theme::text));
        const QFontMetrics title_metrics(font);
        painter->drawText(QRect(x, row.top() + 10, width, title_metrics.height()), Qt::AlignVCenter,
                          title_metrics.elidedText(index.data().toString(), Qt::ElideRight, width));
        font.setWeight(QFont::Normal);
        font.setFamilies({"Consolas", "Microsoft YaHei UI"});
        font.setPixelSize(std::max(11, option.font.pixelSize() - 1));
        painter->setFont(font);
        const QFontMetrics metrics(font);
        int y = row.top() + 14 + title_metrics.height();
        for (const int role : {timing_role, detail_role}) {
            painter->setPen(role == detail_role ? accent : theme::color(theme::text_muted));
            painter->drawText(
                QRect(x, y, width, metrics.height()), Qt::AlignVCenter,
                metrics.elidedText(index.data(role).toString(), Qt::ElideRight, width));
            y += metrics.height() + 2;
        }
        painter->restore();
    }
};
class MissionEditor final : public QDialog {
  public:
    core::MissionPlan plan;
    core::HpopOptions hpop;
    MissionEditor(core::MissionPlan draft, core::HpopOptions options, core::Epoch initial,
                  core::Epoch scenario_first, core::Epoch scenario_last, QWidget *parent,
                  const std::vector<runtime::CatalogGravity> &models,
                  const std::vector<runtime::CatalogEphemeris> &ephemerides,
                  const std::function<std::vector<runtime::CatalogEphemeris>()> &manage_ephemerides,
                  EopCatalog *catalog, bool initialize_hpop)
        : QDialog(parent), plan(std::move(draft)), hpop(options), initial_(initial),
          first_(scenario_first), last_(scenario_last) {
        setObjectName("missionEditorDialog");
        setWindowTitle(QStringLiteral("机动规划 · 任务序列"));
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(theme::space::page);
        outer->setSpacing(12);
        auto *heading = new QVBoxLayout;
        heading->setSpacing(4);
        auto *title = new QLabel(QStringLiteral("机动规划"));
        title->setObjectName("panelTitle");
        heading->addWidget(title);
        heading->addWidget(note(QStringLiteral("保存计划后，在卫星轨道页点击“应用”。")));
        outer->addLayout(heading);

        auto *model_row = new QHBoxLayout;
        model_row->setSpacing(12);
        model_row->addWidget(new QLabel(QStringLiteral("滑行模型")));
        model_ = new QComboBox;
        model_->setObjectName("missionCoastModel");
        model_->addItems({QStringLiteral("二体"), QStringLiteral("HPOP · 数值积分")});
        model_->setCurrentIndex(plan.coast_model == core::MissionCoastModel::hpop ? 1 : 0);
        model_row->addWidget(model_, 1);
        auto *settings = button(QStringLiteral("模型设置…"), "missionHpopSettings");
        settings->setToolTip(QStringLiteral("设置 HPOP 力模型与积分参数"));
        settings->setEnabled(model_->currentIndex() == 1);
        model_row->addWidget(settings);
        outer->addLayout(model_row);
        connect(settings, &QPushButton::clicked, this,
                [this, models, ephemerides, manage_ephemerides, catalog] {
                    edit_hpop_settings(hpop, this, models, ephemerides, manage_ephemerides,
                                       catalog);
                });
        connect(model_, &QComboBox::currentIndexChanged, this,
                [this, settings, initialize_hpop](int index) mutable {
                    if (index == 1 && initialize_hpop) {
                        hpop = new_hpop_options();
                        initialize_hpop = false;
                    }
                    plan.coast_model = index == 0 ? core::MissionCoastModel::two_body
                                                  : core::MissionCoastModel::hpop;
                    settings->setEnabled(index == 1);
                    refresh_summary();
                });

        navigation_ = new QTabBar(this);
        navigation_->setObjectName("missionNavigation");
        navigation_->setAccessibleName(QStringLiteral("机动规划页面"));
        navigation_->addTab(QStringLiteral("任务序列"));
        navigation_->addTab(QStringLiteral("段参数"));
        navigation_->setExpanding(true);
        navigation_->setDrawBase(false);
        navigation_->hide();
        outer->addWidget(navigation_);
        auto *body = new QWidget;
        auto *columns = new QHBoxLayout(body);
        columns->setContentsMargins(0, 0, 0, 0);
        columns->setSpacing(20);
        outer->addWidget(body, 1);
        sequence_ = new QWidget;
        auto *left = new QVBoxLayout(sequence_);
        left->setContentsMargins(0, 0, 0, 0);
        left->setSpacing(10);
        auto *initial_label = new QLabel(QStringLiteral("任务序列"));
        initial_label->setObjectName("sectionTitle");
        left->addWidget(initial_label);
        left->addWidget(note(QStringLiteral("T+ 从卫星初始历元起算 · TAI 时长")));
        list_ = new QListWidget;
        list_->setObjectName("missionSegments");
        list_->setSpacing(4);
        list_->setMinimumWidth(0);
        list_->setItemDelegate(new SegmentDelegate(list_));
        list_->setAccessibleName(QStringLiteral("按执行顺序排列的任务段"));
        list_->setMouseTracking(true);
        list_->setResizeMode(QListView::Adjust);
        list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        left->addWidget(list_, 1);
        // One action row: add commands as labelled buttons, reordering and removal as
        // icon buttons whose tooltips and accessible names keep the full wording.
        auto *actions = new QHBoxLayout;
        actions->setSpacing(6);
        auto *coast = button(QStringLiteral("＋ 滑行"), "missionAddCoast");
        auto *impulse = button(QStringLiteral("＋ 脉冲"), "missionAddImpulse");
        actions->addWidget(coast);
        actions->addWidget(impulse);
        actions->addStretch(1);
        up_ = icon_button(WorkbenchIcon::chevron_up, QStringLiteral("上移"), "missionMoveUp");
        down_ = icon_button(WorkbenchIcon::chevron_down, QStringLiteral("下移"), "missionMoveDown");
        remove_ = icon_button(WorkbenchIcon::remove, QStringLiteral("删除"), "missionRemove");
        up_->setToolTip(QStringLiteral("上移当前段（列表中 Alt+↑）"));
        down_->setToolTip(QStringLiteral("下移当前段（列表中 Alt+↓）"));
        remove_->setToolTip(QStringLiteral("删除当前段及其未完成输入（列表中 Delete）"));
        for (auto *b : {up_, down_, remove_})
            actions->addWidget(b);
        left->addLayout(actions);
        columns->addWidget(sequence_, 2);

        scroll_ = new QScrollArea;
        scroll_->setObjectName("missionParameters");
        scroll_->setWidgetResizable(true);
        scroll_->setFrameShape(QFrame::NoFrame);
        auto *details = new QWidget;
        details->setObjectName("missionParameterFields");
        auto *right = new QVBoxLayout(details);
        right->setContentsMargins(theme::space::panel);
        right->setSpacing(theme::space::sm);
        heading_ = new QLabel;
        heading_->setObjectName("sectionTitle");
        right->addWidget(heading_);
        timing_ = note({});
        timing_->setObjectName("missionSegmentTiming");
        right->addWidget(timing_);
        empty_ =
            note(QStringLiteral("添加一段滑行，再按点火时刻插入脉冲。\n每段继承上一段的末状态。"));
        empty_->setObjectName("missionEmpty");
        right->addWidget(empty_);
        right->addSpacing(theme::space::sm);
        // One row rhythm from the name down to the last Δv component.
        fields_ = new QWidget;
        auto *fields_layout = new QVBoxLayout(fields_);
        fields_layout->setContentsMargins(0, 0, 0, 0);
        fields_layout->setSpacing(theme::space::row);
        auto *form = new QFormLayout;
        form->setVerticalSpacing(theme::space::row);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        name_ = new QLineEdit;
        name_->setObjectName("missionSegmentName");
        name_->setMaxLength(16384);
        name_->setPlaceholderText(QStringLiteral("可选名称"));
        form->addRow(QStringLiteral("名称"), name_);
        fields_layout->addLayout(form);
        pages_ = new QStackedWidget;
        auto *coast_page = new QWidget;
        auto *coast_form = new QFormLayout(coast_page);
        coast_form->setContentsMargins(0, 0, 0, 0);
        coast_form->setVerticalSpacing(theme::space::row);
        coast_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        duration_ = new QuantityEditor(Unit::minute);
        duration_->setObjectName("missionCoastDuration");
        duration_->set_label(QStringLiteral("滑行时长"));
        duration_->set_range(1e-9, 366 * 86400.0, Unit::second);
        duration_->setMaximumWidth(QWIDGETSIZE_MAX);
        coast_form->addRow(QStringLiteral("时长"), duration_);
        coast_form->addRow(note(QStringLiteral("继承上一段的末状态，传播指定的时长。")));
        pages_->addWidget(coast_page);
        auto *impulse_page = new QWidget;
        auto *impulse_form = new QFormLayout(impulse_page);
        impulse_form->setContentsMargins(0, 0, 0, 0);
        impulse_form->setVerticalSpacing(theme::space::row);
        impulse_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        frame_ = new QComboBox;
        frame_->setObjectName("missionManeuverFrame");
        frame_->addItems({"GCRF", "VNC", "LVLH"});
        frame_->setAccessibleName(QStringLiteral("脉冲方向参考系"));
        impulse_form->addRow(QStringLiteral("方向参考系"), frame_);
        for (std::size_t i = 0; i < 3; ++i) {
            dv_[i] = new QuantityEditor(Unit::metres_per_second);
            dv_[i]->setObjectName(QStringLiteral("missionDeltaV%1").arg(i));
            dv_[i]->set_label(QStringLiteral("Δv %1").arg(i + 1));
            dv_[i]->setMaximumWidth(QWIDGETSIZE_MAX);
            axes_[i] = new QLabel;
            impulse_form->addRow(axes_[i], dv_[i]);
        }
        magnitude_ = note({});
        magnitude_->setObjectName("missionImpulseMagnitude");
        impulse_form->addRow(magnitude_);
        axes_note_ = note({});
        impulse_form->addRow(axes_note_);
        pages_->addWidget(impulse_page);
        fields_layout->addWidget(pages_);
        right->addWidget(fields_);
        right->addStretch();
        scroll_->setWidget(details);
        columns->addWidget(scroll_, 3);
        const int label_width = fontMetrics().horizontalAdvance(QStringLiteral("Δv · 共法向 C"));
        for (auto *f : {form, coast_form, impulse_form}) {
            f->setRowWrapPolicy(QFormLayout::WrapLongRows);
            f->setHorizontalSpacing(theme::space::md);
            f->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            for (int row = 0; row < f->rowCount(); ++row)
                if (auto *label = f->itemAt(row, QFormLayout::LabelRole); label && label->widget())
                    label->widget()->setMinimumWidth(label_width);
        }
        new WidthObserver(body, [this, mode = WidthMode{}](int width) mutable {
            narrow_ = mode.narrow(width, 680);
            update_panels();
            QTimer::singleShot(0, list_, [this] {
                if (list_->currentItem())
                    list_->scrollToItem(list_->currentItem());
            });
        });
        connect(navigation_, &QTabBar::currentChanged, this, [this] { update_panels(); });
        auto *status = new QFrame;
        status->setObjectName("missionStatus");
        auto *status_layout = new QVBoxLayout(status);
        status_layout->setContentsMargins(theme::space::card);
        status_layout->setSpacing(4);
        summary_ = new QLabel;
        summary_->setObjectName("missionSummary");
        summary_->setWordWrap(true);
        status_layout->addWidget(summary_);
        coverage_ = note({});
        coverage_->setObjectName("missionCoverage");
        status_layout->addWidget(coverage_);
        outer->addWidget(status);
        error_ = new QLabel;
        error_->setObjectName("errorText");
        error_->setTextFormat(Qt::PlainText);
        error_->setWordWrap(true);
        error_->hide();
        outer->addWidget(error_);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存计划"));
        buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
        buttons->button(QDialogButtonBox::Ok)->setAutoDefault(false);
        buttons->button(QDialogButtonBox::Ok)->setDefault(false);
        buttons->button(QDialogButtonBox::Ok)
            ->setToolTip(QStringLiteral("保存到卫星属性草稿（Ctrl+Enter）"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
        auto *save = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
        connect(save, &QShortcut::activated, buttons->button(QDialogButtonBox::Ok),
                &QPushButton::click);
        outer->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (!store_selected()) {
                navigation_->setCurrentIndex(1);
                if (invalid_field_) {
                    invalid_field_->setFocus();
                    scroll_->ensureWidgetVisible(invalid_field_);
                }
                return;
            }
            try {
                if (const auto issue = plan_issue(); !issue.isEmpty()) {
                    show_error(issue);
                    return;
                }
                if (plan.coast_model == core::MissionCoastModel::hpop)
                    core::validate_hpop_options(hpop);
                accept();
            } catch (const std::exception &e) {
                show_error(QString::fromUtf8(e.what()));
            }
        });
        connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
            if (!store_selected()) {
                const QSignalBlocker block(list_);
                list_->setCurrentRow(selected_);
                return;
            }
            selected_ = row;
            load_selected();
        });
        connect(frame_, &QComboBox::currentIndexChanged, this, [this] {
            update_axes();
            store_selected();
        });
        const auto open_parameters = [this](QListWidgetItem *item) {
            if (list_->row(item) == selected_)
                navigation_->setCurrentIndex(1);
        };
        connect(list_, &QListWidget::itemClicked, this, open_parameters);
        connect(list_, &QListWidget::itemActivated, this, open_parameters);
        connect(coast, &QPushButton::clicked, this,
                [this] { add(core::MissionSegment::coast(std::chrono::minutes{10})); });
        connect(impulse, &QPushButton::clicked, this,
                [this] { add(core::MissionSegment::impulse({0, 0, 0})); });
        connect(up_, &QPushButton::clicked, this, [this] { move(-1); });
        connect(down_, &QPushButton::clicked, this, [this] { move(1); });
        connect(remove_, &QPushButton::clicked, this, [this] {
            if (selected_ < 0)
                return;
            plan.segments.erase(plan.segments.begin() + selected_);
            rebuild(std::min(selected_, static_cast<int>(plan.segments.size()) - 1));
        });
        for (const auto &[key, action] : {std::pair{QKeySequence(Qt::ALT | Qt::Key_Up), up_},
                                          std::pair{QKeySequence(Qt::ALT | Qt::Key_Down), down_},
                                          std::pair{QKeySequence(Qt::Key_Delete), remove_}}) {
            auto *shortcut = new QShortcut(key, list_);
            shortcut->setContext(Qt::WidgetShortcut);
            connect(shortcut, &QShortcut::activated, action, &QPushButton::click);
        }
        connect(name_, &QLineEdit::textEdited, this, [this] { store_selected(); });
        for (auto *value : {duration_, dv_[0], dv_[1], dv_[2]}) {
            connect(value, &QuantityEditor::valueChanged, this, [this] { store_selected(); });
            // valueChanged intentionally omits invalid input. Keep draft feedback in
            // sync even while a number is incomplete, or repaired to its old value.
            connect(value->findChild<QLineEdit *>("quantityValue"), &QLineEdit::textChanged, this,
                    [this] { store_selected(); });
        }
        rebuild(plan.segments.empty() ? -1 : 0);
        const auto available = screen()->availableGeometry().size();
        resize(std::min(980, available.width() - 32), std::min(740, available.height() - 64));
    }

  protected:
    void keyPressEvent(QKeyEvent *event) override {
        // Item activation and numeric editing must not also trigger QDialog's
        // implicit default button. Saving has its own explicit Ctrl+Enter shortcut.
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::NoModifier) {
            event->accept();
            return;
        }
        QDialog::keyPressEvent(event);
    }

  private:
    core::Epoch initial_, first_, last_;
    int selected_ = -1;
    bool loading_ = false, narrow_ = false, draft_valid_ = true;
    QWidget *sequence_, *fields_, *invalid_field_ = nullptr;
    QScrollArea *scroll_;
    QTabBar *navigation_;
    QListWidget *list_;
    QComboBox *model_, *frame_;
    QLineEdit *name_;
    QStackedWidget *pages_;
    QuantityEditor *duration_;
    std::array<QuantityEditor *, 3> dv_{};
    std::array<QLabel *, 3> axes_{};
    QLabel *heading_, *timing_, *summary_, *coverage_, *error_, *empty_, *axes_note_, *magnitude_;
    QPushButton *up_, *down_, *remove_;
    static QPushButton *button(const QString &text, const char *id) {
        auto *b = new QPushButton(text);
        b->setObjectName(id);
        b->setAutoDefault(false);
        return b;
    }
    static QPushButton *icon_button(WorkbenchIcon icon, const QString &text, const char *id) {
        auto *b = button(QString{}, id);
        b->setIcon(workbench_icon(icon));
        b->setIconSize(QSize(15, 15));
        b->setAccessibleName(text);
        b->setProperty("iconOnly", true);
        b->setFixedSize(28, 28);
        return b;
    }
    static QLabel *note(const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName("hint");
        label->setWordWrap(true);
        return label;
    }
    void show_error(const QString &message) {
        error_->setText(message);
        error_->show();
    }
    void update_panels() {
        navigation_->setVisible(narrow_);
        sequence_->setVisible(!narrow_ || navigation_->currentIndex() == 0);
        scroll_->setVisible(!narrow_ || navigation_->currentIndex() == 1);
    }
    void update_axes() {
        const QStringList names =
            frame_->currentIndex() == 1
                ? QStringList{QStringLiteral("Δv · 速度 V"), QStringLiteral("Δv · 法向 N"),
                              QStringLiteral("Δv · 共法向 C")}
            : frame_->currentIndex() == 2
                ? QStringList{QStringLiteral("Δv · 径向 R"), QStringLiteral("Δv · 横向 T"),
                              QStringLiteral("Δv · 法向 N")}
                : QStringList{QStringLiteral("Δv · X"), QStringLiteral("Δv · Y"),
                              QStringLiteral("Δv · Z")};
        for (int i = 0; i < 3; ++i) {
            axes_[static_cast<std::size_t>(i)]->setText(names[i]);
            dv_[static_cast<std::size_t>(i)]->set_label(names[i]);
        }
        axes_note_->setText(
            frame_->currentIndex() == 1
                ? QStringLiteral("V 沿速度，N 沿轨道法向，C = V × N。方向取点火前状态。")
            : frame_->currentIndex() == 2
                ? QStringLiteral("R 径向向外，N 沿轨道法向，T = N × R。方向取点火前状态。")
                : QStringLiteral("沿 GCRF 惯性坐标轴施加 Δv；脉冲瞬间位置保持不变。"));
    }
    bool store_selected() {
        if (loading_ || selected_ < 0)
            return true;
        try {
            auto segment = plan.segments.at(static_cast<std::size_t>(selected_));
            segment.name = name_->text().trimmed().toStdString();
            if (segment.kind == core::MissionSegmentKind::coast) {
                invalid_field_ = duration_;
                const double seconds = duration_->value(Unit::second);
                // Opening and accepting must preserve an exact saved int64 duration,
                // even when its last nanoseconds cannot be represented by a double.
                if (seconds != static_cast<double>(segment.duration_nanoseconds) * 1e-9)
                    segment.duration_nanoseconds = std::llround(seconds * 1e9);
                if (segment.duration_nanoseconds <= 0)
                    throw std::invalid_argument("滑行时长至少为 1 ns。");
            } else {
                segment.frame = static_cast<core::ManeuverFrame>(frame_->currentIndex());
                for (std::size_t i = 0; i < 3; ++i) {
                    invalid_field_ = dv_[i];
                    segment.delta_v_mps[i] = dv_[i]->value(Unit::metres_per_second);
                }
                if (!std::isfinite(std::hypot(segment.delta_v_mps[0], segment.delta_v_mps[1],
                                              segment.delta_v_mps[2])))
                    throw std::invalid_argument("Δv 合量超出数值范围，请减小分量。");
            }
            plan.segments[static_cast<std::size_t>(selected_)] = std::move(segment);
            draft_valid_ = true;
            invalid_field_ = nullptr;
            refresh_summary();
            error_->hide();
            return true;
        } catch (const std::exception &e) {
            draft_valid_ = false;
            refresh_summary();
            show_error(QString::fromUtf8(e.what()));
            return false;
        }
    }
    void load_selected() {
        loading_ = true;
        const bool selected = selected_ >= 0;
        fields_->setVisible(selected);
        empty_->setVisible(!selected);
        timing_->setVisible(selected);
        navigation_->setTabEnabled(1, selected);
        up_->setEnabled(selected_ > 0);
        down_->setEnabled(selected && selected_ + 1 < static_cast<int>(plan.segments.size()));
        remove_->setEnabled(selected);
        heading_->setText(QStringLiteral("从一段滑行开始"));
        name_->clear();
        if (selected) {
            const auto &segment = plan.segments.at(static_cast<std::size_t>(selected_));
            const bool coast = segment.kind == core::MissionSegmentKind::coast;
            heading_->setText(
                QStringLiteral("%1  /  %2")
                    .arg(selected_ + 1, 2, 10, QChar('0'))
                    .arg(coast ? QStringLiteral("滑行段") : QStringLiteral("脉冲机动")));
            name_->setText(QString::fromStdString(segment.name));
            pages_->setCurrentIndex(coast ? 0 : 1);
            if (coast)
                duration_->set_value(static_cast<double>(segment.duration_nanoseconds) * 1e-9,
                                     Unit::second);
            else {
                frame_->setCurrentIndex(static_cast<int>(segment.frame));
                for (std::size_t i = 0; i < 3; ++i)
                    dv_[i]->set_value(segment.delta_v_mps[i], Unit::metres_per_second);
            }
        }
        update_axes();
        loading_ = false;
        scroll_->verticalScrollBar()->setValue(0);
        draft_valid_ = true;
        invalid_field_ = nullptr;
        error_->hide();
        refresh_summary();
    }
    QString plan_issue() const {
        if (plan.segments.empty())
            return QStringLiteral("计划为空，请先添加一段滑行。");
        std::int64_t duration = 0;
        double total = 0;
        for (const auto &s : plan.segments) {
            if (s.kind == core::MissionSegmentKind::coast)
                duration += s.duration_nanoseconds;
            else
                total += std::hypot(s.delta_v_mps[0], s.delta_v_mps[1], s.delta_v_mps[2]);
        }
        if (duration == 0)
            return QStringLiteral("计划至少需要一段正时长滑行。");
        if (duration > 366 * day_ns)
            return QStringLiteral("计划总时长不能超过 366 天，请缩短滑行段。");
        if (!std::isfinite(total))
            return QStringLiteral("累计 Δv 超出数值范围，请减小脉冲分量。");
        try {
            const auto end = core::validate_mission(initial_, plan);
            if (initial_.tai_nanoseconds() > first_.tai_nanoseconds())
                return QStringLiteral("初始历元晚于场景起点，请返回卫星属性调整历元。");
            if (end.tai_nanoseconds() < last_.tai_nanoseconds())
                return QStringLiteral("距场景结束还差 %1，请延长滑行段。")
                    .arg(duration_text(last_.seconds_since(end)));
        } catch (const std::exception &e) {
            return QString::fromUtf8(e.what());
        }
        return {};
    }
    void refresh_summary() {
        std::int64_t duration = 0;
        double total = 0;
        for (std::size_t i = 0; i < plan.segments.size(); ++i) {
            const auto &s = plan.segments[i];
            const bool coast = s.kind == core::MissionSegmentKind::coast;
            const double dv = std::hypot(s.delta_v_mps[0], s.delta_v_mps[1], s.delta_v_mps[2]);
            const auto start = elapsed_text(duration);
            if (coast)
                duration += s.duration_nanoseconds;
            else
                total += dv;
            const auto label = s.name.empty()
                                   ? (coast ? QStringLiteral("滑行") : QStringLiteral("脉冲机动"))
                                   : QString::fromStdString(s.name);
            const auto timing =
                coast ? start + QStringLiteral(" → ") + elapsed_text(duration) : start;
            const auto detail =
                coast ? QStringLiteral("滑行 · %1")
                            .arg(duration_text(static_cast<double>(s.duration_nanoseconds) * 1e-9))
                      : QStringLiteral("Δv %1 m/s · %2")
                            .arg(dv, 0, 'g', 8)
                            .arg(s.frame == core::ManeuverFrame::gcrf  ? "GCRF"
                                 : s.frame == core::ManeuverFrame::vnc ? "VNC"
                                                                       : "LVLH");
            auto *item = list_->item(static_cast<int>(i));
            item->setText(label);
            item->setData(timing_role, timing);
            item->setData(detail_role, detail);
            item->setData(impulse_role, !coast);
            item->setToolTip(label.toHtmlEscaped() + "<br>" + timing + "<br>" + detail);
            item->setData(
                Qt::AccessibleTextRole,
                QStringLiteral("%1 · %2 · %3 · %4").arg(i + 1).arg(label, timing, detail));
            if (static_cast<int>(i) == selected_) {
                timing_->setText(
                    (coast ? QStringLiteral("执行时段  ") : QStringLiteral("点火时刻  ")) + timing);
                magnitude_->setText(QStringLiteral("本次 Δv  %1 m/s").arg(dv, 0, 'g', 9));
            }
        }
        summary_->setText(
            QStringLiteral("%1 段   ·   总时长 %2   ·   总 Δv %3 m/s")
                .arg(plan.segments.size())
                .arg(duration_text(static_cast<double>(duration) * 1e-9))
                .arg(std::isfinite(total) ? QString::number(total, 'g', 9) : QStringLiteral("—")));
        const auto issue = plan_issue();
        coverage_->setText(
            !draft_valid_ ? QStringLiteral("输入尚未完成 · 预览保留上次有效参数")
            : !issue.isEmpty()
                ? issue
                : QStringLiteral("已覆盖场景 · 计划结束于 %1").arg(elapsed_text(duration)));
        const bool warning = !draft_valid_ || !issue.isEmpty();
        if (coverage_->property("warning").toBool() != warning) {
            coverage_->setProperty("warning", warning);
            coverage_->style()->unpolish(coverage_);
            coverage_->style()->polish(coverage_);
        }
    }
    void rebuild(int selected) {
        const QSignalBlocker block(list_);
        list_->clear();
        for (std::size_t i = 0; i < plan.segments.size(); ++i)
            new QListWidgetItem(list_);
        selected_ = selected;
        list_->setCurrentRow(selected);
        load_selected();
        if (list_->currentItem())
            list_->scrollToItem(list_->currentItem());
    }
    void add(core::MissionSegment segment) {
        if (!store_selected())
            return;
        if (plan.segments.size() >= 256) {
            show_error(QStringLiteral("最多可添加 256 个段。"));
            return;
        }
        const int next = selected_ + 1;
        plan.segments.insert(plan.segments.begin() + next, std::move(segment));
        rebuild(next);
        navigation_->setCurrentIndex(1);
    }
    void move(int direction) {
        if (!store_selected())
            return;
        const int next = selected_ + direction;
        if (selected_ < 0 || next < 0 || next >= static_cast<int>(plan.segments.size()))
            return;
        std::swap(plan.segments[static_cast<std::size_t>(selected_)],
                  plan.segments[static_cast<std::size_t>(next)]);
        rebuild(next);
    }
};
} // namespace
bool edit_mission(core::MissionPlan &plan, core::HpopOptions &hpop, core::Epoch initial,
                  core::Epoch scenario_first, core::Epoch scenario_last, QWidget *parent,
                  const std::vector<runtime::CatalogGravity> &models,
                  const std::vector<runtime::CatalogEphemeris> &ephemerides,
                  const std::function<std::vector<runtime::CatalogEphemeris>()> &manage_ephemerides,
                  EopCatalog *catalog, bool initialize_hpop) {
    MissionEditor dialog(plan, hpop, initial, scenario_first, scenario_last, parent, models,
                         ephemerides, manage_ephemerides, catalog, initialize_hpop);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    plan = std::move(dialog.plan);
    hpop = dialog.hpop;
    return true;
}
} // namespace orbitforge::desktop
