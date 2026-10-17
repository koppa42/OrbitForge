#include "startup_splash.hpp"
#include "workbench_style.hpp"
#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QScreen>
#include <algorithm>
#include <stdexcept>

namespace orbitforge::desktop {
namespace {
QPixmap splash_picture(QScreen *screen) {
    const QPixmap artwork(QStringLiteral(":/orbitforge/startup/star-gazing.png"));
    if (artwork.isNull())
        throw std::runtime_error("Startup artwork is missing from the application resources");
    const auto available = screen ? screen->availableGeometry().size() : QSize(1280, 800);
    const auto scale =
        std::min({1.0, available.width() * 0.78 / 960, available.height() * 0.78 / 540});
    const auto ratio = screen ? screen->devicePixelRatio() : 1.0;
    const QSize pixels(qRound(960 * scale * ratio), qRound(540 * scale * ratio));
    auto result = artwork.scaled(pixels, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    result = result.copy((result.width() - pixels.width()) / 2,
                         (result.height() - pixels.height()) / 2, pixels.width(), pixels.height());
    result.setDevicePixelRatio(ratio);
    return result;
}
} // namespace
StartupSplash::StartupSplash(QScreen *screen) : QSplashScreen(screen, splash_picture(screen)) {
    setObjectName("startupSplash");
    setWindowTitle(QStringLiteral("OrbitForge · 启动"));
    setWindowIcon(workbench_icon(WorkbenchIcon::orbit, theme::color(theme::accent)));
    if (screen)
        move(screen->availableGeometry().center() - rect().center());
}
void StartupSplash::drawContents(QPainter *painter) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->scale(width() / 960.0, height() / 540.0);
    QLinearGradient left(0, 0, 580, 0);
    left.setColorAt(0, theme::color(theme::canvas, 150));
    left.setColorAt(1, theme::color(theme::canvas, 0));
    painter->fillRect(QRectF(0, 0, 960, 540), left);
    QLinearGradient footer(0, 434, 0, 540);
    footer.setColorAt(0, theme::color(theme::canvas, 0));
    footer.setColorAt(1, theme::color(theme::canvas, 170));
    painter->fillRect(QRectF(0, 434, 960, 106), footer);

    painter->drawPixmap(QPointF(44, 46), brand_mark(40, painter->device()->devicePixelRatioF()));
    QFont font;
    font.setFamilies({"Microsoft YaHei UI", "Segoe UI"});
    font.setPixelSize(40);
    font.setWeight(QFont::DemiBold);
    painter->setFont(font);
    painter->setPen(theme::color(theme::text));
    painter->drawText(QRectF(98, 37, 520, 58), Qt::AlignLeft | Qt::AlignVCenter, "OrbitForge");
    font.setPixelSize(15);
    font.setWeight(QFont::Normal);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
    painter->setFont(font);
    painter->setPen(theme::color(theme::text_soft));
    painter->drawText(QRectF(100, 94, 420, 26), Qt::AlignLeft | Qt::AlignVCenter,
                      QStringLiteral("轨道仿真工作台"));

    // Ice-to-sakura hairline above the status message.
    QLinearGradient rule(48, 0, 368, 0);
    rule.setColorAt(0, theme::color(theme::accent, 220));
    rule.setColorAt(1, theme::color(theme::sakura, 0));
    painter->setPen(QPen(QBrush(rule), 1));
    painter->drawLine(QPointF(48, 474), QPointF(368, 474));
    painter->setPen(Qt::NoPen);
    painter->setBrush(theme::color(theme::sakura));
    painter->drawEllipse(QPointF(50, 496), 3, 3);
    font.setPixelSize(13);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    painter->setFont(font);
    painter->setPen(theme::color(theme::text_soft));
    painter->drawText(QRectF(62, 480, 800, 32), Qt::AlignLeft | Qt::AlignVCenter, message());
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(theme::color(theme::accent, 50), 1));
    painter->drawRect(QRectF(0.5, 0.5, 959, 539));
    painter->restore();
}
} // namespace orbitforge::desktop
