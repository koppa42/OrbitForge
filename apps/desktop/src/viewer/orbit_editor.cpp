#include "orbit_editor.hpp"
#include "epoch_editor.hpp"
#include "form_kit.hpp"
#include "hpop_settings.hpp"
#include "mission_editor.hpp"
#include "quantity_editor.hpp"
#include "responsive_layout.hpp"
#include "track_style_editor.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cmath>
#include <orbitforge/runtime/j2.hpp>
#include <orbitforge/runtime/j4.hpp>

namespace orbitforge::desktop {
namespace {
// Result lines use the themed muted/danger tones from the application stylesheet.
void restyle_message(QLabel *label, bool error) {
    label->setProperty("error", error);
    label->style()->unpolish(label);
    label->style()->polish(label);
}
constexpr std::array element_units{Unit::metre,  Unit::one,    Unit::radian,
                                   Unit::radian, Unit::radian, Unit::radian};
class StatePages final : public QStackedWidget {
  public:
    StatePages() {
        connect(this, &QStackedWidget::currentChanged, this, [this] { updateGeometry(); });
    }
    QSize sizeHint() const override {
        return currentWidget() ? currentWidget()->sizeHint() : QStackedWidget::sizeHint();
    }
    QSize minimumSizeHint() const override {
        return currentWidget() ? currentWidget()->minimumSizeHint()
                               : QStackedWidget::minimumSizeHint();
    }
};
QFrame *divider() {
    auto *line = new QFrame;
    line->setObjectName("sectionDivider");
    line->setFixedHeight(1);
    return line;
}
} // namespace
void OrbitEditor::set_time_provider(std::shared_ptr<const core::ITimeProvider> time) {
    time_ = time;
    for (auto *field : {epoch_, first_, last_})
        field->set_time_provider(time);
    update_tle_preview();
}
OrbitEditor::OrbitEditor(const OrbitInput &input, QWidget *parent) : QWidget(parent) {
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    // Page navigation adapts to the width: a tree beside wide pages, a row of tabs in an
    // inspector-width pane, and a selector when even tabs would not fit.
    auto *compact_bar = new QWidget(this);
    compact_bar->setObjectName("propertyCompactBar");
    auto *compact_layout = new QHBoxLayout(compact_bar);
    compact_layout->setContentsMargins(24, 12, 24, 0);
    compact_navigation_ = new QComboBox(compact_bar);
    compact_navigation_->setObjectName("propertyPageSelector");
    compact_navigation_->setAccessibleName(QStringLiteral("属性页面"));
    compact_navigation_->addItems({QStringLiteral("轨道"), QStringLiteral("轨迹显示"),
                                   QStringLiteral("二维视图"), QStringLiteral("三维视图与模型")});
    compact_layout->addWidget(compact_navigation_, 1);
    page_tabs_ = new QTabBar(compact_bar);
    page_tabs_->setObjectName("propertyPageTabs");
    page_tabs_->setAccessibleName(QStringLiteral("属性页面"));
    page_tabs_->setDrawBase(false);
    page_tabs_->setExpanding(false);
    page_tabs_->setUsesScrollButtons(true);
    for (const auto &[label, tip] :
         {std::pair{QStringLiteral("轨道"), QStringLiteral("轨道参数")},
          std::pair{QStringLiteral("轨迹"), QStringLiteral("轨迹显示")},
          std::pair{QStringLiteral("二维"), QStringLiteral("二维视图")},
          std::pair{QStringLiteral("三维"), QStringLiteral("三维视图与模型")}})
        page_tabs_->setTabToolTip(page_tabs_->addTab(label), tip);
    compact_layout->addWidget(page_tabs_);
    compact_layout->addStretch();
    outer->addWidget(compact_bar);
    connect(page_tabs_, &QTabBar::currentChanged, compact_navigation_, &QComboBox::setCurrentIndex);
    connect(compact_navigation_, &QComboBox::currentIndexChanged, page_tabs_, [this](int index) {
        const QSignalBlocker block(page_tabs_);
        page_tabs_->setCurrentIndex(index);
    });
    auto *navigation = new QTreeWidget;
    navigation_ = navigation;
    navigation->setObjectName("propertyNavigation");
    navigation->setFixedWidth(164);
    navigation->setHeaderHidden(true);
    navigation->setRootIsDecorated(false);
    navigation->setIndentation(16);
    navigation->setUniformRowHeights(true);
    navigation->setIconSize(QSize(17, 17));
    const auto category = [&](const QString &heading) {
        auto *group = new QTreeWidgetItem(navigation, {heading});
        group->setFlags(Qt::ItemIsEnabled);
        auto font = group->font(0);
        font.setWeight(QFont::DemiBold);
        group->setFont(0, font);
        group->setExpanded(true);
        return group;
    };
    const auto page = [](QTreeWidgetItem *group, const QString &name, int index) {
        auto *item = new QTreeWidgetItem(group, {name});
        const std::array icons{WorkbenchIcon::orbit, WorkbenchIcon::properties, WorkbenchIcon::map,
                               WorkbenchIcon::satellite};
        item->setIcon(0, workbench_icon(icons.at(static_cast<std::size_t>(index))));
        item->setData(0, Qt::UserRole, index);
    };
    auto *basic = category(QStringLiteral("基本"));
    page(basic, QStringLiteral("轨道"), 0);
    page(basic, QStringLiteral("轨迹显示"), 1);
    page(category(QStringLiteral("二维视图")), QStringLiteral("显示"), 2);
    page(category(QStringLiteral("三维视图")), QStringLiteral("显示与模型"), 3);
    auto *tabs = new QStackedWidget;
    // Hidden pages must not force the whole document to their preferred height.
    // The orbit/model fields already provide their own scroll areas.
    tabs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    outer->addWidget(navigation);
    outer->addWidget(tabs, 1);
    navigation->hide();
    outer->setDirection(QBoxLayout::TopToBottom);
    connect(navigation, &QTreeWidget::currentItemChanged, tabs, [tabs](QTreeWidgetItem *item) {
        if (item && item->parent())
            tabs->setCurrentIndex(item->data(0, Qt::UserRole).toInt());
    });
    connect(tabs, &QStackedWidget::currentChanged, compact_navigation_,
            &QComboBox::setCurrentIndex);
    connect(compact_navigation_, &QComboBox::currentIndexChanged, navigation,
            [navigation](int index) {
                for (int i = 0; i < navigation->topLevelItemCount(); ++i)
                    for (int j = 0; j < navigation->topLevelItem(i)->childCount(); ++j) {
                        auto *item = navigation->topLevelItem(i)->child(j);
                        if (item->data(0, Qt::UserRole).toInt() == index)
                            navigation->setCurrentItem(item);
                    }
            });
    new WidthObserver(this, [this, outer, compact_bar, mode = WidthMode{},
                             tight = WidthMode{}](int width) mutable {
        const bool narrow = mode.narrow(width, 680);
        const bool selector = tight.narrow(width, 400);
        navigation_->setVisible(!narrow);
        compact_bar->setVisible(narrow);
        compact_navigation_->setVisible(narrow && selector);
        page_tabs_->setVisible(narrow && !selector);
        outer->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    });
    auto *orbit_tab = new QWidget;
    tabs->addWidget(orbit_tab);
    auto *layout = new QVBoxLayout(orbit_tab);
    layout->setContentsMargins(theme::space::page);
    layout->setSpacing(12);
    auto *header = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("轨道参数"));
    title->setObjectName("panelTitle");
    header->addWidget(title);
    auto *identity_row = new QWidget;
    identity_row->setMaximumWidth(350);
    auto *identity = new QHBoxLayout(identity_row);
    identity->setContentsMargins(0, 0, 0, 0);
    identity->setSpacing(10);
    auto *identity_label = new QLabel(QStringLiteral("对象 ID"));
    identity_label->setObjectName("muted");
    identity_label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    identity->addWidget(identity_label);
    object_ = new QLineEdit;
    object_->setObjectName("objectId");
    object_->setAccessibleName(QStringLiteral("卫星对象 ID"));
    object_->setMaximumWidth(250);
    object_->setMinimumWidth(60);
    identity->addWidget(object_, 1);
    header->addSpacing(8);
    // Width-limited rows are centred by default once the header stacks vertically.
    header->addWidget(identity_row, 0, Qt::AlignLeft);
    header->addStretch();
    auto *body = new QLabel(QStringLiteral("中心天体  /  地球"));
    body->setObjectName("hint");
    header->addWidget(body);
    layout->addLayout(header);
    responsive_columns(orbit_tab, header, 480);
    new WidthObserver(orbit_tab, [body](int width) { body->setVisible(width >= 600); });
    // The first group's top rule separates the header; no extra divider here.
    fields_ = new QWidget;
    fields_->setObjectName("propertyFields");
    fields_->setMaximumWidth(900);
    auto *columns = new QHBoxLayout(fields_);
    // Room for the overlay scroll bar so it never covers field edges.
    columns->setContentsMargins(0, 0, 10, 0);
    columns->setSpacing(24);
    // Left: propagation settings and the read-only scenario span; right: the initial state.
    auto *settings = new QWidget;
    auto *settings_layout = new QVBoxLayout(settings);
    settings_layout->setContentsMargins(0, 0, 0, 0);
    settings_layout->setSpacing(0);
    auto *propagation_group = new FormGroup(QStringLiteral("传播设置"));
    auto *span_group = new FormGroup(QStringLiteral("所属场景时段"));
    settings_layout->addWidget(propagation_group);
    settings_layout->addWidget(span_group);
    auto *state_group = new FormGroup(QStringLiteral("初始状态"));
    columns->addWidget(settings, 1, Qt::AlignTop);
    columns->addWidget(state_group, 1, Qt::AlignTop);
    auto *form = propagation_group->form();
    auto *span_form = span_group->form();
    auto *state_form = state_group->form();
    state_title_ = state_group->title();
    auto *propagator_row = new QHBoxLayout;
    propagator_row->setSpacing(6);
    propagator_ = new QComboBox;
    propagator_->setObjectName("orbitPropagator");
    propagator_->setAccessibleName(QStringLiteral("轨道传播器"));
    propagator_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    propagator_->setMinimumContentsLength(8);
    propagator_->addItems({QStringLiteral("二体 · Two Body"), QStringLiteral("J2 · 长期摄动"),
                           QStringLiteral("J4 · 高阶长期摄动"), QStringLiteral("SGP4 · TLE"),
                           QStringLiteral("HPOP · 数值传播"),
                           QStringLiteral("任务序列 · 机动规划")});
    propagator_row->addWidget(propagator_, 1);
    auto *model_button = new QPushButton(QStringLiteral("模型…"));
    model_button_ = model_button;
    model_button->setObjectName("propagatorDetails");
    model_button->setToolTip(QStringLiteral("查看传播模型与中心天体常数"));
    model_button->setAutoDefault(false);
    propagator_row->addWidget(model_button, 0, Qt::AlignRight);
    form->addRow(QStringLiteral("传播器"), propagator_row);
    hpop_summary_ = new QLabel;
    hpop_summary_->setObjectName("hint");
    hpop_summary_->setWordWrap(true);
    form->addRow(hpop_summary_);
    connect(model_button, &QPushButton::clicked, this, &OrbitEditor::open_propagator_details);
    connect(propagator_, &QComboBox::currentIndexChanged, this,
            [this](int index) { set_propagator(static_cast<OrbitPropagator>(index)); });
    epoch_ = new EpochEditor;
    epoch_label_ = new QLabel(QStringLiteral("初始历元"));
    form->addRow(epoch_label_, epoch_);
    reference_frame_ = new QComboBox;
    reference_frame_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    reference_frame_->setMinimumContentsLength(8);
    reference_frame_->setObjectName("initialReferenceFrame");
    reference_frame_->setAccessibleName(QStringLiteral("初始状态参考系"));
    reference_frame_->addItems({QStringLiteral("GCRF · 地心惯性"), QStringLiteral("ITRF · 地固")});
    reference_frame_->setToolTip(QStringLiteral("ITRF 使用位置 / 速度；轨道根数在 GCRF 下定义。"));
    form->insertRow(1, QStringLiteral("参考系"), reference_frame_);
    connect(reference_frame_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (propagator_kind_ == OrbitPropagator::sgp4)
            return;
        if (index == 2) {
            const QSignalBlocker block(reference_frame_);
            reference_frame_->setCurrentIndex(input_frame_ == core::FrameId::gcrf ? 0 : 1);
            return;
        }
        set_reference_frame(index == 0 ? core::FrameId::gcrf : core::FrameId::itrf);
    });
    state_mode_ = new QComboBox;
    state_mode_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    state_mode_->setMinimumContentsLength(8);
    state_mode_->setObjectName("stateRepresentation");
    state_mode_->addItems(
        {QStringLiteral("位置 / 速度 · Cartesian"), QStringLiteral("经典轨道根数")});
    state_form->addRow(state_mode_);
    state_pages_ = new StatePages;
    auto *cartesian_page = new QWidget;
    auto *cartesian_form = new QFormLayout(cartesian_page);
    cartesian_form->setContentsMargins(0, 0, 0, 0);
    cartesian_form->setHorizontalSpacing(theme::space::md);
    cartesian_form->setVerticalSpacing(theme::space::row);
    cartesian_form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    state_pages_->addWidget(cartesian_page);
    const auto vector_row = [&](const QString &label, Unit unit, const QString &name,
                                auto &fields) {
        auto *caption = new QLabel(label);
        caption->setObjectName("muted");
        cartesian_form->addRow(caption);
        for (std::size_t i = 0; i < 3; ++i) {
            const auto axis = QString(QChar(static_cast<char>('X' + i)));
            fields[i] = new QuantityEditor(unit);
            fields[i]->setObjectName(name + axis);
            fields[i]->set_label(label + " " + axis);
            fields[i]->set_single_step(1, unit);
            fields[i]->setMaximumWidth(QWIDGETSIZE_MAX);
            cartesian_form->addRow(axis, fields[i]);
        }
    };
    vector_row(QStringLiteral("位置"), Unit::kilometre, "position", position_);
    vector_row(QStringLiteral("速度"), Unit::kilometres_per_second, "velocity", velocity_);
    auto *keplerian_page = new QWidget;
    auto *keplerian_form = new QFormLayout(keplerian_page);
    keplerian_form->setContentsMargins(0, 0, 0, 0);
    keplerian_form->setHorizontalSpacing(theme::space::md);
    keplerian_form->setVerticalSpacing(theme::space::row);
    keplerian_form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    const QStringList labels{QStringLiteral("半长轴 a"),     QStringLiteral("偏心率 e"),
                             QStringLiteral("倾角 i"),       QStringLiteral("升交点赤经 Ω"),
                             QStringLiteral("近地点幅角 ω"), QStringLiteral("真近点角 ν")};
    const QStringList names{"semiMajorAxis", "eccentricity",        "inclination",
                            "raan",          "argumentOfPeriapsis", "trueAnomaly"};
    for (std::size_t i = 0; i < 6; ++i) {
        const auto unit = i == 0 ? Unit::kilometre : i == 1 ? Unit::one : Unit::degree;
        auto *field = new QuantityEditor(unit);
        field->set_range(i == 0   ? -1e9
                         : i == 5 ? -360
                                  : 0,
                         i == 0   ? 1e9
                         : i == 1 ? 1000
                         : i == 2 ? 180
                                  : 360,
                         unit);
        field->set_single_step(i == 0 ? 10 : i == 1 ? 0.001 : 1, unit);
        field->set_label(labels[static_cast<int>(i)]);
        field->setObjectName(names[static_cast<int>(i)]);
        // Fields fill their column so every right edge in the column lines up.
        field->setMaximumWidth(QWIDGETSIZE_MAX);
        elements_[i] = field;
        keplerian_form->addRow(labels[static_cast<int>(i)], field);
        if (i == 5)
            anomaly_label_ = qobject_cast<QLabel *>(keplerian_form->labelForField(field));
    }
    state_mode_->setToolTip(QStringLiteral("GCRF 瞬时根数；抛物线轨道使用位置 / 速度。"));
    state_pages_->addWidget(keplerian_page);
    auto *tle_page = new QWidget;
    auto *tle_layout = new QVBoxLayout(tle_page);
    tle_layout->setContentsMargins(0, 0, 0, 0);
    tle_layout->setSpacing(12);
    auto *tle_hint = new QLabel(QStringLiteral("粘贴一组两行轨道根数，可带卫星名称。"));
    tle_hint->setObjectName("hint");
    tle_hint->setWordWrap(true);
    tle_layout->addWidget(tle_hint);
    tle_text_ = new QPlainTextEdit;
    tle_text_->setObjectName("tleInput");
    tle_text_->setAccessibleName(QStringLiteral("TLE 两行轨道根数"));
    tle_text_->setPlaceholderText("1 NNNNN…\n2 NNNNN…");
    tle_text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    tle_text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    tle_text_->setMinimumHeight(92);
    tle_text_->setMaximumHeight(112);
    tle_text_->setStyleSheet("font-family: Consolas; font-size: 12px;");
    tle_layout->addWidget(tle_text_);
    auto *import_tle = new QPushButton(QStringLiteral("导入 TLE 文件…"));
    import_tle->setObjectName("importTle");
    import_tle->setAutoDefault(false);
    tle_layout->addWidget(import_tle, 0, Qt::AlignLeft);
    tle_preview_ = new QLabel;
    tle_preview_->setObjectName("tlePreview");
    tle_preview_->setWordWrap(true);
    tle_preview_->setTextFormat(Qt::PlainText);
    tle_preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tle_layout->addWidget(tle_preview_);
    tle_layout->addStretch();
    connect(tle_text_, &QPlainTextEdit::textChanged, this, &OrbitEditor::update_tle_preview);
    connect(import_tle, &QPushButton::clicked, this, [this] {
        const auto path =
            QFileDialog::getOpenFileName(this, QStringLiteral("导入卫星 TLE"), {},
                                         QStringLiteral("TLE 文件 (*.tle *.txt);;所有文件 (*)"));
        if (path.isEmpty())
            return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) {
            show_result(QStringLiteral("无法读取文件；请导入仅包含一组 TLE 的文本文件。"), true);
            return;
        }
        const auto previous = tle_text_->toPlainText();
        tle_text_->setPlainText(QString::fromUtf8(file.readAll()));
        try {
            (void)core::parse_tle(read_tle());
        } catch (const std::exception &error) {
            tle_text_->setPlainText(previous);
            show_result(QString::fromUtf8(error.what()), true);
        }
    });
    state_pages_->addWidget(tle_page);
    state_form->addRow(state_pages_);
    connect(state_mode_, &QComboBox::currentIndexChanged, this,
            [this](int mode) { set_state_mode(mode); });
    const auto time_row = [&](const QString &label, EpochEditor *&field) {
        field = new EpochEditor;
        field->set_read_only(true);
        auto *caption = new QLabel(label);
        caption->setObjectName("hint");
        span_form->addRow(caption, field);
    };
    // The display step is the propagation output sampling, so it belongs with the
    // propagation settings; the scenario span below is read-only context.
    step_ = new QuantityEditor(Unit::second);
    step_->setObjectName("displayStep");
    step_->set_range(1, 60, Unit::second);
    step_->set_quantum(1, Unit::second);
    step_->set_label(QStringLiteral("轨道步长"));
    step_->setMaximumWidth(QWIDGETSIZE_MAX);
    form->addRow(QStringLiteral("轨道步长"), step_);
    span_group->add_help(QStringLiteral("分析时段由所属场景统一管理，可在场景设置中修改。"));
    time_row(QStringLiteral("起点"), first_);
    time_row(QStringLiteral("终点"), last_);
    first_->setToolTip(QStringLiteral("分析时段由所属 Scenario 统一管理"));
    last_->setToolTip(first_->toolTip());
    auto *parameters = new QScrollArea;
    parameters->setObjectName("orbitParametersScroll");
    parameters->setWidgetResizable(true);
    parameters->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    parameters->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    parameters->setWidget(fields_);
    responsive_columns(parameters->viewport(), columns, 720);
    // Labels stay beside fields down to inspector-column widths.
    responsive_form(form, 360);
    responsive_form(span_form, 360);
    responsive_form(keplerian_form, 350);
    align_form_labels(settings);
    layout->addWidget(parameters, 1);
    layout->addWidget(divider());
    message_ = new QLabel;
    message_->setObjectName("orbitResult");
    message_->setWordWrap(true);
    message_->setTextFormat(Qt::PlainText);
    message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    message_->hide();
    layout->addWidget(message_);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    apply_ = new QPushButton(QStringLiteral("应用"));
    apply_->setToolTip(QStringLiteral("应用轨道参数，重新计算并更新场景"));
    apply_->setAccessibleName(QStringLiteral("应用轨道"));
    apply_->setObjectName("applyOrbit");
    cancel_ = new QPushButton(QStringLiteral("取消计算"));
    cancel_->setEnabled(false);
    cancel_->hide();
    buttons->addWidget(cancel_);
    auto *close = new QPushButton(QStringLiteral("关闭"));
    close->setAutoDefault(false);
    connect(close, &QPushButton::clicked, this, [this] {
        if (close_requested)
            close_requested();
    });
    buttons->addWidget(close);
    buttons->addWidget(apply_);
    layout->addLayout(buttons);
    connect(apply_, &QPushButton::clicked, this, [this] { submit(); });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        if (cancel)
            cancel();
    });
    const auto display_page = [&](const QString &heading, const QString &description) {
        auto *page = new QWidget;
        tabs->addWidget(page);
        auto *stack = new QVBoxLayout(page);
        stack->setContentsMargins(theme::space::page);
        stack->setSpacing(12);
        auto *title = new QLabel(heading);
        title->setObjectName("panelTitle");
        stack->addWidget(title);
        auto *caption = new QLabel(description);
        caption->setObjectName("dialogDescription");
        caption->setWordWrap(true);
        stack->addWidget(caption);
        return stack;
    };
    const auto apply_display = [&](QVBoxLayout *stack) {
        auto *apply = new QPushButton(QStringLiteral("应用"));
        apply->setObjectName("applyDisplay");
        apply->setToolTip(QStringLiteral("应用轨迹显示设置"));
        apply->setAccessibleName(QStringLiteral("应用显示设置"));
        apply->setProperty("primary", true);
        display_buttons_.push_back(apply);
        connect(apply, &QPushButton::clicked, this, [this] { submit_display(); });
        stack->addStretch();
        stack->addWidget(divider());
        auto *error = new QLabel;
        error->setObjectName("inputError");
        error->setWordWrap(true);
        error->setTextFormat(Qt::PlainText);
        error->hide();
        display_messages_.push_back(error);
        stack->addWidget(error);
        // Same footer as the orbit and 3D pages: Close, then the page's single Apply.
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        auto *close = new QPushButton(QStringLiteral("关闭"));
        close->setAutoDefault(false);
        connect(close, &QPushButton::clicked, this, [this] {
            if (close_requested)
                close_requested();
        });
        buttons->addWidget(close);
        buttons->addWidget(apply);
        stack->addLayout(buttons);
    };
    auto *common = display_page(QStringLiteral("轨迹显示"),
                                QStringLiteral("当前卫星的二维与三维视图共用这些样式与显示范围。"));
    auto *style_group = new FormGroup(QStringLiteral("线条样式"));
    auto *extent_group = new FormGroup(QStringLiteral("显示范围"));
    auto *common_form = extent_group->form();
    track_extent_ = new QComboBox;
    track_extent_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    track_extent_->setMinimumContentsLength(8);
    track_extent_->setObjectName("trackExtent");
    track_extent_->setMaximumWidth(276);
    track_extent_->addItems({QStringLiteral("一圈轨迹"), QStringLiteral("整个场景时段"),
                             QStringLiteral("自定义前后时长")});
    common_form->addRow(QStringLiteral("范围"), track_extent_);
    track_extent_->setAccessibleName(QStringLiteral("显示范围"));
    extent_group->add_help(QStringLiteral("同时用于二维与三维；非闭合轨道的一圈按一小时显示。"));
    const auto duration = [&] {
        auto *field = new QuantityEditor(Unit::minute);
        field->set_range(0, 10080, Unit::minute);
        field->set_single_step(1, Unit::minute);
        field->set_value(45, Unit::minute);
        return field;
    };
    past_ = duration();
    future_ = duration();
    past_->setObjectName("pastDuration");
    past_->set_label(QStringLiteral("当前时刻之前"));
    future_->setObjectName("futureDuration");
    future_->set_label(QStringLiteral("当前时刻之后"));
    common_form->addRow(QStringLiteral("当前时刻之前"), past_);
    common_form->addRow(QStringLiteral("当前时刻之后"), future_);
    connect(track_extent_, &QComboBox::currentIndexChanged, this, [this, common_form](int index) {
        common_form->setRowVisible(past_, index == 2);
        common_form->setRowVisible(future_, index == 2);
    });
    common_form->setRowVisible(past_, false);
    common_form->setRowVisible(future_, false);
    auto *range_fields = new QWidget;
    range_fields->setObjectName("propertyFields");
    range_fields->setMaximumWidth(520);
    auto *range_layout = new QVBoxLayout(range_fields);
    range_layout->setContentsMargins(0, 0, 4, 0);
    range_layout->setSpacing(0);
    track_style_ = new TrackStyleEditor;
    style_group->form()->addRow(track_style_);
    range_layout->addWidget(style_group);
    range_layout->addWidget(extent_group);
    range_layout->addStretch();
    auto *range_scroll = new QScrollArea;
    range_scroll->setWidgetResizable(true);
    range_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    range_scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    range_scroll->setWidget(range_fields);
    responsive_form(common_form, 400);
    align_form_labels(range_fields);
    common->addWidget(range_scroll, 1);
    apply_display(common);
    auto *map = display_page(QStringLiteral("二维视图设置"),
                             QStringLiteral("设置当前卫星在地图中的轨迹与标注。"));
    auto *map_options = new FormGroup(QStringLiteral("地图标注"));
    map_options->setMaximumWidth(480);
    track_2d_ = new QCheckBox(QStringLiteral("显示地面轨迹"));
    label_2d_ = new QCheckBox(QStringLiteral("显示对象名称"));
    for (auto *toggle : {track_2d_, label_2d_}) {
        toggle->setChecked(true);
        make_switch(toggle);
        map_options->form()->addRow(toggle);
    }
    map->addWidget(map_options);
    apply_display(map);
    auto *appearance_tab = new QWidget;
    tabs->addWidget(appearance_tab);
    auto *appearance_layout = new QVBoxLayout(appearance_tab);
    appearance_layout->setContentsMargins(theme::space::page);
    appearance_layout->setSpacing(12);
    auto *appearance_title = new QLabel(QStringLiteral("三维视图设置"));
    appearance_title->setObjectName("panelTitle");
    appearance_layout->addWidget(appearance_title);
    auto *appearance_description = new QLabel(QStringLiteral("设置空间轨迹、卫星外观与示意尺寸。"));
    appearance_description->setObjectName("dialogDescription");
    appearance_description->setWordWrap(true);
    appearance_layout->addWidget(appearance_description);
    auto *appearance_fields = new QWidget;
    appearance_fields->setObjectName("propertyFields");
    appearance_fields->setMaximumWidth(560);
    auto *appearance_content = new QVBoxLayout(appearance_fields);
    appearance_content->setContentsMargins(0, 0, 0, 0);
    appearance_content->setSpacing(0);
    // Two inspector groups share the page's single Apply in the footer.
    const auto group = [appearance_content](const QString &title) {
        auto *frame = new FormGroup(title);
        appearance_content->addWidget(frame);
        return frame->form();
    };
    auto *track_group = group(QStringLiteral("空间轨迹"));
    track_3d_ = new QCheckBox(QStringLiteral("显示空间轨迹"));
    track_3d_->setChecked(true);
    make_switch(track_3d_);
    track_group->addRow(track_3d_);
    auto *display_error = new QLabel;
    display_error->setObjectName("inputError");
    display_error->setWordWrap(true);
    display_error->setTextFormat(Qt::PlainText);
    display_error->hide();
    display_messages_.push_back(display_error);
    track_group->addRow(display_error);
    auto *model_group = group(QStringLiteral("卫星外观"));
    model_fields_ = new QWidget;
    auto *form_model = new QFormLayout(model_fields_);
    form_model->setContentsMargins(0, 0, 0, 0);
    form_model->setHorizontalSpacing(theme::space::md);
    form_model->setVerticalSpacing(theme::space::row);
    form_model->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form_model->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    model_kind_ = new QComboBox;
    model_kind_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    model_kind_->setMinimumContentsLength(8);
    model_kind_->setMaximumWidth(320);
    model_kind_->addItems({QStringLiteral("点标记（默认）"), QStringLiteral("内置卫星模型"),
                           QStringLiteral("本地三维模型")});
    form_model->addRow(QStringLiteral("显示方式"), model_kind_);
    model_path_ = new QLineEdit;
    model_path_->setMaximumWidth(440);
    model_path_->setPlaceholderText(QStringLiteral(".glb / .gltf / .vsgb / .vsgt"));
    auto *browse = new QPushButton(QStringLiteral("选择模型文件…"));
    form_model->addRow(QStringLiteral("模型文件"), model_path_);
    form_model->addRow(QString{}, browse);
    model_diameter_ = new QuantityEditor(Unit::kilometre);
    model_diameter_->setObjectName("modelDiameter");
    model_diameter_->set_label(QStringLiteral("示意显示直径"));
    model_diameter_->set_range(0.001, 10000, Unit::kilometre);
    model_diameter_->set_single_step(1, Unit::kilometre);
    model_diameter_->set_value(1000, Unit::kilometre);
    form_model->addRow(QStringLiteral("示意显示直径"), model_diameter_);
    model_group->addRow(model_fields_);
    model_diameter_->setToolTip(QStringLiteral("模型示意尺寸；朝向固定于 ITRF。"));
    appearance_content->addStretch();
    auto *appearance_scroll = new QScrollArea;
    appearance_scroll->setWidgetResizable(true);
    appearance_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    appearance_scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    appearance_scroll->setWidget(appearance_fields);
    responsive_form(form_model, 400);
    align_form_labels(appearance_fields);
    appearance_layout->addWidget(appearance_scroll, 1);
    apply_model_ = new QPushButton(QStringLiteral("应用"));
    apply_model_->setObjectName("applyAppearance");
    apply_model_->setToolTip(QStringLiteral("应用空间轨迹显示与卫星外观"));
    apply_model_->setAccessibleName(QStringLiteral("应用三维视图设置"));
    apply_model_->setProperty("primary", true);
    appearance_layout->addWidget(divider());
    model_message_ = new QLabel;
    model_message_->setObjectName("modelResult");
    model_message_->hide();
    model_message_->setWordWrap(true);
    model_message_->setTextFormat(Qt::PlainText);
    appearance_layout->addWidget(model_message_);
    auto *appearance_buttons = new QHBoxLayout;
    appearance_buttons->addStretch();
    auto *close_appearance = new QPushButton(QStringLiteral("关闭"));
    close_appearance->setAutoDefault(false);
    connect(close_appearance, &QPushButton::clicked, this, [this] {
        if (close_requested)
            close_requested();
    });
    appearance_buttons->addWidget(close_appearance);
    appearance_buttons->addWidget(apply_model_);
    appearance_layout->addLayout(appearance_buttons);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("选择卫星模型"), {},
                                                       "3D models (*.glb *.gltf *.vsgb *.vsgt)");
        if (!path.isEmpty()) {
            model_path_->setText(path);
            model_kind_->setCurrentIndex(2);
        }
    });
    connect(model_kind_, &QComboBox::currentIndexChanged, this,
            [this, browse, form_model](int kind) {
                model_path_->setEnabled(kind == 2);
                browse->setEnabled(kind == 2);
                model_diameter_->setEnabled(kind != 0);
                form_model->setRowVisible(model_path_, kind == 2);
                form_model->setRowVisible(browse, kind == 2);
                form_model->setRowVisible(model_diameter_, kind != 0);
            });
    model_path_->setEnabled(false);
    browse->setEnabled(false);
    model_diameter_->setEnabled(false);
    form_model->setRowVisible(model_path_, false);
    form_model->setRowVisible(browse, false);
    form_model->setRowVisible(model_diameter_, false);
    connect(apply_model_, &QPushButton::clicked, this, &OrbitEditor::apply_appearance_page);
    navigation->setCurrentItem(navigation->topLevelItem(0)->child(0));
    set_input(input);
}
void OrbitEditor::set_input(const OrbitInput &input) {
    const QSignalBlocker state_block(state_mode_), frame_block(reference_frame_),
        propagator_block(propagator_);
    body_ = input.initial.origin;
    propagator_kind_ = input.propagator;
    hpop_ = input.hpop;
    mission_ = input.mission;
    hpop_initialized_ = uses_hpop() || hpop_ != core::HpopOptions{};
    manual_state_ = input.initial;
    tle_text_->setPlainText(QString::fromStdString(
        input.tle.line1 + (input.tle.line1.empty() ? "" : "\n") + input.tle.line2));
    // Reopening a TLE must not transform the inactive manual draft using EOP at
    // its old epoch. Its canonical state is retained and shown in GCRF if restored.
    input_frame_ =
        input.propagator == OrbitPropagator::sgp4 ? core::FrameId::gcrf : input.input_frame;
    propagator_->setCurrentIndex(static_cast<int>(propagator_kind_));
    reference_frame_->setCurrentIndex(input_frame_ == core::FrameId::gcrf ? 0 : 1);
    object_->setText(QString::fromStdString(input.object_id));
    epoch_->set_epoch(input.initial.epoch);
    first_->set_epoch(input.first);
    last_->set_epoch(input.last);
    step_->set_value(input.step_seconds, Unit::second);
    write_cartesian(in_frame(input.initial, input_frame_));
    state_mode_index_ = 0;
    if (input_frame_ == core::FrameId::gcrf)
        try {
            write_elements(input.initial);
            state_mode_index_ = 1;
        } catch (const std::exception &) { /* Parabolic states retain Cartesian input. */
        }
    state_mode_->setCurrentIndex(state_mode_index_);
    state_pages_->setCurrentIndex(state_mode_index_);
    update_state_labels();
    update_tle_preview();
}
core::CartesianState OrbitEditor::read_state() const {
    if (propagator_kind_ == OrbitPropagator::sgp4)
        return manual_state_;
    if (state_mode_index_ == 1) {
        if (propagator_kind_ == OrbitPropagator::j4_secular)
            return runtime::j4_mean_ellipse_state(
                {epoch_->epoch(), elements_[0]->value(Unit::metre), elements_[1]->value(Unit::one),
                 elements_[2]->value(Unit::radian), elements_[3]->value(Unit::radian),
                 elements_[4]->value(Unit::radian), elements_[5]->value(Unit::radian)});
        if (propagator_kind_ == OrbitPropagator::j2_secular)
            return runtime::j2_mean_ellipse_state(
                {epoch_->epoch(), elements_[0]->value(Unit::metre), elements_[1]->value(Unit::one),
                 elements_[2]->value(Unit::radian), elements_[3]->value(Unit::radian),
                 elements_[4]->value(Unit::radian), elements_[5]->value(Unit::radian)},
                body_);
        return runtime::to_cartesian(
            {epoch_->epoch(), elements_[0]->value(Unit::metre), elements_[1]->value(Unit::one),
             elements_[2]->value(Unit::radian), elements_[3]->value(Unit::radian),
             elements_[4]->value(Unit::radian), elements_[5]->value(Unit::radian)},
            uses_hpop() ? core::GravitationalParameter{hpop_.gravity_mu}
                        : core::central_body(body_).mu);
    }
    core::CartesianState state{epoch_->epoch(), body_, input_frame_, {}, {}};
    for (std::size_t k = 0; k < 3; ++k) {
        state.position_m[k] = position_[k]->value(Unit::metre);
        state.velocity_mps[k] = velocity_[k]->value(Unit::metres_per_second);
    }
    return in_frame(state, core::FrameId::gcrf);
}
core::CartesianState OrbitEditor::in_frame(core::CartesianState state, core::FrameId frame) const {
    if (state.frame == frame)
        return state;
    if (!transform_state)
        throw std::runtime_error("参考系转换需要场景的地球定向数据。");
    return transform_state(std::move(state), frame);
}
void OrbitEditor::write_cartesian(const core::CartesianState &state) {
    for (std::size_t k = 0; k < 3; ++k) {
        position_[k]->validate_value(state.position_m[k], Unit::metre);
        velocity_[k]->validate_value(state.velocity_mps[k], Unit::metres_per_second);
    }
    for (std::size_t k = 0; k < 3; ++k) {
        position_[k]->set_value(state.position_m[k], Unit::metre);
        velocity_[k]->set_value(state.velocity_mps[k], Unit::metres_per_second);
    }
}
void OrbitEditor::write_elements(const core::CartesianState &state) {
    std::array<double, 6> values;
    if (propagator_kind_ == OrbitPropagator::j2_secular ||
        propagator_kind_ == OrbitPropagator::j4_secular) {
        const auto elements = propagator_kind_ == OrbitPropagator::j4_secular
                                  ? runtime::j4_mean_ellipse_elements(state)
                                  : runtime::j2_mean_ellipse_elements(state);
        values = {elements.semi_major_axis_m,         elements.eccentricity,
                  elements.inclination_rad,           elements.raan_rad,
                  elements.argument_of_periapsis_rad, elements.mean_anomaly_rad};
    } else {
        const auto elements = runtime::to_keplerian(
            state, uses_hpop() ? core::GravitationalParameter{hpop_.gravity_mu}
                               : core::central_body(body_).mu);
        values = {elements.semi_major_axis_m,         elements.eccentricity,
                  elements.inclination_rad,           elements.raan_rad,
                  elements.argument_of_periapsis_rad, elements.true_anomaly_rad};
    }
    for (std::size_t k = 0; k < 6; ++k)
        elements_[k]->validate_value(values[k], element_units[k]);
    for (std::size_t k = 0; k < 6; ++k)
        elements_[k]->set_value(values[k], element_units[k]);
}
void OrbitEditor::set_state_mode(int mode) {
    if (propagator_kind_ == OrbitPropagator::sgp4)
        return;
    if (mode == state_mode_index_)
        return;
    try {
        const auto state = read_state();
        if (mode == 1)
            write_elements(state);
        else
            write_cartesian(in_frame(state, input_frame_));
        if (mode == 1) {
            const QSignalBlocker frame_block(reference_frame_);
            input_frame_ = core::FrameId::gcrf;
            reference_frame_->setCurrentIndex(0);
        }
        state_mode_index_ = mode;
        const QSignalBlocker block(state_mode_);
        state_mode_->setCurrentIndex(mode);
        state_pages_->setCurrentIndex(mode);
        show_result(QStringLiteral("已转换状态表示，点击应用后更新对象。"));
    } catch (const std::exception &exception) {
        const QSignalBlocker block(state_mode_);
        state_mode_->setCurrentIndex(state_mode_index_);
        show_result(QString::fromUtf8(exception.what()), true);
    }
}
void OrbitEditor::set_reference_frame(core::FrameId frame) {
    if (propagator_kind_ == OrbitPropagator::sgp4)
        return;
    if (frame == input_frame_)
        return;
    const QSignalBlocker frame_block(reference_frame_), mode_block(state_mode_);
    try {
        if (frame != core::FrameId::gcrf && frame != core::FrameId::itrf)
            throw std::invalid_argument("Unsupported initial-state reference frame");
        const auto state = in_frame(read_state(), frame);
        write_cartesian(state);
        input_frame_ = frame;
        state_mode_index_ = 0;
        state_mode_->setCurrentIndex(0);
        state_pages_->setCurrentIndex(0);
        show_result({});
    } catch (const std::exception &error) {
        show_result(QString::fromUtf8(error.what()), true);
    }
    reference_frame_->setCurrentIndex(input_frame_ == core::FrameId::gcrf ? 0 : 1);
}
void OrbitEditor::set_propagator(OrbitPropagator propagator) {
    if (propagator == propagator_kind_)
        return;
    const auto previous = propagator_kind_;
    const QSignalBlocker block(propagator_);
    try {
        auto state = manual_state_;
        if (propagator == OrbitPropagator::sgp4) {
            try {
                state = read_state();
            } catch (const std::exception &) {
                // An incomplete manual form must not block independent TLE input.
            }
        } else {
            state = read_state();
        }
        if (propagator == OrbitPropagator::j2_secular)
            (void)runtime::j2_mean_ellipse_elements(state);
        else if (propagator == OrbitPropagator::j4_secular)
            (void)runtime::j4_mean_ellipse_elements(state);
        if (propagator == OrbitPropagator::sgp4)
            manual_state_ = state;
        propagator_kind_ = propagator;
        if (propagator == OrbitPropagator::hpop && !hpop_initialized_) {
            hpop_ = new_hpop_options();
            hpop_initialized_ = true;
        }
        if (propagator == OrbitPropagator::mission && mission_.segments.empty()) {
            const double seconds = last_->epoch().seconds_since(state.epoch);
            if (seconds > 0 && seconds <= core::hpop_max_duration_seconds)
                mission_.segments.push_back(core::MissionSegment::coast(std::chrono::nanoseconds{
                    last_->epoch().tai_nanoseconds() - state.epoch.tai_nanoseconds()}));
        }
        if (state_mode_index_ == 1 && propagator != OrbitPropagator::sgp4)
            write_elements(state);
        update_state_labels();
        show_result(propagator == OrbitPropagator::sgp4
                        ? QStringLiteral("使用独立的 TLE 输入；应用后更新轨道。")
                    : propagator == OrbitPropagator::two_body
                        ? QStringLiteral("以当前椭圆定义二体初始状态。")
                    : propagator == OrbitPropagator::hpop
                        ? QStringLiteral("以当前状态作为瞬时初值；可在配置中调整引力与积分设置。")
                    : propagator == OrbitPropagator::mission
                        ? QStringLiteral("以当前状态作为任务起点；点击规划添加滑行段与脉冲机动。")
                        : QStringLiteral("以当前椭圆定义平均根数，不含周期项转换。"));
    } catch (const std::exception &error) {
        propagator_kind_ = previous;
        show_result(QString::fromUtf8(error.what()), true);
    }
    propagator_->setCurrentIndex(static_cast<int>(propagator_kind_));
}
void OrbitEditor::update_state_labels() {
    const bool hpop = propagator_kind_ == OrbitPropagator::hpop;
    model_button_->setText(hpop ? QStringLiteral("配置…") : QStringLiteral("模型…"));
    model_button_->setToolTip(hpop ? QStringLiteral("设置 HPOP 力模型与数值积分")
                                   : QStringLiteral("查看传播模型与中心天体常数"));
    hpop_summary_->setVisible(hpop);
    hpop_summary_->setText(
        (hpop_.gravity_degree == 0
             ? QStringLiteral("点质量")
             : QStringLiteral("%1 %2 × %3")
                   .arg(hpop_.gravity_model.empty() ? QStringLiteral("EGM96 内置")
                                                    : QString::fromStdString(hpop_.gravity_model))
                   .arg(hpop_.gravity_degree)
                   .arg(hpop_.gravity_order)) +
        (hpop_.uses_third_bodies()
             ? QStringLiteral(" + %1 个第三体").arg(hpop_.enabled_third_bodies().size())
             : QString{}) +
        (hpop_.srp.enabled ? QStringLiteral(" + 太阳辐射压") : QString{}) +
        (hpop_.drag.enabled ? QStringLiteral(" + 大气阻力") : QString{}) +
        (hpop_.integrator == core::HpopIntegrator::rkf78   ? QStringLiteral(" · RKF 7(8)")
         : hpop_.integrator == core::HpopIntegrator::rkv89 ? QStringLiteral(" · RKV 8(9) Efficient")
         : hpop_.integrator == core::HpopIntegrator::bulirsch_stoer
             ? QStringLiteral(" · Bulirsch–Stoer")
         : hpop_.integrator == core::HpopIntegrator::gauss_jackson
             ? QStringLiteral(" · Gauss–Jackson 12")
             : QStringLiteral(" · RK4")) +
        (hpop_.uses_fixed_step() ? QStringLiteral(" 固定步长") : QStringLiteral(" 相对误差")));
    if (propagator_kind_ == OrbitPropagator::mission) {
        model_button_->setText(QStringLiteral("规划…"));
        model_button_->setToolTip(QStringLiteral("编辑滑行与脉冲机动序列"));
        hpop_summary_->setVisible(true);
        double total_delta_v = 0;
        for (const auto &segment : mission_.segments)
            if (segment.kind == core::MissionSegmentKind::impulse)
                total_delta_v += std::hypot(segment.delta_v_mps[0], segment.delta_v_mps[1],
                                            segment.delta_v_mps[2]);
        hpop_summary_->setText(
            QStringLiteral("%1 段 · %2 滑行 · Δv %3 m/s")
                .arg(mission_.segments.size())
                .arg(uses_hpop() ? QStringLiteral("HPOP") : QStringLiteral("二体"))
                .arg(total_delta_v, 0, 'g', 8));
    }
    const bool sgp4 = propagator_kind_ == OrbitPropagator::sgp4;
    const bool j2 = propagator_kind_ == OrbitPropagator::j2_secular ||
                    propagator_kind_ == OrbitPropagator::j4_secular;
    const auto model = propagator_kind_ == OrbitPropagator::j4_secular ? "J4" : "J2";
    state_mode_->setVisible(!sgp4);
    epoch_->setVisible(!sgp4);
    epoch_label_->setVisible(!sgp4);
    reference_frame_->setEnabled(!sgp4);
    const QSignalBlocker frame_block(reference_frame_);
    if (sgp4 && reference_frame_->count() == 2)
        reference_frame_->addItem(QStringLiteral("TEME · TLE 原生"));
    else if (!sgp4 && reference_frame_->count() == 3)
        reference_frame_->removeItem(2);
    reference_frame_->setCurrentIndex(sgp4 ? 2 : input_frame_ == core::FrameId::gcrf ? 0 : 1);
    state_pages_->setCurrentIndex(sgp4 ? 2 : state_mode_index_);
    state_title_->setText(sgp4 ? QStringLiteral("两行轨道根数 · TLE")
                          : j2 ? QStringLiteral("初始平均椭圆 · %1").arg(model)
                               : QStringLiteral("初始状态"));
    state_mode_->setItemText(0, j2 ? QStringLiteral("平均椭圆位置 / 速度")
                                   : QStringLiteral("位置 / 速度 · Cartesian"));
    state_mode_->setItemText(1, j2 ? QStringLiteral("%1 平均轨道根数").arg(model)
                                   : QStringLiteral("经典轨道根数 · 瞬时"));
    state_mode_->setToolTip(j2 ? QStringLiteral("长期项平均椭圆；位置/速度是辅助开普勒状态，"
                                                "不代表瞬时根数或传播轨迹的实际速度。")
                               : QStringLiteral("GCRF 瞬时根数；抛物线轨道使用位置 / 速度。"));
    anomaly_label_->setText(j2 ? QStringLiteral("平近点角 M") : QStringLiteral("真近点角 ν"));
    elements_[5]->set_label(anomaly_label_->text());
    elements_[5]->setObjectName(j2 ? "meanAnomaly" : "trueAnomaly");
}
core::Tle OrbitEditor::read_tle() const {
    const auto text = tle_text_->toPlainText();
    if (text.size() > 4096)
        throw std::invalid_argument("请只输入一颗卫星的一组 TLE。");
    auto lines = text.split('\n', Qt::SkipEmptyParts);
    for (auto &line : lines)
        line = line.trimmed();
    if (lines.size() == 3 && !lines[0].startsWith("1 ") && !lines[0].startsWith("2 "))
        lines.removeFirst();
    if (lines.size() != 2)
        throw std::invalid_argument("请输入完整的两行 TLE，可在前面附一行卫星名称。");
    return {lines[0].toStdString(), lines[1].toStdString()};
}
void OrbitEditor::update_tle_preview() {
    if (!tle_preview_)
        return;
    if (tle_text_->toPlainText().trimmed().isEmpty()) {
        tle_preview_->setText(QStringLiteral("导入后显示编号、历元与轨道周期。"));
        return;
    }
    try {
        const auto parsed = core::parse_tle(read_tle());
        const auto &utc = parsed.epoch_utc;
        const auto date = QDate(utc.year, utc.month, utc.day).toString("yyyy-MM-dd");
        const auto clock = QTime(utc.hour, utc.minute, utc.second).toString("HH:mm:ss") + "." +
                           QString::number(utc.nanosecond).rightJustified(9, '0');
        QString text = QStringLiteral("编号  %1\n历元  %2 %3 UTC\n周期  %4 min")
                           .arg(QString::fromStdString(parsed.catalog_id), date, clock)
                           .arg(1440 / parsed.mean_motion_revolutions_per_day, 0, 'f', 3);
        if (time_) {
            const auto epoch = time_->from_utc(utc);
            const double age = std::max(std::abs(first_->epoch().seconds_since(epoch)),
                                        std::abs(last_->epoch().seconds_since(epoch))) /
                               86400;
            if (age > 14)
                text +=
                    QStringLiteral("\n\n场景距 TLE 历元最远 %1 天，建议更换接近场景时段的根数。")
                        .arg(age, 0, 'f', 1);
        }
        tle_preview_->setText(text);
    } catch (const std::exception &error) {
        tle_preview_->setText(
            QStringLiteral("TLE 未就绪：%1").arg(QString::fromUtf8(error.what())));
    }
}
void OrbitEditor::open_propagator_details() {
    if (propagator_kind_ == OrbitPropagator::mission) {
        try {
            const auto state = read_state();
            if (edit_mission(mission_, hpop_, state.epoch, first_->epoch(), last_->epoch(), this,
                             gravity_models ? gravity_models()
                                            : std::vector<runtime::CatalogGravity>{},
                             ephemerides ? ephemerides() : std::vector<runtime::CatalogEphemeris>{},
                             manage_ephemerides, data_catalog, !hpop_initialized_)) {
                hpop_initialized_ =
                    hpop_initialized_ || uses_hpop() || hpop_ != core::HpopOptions{};
                if (state_mode_index_ == 1)
                    write_elements(state);
                update_state_labels();
            }
        } catch (const std::exception &error) {
            show_result(QString::fromUtf8(error.what()), true);
        }
        return;
    }
    if (propagator_kind_ == OrbitPropagator::hpop) {
        if (edit_hpop_settings(
                hpop_, this,
                gravity_models ? gravity_models() : std::vector<runtime::CatalogGravity>{},
                ephemerides ? ephemerides() : std::vector<runtime::CatalogEphemeris>{},
                manage_ephemerides, data_catalog))
            update_state_labels();
        return;
    }
    try {
        QDialog dialog(this);
        dialog.setObjectName("propagatorDetailsDialog");
        const bool j4 = propagator_kind_ == OrbitPropagator::j4_secular;
        const bool sgp4 = propagator_kind_ == OrbitPropagator::sgp4;
        const bool j2 = propagator_kind_ == OrbitPropagator::j2_secular || j4;
        dialog.setWindowTitle(propagator_->currentText() + QStringLiteral(" · 模型信息"));
        dialog.setMinimumWidth(400);
        auto *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(theme::space::page);
        layout->setSpacing(12);
        auto *form = new QFormLayout;
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(10);
        const auto row = [&](const QString &label, const QString &value) {
            auto *text = new QLabel(value);
            text->setTextFormat(Qt::PlainText);
            text->setTextInteractionFlags(Qt::TextSelectableByMouse);
            form->addRow(label, text);
        };
        const auto &body = j4 ? core::j4_earth() : core::central_body(body_);
        row(QStringLiteral("传播模型"), sgp4 ? QStringLiteral("SGP4 · 近地 / 深空自动选择")
                                        : j4 ? QStringLiteral("J2、J2²、J4 长期项")
                                        : j2 ? QStringLiteral("一阶 J2 长期项")
                                             : QStringLiteral("二体"));
        row(QStringLiteral("中心天体"), sgp4 ? QStringLiteral("地球 · WGS72")
                                        : j4 ? QStringLiteral("地球 · EGM2008")
                                             : QStringLiteral("地球 · IERS 2003"));
        row(QStringLiteral("引力参数 μ"),
            QString::number(sgp4 ? 398600.8 : body.mu.metres_cubed_per_second_squared / 1e9, 'g',
                            15) +
                QStringLiteral(" km³/s²"));
        row(QStringLiteral("传播参考系"),
            sgp4 ? QStringLiteral("TEME → GCRF / ITRF") : QStringLiteral("GCRF"));
        if (sgp4) {
            row(QStringLiteral("根数输入"), QStringLiteral("一组固定 TLE，含 B* 阻力项"));
            row(QStringLiteral("使用建议"), QStringLiteral("使用接近分析时段的根数"));
        }
        if (j2) {
            row(QStringLiteral("参考半径"),
                QString::number(body.equatorial_radius_m / 1000, 'g', 15) + " km");
            row(QStringLiteral("J2 系数"), QString::number(body.j2, 'g', 15));
            if (j4)
                row(QStringLiteral("J4 系数"), QString::number(body.j4, 'g', 15));
            row(QStringLiteral("长期项"), QStringLiteral("Ω、ω、M（全部计入）"));
        }
        layout->addLayout(form);
        layout->addWidget(divider());
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
        buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
        buttons->button(QDialogButtonBox::Close)->setDefault(true);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.exec();
    } catch (const std::exception &error) {
        show_result(QString::fromUtf8(error.what()), true);
    }
}
void OrbitEditor::set_create_mode(bool create) {
    create_mode_ = create;
    object_->setReadOnly(!create);
    apply_->setText(create ? QStringLiteral("插入卫星") : QStringLiteral("应用"));
    navigation_->setCurrentItem(navigation_->topLevelItem(0)->child(0));
    compact_navigation_->setEnabled(!create);
    page_tabs_->setEnabled(!create);
    for (int i = 0; i < navigation_->topLevelItemCount(); ++i)
        for (int j = 0; j < navigation_->topLevelItem(i)->childCount(); ++j) {
            auto *item = navigation_->topLevelItem(i)->child(j);
            if (item->data(0, Qt::UserRole).toInt() == 0)
                continue;
            item->setDisabled(create);
            item->setToolTip(0, create ? QStringLiteral("插入卫星后可配置") : QString{});
        }
    refresh_enabled();
}
void OrbitEditor::submit() {
    if (!apply_->isEnabled())
        return;
    try {
        OrbitInput input;
        input.object_id = object_->text().trimmed().toStdString();
        input.initial = read_state();
        input.propagator = propagator_kind_;
        input.hpop = hpop_;
        input.mission = mission_;
        if (propagator_kind_ == OrbitPropagator::sgp4) {
            input.tle = read_tle();
            const auto parsed = core::parse_tle(input.tle);
            if (!time_)
                throw std::invalid_argument("TLE 历元需要已确认的 UTC 数据。");
            (void)time_->from_utc(parsed.epoch_utc);
        }
        input.input_frame = input_frame_;
        input.first = first_->epoch();
        input.last = last_->epoch();
        input.step_seconds = static_cast<int>(step_->value(Unit::second));
        if (apply)
            apply(std::move(input));
    } catch (const std::exception &error) {
        show_result(QString::fromUtf8(error.what()), true);
    }
}
void OrbitEditor::set_busy(bool busy) {
    orbit_busy_ = busy;
    refresh_enabled();
    cancel_->setEnabled(busy);
    cancel_->setVisible(busy);
    if (busy)
        show_result(QStringLiteral("正在计算…"));
}
void OrbitEditor::show_result(const QString &message, bool error) {
    restyle_message(message_, error);
    message_->setText(message);
    message_->setVisible(!message.isEmpty());
}
void OrbitEditor::configure_appearance(int kind, const QString &path, double diameter_km) {
    set_appearance(kind, path, diameter_km);
    // Explicit configuration always reaches the workbench, even when unchanged.
    applied_model_kind_ = -1;
    apply_appearance_page();
}
void OrbitEditor::set_appearance(int kind, const QString &path, double diameter_km) {
    model_kind_->setCurrentIndex(kind);
    model_path_->setText(path);
    model_diameter_->set_value(diameter_km, Unit::kilometre);
    applied_model_kind_ = kind;
    applied_model_path_ = path;
    applied_model_diameter_km_ = diameter_km;
}
// Applies the 3D page: the track switch through the display settings, and the model only
// when it changed, so an unchanged appearance does not rebuild the model.
void OrbitEditor::apply_appearance_page() {
    if (track_3d_->isChecked() != applied_track_3d_)
        submit_display();
    try {
        const int kind = model_kind_->currentIndex();
        const auto path = model_path_->text();
        const double diameter = model_diameter_->value(Unit::kilometre);
        if (kind == applied_model_kind_ && path == applied_model_path_ &&
            diameter == applied_model_diameter_km_)
            return;
        if (appearance)
            appearance(kind, path, diameter);
        applied_model_kind_ = kind;
        applied_model_path_ = path;
        applied_model_diameter_km_ = diameter;
    } catch (const std::exception &error) {
        show_appearance_result(QString::fromUtf8(error.what()), true);
    }
}
void OrbitEditor::set_appearance_busy(bool busy) {
    model_busy_ = busy;
    refresh_enabled();
    if (busy)
        show_appearance_result(QStringLiteral("正在准备三维模型…"));
}
void OrbitEditor::refresh_enabled() {
    for (auto *button : display_buttons_)
        button->setEnabled(!create_mode_ && !orbit_busy_ && !model_busy_);
    const bool idle = !orbit_busy_ && !model_busy_;
    object_->setEnabled(idle);
    fields_->setEnabled(idle);
    apply_->setEnabled(idle);
    model_fields_->setEnabled(idle && !create_mode_);
    apply_model_->setEnabled(idle && !create_mode_);
}
void OrbitEditor::show_appearance_result(const QString &message, bool error) {
    restyle_message(model_message_, error);
    model_message_->setText(message);
    model_message_->setVisible(!message.isEmpty());
}
void OrbitEditor::set_display(const DisplaySettings &settings) {
    track_style_->set_settings(settings);
    track_extent_->setCurrentIndex(settings.extent);
    past_->set_value(settings.past_minutes, Unit::minute);
    future_->set_value(settings.future_minutes, Unit::minute);
    track_2d_->setChecked(settings.track_2d);
    label_2d_->setChecked(settings.labels_2d);
    track_3d_->setChecked(settings.track_3d);
    applied_track_3d_ = settings.track_3d;
}
void OrbitEditor::submit_display() {
    try {
        if (display) {
            DisplaySettings settings{track_extent_->currentIndex(), past_->value(Unit::minute),
                                     future_->value(Unit::minute),  track_2d_->isChecked(),
                                     label_2d_->isChecked(),        track_3d_->isChecked()};
            track_style_->write_settings(settings);
            display(settings);
            applied_track_3d_ = settings.track_3d;
        }
        for (auto *error : display_messages_)
            error->hide();
    } catch (const std::exception &exception) {
        for (auto *error : display_messages_) {
            error->setText(QString::fromUtf8(exception.what()));
            error->show();
        }
    }
}
} // namespace orbitforge::desktop
