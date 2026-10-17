#include "workbench_style.hpp"
#include "workbench_theme.hpp"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QDockWidget>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmapCache>
#include <QProxyStyle>
#include <QRegularExpression>
#include <QStyleFactory>
#include <QStyleHints>
#include <QStyleOption>
#include <QStyleOptionViewItem>
#include <QToolButton>
#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace orbitforge::desktop {
namespace {
class WorkbenchStyle final : public QProxyStyle {
  public:
    WorkbenchStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {}
    void polish(QWidget *widget) override {
        QProxyStyle::polish(widget);
        if (auto *number = qobject_cast<QAbstractSpinBox *>(widget))
            number->setButtonSymbols(widget->property("stepButtons").toBool()
                                         ? QAbstractSpinBox::UpDownArrows
                                         : QAbstractSpinBox::NoButtons);
    }
    int pixelMetric(PixelMetric metric, const QStyleOption *option,
                    const QWidget *widget) const override {
        if (widget && widget->property("switch").toBool()) {
            if (metric == PM_IndicatorWidth)
                return 30;
            if (metric == PM_IndicatorHeight)
                return 18;
        }
        if (metric == PM_IndicatorWidth || metric == PM_IndicatorHeight ||
            metric == PM_ExclusiveIndicatorWidth || metric == PM_ExclusiveIndicatorHeight)
            return 16;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget) const override {
        if (element == PE_IndicatorMenuCheckMark) {
            // Menu check marks share the check box glyph, drawn in the accent colour.
            const QRectF box = QRectF(option->rect);
            const auto center = box.center();
            const double unit = std::min(box.width(), box.height()) / 14.0;
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(QPen(
                theme::color(option->state & State_Enabled ? theme::accent : theme::text_faint),
                1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPainterPath check;
            check.moveTo(center + QPointF(-4.0, 0.2) * unit);
            check.lineTo(center + QPointF(-1.2, 3.0) * unit);
            check.lineTo(center + QPointF(4.2, -3.0) * unit);
            painter->drawPath(check);
            painter->restore();
            return;
        }
        if (element == PE_IndicatorCheckBox && widget && widget->property("switch").toBool()) {
            const bool on = option->state & State_On;
            const bool enabled = option->state & State_Enabled;
            const QRectF track = QRectF(option->rect).adjusted(1, 2, -1, -2);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(
                on ? Qt::NoPen
                   : QPen(theme::color(option->state & State_HasFocus ? theme::accent
                                                                      : theme::border_strong),
                          1));
            painter->setBrush(theme::color(!enabled ? theme::panel
                                           : on     ? theme::accent_deep
                                                    : theme::field));
            painter->drawRoundedRect(track, track.height() / 2, track.height() / 2);
            const double knob = track.height() - 4;
            const QPointF centre(on ? track.right() - 2 - knob / 2 : track.left() + 2 + knob / 2,
                                 track.center().y());
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme::color(!enabled ? theme::text_faint
                                           : on     ? theme::on_accent
                                                    : theme::text_muted));
            painter->drawEllipse(centre, knob / 2, knob / 2);
            painter->restore();
            return;
        }
        if (element != PE_IndicatorCheckBox && element != PE_IndicatorRadioButton) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        const bool enabled = option->state & State_Enabled;
        const bool checked = option->state & (State_On | State_NoChange);
        const bool active = option->state & (State_MouseOver | State_HasFocus);
        const QRectF box = QRectF(option->rect).adjusted(1, 1, -1, -1);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(theme::color(!enabled            ? theme::border
                                          : checked || active ? theme::accent
                                                              : theme::border_strong),
                             1.2));
        painter->setBrush(theme::color(!enabled  ? theme::panel
                                       : checked ? theme::accent_deep
                                                 : theme::field));
        const auto mark = theme::color(enabled ? theme::on_accent : theme::text_faint);
        if (element == PE_IndicatorRadioButton) {
            painter->drawEllipse(box);
            if (checked) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(mark);
                painter->drawEllipse(box.center(), 2.9, 2.9);
            }
        } else {
            painter->drawRoundedRect(box, 4, 4);
            if (checked) {
                painter->setPen(QPen(mark, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                const auto center = box.center();
                if (option->state & State_NoChange)
                    painter->drawLine(center + QPointF(-3, 0), center + QPointF(3, 0));
                else {
                    QPainterPath check;
                    check.moveTo(center + QPointF(-3.5, 0));
                    check.lineTo(center + QPointF(-1, 2.5));
                    check.lineTo(center + QPointF(4, -3));
                    painter->drawPath(check);
                }
            }
        }
        painter->restore();
    }
};
} // namespace
QString theme::resolve(QString stylesheet) {
    static const QRegularExpression reference(QStringLiteral("@([A-Za-z]+)(?:/([0-9]+))?"));
    QString result;
    result.reserve(stylesheet.size());
    qsizetype copied = 0;
    for (auto it = reference.globalMatch(stylesheet); it.hasNext();) {
        const auto match = it.next();
        const auto name = match.captured(1);
        const auto *token = std::find_if(std::begin(all), std::end(all), [&](const Token &entry) {
            return name == QLatin1StringView(entry.name.data(), qsizetype(entry.name.size()));
        });
        if (token == std::end(all))
            throw std::invalid_argument("Unknown workbench theme token @" + name.toStdString());
        result += QStringView(stylesheet).sliced(copied, match.capturedStart() - copied);
        if (match.hasCaptured(2)) {
            const int opacity = match.captured(2).toInt();
            if (opacity > 100)
                throw std::invalid_argument("Theme opacity must be at most 100: " +
                                            match.captured(0).toStdString());
            const auto colour = color(*token);
            result += QStringLiteral("rgba(%1, %2, %3, %4)")
                          .arg(colour.red())
                          .arg(colour.green())
                          .arg(colour.blue())
                          .arg((opacity * 255 + 50) / 100);
        } else {
            result += QLatin1StringView(token->value.data(), qsizetype(token->value.size()));
        }
        copied = match.capturedEnd();
    }
    result += QStringView(stylesheet).sliced(copied);
    return result;
}
QIcon workbench_icon(WorkbenchIcon icon, const QColor &color) {
    // Original vector geometry, painted at multiple sizes for native high-DPI controls.
    QIcon result;
    for (int size : {16, 20, 24, 32, 48, 64}) {
        const auto key = QStringLiteral("orbitforge.icon.%1.%2.%3")
                             .arg(static_cast<int>(icon))
                             .arg(color.rgba())
                             .arg(size);
        QPixmap cached;
        if (QPixmapCache::find(key, &cached)) {
            result.addPixmap(cached);
            continue;
        }
        QPixmap pixels(size, size);
        pixels.fill(Qt::transparent);
        QPainter p(&pixels);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(size / 24.0, size / 24.0);
        p.setPen(QPen(color, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const auto line = [&](double x, double y, double a, double b) {
            p.drawLine(QPointF(x, y), QPointF(a, b));
        };
        switch (icon) {
        case WorkbenchIcon::folder: {
            QPainterPath path;
            path.moveTo(3, 19);
            path.lineTo(3, 5);
            path.lineTo(9, 5);
            path.lineTo(12, 8);
            path.lineTo(21, 8);
            path.lineTo(21, 19);
            path.closeSubpath();
            p.drawPath(path);
            line(3, 11, 21, 11);
            break;
        }
        case WorkbenchIcon::save:
            p.drawRoundedRect(QRectF(4, 3, 16, 18), 2, 2);
            p.drawRect(QRectF(8, 3, 8, 6));
            p.drawRect(QRectF(8, 14, 8, 7));
            break;
        case WorkbenchIcon::download:
            line(12, 3, 12, 15);
            line(7, 10, 12, 15);
            line(12, 15, 17, 10);
            line(4, 15, 4, 21);
            line(4, 21, 20, 21);
            line(20, 21, 20, 15);
            break;
        case WorkbenchIcon::database:
            p.drawEllipse(QRectF(4, 3, 16, 6));
            line(4, 6, 4, 18);
            line(20, 6, 20, 18);
            p.drawArc(QRectF(4, 9, 16, 6), 180 * 16, 180 * 16);
            p.drawArc(QRectF(4, 15, 16, 6), 180 * 16, 180 * 16);
            break;
        case WorkbenchIcon::new_scenario:
            line(5, 3, 14, 3);
            line(14, 3, 19, 8);
            line(19, 8, 19, 11);
            line(14, 3, 14, 8);
            line(14, 8, 19, 8);
            line(5, 3, 5, 21);
            line(5, 21, 12, 21);
            line(13, 17, 21, 17);
            line(17, 13, 17, 21);
            break;
        case WorkbenchIcon::chevron_up:
        case WorkbenchIcon::chevron_down:
            if (icon == WorkbenchIcon::chevron_up) {
                p.translate(0, 24);
                p.scale(1, -1);
            }
            line(6, 9, 12, 15);
            line(12, 15, 18, 9);
            break;
        case WorkbenchIcon::zoom_in:
        case WorkbenchIcon::zoom_out:
            p.drawEllipse(QRectF(3, 3, 13, 13));
            line(14, 14, 21, 21);
            line(6, 9.5, 13, 9.5);
            if (icon == WorkbenchIcon::zoom_in)
                line(9.5, 6, 9.5, 13);
            break;
        case WorkbenchIcon::time_fit:
            line(3, 5, 3, 19);
            line(21, 5, 21, 19);
            line(7, 12, 17, 12);
            line(10, 9, 7, 12);
            line(10, 15, 7, 12);
            line(14, 9, 17, 12);
            line(14, 15, 17, 12);
            break;
        case WorkbenchIcon::calendar:
            p.drawRoundedRect(QRectF(4, 5, 16, 16), 2, 2);
            line(4, 10, 20, 10);
            line(8, 3, 8, 7);
            line(16, 3, 16, 7);
            for (int y : {13, 17})
                for (int x : {8, 12, 16})
                    p.drawPoint(QPointF(x, y));
            break;
        case WorkbenchIcon::clock:
            p.drawEllipse(QRectF(3, 3, 18, 18));
            line(12, 6, 12, 12);
            line(12, 12, 16, 14);
            break;
        case WorkbenchIcon::orbit:
            p.drawEllipse(QRectF(5, 5, 14, 14));
            p.save();
            p.translate(12, 12);
            p.rotate(-32);
            p.drawEllipse(QRectF(-11, -4, 22, 8));
            p.restore();
            p.setBrush(color);
            p.drawEllipse(QPointF(19.8, 6.8), 2, 2);
            break;
        case WorkbenchIcon::earth:
            p.drawEllipse(QRectF(3, 3, 18, 18));
            p.drawEllipse(QRectF(8, 3, 8, 18));
            line(3, 12, 21, 12);
            p.drawArc(QRectF(3, 6, 18, 12), 0, 180 * 16);
            break;
        case WorkbenchIcon::map:
            line(3, 5, 9, 3);
            line(9, 3, 15, 6);
            line(15, 6, 21, 4);
            line(21, 4, 21, 19);
            line(21, 19, 15, 21);
            line(15, 21, 9, 18);
            line(9, 18, 3, 20);
            line(3, 20, 3, 5);
            line(9, 3, 9, 18);
            line(15, 6, 15, 21);
            break;
        case WorkbenchIcon::ground_station:
            p.drawArc(QRectF(4, 3, 13, 13), 180 * 16, 180 * 16);
            line(4, 9.5, 17, 9.5);
            line(10, 10, 16, 4);
            p.drawEllipse(QPointF(17, 3), 1, 1);
            line(10, 16, 8, 21);
            line(11, 16, 14, 21);
            line(5, 21, 17, 21);
            break;
        case WorkbenchIcon::satellite:
            p.save();
            p.translate(12, 12);
            p.rotate(-35);
            p.drawRect(QRectF(-3, -4, 6, 8));
            p.drawRect(QRectF(-10, -3, 5, 6));
            p.drawRect(QRectF(5, -3, 5, 6));
            line(-5, 0, -3, 0);
            line(3, 0, 5, 0);
            line(0, -4, 0, -7);
            line(-2, -8, 2, -8);
            p.restore();
            break;
        case WorkbenchIcon::properties:
            for (int y : {6, 12, 18})
                line(4, y, 20, y);
            p.setBrush(color);
            p.drawEllipse(QPointF(9, 6), 2, 2);
            p.drawEllipse(QPointF(16, 12), 2, 2);
            p.drawEllipse(QPointF(8, 18), 2, 2);
            break;
        case WorkbenchIcon::fit:
            line(4, 9, 4, 4);
            line(4, 4, 9, 4);
            line(15, 4, 20, 4);
            line(20, 4, 20, 9);
            line(4, 15, 4, 20);
            line(4, 20, 9, 20);
            line(15, 20, 20, 20);
            line(20, 20, 20, 15);
            p.drawEllipse(QRectF(8, 8, 8, 8));
            break;
        case WorkbenchIcon::focus:
            p.drawEllipse(QRectF(6, 6, 12, 12));
            p.drawEllipse(QRectF(10, 10, 4, 4));
            line(12, 2, 12, 6);
            line(12, 18, 12, 22);
            line(2, 12, 6, 12);
            line(18, 12, 22, 12);
            break;
        case WorkbenchIcon::views:
            p.drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
            line(14, 4, 14, 20);
            break;
        case WorkbenchIcon::play: {
            QPainterPath path;
            path.moveTo(8, 5);
            path.lineTo(19, 12);
            path.lineTo(8, 19);
            path.closeSubpath();
            p.fillPath(path, color);
            break;
        }
        case WorkbenchIcon::pause:
            p.fillRect(QRectF(7, 5, 3, 14), color);
            p.fillRect(QRectF(14, 5, 3, 14), color);
            break;
        case WorkbenchIcon::first:
        case WorkbenchIcon::last: {
            if (icon == WorkbenchIcon::last) {
                p.translate(24, 0);
                p.scale(-1, 1);
            }
            line(5, 6, 5, 18);
            QPainterPath path;
            path.moveTo(18, 5);
            path.lineTo(8, 12);
            path.lineTo(18, 19);
            path.closeSubpath();
            p.fillPath(path, color);
            break;
        }
        case WorkbenchIcon::search:
            p.drawEllipse(QRectF(4, 3, 12, 12));
            line(14, 14, 21, 21);
            break;
        case WorkbenchIcon::slower:
        case WorkbenchIcon::faster:
            if (icon == WorkbenchIcon::slower) {
                p.translate(24, 0);
                p.scale(-1, 1);
            }
            line(4, 6, 10, 12);
            line(10, 12, 4, 18);
            line(13, 6, 19, 12);
            line(19, 12, 13, 18);
            break;
        case WorkbenchIcon::info:
            p.drawEllipse(QRectF(3.5, 3.5, 17, 17));
            line(12, 11, 12, 16.5);
            p.setBrush(color);
            p.drawEllipse(QPointF(12, 7.8), 0.9, 0.9);
            break;
        case WorkbenchIcon::chevron_right:
            line(9, 6, 15, 12);
            line(15, 12, 9, 18);
            break;
        case WorkbenchIcon::remove:
            line(4, 7, 20, 7);
            line(10, 4, 14, 4);
            p.drawRoundedRect(QRectF(6.5, 7, 11, 13), 1.5, 1.5);
            line(10, 11, 10, 16);
            line(14, 11, 14, 16);
            break;
        case WorkbenchIcon::add:
            line(12, 5, 12, 19);
            line(5, 12, 19, 12);
            break;
        case WorkbenchIcon::close:
            line(7, 7, 17, 17);
            line(7, 17, 17, 7);
            break;
        case WorkbenchIcon::detach:
            p.drawRoundedRect(QRectF(4, 8, 12, 12), 1, 1);
            line(10, 4, 20, 4);
            line(20, 4, 20, 14);
            line(13, 11, 20, 4);
            break;
        }
        p.end();
        // Qt owns a bounded cache and releases its pixmaps with the application.
        QPixmapCache::insert(key, pixels);
        result.addPixmap(pixels);
    }
    return result;
}
QPixmap brand_mark(int size, qreal device_pixel_ratio) {
    QPixmap pixels(QSize(size, size) * device_pixel_ratio);
    pixels.setDevicePixelRatio(device_pixel_ratio);
    pixels.fill(Qt::transparent);
    QPainter p(&pixels);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF tile(0.5, 0.5, size - 1.0, size - 1.0);
    QLinearGradient fill(tile.topLeft(), tile.bottomRight());
    fill.setColorAt(0, theme::color(theme::accent_bright));
    fill.setColorAt(0.6, theme::color(theme::accent));
    fill.setColorAt(1, theme::color(theme::sakura));
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(tile, size * 0.28, size * 0.28);
    const int glyph = qRound(size * 0.68);
    const auto icon = workbench_icon(WorkbenchIcon::orbit, theme::color(theme::canvas));
    const int inset = (size - glyph) / 2;
    p.drawPixmap(QRect(inset, inset, glyph, glyph),
                 icon.pixmap(QSize(glyph, glyph), device_pixel_ratio));
    return pixels;
}
void paint_list_row(QPainter *painter, const QStyleOptionViewItem &option, const QRectF &row,
                    const QColor &stripe) {
    const bool selected = option.state & QStyle::State_Selected;
    const bool hover = option.state & QStyle::State_MouseOver;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(option.state & QStyle::State_HasFocus
                        ? QPen(theme::color(theme::accent, 150), 1)
                        : QPen(Qt::NoPen));
    painter->setBrush(selected ? theme::color(theme::accent_wash)
                      : hover  ? theme::color(theme::accent_hover)
                               : QColor(Qt::transparent));
    painter->drawRoundedRect(row.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    if (selected) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(stripe);
        const double inset = std::min(8.0, row.height() / 4);
        painter->drawRoundedRect(
            QRectF(row.left() + 1, row.top() + inset, 3, row.height() - 2 * inset), 1.5, 1.5);
    }
    painter->restore();
}
void style_dock(QDockWidget *dock, WorkbenchIcon icon) {
    auto *bar = new QWidget(dock);
    bar->setObjectName("dockHeading");
    bar->setFixedHeight(34);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(6, 4, 4, 2);
    layout->setSpacing(6);
    // The panel title reads as the selected tab of its panel, matching the workspace tabs.
    auto *title = new QLabel(dock->windowTitle());
    title->setObjectName("dockTitle");
    (void)icon;
    layout->addWidget(title);
    layout->addStretch();
    const auto tool = [bar](WorkbenchIcon glyph, const QString &tip) {
        auto *button = new QToolButton(bar);
        button->setIcon(workbench_icon(glyph, theme::color(theme::text_muted)));
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setFixedSize(24, 24);
        button->setIconSize(QSize(14, 14));
        return button;
    };
    auto *detach = tool(WorkbenchIcon::detach, QStringLiteral("浮动 / 停靠面板"));
    auto *close = tool(WorkbenchIcon::close, QStringLiteral("隐藏面板 · 可从视图菜单恢复"));
    layout->addWidget(detach);
    layout->addWidget(close);
    QObject::connect(detach, &QToolButton::clicked, dock,
                     [dock] { dock->setFloating(!dock->isFloating()); });
    QObject::connect(close, &QToolButton::clicked, dock, &QWidget::hide);
    dock->setTitleBarWidget(bar);
}
void apply_workbench_style(QApplication &app) {
    // A dark scheme also gives Windows a dark native title bar and dark system dialogs.
    app.styleHints()->setColorScheme(Qt::ColorScheme::Dark);
    app.setStyle(new WorkbenchStyle);
    QPalette palette;
    palette.setColor(QPalette::Window, theme::color(theme::surface));
    palette.setColor(QPalette::WindowText, theme::color(theme::text));
    palette.setColor(QPalette::Base, theme::color(theme::field));
    palette.setColor(QPalette::AlternateBase, theme::color(theme::panel));
    palette.setColor(QPalette::Text, theme::color(theme::text));
    palette.setColor(QPalette::Button, theme::color(theme::raised));
    palette.setColor(QPalette::ButtonText, theme::color(theme::text_soft));
    palette.setColor(QPalette::Highlight, theme::color(theme::accent_wash));
    palette.setColor(QPalette::HighlightedText, theme::color(theme::accent_bright));
    palette.setColor(QPalette::Link, theme::color(theme::accent));
    // Placeholders carry meaning (units, search scope), so they keep body-text contrast.
    palette.setColor(QPalette::PlaceholderText, theme::color(theme::text_muted));
    palette.setColor(QPalette::ToolTipBase, theme::color(theme::raised));
    palette.setColor(QPalette::ToolTipText, theme::color(theme::text_soft));
    palette.setColor(QPalette::Disabled, QPalette::Text, theme::color(theme::text_faint));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, theme::color(theme::text_faint));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, theme::color(theme::text_faint));
    app.setPalette(palette);
    QFont font;
    font.setFamilies({"Microsoft YaHei UI", "Segoe UI"});
    font.setStyleHint(QFont::SansSerif);
    font.setStyleStrategy(QFont::PreferAntialias);
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setPixelSize(12);
    app.setFont(font);
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Han, "Microsoft YaHei UI");
    app.setWindowIcon(workbench_icon(WorkbenchIcon::orbit, theme::color(theme::accent)));
    // Keep each raw literal below VS 2022's limit; C++ concatenates them unchanged.
    // Desktop density: 12 px text, 26-28 px controls, 6 px gaps between panels.
    // Radii: 4 fields · 5 buttons · 6 menus · 8 panels. Flat fills: depth comes from the
    // surface tones and 1 px rules, not from gradients or bevelled edges.
    // Large headings use the regular weight: YaHei has no semibold, so 600 renders heavy.
    app.setStyleSheet(theme::resolve(QStringLiteral(
        R"QSS(
QWidget { font-family: "Microsoft YaHei UI"; font-size: 12px; }
QLabel { qproperty-indent: 0; }
QDialog { background: @surface; }
QWidget#propertyFields { background: @surface; }
QMainWindow { background: @canvas; }
QMainWindow::separator { background: @canvas; width: 6px; height: 6px; }
QMainWindow::separator:hover { background: @accentWash; }
QWidget#titleBar { background: @chrome; border: 1px solid @line; border-radius: 8px; }
QLabel#brand { color: @text; font-family: "Segoe UI Variable Display", "Segoe UI Semibold", "Segoe UI"; font-size: 14px; font-weight: 600; padding: 0 10px 0 2px; }
QLabel#brandMark { background: transparent; }
QMenuBar#titleMenus { background: transparent; border: 0; padding: 0; color: @textSoft; }
QMenuBar#titleMenus::item { padding: 5px 9px; border-radius: 4px; background: transparent; }
QMenuBar#titleMenus::item:selected, QMenuBar#titleMenus::item:pressed { background: @accentHover; color: @text; }
QFrame#workspaceChip { background: @field; border: 1px solid @line; border-radius: 12px; }
QLabel#workspaceName { color: @textSoft; }
QLabel#workspaceDot { background: @textFaint; border-radius: 3px; }
QLabel#workspaceDot[active="true"] { background: @sakura; }
QLabel#workspaceDot[warning="true"] { background: @warning; }
QToolBar#commandBar { background: transparent; border: 0; padding: 0; spacing: 1px; }
QToolBar#commandBar::separator { background: @line; width: 1px; margin: 9px 6px; }
QToolBar#commandBar QToolButton { padding: 5px; min-width: 18px; min-height: 18px; border-radius: 5px; }
QWidget#dockHeading { background: @chrome; border: 1px solid @line; border-bottom: 0; border-top-left-radius: 8px; border-top-right-radius: 8px; }
QWidget#dockHeading QToolButton { padding: 0; border-radius: 4px; }
QLabel#dockTitle { color: @accentBright; background: @accentWash; border-radius: 4px; padding: 3px 9px; font-size: 12px; }
QWidget#objectExplorer { background: @chrome; border: 1px solid @line; border-top: 0; border-bottom-left-radius: 8px; border-bottom-right-radius: 8px; }
QToolButton#explorerInsert { background: @raised; border: 1px solid @border; border-radius: 5px; padding: 0; }
QToolButton#explorerInsert:hover { background: @overlay; border-color: @borderStrong; }
QToolButton#explorerInsert:pressed { background: @accentWash; }
QToolButton#explorerInsert:disabled { background: @panel; border-color: @line; }
QWidget#workspacePane { background: @chrome; border: 1px solid @line; border-radius: 8px; }
QWidget#workspaceHeading { background: @chrome; border-top-left-radius: 7px; border-top-right-radius: 7px; }
QWidget#workspaceHeading QToolButton, QTabBar QToolButton { padding: 0; border-radius: 4px; }
QToolButton#closePage { border: 0; border-radius: 4px; }
QToolButton#closePage:hover { background: @overlay; }
QToolButton#closePage:focus { background: @accentHover; }
QTabBar#workspaceTabs::tab { color: @textMuted; background: transparent; border: 0; border-radius: 4px; padding: 4px 10px; margin: 5px 2px 5px 0; }
QTabBar#workspaceTabs::tab:selected { color: @accentBright; background: @accentWash; }
QTabBar#workspaceTabs::tab:hover:!selected { color: @text; background: @accentHover; }
QTabBar#propertyPageTabs::tab { color: @textMuted; background: transparent; border: 0; border-radius: 4px; padding: 4px 12px; margin-right: 2px; }
QTabBar#propertyPageTabs::tab:selected { color: @accentBright; background: @accentWash; }
QTabBar#propertyPageTabs::tab:hover:!selected { color: @text; background: @accentHover; }
QTabBar#propertyPageTabs::tab:disabled { color: @textFaint; }
QWidget#viewHeading { background: @chrome; border-top: 1px solid @line; }
QLabel#viewTitle { color: @textSoft; padding-right: 6px; }
QWidget#viewHeading QLabel#hint { color: @textMuted; }
QWidget#viewHeading QLabel#badge { background: @accentWash; border-color: @border; color: @accentBright; }
QWidget#viewHeading QToolButton { padding: 2px; border-radius: 4px; }
QWidget#viewHeading QComboBox { background: transparent; border-color: transparent; min-height: 16px; padding: 2px 24px 2px 6px; color: @textSoft; }
QWidget#viewHeading QComboBox:hover { background: @accentHover; border-color: @border; }
QFrame#viewTools { background: transparent; border: 0; }
QFrame#viewTools QToolButton { border-radius: 4px; padding: 2px; }
QToolBar#timelineBar { background: @canvas; border: 0; padding: 0; spacing: 0; }
QWidget#timelineCard { background: @chrome; border: 1px solid @line; border-radius: 8px; }
QWidget#timelineControls { background: @chrome; border-top-left-radius: 7px; border-top-right-radius: 7px; border-bottom: 1px solid @line; }
QLabel#timelineTitle { color: @textMuted; padding-right: 8px; }
QFrame#transportGroup { background: transparent; border: 0; }
QFrame#transportGroup QToolButton { border-radius: 13px; padding: 3px; }
QToolBar#timelineBar QToolButton { padding: 3px; }
QToolBar#timelineBar QComboBox, QToolBar#timelineBar QDoubleSpinBox { min-height: 16px; padding-top: 2px; padding-bottom: 2px; }
QToolButton#transportPlay { background: @accentDeep; border: 0; border-radius: 13px; }
QToolButton#transportPlay:hover { background: #5d75d8; }
QToolButton#transportPlay:pressed { background: #4659b0; }
QToolButton#timeReadout { color: @accentBright; font-family: "Cascadia Mono", "Consolas"; font-size: 12px; padding: 3px 8px; border-radius: 4px; }
QToolButton#timeReadout:hover { background: @accentHover; }
QFrame#playbackRateField { background: @field; border: 1px solid @border; border-radius: 4px; }
QFrame#playbackRateField:hover { border-color: @borderStrong; }
QFrame#playbackRateField QDoubleSpinBox { background: transparent; border: 0; padding: 2px 6px; }
QToolButton#playbackRatePresets { border: 0; border-left: 1px solid @line; border-radius: 0; padding: 3px; }
QToolButton#playbackRatePresets:hover { background: @accentHover; }
QLabel#timeBound { color: @textMuted; font-family: "Cascadia Mono", "Consolas"; font-size: 10px; }
QFrame#timelineDivider { background: @line; max-width: 1px; border: 0; }
QStatusBar { color: @textMuted; background: @canvas; border: 0; font-size: 11px; }
QStatusBar::item { border: 0; }
QStatusBar QLabel#hint { color: @textMuted; font-size: 11px; padding: 0 6px; }
QSplitter::handle { background: @canvas; }
QSplitter::handle:hover { background: @accentWash; }
QToolBar { background: @chrome; border: 0; spacing: 4px; padding: 3px 8px; }
QToolBar::separator { background: @line; width: 1px; margin: 8px 6px; }
QLabel#badge { color: @accentBright; background: @accentWash; border: 1px solid @border; border-radius: 8px; padding: 1px 8px; font-size: 10px; }
QLabel#muted { color: @textMuted; }
QLabel#sectionTitle { color: @text; font-size: 12px; font-weight: bold; padding-top: 4px; padding-bottom: 2px; }
QLabel#panelTitle { color: @text; font-size: 17px; }
QLabel#panelDescription { color: @textMuted; font-size: 12px; }
QLabel#hint { color: @textMuted; font-size: 12px; }
QLabel#stationPositionPreview { color: @textSoft; font-family: "Cascadia Mono", "Consolas"; font-size: 12px; }
QLabel[fieldCaption="true"] { color: @textMuted; }
QLabel#eyebrow { color: @accent; font-size: 11px; font-weight: 700; letter-spacing: 3px; }
QLabel#dialogDescription { color: @textMuted; padding-bottom: 4px; }
QFrame#sectionDivider { background: @line; max-height: 1px; border: 0; }
)QSS"
        R"QSS(QTreeWidget { background: @chrome; border: 0; outline: 0; padding: 2px 0; }
QTreeWidget::item { height: 24px; border-radius: 4px; padding: 1px 6px; color: @textSoft; }
QTreeWidget::item:selected { background: @accentWash; color: @accentBright; }
QTreeWidget::item:hover:!selected { background: @accentHover; color: @text; }
QTreeWidget::branch { background: @chrome; }
QTreeWidget::branch:selected, QTreeWidget::branch:hover { background: @chrome; }
QTreeWidget#sceneObjects::branch:has-children:closed { image: url(:/orbitforge/icons/chevron-right.png); }
QTreeWidget#sceneObjects::branch:has-children:open { image: url(:/orbitforge/icons/chevron-down.png); }
QTreeWidget#propertyNavigation { background: @panel; border: 0; border-right: 1px solid @line; padding: 10px 6px; outline: 0; }
QTreeWidget#propertyNavigation::item { height: 26px; padding: 2px 8px; border-radius: 4px; color: @textMuted; }
QTreeWidget#propertyNavigation::item:hover:!selected { background: @accentHover; color: @textSoft; }
QTreeWidget#propertyNavigation::item:selected { background: @accentWash; color: @accentBright; }
QTreeWidget#propertyNavigation::item:disabled { color: @textFaint; }
QTreeWidget#propertyNavigation::branch { background: transparent; }
QTreeWidget#propertyNavigation::branch:selected { background: @panel; }
QTreeWidget#insertObjectTypes { background: @field; border: 1px solid @line; border-radius: 6px; padding: 4px; }
QTreeWidget#insertObjectTypes::item { height: 24px; padding: 1px 6px; }
QTreeWidget#insertObjectTypes::branch { background: @field; }
QDialog#insertObjectsDialog QRadioButton { color: @text; spacing: 8px; }
QDialog#insertObjectsDialog QRadioButton:hover { color: @accentBright; }
QHeaderView::section { background: @panel; color: @textMuted; border: 0; border-right: 1px solid @line; border-bottom: 1px solid @line; padding: 4px 8px; font-size: 11px; }
QLineEdit#objectSearch { background: @field; padding: 4px 8px; border-color: @line; border-radius: 5px; }
QLineEdit#objectSearch:focus { border-color: @accent; }
QLineEdit, QDateTimeEdit, QDoubleSpinBox, QSpinBox, QComboBox {
    background: @field; color: @text; border: 1px solid @border;
    border-radius: 4px; padding: 3px 8px; min-height: 20px; max-height: 20px;
    selection-background-color: @accentDeep; selection-color: #ffffff;
}
QLineEdit:hover, QDoubleSpinBox:hover, QSpinBox:hover, QComboBox:hover, QDateTimeEdit:hover { border-color: @borderStrong; }
QLineEdit:focus, QDateTimeEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QComboBox:focus { border-color: @accent; background: @field; }
QLineEdit:read-only { color: @textMuted; background: @fieldMuted; border-color: @line; }
QLineEdit:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled, QComboBox:disabled { color: @textFaint; background: @panel; border-color: @line; }
QLineEdit#vectorInput, QSpinBox, QDoubleSpinBox { font-family: "Cascadia Mono", "Consolas"; font-size: 12px; }
QSpinBox[stepButtons="true"], QDoubleSpinBox[stepButtons="true"] { padding-right: 27px; min-height: 20px; }
QAbstractSpinBox[stepButtons="true"]::up-button, QAbstractSpinBox[stepButtons="true"]::down-button { subcontrol-origin: border; width: 22px; border-left: 1px solid @line; background: transparent; }
QAbstractSpinBox[stepButtons="true"]::up-button { subcontrol-position: top right; border-top-right-radius: 3px; border-bottom: 1px solid @line; }
QAbstractSpinBox[stepButtons="true"]::down-button { subcontrol-position: bottom right; border-bottom-right-radius: 3px; }
QAbstractSpinBox[stepButtons="true"]::up-button:hover, QAbstractSpinBox[stepButtons="true"]::down-button:hover { background: @accentWash; }
QAbstractSpinBox[stepButtons="true"]::up-button:pressed, QAbstractSpinBox[stepButtons="true"]::down-button:pressed { background: @accentDeep; }
QAbstractSpinBox[stepButtons="true"]::up-button:off, QAbstractSpinBox[stepButtons="true"]::down-button:off { background: transparent; }
QFrame[quantityField="true"] { background: @field; border: 1px solid @border; border-radius: 4px; }
QFrame[quantityField="true"]:hover { border-color: @borderStrong; }
QFrame[quantityField="true"][focused="true"] { border-color: @accent; }
QFrame[quantityField="true"][invalid="true"] { border-color: @danger; }
QFrame[quantityField="true"]:disabled { background: @panel; border-color: @line; }
QFrame[quantityField="true"] QLineEdit { background: transparent; border: 0; padding: 3px 8px; min-height: 20px; max-height: 20px; font-family: "Cascadia Mono", "Consolas"; font-size: 12px; }
QToolButton#quantityUnit { color: @textSoft; border: 0; border-left: 1px solid @line; border-radius: 0; padding: 3px 17px 3px 7px; min-height: 20px; max-height: 20px; }
QToolButton#quantityUnit:hover, QToolButton#quantityUnit:focus { color: @accentBright; background: @accentHover; }
QToolButton#quantityUnit:disabled { color: @textFaint; }
QToolButton#quantityUnit::menu-indicator { image: url(:/orbitforge/icons/chevron-down.png); subcontrol-origin: padding; subcontrol-position: right center; right: 5px; width: 9px; height: 6px; }
QComboBox { padding-right: 24px; }
QComboBox::drop-down { border: 0; width: 22px; }
QComboBox::down-arrow { image: url(:/orbitforge/icons/chevron-down.png); width: 9px; height: 6px; }
QComboBox QAbstractItemView { background: @raised; color: @textSoft; border: 1px solid @border; selection-background-color: @accentWash; selection-color: @accentBright; outline: 0; padding: 3px; }
QPushButton { background: @raised; color: @textSoft; border: 1px solid @border; border-radius: 5px; padding: 4px 14px; min-height: 18px; }
QPushButton:hover { background: @overlay; border-color: @borderStrong; color: @text; }
QPushButton:pressed { background: @accentWash; }
QPushButton:disabled { background: @panel; color: @textFaint; border-color: @line; }
QToolButton { color: @textSoft; background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 3px 6px; }
QToolButton:hover { background: @accentHover; border-color: transparent; color: @text; }
QToolButton:pressed { background: @accentWash; }
QToolButton:checked { background: @accentWash; color: @accentBright; border-color: transparent; }
QToolButton:focus, QPushButton:focus { border-color: @accent; }
QToolButton:disabled { color: @textFaint; }
QToolButton::menu-indicator { image: none; }
QPushButton#applyOrbit, QPushButton#insertObject, QPushButton[primary="true"], QPushButton#reportSelectionConfirm { background: @accentDeep; border: 1px solid @accentDeep; color: #ffffff; font-weight: 600; }
QPushButton#applyOrbit:hover, QPushButton#insertObject:hover, QPushButton[primary="true"]:hover, QPushButton#reportSelectionConfirm:hover { background: #5d75d8; border-color: #5d75d8; }
QPushButton#applyOrbit:pressed, QPushButton#insertObject:pressed, QPushButton[primary="true"]:pressed, QPushButton#reportSelectionConfirm:pressed { background: #4355ad; }
QPushButton#applyOrbit:focus, QPushButton#insertObject:focus, QPushButton[primary="true"]:focus, QPushButton#reportSelectionConfirm:focus { border-color: @accentBright; }
QPushButton#applyOrbit:disabled, QPushButton#insertObject:disabled, QPushButton[primary="true"]:disabled, QPushButton#reportSelectionConfirm:disabled { background: @raised; border-color: @line; color: @textFaint; }
QPushButton#reportSelectionConfirm { padding: 5px 16px; }
QPushButton[iconOnly="true"] { padding: 0; }
QPushButton#trackCustomColor { padding: 3px 10px 3px 8px; font-family: "Cascadia Mono", "Consolas"; font-size: 11px; }
QCheckBox, QRadioButton { spacing: 7px; min-height: 22px; }
QCheckBox:hover, QRadioButton:hover { color: @text; }
)QSS"
        R"QSS(QTabWidget::pane { border: 0; }
QTabBar::tab { color: @textMuted; background: transparent; min-height: 18px; padding: 5px 12px; border: 0; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: @text; border-bottom-color: @sakura; }
QTabBar::tab:hover:!selected { background: @accentHover; color: @textSoft; }
QMenuBar { background: @canvas; color: @textSoft; padding: 2px 8px; border-bottom: 1px solid @line; }
QMenuBar::item { padding: 4px 9px; border-radius: 4px; }
QMenu { background: @raised; color: @textSoft; border: 1px solid @border; border-radius: 6px; padding: 4px; }
QMenu::item { padding: 5px 24px 5px 22px; border-radius: 4px; }
QMenu::icon { padding-left: 6px; }
QMenu::indicator { width: 14px; height: 14px; left: 5px; }
QMenuBar::item:selected, QMenu::item:selected, QMenuBar::item:pressed { background: @accentWash; color: @accentBright; }
QMenu::item:disabled { color: @textFaint; }
QMenu::separator { height: 1px; background: @line; margin: 4px 6px; }
QToolTip { background: @raised; color: @textSoft; border: 1px solid @border; border-radius: 4px; padding: 4px 7px; }
QScrollArea { background: @surface; border: 0; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px 0; }
QScrollBar::handle:vertical { background: @border; min-height: 32px; border-radius: 4px; }
QScrollBar:horizontal { background: transparent; height: 8px; margin: 0 2px; }
QScrollBar::handle:horizontal { background: @border; min-width: 32px; border-radius: 4px; }
QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover { background: @borderStrong; }
QScrollBar::handle:vertical:pressed, QScrollBar::handle:horizontal:pressed { background: @accentDeep; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QProgressBar { background: @field; border: 0; border-radius: 3px; color: transparent; max-height: 6px; }
QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @accent, stop:1 @sakura); border-radius: 3px; }
QRubberBand { background: @accent/15; border: 1px solid @accent; }
QLabel#dragPage { background: @raised; color: @text; border: 1px solid @accent; border-radius: 5px; padding: 5px 10px; }
QLabel#inputError { color: @danger; font-size: 12px; padding: 4px 0; }
QLabel#orbitResult, QLabel#modelResult { color: @textMuted; font-size: 12px; }
QLabel#orbitResult[error="true"], QLabel#modelResult[error="true"] { color: @danger; }
QLineEdit#scenarioDataHash { font-family: "Cascadia Mono", "Consolas"; font-size: 11px; }
QFrame#formSection { background: transparent; border: 0; border-top: 1px solid @line; border-radius: 0; }
QFrame#choiceCard { background: @panel; border: 1px solid @line; border-radius: 6px; }
QFrame#choiceCard:hover { border-color: @borderStrong; }
QFrame#choiceCard[selected="true"] { background: @accentWash; border-color: @accent; }
QFrame#choiceCard QLabel, QFrame#choiceCard QRadioButton { background: transparent; border: 0; }
QFrame#epochField { background: @field; border: 1px solid @border; border-radius: 4px; }
QFrame#epochField:hover { border-color: @borderStrong; }
QFrame#epochField[readOnly="true"] { background: @fieldMuted; border-color: @line; }
QFrame#epochField[focused="true"] { border-color: @accent; }
QFrame#epochField QLineEdit { border: 0; background: transparent; padding: 3px 8px; min-height: 20px; max-height: 20px; }
QFrame#epochField QToolButton { padding: 2px; border: 0; border-radius: 4px; }
QFrame#epochField QToolButton:hover { background: @accentHover; }
QLineEdit#epochValue { font-family: "Cascadia Mono", "Consolas"; font-size: 12px; }
QFrame#epochPopup { background: @raised; border: 1px solid @borderStrong; border-radius: 8px; }
QCalendarWidget QWidget { background: @raised; }
QCalendarWidget QToolButton { padding: 5px; color: @textSoft; font-weight: 600; border: 0; }
QCalendarWidget QToolButton#qt_calendar_prevmonth { qproperty-icon: url(:/orbitforge/icons/chevron-left.png); }
QCalendarWidget QToolButton#qt_calendar_nextmonth { qproperty-icon: url(:/orbitforge/icons/chevron-right.png); }
QCalendarWidget QToolButton:hover { background: @accentHover; }
QCalendarWidget QAbstractItemView { border: 0; outline: 0; color: @textSoft; background: @raised; selection-background-color: @accentDeep; selection-color: #ffffff; alternate-background-color: @overlay; }
QListWidget#missionSegments { background: transparent; border: 0; padding: 0; outline: 0; }
QScrollArea#missionParameters { background: @panel; border: 1px solid @line; border-radius: 8px; }
QScrollArea#missionParameters > QWidget, QWidget#missionParameterFields { background: transparent; }
QTabBar#missionNavigation::tab { background: transparent; color: @textMuted; border: 0; border-bottom: 2px solid transparent; padding: 6px 12px; }
QTabBar#missionNavigation::tab:selected { color: @text; border-bottom-color: @sakura; }
QTabBar#missionNavigation::tab:hover:!selected { color: @textSoft; background: @accentHover; }
QTabBar#missionNavigation::tab:disabled { color: @textFaint; }
QLabel#missionSegmentTiming { color: @accent; font-family: "Cascadia Mono", "Consolas", "Microsoft YaHei UI"; font-size: 11px; }
QLabel#missionImpulseMagnitude { color: @sakura; font-family: "Cascadia Mono", "Consolas", "Microsoft YaHei UI"; font-size: 11px; padding: 4px 0; }
QLabel#missionEmpty { color: @textMuted; font-size: 12px; }
QFrame#missionStatus { background: @panel; border: 1px solid @line; border-radius: 8px; }
QLabel#missionSummary { color: @textSoft; font-family: "Cascadia Mono", "Consolas", "Microsoft YaHei UI"; font-size: 11px; }
QLabel#missionCoverage { color: @textMuted; font-family: "Cascadia Mono", "Consolas", "Microsoft YaHei UI"; font-size: 11px; }
QLabel#missionCoverage[warning="true"] { color: @warning; }
QDialog#missionEditorDialog QLabel#errorText { color: @danger; font-size: 12px; }
)QSS"
        R"QSS(QWidget#dataSidebar { background: @panel; border-right: 1px solid @line; }
QLabel#dataSidebarCaption { color: @textMuted; padding: 0 12px; font-size: 11px; font-weight: 600; letter-spacing: 1px; }
QTreeWidget#dataInventory { background: transparent; border: 0; outline: 0; padding: 0; }
QTreeWidget#dataInventory::item { border: 0; padding: 0; }
QTreeWidget#dataInventory::branch { background: @panel; }
QListWidget#eopSourcePriority { background: transparent; border: 0; outline: 0; padding: 0; }
QListWidget#eopSourcePriority::item { border: 0; padding: 0; }
QLabel#dataName { font-size: 17px; color: @text; }
QLabel#dataDescription { color: @textMuted; }
QTabWidget#dataPages::pane { border: 0; background: transparent; }
QTabWidget#dataPages QTabBar::tab { background: transparent; color: @textMuted; border: 0; border-bottom: 2px solid @line; padding: 8px 16px; margin-right: 0; }
QTabWidget#dataPages QTabBar::tab:selected { color: @text; border-bottom-color: @sakura; }
QTabWidget#dataPages QTabBar::tab:hover:!selected { color: @textSoft; background: @accentHover; }
QLabel#dataStatus { border-radius: 8px; padding: 2px 9px; font-size: 11px; font-weight: 600; background: @overlay; color: @textSoft; }
QLabel#dataStatus[tone="ready"] { background: @successWash; color: @success; }
QLabel#dataStatus[tone="warning"] { background: @warningWash; color: @warning; }
QFrame#dataFacts { background: @panel; border: 1px solid @line; border-radius: 8px; }
QLabel#dataValidFrom, QLabel#dataValidUntil { color: @text; font-family: "Cascadia Mono", "Consolas"; font-size: 12px; }
QLabel#dataUpdateSchedule { color: @textMuted; font-size: 11px; }
QProgressBar#dataDownloadProgress { background: @field; border: 0; border-radius: 2px; }
QProgressBar#dataDownloadProgress::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 @accent, stop:1 @sakura); border-radius: 2px; }
QLabel#dataSource { color: @accent; }
QLabel#dataNote { color: @textMuted; padding: 4px 0; font-size: 11px; }
QLabel#dataPriorityNote { color: @textMuted; padding: 14px; background: @panel; border: 1px solid @line; border-radius: 8px; }
QLabel#dataInventoryDetails { color: @textMuted; font-family: "Cascadia Mono", "Consolas"; font-size: 11px; padding: 8px; background: @field; border: 1px solid @line; border-radius: 6px; }
QLabel#dataCenterStatus { color: @accent; }
QLabel#dataCenterStatus[warning="true"] { color: @warning; }
QWidget#dataFooter { background: @panel; border-top: 1px solid @line; }
QWidget#reportHeading { background: @surface; border-bottom: 1px solid @line; }
QLabel#reportHeadingTitle { color: @text; font-size: 16px; }
QWidget#reportSettingsPanel, QWidget#reportSettings { background: @panel; }
QWidget#reportGenerateActions { background: @panel; border-top: 1px solid @line; }
QLabel#reportSectionTitle { color: @textMuted; font-size: 11px; font-weight: 700; letter-spacing: 1px; }
QLabel#reportScopeLabel { color: @text; font-weight: 600; }
QLabel#reportScopeDetail, QLabel#reportInterval, QLabel#reportEstimate { color: @textMuted; font-size: 11px; }
QLabel#reportInterval { font-family: "Cascadia Mono", "Consolas"; }
QLabel#reportResultTitle { color: @text; font-size: 16px; }
QLabel#reportResultLabel { color: @textMuted; font-size: 11px; }
QLabel#reportEmptyTitle { color: @textSoft; font-size: 16px; }
QLabel#reportEmptyIcon { background: @accentWash; border-radius: 26px; }
QLabel#reportStatus { color: @textMuted; background: @chrome; border-top: 1px solid @line; font-size: 11px; }
QTableView#reportTable { background: @field; alternate-background-color: @panel; border: 1px solid @line; border-radius: 6px; gridline-color: @line; selection-background-color: @accentWash; selection-color: @accentBright; }
QWidget#reportHeading QPushButton, QWidget#reportSettings QPushButton { background: transparent; border-color: transparent; }
QWidget#reportHeading QPushButton:hover, QWidget#reportSettings QPushButton:hover { background: @accentHover; }
QWidget#reportSettings QComboBox, QWidget#reportSettings QLineEdit, QWidget#reportSettings QDoubleSpinBox { min-height: 18px; }
QLabel#reportTemplateDescription { color: @textMuted; font-size: 11px; }
QDialog#reportSelectionDialog { background: @surface; }
QLabel#reportSelectionHeading { color: @text; font-size: 19px; }
QLabel#reportSelectionSubtitle { color: @textMuted; font-size: 12px; }
QLineEdit#reportSelectionSearch { background: @field; border: 1px solid @border; border-radius: 5px; padding: 6px 9px; min-width: 0; }
QLineEdit#reportSelectionSearch:focus { border-color: @accent; }
QTabBar#reportSelectionSource::tab { color: @textMuted; background: transparent; border: 0; border-bottom: 2px solid transparent; padding: 5px 10px; margin-right: 3px; font-size: 12px; }
QTabBar#reportSelectionSource::tab:selected { color: @text; border-bottom-color: @sakura; }
QTabBar#reportSelectionSource::tab:hover:!selected { color: @textSoft; background: @accentHover; }
QListWidget#reportSelectionList { background: transparent; border: 0; padding: 0; outline: 0; }
QSplitter#reportSelectionSplit::handle { background: transparent; }
QSplitter#reportSelectionSplit::handle:hover { background: @accentWash; border-radius: 4px; }
QScrollArea#reportSelectionPreview { background: @panel; border: 1px solid @line; border-radius: 8px; }
QScrollArea#reportSelectionPreview > QWidget, QWidget#reportSelectionDetails { background: transparent; }
QLabel#reportSelectionSymbol { color: @accentBright; background: @accentWash; border-radius: 8px; }
QLabel#reportSelectionCategory { color: @sakura; font-size: 11px; font-weight: 600; }
QLabel#reportSelectionTitle { color: @text; font-size: 18px; }
QLabel#reportSelectionDescription { color: @textSoft; font-size: 12px; }
QFrame#reportSelectionDivider { background: @line; border: 0; }
QLabel#reportSelectionFieldHeading { color: @textMuted; font-size: 11px; }
QLabel#reportSelectionOutput, QLabel#reportSelectionScope { color: @textSoft; font-size: 12px; }
QLabel#reportSelectionNote { color: @textMuted; font-size: 12px; padding-top: 8px; }
QLabel#reportSelectionCount, QLabel#reportSelectionEmpty { color: @textMuted; font-size: 12px; }
QLabel#reportSelectionWarning { color: @warning; background: @warningWash; border-radius: 6px; padding: 8px; }
QToolButton#reportConfigMenu { color: @textSoft; background: transparent; border: none; padding: 2px 12px 2px 6px; font-size: 11px; }
QToolButton#reportConfigMenu:hover { color: @accentBright; background: @accentHover; border-radius: 4px; }
QPushButton#reportChangeScope { color: @accent; background: transparent; border: none; padding: 3px 6px; }
QPushButton#reportChangeScope:hover { color: @accentBright; background: @accentHover; }
QTabBar#reportViews::tab { color: @textMuted; background: transparent; border: none; border-radius: 4px; padding: 4px 14px; margin: 2px; min-width: 28px; }
QTabBar#reportViews::tab:selected { color: @accentBright; background: @accentWash; }
QTabBar#reportViews::tab:hover:!selected { color: @text; background: @accentHover; }
QTabBar#reportViews::tab:disabled { color: @textFaint; background: transparent; }
QToolButton#refreshReport { font-size: 18px; padding: 0px; min-width: 28px; min-height: 28px; border: none; background: transparent; }
QToolButton#reportResultMenu { font-size: 15px; padding: 2px 6px; border: none; background: transparent; }
QToolButton#reportResultMenu::menu-indicator { image: none; }
QPushButton#exportReportCsv { background: @accentWash; border: 1px solid @accentWash; color: @accentBright; border-radius: 5px; padding: 4px 12px; }
QPushButton#exportReportCsv:hover { background: @overlay; border-color: @accent; }
QPushButton#exportReportCsv:disabled { color: @textFaint; background: transparent; border-color: @line; }
QComboBox#reportPlotField { background: @raised; border: 1px solid @border; border-radius: 4px; padding: 3px 24px 3px 10px; min-height: 18px; }
QComboBox#reportPlotField:hover { border-color: @borderStrong; }
)QSS"
        R"QSS(QWidget#sensorFields, QScrollArea#sensorScroll { background: @surface; }
QLabel#sensorTitle { color: @text; font-size: 16px; }
QLabel#sensorCaption { color: @textMuted; font-size: 12px; }
QLabel#sensorBadge { background: @accentWash; border: 1px solid @border; border-radius: 10px; }
QFrame#sensorParent { background: @panel; border: 1px solid @line; border-radius: 6px; }
QLabel#sensorParentName { color: @textSoft; }
QFrame#sensorFov { background: @panel; border: 1px solid @line; border-radius: 8px; }
QLabel#sensorFullAngle { color: @accent; font-size: 12px; }
QPushButton[sensorChoice="true"] { background: @panel; border: 1px solid @border; border-radius: 4px; padding: 3px 8px; color: @textSoft; }
QPushButton[sensorChoice="true"]:hover { background: @accentHover; border-color: @borderStrong; }
QPushButton[sensorChoice="true"]:checked { background: @accentWash; border-color: @accent; color: @accentBright; }
QPushButton[sensorChoice="true"]:focus { border-color: @accentBright; }
QFrame#sensorFooter { background: @panel; border-top: 1px solid @line; }
QLabel#sensorTargetHint { color: @sakura; font-size: 12px; }
QLabel#sensorError { color: @danger; font-size: 12px; }
QWidget#welcome { background: @canvas; }
QScrollArea#welcomeScroll { background: @canvas; }
QLabel#welcomeTitle { color: @text; font-size: 40px; font-weight: 300; padding: 0; }
QLabel#welcomeDescription { color: @textSoft; font-size: 14px; }
QLabel#welcomeCaption { color: @textMuted; font-size: 11px; }
QLabel#welcomeKey { color: @textMuted; background: transparent; border: 0; font-family: "Cascadia Mono", "Consolas"; font-size: 11px; }
QPushButton#welcomeCreate, QPushButton#welcomeOpen, QPushButton#welcomeExample { background: @chrome/82; border: 1px solid @line; border-radius: 8px; padding: 0; text-align: left; }
QPushButton#welcomeCreate:hover, QPushButton#welcomeOpen:hover, QPushButton#welcomeExample:hover { background: @raised/92; border-color: @borderStrong; }
QPushButton#welcomeCreate:focus, QPushButton#welcomeOpen:focus, QPushButton#welcomeExample:focus { border: 1px solid @accent; }
QPushButton#welcomeCreate:pressed, QPushButton#welcomeOpen:pressed, QPushButton#welcomeExample:pressed { background: @accentWash; }
QLabel#welcomeCardTitle { color: @text; font-size: 13px; background: transparent; }
QLabel#welcomeCardText { color: @textMuted; font-size: 12px; background: transparent; }
QLabel#welcomeCardIcon { background: @accentWash; border-radius: 6px; }
QLabel#welcomeCardIcon[tone="sakura"] { background: @sakuraWash; }
QLabel#welcomeCardIcon[tone="primary"] { background: @accentDeep; }
QPushButton[objectSelector="true"] { background: @field; border: 1px solid @border; border-radius: 4px; text-align: left; padding: 4px 9px; min-height: 18px; }
QPushButton[objectSelector="true"]:hover { background: @accentHover; border-color: @accent; }
QPushButton[objectSelector="true"]:focus { border-color: @accentBright; }
QPushButton[objectSelector="true"]:disabled { color: @textFaint; border-color: @line; }
QTreeWidget#objectSelectionTree { background: @field; border: 1px solid @line; border-radius: 6px; outline: 0; padding: 3px; }
QTreeWidget#objectSelectionTree::item { height: 24px; padding: 2px 6px; border: 0; border-radius: 4px; }
QTreeWidget#objectSelectionTree::branch { background: @field; }
QTreeWidget#objectSelectionTree::branch:closed:has-children { image: url(:/orbitforge/icons/chevron-right.png); }
QTreeWidget#objectSelectionTree::branch:open:has-children { image: url(:/orbitforge/icons/chevron-down.png); }
QLabel#objectSelectionPurpose, QLabel#objectSelectionDetails, QLabel#objectSelectionCount { color: @textMuted; font-size: 12px; }
QLabel#objectSelectionEmpty { color: @textMuted; background: transparent; padding: 20px; }
QFrame#hpopResource { background: @panel; border: 1px solid @line; border-radius: 8px; }
QLabel#hpopEphemerisStatus { color: @textMuted; font-size: 12px; }
QLabel#hpopResourceTitle { color: @text; font-size: 15px; }
QLabel#hpopResourceBadge { color: @accentBright; background: @accentWash; border-radius: 8px; padding: 1px 8px; font-size: 11px; }
QFrame[hpopSection="true"] { background: transparent; border: 0; border-top: 1px solid @line; border-radius: 0; }
QLabel#hpopSectionTitle { color: @text; font-size: 12px; font-weight: bold; }
QLabel#hpopPendingBadge { color: @sakura; background: @sakuraWash; border-radius: 8px; padding: 1px 8px; font-size: 11px; }
QTabWidget#hpopForceModelTabs QTabBar::tab { background: transparent; padding: 4px 10px; }
QTabWidget#hpopForceModelTabs QTabBar::tab:selected { background: @accentWash; border-bottom-color: @accent; color: @accentBright; }
QDialog#hpopSettingsDialog QLineEdit[readOnly="true"] { color: @textMuted; }
QFrame[formGroup="true"] { background: transparent; border: 0; border-top: 1px solid @line; }
QFrame[formGroup="true"][leading="true"] { border-top: 0; }
QLabel#groupSummary { color: @textMuted; font-size: 11px; }
QLabel#hpopGravityStatus, QLabel#hpopDragEphemerisStatus, QLabel#hpopSrpEphemerisStatus { color: @textMuted; font-size: 11px; }
QToolButton#infoMarker, QToolButton#groupChevron { border: 0; padding: 0; border-radius: 4px; background: transparent; }
QToolButton#infoMarker:hover, QToolButton#groupChevron:hover { background: @accentHover; }
QLabel#caution { color: @textSoft; background: @warning/8; border: 0; border-left: 2px solid @warning; padding: 5px 9px; font-size: 11px; }
QCheckBox[switch="true"] { spacing: 10px; font-weight: 600; color: @text; min-height: 26px; }
QListWidget#hpopNavigation { background: @panel; border: 0; border-right: 1px solid @line; outline: 0; padding: 8px 6px; }
QTableWidget#hpopThirdBodyTable { background: @field; border: 1px solid @line; border-radius: 6px; gridline-color: @line; }
QTableWidget#hpopThirdBodyTable::item { color: @textSoft; padding: 0 6px; }
QTableWidget#hpopThirdBodyTable::item:selected { background: transparent; color: @text; }
QTableWidget#hpopThirdBodyTable QComboBox, QTableWidget#hpopThirdBodyTable QFrame[quantityField="true"] { background: transparent; border-color: transparent; }
QTableWidget#hpopThirdBodyTable QComboBox:hover, QTableWidget#hpopThirdBodyTable QFrame[quantityField="true"]:hover { border-color: @border; }
QTableWidget#hpopThirdBodyTable QComboBox:focus, QTableWidget#hpopThirdBodyTable QFrame[quantityField="true"][focused="true"] { border-color: @accent; background: @field; }
QDialog#hpopSettingsDialog QComboBox { max-width: 242px; }
QDialog#hpopSettingsDialog QSpinBox, QDialog#hpopSettingsDialog QDoubleSpinBox { max-width: 258px; }
)QSS")));
}
} // namespace orbitforge::desktop
