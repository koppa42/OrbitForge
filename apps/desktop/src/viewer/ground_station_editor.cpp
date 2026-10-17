#include "ground_station_editor.hpp"
#include "form_kit.hpp"
#include "quantity_editor.hpp"
#include "responsive_layout.hpp"
#include "workbench_style.hpp"
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace orbitforge::desktop {
GroundStationEditor::GroundStationEditor(QWidget *parent) : QDialog(parent) {
    setObjectName("groundStationProperties");
    setWindowIcon(workbench_icon(WorkbenchIcon::ground_station));
    resize(480, 620);
    setMinimumSize(320, 280);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea;
    scroll->setObjectName("groundStationScroll");
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    fields_ = new QWidget;
    fields_->setMaximumWidth(680);
    scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto *body = new QVBoxLayout(fields_);
    body->setContentsMargins(theme::space::page);
    body->setSpacing(theme::space::sm);
    auto *title = new QLabel(QStringLiteral("地面站"));
    title->setObjectName("panelTitle");
    body->addWidget(title);
    auto *description = new QLabel(QStringLiteral("固定在地球表面的观测位置，随地球一起转动。"));
    description->setObjectName("dialogDescription");
    description->setWordWrap(true);
    body->addWidget(description);
    // Inspector groups: identity, position (with the derived ITRF position) and access
    // constraints. Datum and convention notes sit behind each group's info marker.
    auto *groups = new QVBoxLayout;
    groups->setContentsMargins(0, theme::space::sm, 0, 0);
    groups->setSpacing(0);
    body->addLayout(groups);
    std::vector<FormGroup *> frames;
    const auto group = [&](const QString &heading) {
        frames.push_back(new FormGroup(heading));
        groups->addWidget(frames.back());
        return frames.back()->form();
    };
    auto *identity = group(QStringLiteral("站点"));
    name_ = new QLineEdit;
    name_->setObjectName("stationName");
    name_->setMaxLength(16384);
    name_->setMaximumWidth(276);
    identity->addRow(QStringLiteral("名称"), name_);
    auto *form = group(QStringLiteral("位置 · WGS84"));
    longitude_ = new QuantityEditor(Unit::degree);
    latitude_ = new QuantityEditor(Unit::degree);
    height_ = new QuantityEditor(Unit::metre);
    longitude_->setObjectName("stationLongitude");
    latitude_->setObjectName("stationLatitude");
    height_->setObjectName("stationHeight");
    longitude_->set_label(QStringLiteral("经度"));
    latitude_->set_label(QStringLiteral("纬度"));
    height_->set_label(QStringLiteral("椭球高"));
    longitude_->set_range(-180, 180, Unit::degree);
    latitude_->set_range(-90, 90, Unit::degree);
    height_->set_range(-12000, 100000, Unit::metre);
    height_->set_single_step(10, Unit::metre);
    form->addRow(QStringLiteral("经度"), longitude_);
    form->addRow(QStringLiteral("纬度"), latitude_);
    form->addRow(QStringLiteral("椭球高"), height_);
    frames[1]->add_help(QStringLiteral("大地纬度；东经、北纬为正，西经、南纬为负。"));
    frames[1]->add_help(
        QStringLiteral("椭球高以 WGS84 椭球面为零，与海拔不同。支持 −12,000 至 100,000 m。"));
    preview_ = new QLabel;
    preview_->setObjectName("stationPositionPreview");
    preview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    preview_->setWordWrap(true);
    form->addRow(QStringLiteral("ITRF / m"), preview_);
    auto *constraint_form = group(QStringLiteral("可见性约束"));
    minimum_elevation_ = new QuantityEditor(Unit::degree);
    minimum_elevation_->setObjectName("stationMinimumElevation");
    minimum_elevation_->set_label(QStringLiteral("最低仰角"));
    minimum_elevation_->set_range(0, 90, Unit::degree);
    constraint_form->addRow(QStringLiteral("最低仰角"), minimum_elevation_);
    frames[2]->add_help(QStringLiteral("可见性分析同时检查地球遮挡与最低仰角。0° 为当地水平线。"));
    responsive_form(identity);
    responsive_form(form);
    responsive_form(constraint_form);
    align_form_labels(fields_);
    body->addStretch();
    scroll->setWidget(fields_);
    layout->addWidget(scroll, 1);
    auto *footer = new QWidget;
    auto *footer_layout = new QVBoxLayout(footer);
    footer_layout->setContentsMargins(theme::space::footer);
    result_ = new QLabel;
    result_->setObjectName("stationResult");
    result_->setTextFormat(Qt::PlainText);
    result_->setWordWrap(true);
    result_->hide();
    footer_layout->addWidget(result_);
    auto *buttons = new QHBoxLayout;
    reset_ = new QPushButton(QStringLiteral("重置"));
    reset_->setObjectName("resetGroundStation");
    auto *close = new QPushButton(QStringLiteral("关闭"));
    close->setAutoDefault(false);
    apply_ = new QPushButton(QStringLiteral("应用"));
    apply_->setObjectName("applyGroundStation");
    apply_->setProperty("primary", true);
    apply_->setDefault(true);
    buttons->addWidget(reset_);
    buttons->addStretch();
    buttons->addWidget(close);
    footer_layout->addLayout(buttons);
    footer_layout->addWidget(apply_);
    layout->addWidget(footer);
    new WidthObserver(footer, [this, buttons, footer_layout, mode = WidthMode{},
                               previous = true](int width) mutable {
        const bool narrow = mode.narrow(width, 380);
        if (narrow == previous)
            return;
        previous = narrow;
        if (narrow) {
            buttons->removeWidget(apply_);
            footer_layout->addWidget(apply_);
        } else {
            footer_layout->removeWidget(apply_);
            buttons->addWidget(apply_);
        }
    });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(reset_, &QPushButton::clicked, this,
            [this] { set_station(original_, apply_->text() == QStringLiteral("插入地面站")); });
    connect(apply_, &QPushButton::clicked, this, [this] {
        try {
            if (apply)
                apply(station());
        } catch (const std::exception &error) {
            show_result(QString::fromUtf8(error.what()));
        }
    });
    for (auto *control : {longitude_, latitude_, height_}) {
        connect(
            control, &QuantityEditor::valueChanged, this, [this] { preview(); },
            Qt::QueuedConnection);
        connect(
            control->findChild<QLineEdit *>("quantityValue"), &QLineEdit::textChanged, this,
            [this] { preview(); }, Qt::QueuedConnection);
    }
}
void GroundStationEditor::set_station(const application::ProjectGroundStation &station,
                                      bool creating) {
    original_ = station;
    name_->setText(QString::fromStdString(station.name()));
    longitude_->set_value(station.location.longitude_rad, Unit::radian);
    latitude_->set_value(station.location.latitude_rad, Unit::radian);
    height_->set_value(station.location.height_m, Unit::metre);
    minimum_elevation_->set_value(station.minimum_elevation_rad, Unit::radian);
    apply_->setText(creating ? QStringLiteral("插入地面站") : QStringLiteral("应用"));
    setWindowTitle(creating
                       ? QStringLiteral("插入地面站")
                       : QString::fromStdString(station.name()) + QStringLiteral(" · 地面站属性"));
    show_result({});
    preview();
}
application::ProjectGroundStation GroundStationEditor::station() const {
    auto value = original_;
    value.display_name = name_->text().trimmed().toStdString();
    if (value.display_name.empty())
        throw std::invalid_argument("请输入地面站名称。");
    value.location = {longitude_->value(Unit::radian), latitude_->value(Unit::radian),
                      height_->value(Unit::metre)};
    (void)core::ground_station_position(value.location);
    value.minimum_elevation_rad = minimum_elevation_->value(Unit::radian);
    return value;
}
void GroundStationEditor::preview() {
    try {
        const auto xyz = core::ground_station_position({longitude_->value(Unit::radian),
                                                        latitude_->value(Unit::radian),
                                                        height_->value(Unit::metre)});
        preview_->setText(QStringLiteral("X  %1\nY  %2\nZ  %3")
                              .arg(xyz[0], 0, 'f', 3)
                              .arg(xyz[1], 0, 'f', 3)
                              .arg(xyz[2], 0, 'f', 3));
    } catch (const std::exception &) {
        preview_->setText(QStringLiteral("输入有效坐标后显示 ITRF 位置。"));
    }
}
void GroundStationEditor::set_busy(bool busy) {
    fields_->setEnabled(!busy);
    apply_->setEnabled(!busy);
    reset_->setEnabled(!busy);
}
void GroundStationEditor::show_result(const QString &text) {
    result_->setText(text);
    result_->setVisible(!text.isEmpty());
}
} // namespace orbitforge::desktop
