#include "welcome_page.hpp"
#include "workbench_style.hpp"
#include "workbench_theme.hpp"
#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>

namespace orbitforge::desktop {
namespace {
constexpr int narrow_width = 720;

// The artwork fills the whole page (cover, anchored on the figure). A canvas-coloured veil
// on the left keeps the copy and actions legible; narrow pages veil the whole picture.
class WelcomeBackdrop final : public QWidget {
  public:
    WelcomeBackdrop() : artwork_(":/orbitforge/artwork/orbital-welcome.png") {}

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.fillRect(rect(), theme::color(theme::canvas));
        if (!artwork_.isNull()) {
            const auto scale =
                std::max(width() / double(artwork_.width()), height() / double(artwork_.height()));
            const QSizeF source_size(width() / scale, height() / scale);
            const QRectF source((artwork_.width() - source_size.width()) * 0.78,
                                (artwork_.height() - source_size.height()) * 0.15,
                                source_size.width(), source_size.height());
            painter.drawPixmap(QRectF(rect()), artwork_, source);
        }
        const bool narrow = width() < narrow_width;
        QLinearGradient veil(0, 0, width(), 0);
        for (const auto &[stop, alpha] :
             {std::pair{0.0, 245}, std::pair{0.32, narrow ? 225 : 215},
              std::pair{0.58, narrow ? 215 : 90}, std::pair{1.0, narrow ? 200 : 0}})
            veil.setColorAt(stop, theme::color(theme::canvas, alpha));
        painter.fillRect(rect(), veil);
        QLinearGradient floor(0, height() * 0.6, 0, height());
        floor.setColorAt(0, theme::color(theme::canvas, 0));
        floor.setColorAt(1, theme::color(theme::canvas, 190));
        painter.fillRect(rect(), floor);
    }

  private:
    QPixmap artwork_;
};

// A launcher row. Child labels are presentation only, so the button keeps the usual click,
// keyboard and accessibility behaviour; height follows the wrapped text.
class ActionCard final : public QPushButton {
  public:
    ActionCard(const char *name, WorkbenchIcon icon, const char *tone, const QString &title,
               const QString &text, const QString &shortcut) {
        setObjectName(name);
        setAccessibleName(title);
        setToolTip(shortcut.isEmpty() ? title : title + QStringLiteral(" · ") + shortcut);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(theme::space::card);
        layout->setSpacing(12);
        const QByteArray kind(tone);
        auto *symbol = new QLabel;
        symbol->setObjectName("welcomeCardIcon");
        symbol->setProperty("tone", tone);
        symbol->setFixedSize(32, 32);
        symbol->setAlignment(Qt::AlignCenter);
        symbol->setPixmap(workbench_icon(icon, theme::color(kind == "primary"  ? theme::on_accent
                                                            : kind == "sakura" ? theme::sakura
                                                                               : theme::accent))
                              .pixmap(18, 18));
        layout->addWidget(symbol, 0, Qt::AlignVCenter);
        auto *copy = new QVBoxLayout;
        copy->setContentsMargins(0, 0, 0, 0);
        copy->setSpacing(2);
        auto *heading = new QLabel(title);
        heading->setObjectName("welcomeCardTitle");
        heading->setWordWrap(true);
        copy->addWidget(heading);
        auto *detail = new QLabel(text);
        detail->setObjectName("welcomeCardText");
        detail->setWordWrap(true);
        // Wrapped labels otherwise report a wide minimum and force horizontal scrolling.
        for (auto *label : {heading, detail})
            label->setMinimumWidth(1);
        copy->addWidget(detail);
        layout->addLayout(copy, 1);
        if (!shortcut.isEmpty()) {
            auto *key = new QLabel(shortcut);
            key->setObjectName("welcomeKey");
            layout->addWidget(key, 0, Qt::AlignVCenter);
        }
        for (auto *child : findChildren<QWidget *>())
            child->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    [[nodiscard]] QSize sizeHint() const override { return layout()->sizeHint(); }
    [[nodiscard]] QSize minimumSizeHint() const override { return layout()->minimumSize(); }
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(int width) const override {
        return layout()->totalHeightForWidth(width);
    }
};

// The side margin follows the page width so the column sits in the artwork's open sky.
class WelcomeColumn final : public QWidget {
  public:
    QBoxLayout *outer = nullptr;

  protected:
    void resizeEvent(QResizeEvent *event) override {
        if (outer) {
            const int width = event->size().width();
            const int side = width < narrow_width ? 24 : std::clamp(width / 12, 40, 120);
            outer->setContentsMargins(side, 40, side, 28);
        }
        QWidget::resizeEvent(event);
    }
};
} // namespace

QWidget *make_welcome_page(QAction *create, QAction *open, QAction *example) {
    auto *scroll = new QScrollArea;
    scroll->setObjectName("welcomeScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *page = new WelcomeBackdrop;
    page->setObjectName("welcome");
    scroll->setWidget(page);
    auto *page_layout = new QVBoxLayout(page);
    page_layout->setContentsMargins(0, 0, 0, 0);
    auto *column_host = new WelcomeColumn;
    page_layout->addWidget(column_host);
    auto *outer = new QVBoxLayout(column_host);
    column_host->outer = outer;
    outer->setContentsMargins(40, 40, 40, 28);
    outer->setSpacing(0);
    outer->addStretch(3);

    auto *column = new QWidget;
    column->setMaximumWidth(420);
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto *brand = new QLabel(QStringLiteral("ORBITFORGE"));
    brand->setObjectName("eyebrow");
    layout->addWidget(brand);
    auto *heading =
        new QLabel(QStringLiteral("下一站，<br><span style='color:%1'>你的轨道。</span>")
                       .arg(theme::color(theme::sakura).name()));
    heading->setObjectName("welcomeTitle");
    heading->setWordWrap(true);
    layout->addWidget(heading);
    auto *description =
        new QLabel(QStringLiteral("从一颗卫星开始，探索轨道、可见性与每一次交会。"));
    description->setObjectName("welcomeDescription");
    description->setWordWrap(true);
    layout->addWidget(description);
    layout->addSpacing(14);

    auto *new_button = new ActionCard(
        "welcomeCreate", WorkbenchIcon::new_scenario, "primary", QStringLiteral("新建场景"),
        QStringLiteral("设定分析时段与地球定向数据，从空白场景开始。"), QStringLiteral("Ctrl+N"));
    QObject::connect(new_button, &QPushButton::clicked, create, &QAction::trigger);
    auto *open_button = new ActionCard(
        "welcomeOpen", WorkbenchIcon::folder, "accent", QStringLiteral("打开项目…"),
        QStringLiteral("继续编辑已保存的 .orbitforge 项目。"), QStringLiteral("Ctrl+O"));
    QObject::connect(open_button, &QPushButton::clicked, open, &QAction::trigger);
    auto *example_button =
        new ActionCard("welcomeExample", WorkbenchIcon::orbit, "sakura", QStringLiteral("二体示例"),
                       QStringLiteral("载入预置卫星，立即查看三维轨道与地面轨迹。"), QString{});
    QObject::connect(example_button, &QPushButton::clicked, example, &QAction::trigger);
    layout->addWidget(new_button);
    layout->addWidget(open_button);
    layout->addWidget(example_button);
    outer->addWidget(column);
    outer->addStretch(4);

    auto *caption = new QLabel(QStringLiteral("星夜相伴 · 探索有据"));
    caption->setObjectName("welcomeCaption");
    outer->addWidget(caption);
    return scroll;
}
} // namespace orbitforge::desktop
