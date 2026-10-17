#include "track_style_editor.hpp"
#include "responsive_layout.hpp"
#include "workbench_theme.hpp"
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyleOptionSpinBox>
#include <QToolButton>

namespace orbitforge::desktop {
namespace {
class LineWidthSpinBox final : public QDoubleSpinBox {
    void paintEvent(QPaintEvent *event) override {
        QDoubleSpinBox::paintEvent(event);
        // Fusion + the workbench stylesheet can leave spin glyphs blank. Draw
        // vector chevrons in the actual hit rectangles, preserving native stepping.
        QStyleOptionSpinBox option;
        initStyleOption(&option);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        for (const bool up : {true, false}) {
            const auto control = up ? QStyle::SC_SpinBoxUp : QStyle::SC_SpinBoxDown;
            const auto box = style()->subControlRect(QStyle::CC_SpinBox, &option, control, this);
            const QPointF center = QRectF(box).center();
            const bool enabled =
                isEnabled() && (stepEnabled() & (up ? StepUpEnabled : StepDownEnabled));
            painter.setPen(QPen(theme::color(enabled ? theme::text_soft : theme::text_faint), 1.3,
                                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const double direction = up ? 1 : -1;
            painter.drawPolyline(QPolygonF{center + QPointF(-3, direction * 1.5),
                                           center + QPointF(0, -direction * 1.5),
                                           center + QPointF(3, direction * 1.5)});
        }
    }
};
class Preview final : public QWidget {
  public:
    DisplaySettings settings;
    explicit Preview(QWidget *parent) : QWidget(parent) {
        setObjectName("trackStylePreview");
        setMinimumHeight(88);
        setAccessibleName(QStringLiteral("轨迹样式预览"));
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(theme::color(theme::border), 1));
        p.setBrush(theme::color(theme::field));
        p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 8, 8);
        p.setPen(theme::color(theme::text_muted));
        p.drawText(QRect(16, 10, width() - 32, 20), Qt::AlignLeft, QStringLiteral("预览"));
        QPainterPath path;
        path.moveTo(20, 65);
        path.cubicTo(width() * .30, 65, width() * .55, 34, width() - 22, 44);
        const QColor color = QColor::fromRgb(settings.line_color);
        p.setPen(track_pen(color, settings.line_width, settings.line_style));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(QPen(theme::color(theme::field), 2));
        p.setBrush(color);
        p.drawEllipse(path.pointAtPercent(.66), 4.5, 4.5);
    }
};
} // namespace
TrackStyleEditor::TrackStyleEditor(QWidget *parent) : QWidget(parent) {
    setObjectName("trackStyleEditor");
    // Let the viewport allocate a narrow width before the controls reflow.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // Side labels like every other property form; narrow windows stack them above.
    auto *layout = new QFormLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setHorizontalSpacing(theme::space::md);
    layout->setVerticalSpacing(theme::space::row);
    layout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    preview_ = new Preview(this);
    layout->addRow(preview_);
    auto *palette = new QHBoxLayout;
    palette->setSpacing(6);
    const std::array colors{"#73cdee", "#a9a1ee", "#e9a2ba", "#edbb7a", "#87d4b1", "#e4eaf5"};
    const std::array names{QStringLiteral("浅蓝"), QStringLiteral("淡紫"), QStringLiteral("樱粉"),
                           QStringLiteral("暖金"), QStringLiteral("薄荷"), QStringLiteral("月白")};
    for (std::size_t i = 0; i < colors.size(); ++i) {
        auto *swatch = new QToolButton;
        swatch->setObjectName(QStringLiteral("trackColor%1").arg(i));
        swatch->setFixedSize(22, 22);
        swatch->setCheckable(true);
        swatch->setAccessibleName(names[i]);
        swatch->setToolTip(names[i]);
        swatch->setProperty("rgb", QColor(colors[i]));
        swatch->setStyleSheet(
            QStringLiteral(
                "QToolButton { background: %1; border: 2px solid %2; border-radius: 11px; }"
                "QToolButton:hover { border-color: %3; }"
                "QToolButton:checked, QToolButton:focus { border-color: %4; }")
                .arg(colors[i], theme::color(theme::surface).name(),
                     theme::color(theme::border_strong).name(),
                     theme::color(theme::accent_bright).name()));
        connect(swatch, &QToolButton::clicked, this,
                [this, swatch] { set_color(swatch->property("rgb").value<QColor>()); });
        palette->addWidget(swatch);
        swatches_.push_back(swatch);
    }
    // One row that fits narrow property windows: round swatches, then a compact chip
    // showing the current colour and its hex value, which opens the custom picker.
    palette->addSpacing(4);
    auto *color_row = new QHBoxLayout;
    color_row->setSpacing(0);
    color_row->addLayout(palette);
    custom_ = new QPushButton;
    custom_->setObjectName("trackCustomColor");
    custom_->setIconSize(QSize(12, 12));
    custom_->setToolTip(QStringLiteral("选择自定义颜色"));
    custom_->setAccessibleName(QStringLiteral("自定义轨迹颜色"));
    color_row->addWidget(custom_, 0, Qt::AlignVCenter);
    color_row->addStretch(1);
    connect(custom_, &QPushButton::clicked, this, [this] {
        QColorDialog dialog(color_, this);
        dialog.setObjectName("trackColorDialog");
        dialog.setWindowTitle(QStringLiteral("轨迹颜色"));
        dialog.setOption(QColorDialog::DontUseNativeDialog);
        if (dialog.exec() == QDialog::Accepted)
            set_color(dialog.selectedColor());
    });
    layout->addRow(QStringLiteral("颜色"), color_row);
    style_ = new QComboBox;
    style_->setObjectName("trackLineStyle");
    style_->setIconSize(QSize(64, 18));
    const std::array styles{QStringLiteral("实线"), QStringLiteral("虚线"), QStringLiteral("点线"),
                            QStringLiteral("点划线")};
    for (std::size_t i = 0; i < styles.size(); ++i) {
        QPixmap icon(192, 54);
        icon.setDevicePixelRatio(3);
        icon.fill(Qt::transparent);
        QPainter p(&icon);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(
            track_pen(theme::color(theme::text_soft), 1.5, static_cast<application::LineStyle>(i)));
        p.drawLine(QPointF(2, 9), QPointF(62, 9));
        p.end();
        style_->addItem(QIcon(icon), styles[i], static_cast<int>(i));
    }
    width_ = new LineWidthSpinBox;
    width_->setObjectName("trackLineWidth");
    width_->setProperty("stepButtons", true);
    width_->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    width_->setRange(.5, 8);
    width_->setSingleStep(.5);
    width_->setDecimals(2);
    width_->setSuffix(QStringLiteral(" px"));
    width_->setToolTip(QStringLiteral("屏幕逻辑像素；缩放视图时保持粗细。"));
    width_->setKeyboardTracking(false);
    style_->setMaximumWidth(276);
    width_->setMaximumWidth(160);
    layout->addRow(QStringLiteral("线型"), style_);
    layout->addRow(QStringLiteral("粗细"), width_);
    responsive_form(layout, 340);
    connect(style_, &QComboBox::currentIndexChanged, this, [this] { refresh_preview(); });
    connect(width_, &QDoubleSpinBox::valueChanged, this, [this] { refresh_preview(); });
    set_settings({});
}
void TrackStyleEditor::set_color(QColor color) {
    if (!color.isValid())
        return;
    color_ = color;
    custom_->setText(color.name().toUpper());
    QPixmap dot(QSize(12, 12) * custom_->devicePixelRatioF());
    dot.setDevicePixelRatio(custom_->devicePixelRatioF());
    dot.fill(Qt::transparent);
    QPainter painter(&dot);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(theme::color(theme::border_strong), 1));
    painter.setBrush(color);
    painter.drawEllipse(QRectF(0.5, 0.5, 11, 11));
    painter.end();
    custom_->setIcon(QIcon(dot));
    for (auto *swatch : swatches_)
        swatch->setChecked(swatch->property("rgb").value<QColor>() == color);
    refresh_preview();
}
void TrackStyleEditor::set_settings(const DisplaySettings &settings) {
    style_->setCurrentIndex(static_cast<int>(settings.line_style));
    width_->setValue(settings.line_width);
    set_color(QColor::fromRgb(settings.line_color));
}
void TrackStyleEditor::write_settings(DisplaySettings &settings) const {
    settings.line_color = color_.rgb() & 0xffffff;
    settings.line_width = width_->value();
    settings.line_style = static_cast<application::LineStyle>(style_->currentIndex());
}
void TrackStyleEditor::refresh_preview() {
    auto *preview = static_cast<Preview *>(preview_);
    write_settings(preview->settings);
    preview->update();
}
} // namespace orbitforge::desktop
