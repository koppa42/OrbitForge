#include "../src/viewer/welcome_page.hpp"
#include "../src/viewer/workbench_style.hpp"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QKeyEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>

namespace orbitforge::desktop {
namespace {
QApplication &styled_application() {
    static int argc = 1;
    static char executable[] = "welcome_test";
    static char *argv[] = {executable, nullptr};
    static QApplication app(argc, argv);
    static const bool styled = (apply_workbench_style(app), true);
    (void)styled;
    return app;
}
} // namespace

TEST(WelcomePage, ActionsRemainReachableAtNarrowAndWideSizes) {
    (void)styled_application();
    QAction create, open, example;
    int created = 0, opened = 0, tried = 0;
    QObject::connect(&create, &QAction::triggered, [&] { ++created; });
    QObject::connect(&open, &QAction::triggered, [&] { ++opened; });
    QObject::connect(&example, &QAction::triggered, [&] { ++tried; });
    const std::unique_ptr<QWidget> page(make_welcome_page(&create, &open, &example));
    auto *scroll = qobject_cast<QScrollArea *>(page.get());
    ASSERT_NE(scroll, nullptr);
    for (const auto size : {QSize(320, 360), QSize(1280, 760)}) {
        page->resize(size);
        page->show();
        QCoreApplication::processEvents();
        EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
        for (const auto *name : {"welcomeCreate", "welcomeOpen", "welcomeExample"}) {
            auto *button = page->findChild<QPushButton *>(name);
            ASSERT_NE(button, nullptr);
            scroll->ensureWidgetVisible(button);
            QCoreApplication::processEvents();
            const QRect visible(button->mapTo(scroll->viewport(), QPoint(0, 0)), button->size());
            EXPECT_TRUE(scroll->viewport()->rect().contains(visible));
            button->setFocus(Qt::TabFocusReason);
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
            QCoreApplication::sendEvent(button, &press);
            QCoreApplication::sendEvent(button, &release);
        }
        const auto captures = qEnvironmentVariable("ORBITFORGE_WELCOME_CAPTURE");
        if (!captures.isEmpty()) {
            ASSERT_TRUE(QDir().mkpath(captures));
            scroll->verticalScrollBar()->setValue(0);
            EXPECT_TRUE(page->grab().save(captures + QString("/welcome-%1.png").arg(size.width())));
        }
    }
    EXPECT_EQ(created, 2);
    EXPECT_EQ(opened, 2);
    EXPECT_EQ(tried, 2);
}

TEST(WorkbenchTheme, StylesheetReferencesOnlyKnownTokens) {
    EXPECT_EQ(theme::resolve("a { color: @accent; background: @accentWash; }"),
              QString("a { color: %1; background: %2; }")
                  .arg(QLatin1StringView(theme::accent.value.data(), theme::accent.value.size()),
                       QLatin1StringView(theme::accent_wash.value.data(),
                                         theme::accent_wash.value.size())));
    const auto canvas = theme::color(theme::canvas);
    EXPECT_EQ(
        theme::resolve("@canvas/50"),
        QString("rgba(%1, %2, %3, 128)").arg(canvas.red()).arg(canvas.green()).arg(canvas.blue()));
    EXPECT_THROW((void)theme::resolve("a { color: @unknownToken; }"), std::invalid_argument);
    EXPECT_THROW((void)theme::resolve("a { color: @canvas/150; }"), std::invalid_argument);
    // The application stylesheet resolved every reference when the style was applied.
    const auto &app = styled_application();
    EXPECT_FALSE(app.styleSheet().isEmpty());
    EXPECT_FALSE(app.styleSheet().contains('@'));
}
} // namespace orbitforge::desktop
