#include "workspace.hpp"
#include "workbench_style.hpp"
#include <QApplication>
#include <QDialog>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QRubberBand>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace orbitforge::desktop {
namespace {
// Mouse capture keeps dragging reliable over the native Vulkan child window.
class PageTabs final : public QTabBar {
  public:
    std::function<void(int, const QPoint &)> dragging;
    std::function<void(int, const QPoint &, bool)> dropped;

  protected:
    void mousePressEvent(QMouseEvent *event) override {
        QTabBar::mousePressEvent(event);
        if (event->button() == Qt::LeftButton) {
            pressed_ = tabAt(event->position().toPoint());
            start_ = event->globalPosition().toPoint();
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        const auto global = event->globalPosition().toPoint();
        if (pressed_ >= 0 && (event->buttons() & Qt::LeftButton) &&
            (moving_ || (global - start_).manhattanLength() >= QApplication::startDragDistance())) {
            if (!moving_) {
                moving_ = true;
                grabKeyboard();
            }
            if (dragging)
                dragging(pressed_, global);
            event->accept();
        } else {
            QTabBar::mouseMoveEvent(event);
        }
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (moving_ && event->button() == Qt::LeftButton) {
            const int tab = pressed_;
            moving_ = false;
            pressed_ = -1;
            releaseKeyboard();
            QTabBar::mouseReleaseEvent(event);
            if (dropped)
                dropped(tab, event->globalPosition().toPoint(), false);
            event->accept();
        } else {
            pressed_ = -1;
            QTabBar::mouseReleaseEvent(event);
        }
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (moving_ && event->key() == Qt::Key_Escape) {
            const int tab = pressed_;
            moving_ = false;
            pressed_ = -1;
            releaseKeyboard();
            QMouseEvent release(QEvent::MouseButtonRelease, mapFromGlobal(start_), start_,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QTabBar::mouseReleaseEvent(&release);
            if (dropped)
                dropped(tab, {}, true);
            event->accept();
        } else {
            QTabBar::keyPressEvent(event);
        }
    }

  private:
    QPoint start_;
    int pressed_ = -1;
    bool moving_ = false;
};
} // namespace

struct Workspace::Pane : QWidget {
    PageTabs *tabs;
    QStackedWidget *stack;
    QDialog *window;
};

Workspace::Workspace(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    parking_ = new QWidget(this);
    parking_->hide();
    root_ = new QSplitter(Qt::Horizontal);
    root_->setChildrenCollapsible(false);
    root_->setHandleWidth(6);
    layout->addWidget(root_);
    root_->addWidget(make_pane());
    drop_hint_ = new QRubberBand(QRubberBand::Rectangle);
    drop_hint_->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint |
                               Qt::WindowTransparentForInput);
    drop_hint_->setAttribute(Qt::WA_ShowWithoutActivating);
    drag_label_ = new QLabel;
    drag_label_->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint |
                                Qt::WindowTransparentForInput);
    drag_label_->setAttribute(Qt::WA_ShowWithoutActivating);
    drag_label_->setObjectName("dragPage");
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (auto *pane : panes_)
            if (now && pane->isAncestorOf(now)) {
                active_ = pane;
                break;
            }
    });
}
Workspace::~Workspace() {
    for (const auto &page : pages_)
        if (page->content) {
            page->content->removeEventFilter(this);
            disconnect(page->content, nullptr, this, nullptr);
        }
    for (auto *pane : panes_) {
        disconnect(pane->tabs, nullptr, this, nullptr);
        if (pane->window)
            pane->window->removeEventFilter(this);
    }
    disconnect(qApp, nullptr, this, nullptr);
    delete drop_hint_;
    delete drag_label_;
}
Workspace::Pane *Workspace::make_pane(QDialog *window) {
    auto *pane = new Pane;
    pane->setObjectName("workspacePane");
    pane->setMinimumSize(160, 170);
    pane->window = window;
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *heading = new QWidget;
    heading->setObjectName("workspaceHeading");
    heading->setFixedHeight(34);
    auto *bar = new QHBoxLayout(heading);
    bar->setContentsMargins(6, 0, 6, 0);
    bar->setSpacing(2);
    pane->tabs = new PageTabs;
    pane->tabs->setObjectName("workspaceTabs");
    pane->tabs->setAccessibleName(QStringLiteral("主工作区页面"));
    pane->tabs->setToolTip(QStringLiteral("拖动标签：边缘分屏，标签栏合并，窗口外独立显示"));
    pane->tabs->setExpanding(false);
    pane->tabs->setSelectionBehaviorOnRemove(QTabBar::SelectPreviousTab);
    pane->tabs->setDrawBase(false);
    pane->tabs->setUsesScrollButtons(true);
    pane->tabs->setElideMode(Qt::ElideRight);
    pane->tabs->setMinimumWidth(0);
    pane->tabs->setIconSize(QSize(16, 16));
    bar->addWidget(pane->tabs, 1);
    auto *detach = new QToolButton;
    detach->setObjectName("detachDocument");
    detach->setIcon(workbench_icon(window ? WorkbenchIcon::views : WorkbenchIcon::detach));
    detach->setFixedSize(24, 24);
    detach->setIconSize(QSize(16, 16));
    detach->setToolTip(window ? QStringLiteral("放回工作区")
                              : QStringLiteral("在独立窗口打开当前页面"));
    detach->setAccessibleName(detach->toolTip());
    bar->addWidget(detach);
    layout->addWidget(heading);
    pane->stack = new QStackedWidget;
    pane->stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto *empty = new QLabel(QStringLiteral("暂无打开的页面"));
    empty->setObjectName("muted");
    empty->setAlignment(Qt::AlignCenter);
    pane->stack->addWidget(empty);
    layout->addWidget(pane->stack, 1);
    connect(pane->tabs, &QTabBar::currentChanged, this, [this, pane](int) {
        if (auto *page = current(pane))
            activate(*page);
    });
    connect(detach, &QToolButton::clicked, this, [this, pane] {
        if (auto *page = current(pane)) {
            if (pane->window)
                dock_page(page->content);
            else
                detach_page(page->content);
        }
    });
    pane->tabs->dragging = [this, pane](int tab, const QPoint &global) {
        drag_to(reinterpret_cast<Page *>(pane->tabs->tabData(tab).value<quintptr>()), global);
    };
    pane->tabs->dropped = [this, pane](int tab, const QPoint &global, bool cancelled) {
        finish_drag(reinterpret_cast<Page *>(pane->tabs->tabData(tab).value<quintptr>()), global,
                    cancelled);
    };
    pane->tabs->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(pane->tabs, &QWidget::customContextMenuRequested, this,
            [this, pane](const QPoint &point) {
                context_menu(pane, pane->tabs->tabAt(point), pane->tabs->mapToGlobal(point));
            });
    panes_.push_back(pane);
    return pane;
}
Workspace::Pane *Workspace::main_pane() const {
    if (active_ && !active_->window)
        return active_;
    for (auto *pane : panes_)
        if (!pane->window)
            return pane;
    return nullptr;
}
Workspace::Page *Workspace::find(QWidget *content) const {
    for (const auto &page : pages_)
        if (page->content == content)
            return page.get();
    return nullptr;
}
Workspace::Page *Workspace::current(Pane *pane) const {
    return pane ? reinterpret_cast<Page *>(
                      pane->tabs->tabData(pane->tabs->currentIndex()).value<quintptr>())
                : nullptr;
}
int Workspace::add_view(QWidget *content, const QString &title, const QIcon &icon) {
    auto page = std::make_unique<Page>();
    page->content = content;
    page->host = content;
    page->title = title;
    page->icon = icon;
    auto *entry = page.get();
    pages_.push_back(std::move(page));
    views_.push_back(entry);
    append(*entry, main_pane());
    if (views_.size() == 1)
        activate(*entry);
    return static_cast<int>(views_.size()) - 1;
}
void Workspace::select_view(int index) {
    if (index < 0 || index >= static_cast<int>(views_.size()))
        return;
    auto &page = *views_[index];
    if (!page.pane)
        append(page, main_pane());
    activate(page);
}
void Workspace::append(Page &page, Pane *pane, int insertion) {
    const QSignalBlocker block(pane->tabs);
    page.pane = pane;
    pane->stack->addWidget(page.host);
    const int tab = pane->tabs->insertTab(insertion < 0 ? pane->tabs->count() : insertion,
                                          page.icon, page.title);
    pane->tabs->setTabData(tab, QVariant::fromValue(reinterpret_cast<quintptr>(&page)));
    pane->tabs->setTabToolTip(tab, page.title);
    auto *close = new QToolButton;
    close->setObjectName("closePage");
    close->setIcon(workbench_icon(WorkbenchIcon::close, theme::color(theme::text_muted)));
    close->setFixedSize(20, 20);
    close->setIconSize(QSize(14, 14));
    close->setToolTip(QStringLiteral("关闭页面"));
    close->setAccessibleName(close->toolTip());
    connect(close, &QToolButton::clicked, this,
            [this, content = page.content] { close_page(content); });
    pane->tabs->setTabButton(tab, QTabBar::RightSide, close);
}
void Workspace::remove(Page &page) {
    auto *pane = page.pane;
    if (!pane)
        return;
    {
        const QSignalBlocker block(pane->tabs);
        for (int i = 0; i < pane->tabs->count(); ++i)
            if (pane->tabs->tabData(i).value<quintptr>() == reinterpret_cast<quintptr>(&page)) {
                pane->tabs->removeTab(i);
                break;
            }
    }
    pane->stack->removeWidget(page.host);
    page.host->hide();
    page.host->setParent(parking_);
    page.pane = nullptr;
    if (auto *next = current(pane))
        activate(*next);
}
void Workspace::activate(Page &page) {
    auto *pane = page.pane;
    if (!pane || !page.content)
        return;
    active_ = pane;
    const QSignalBlocker block(pane->tabs);
    for (int i = 0; i < pane->tabs->count(); ++i)
        if (pane->tabs->tabData(i).value<quintptr>() == reinterpret_cast<quintptr>(&page))
            pane->tabs->setCurrentIndex(i);
    pane->stack->setCurrentWidget(page.host);
    // The splitter must respect the active document's usable width. Hidden pages
    // should neither widen a view pane nor allow a document to be clipped inside it.
    pane->setMinimumWidth(qobject_cast<QDialog *>(page.content.data())
                              ? std::max(320, page.host->minimumWidth())
                              : 200);
    page.host->show();
    page.content->show();
    if (pane->window) {
        pane->window->setWindowTitle(page.title);
        pane->window->show();
        pane->window->raise();
    }
}
void Workspace::update_title(Page &page) {
    page.title = page.content->windowTitle();
    if (auto *pane = page.pane) {
        for (int i = 0; i < pane->tabs->count(); ++i)
            if (pane->tabs->tabData(i).value<quintptr>() == reinterpret_cast<quintptr>(&page)) {
                pane->tabs->setTabText(i, page.title);
                pane->tabs->setTabToolTip(i, page.title);
            }
        if (pane->window && current(pane) == &page)
            pane->window->setWindowTitle(page.title);
    }
}
void Workspace::open(QDialog *content, bool wrap_in_scroll_area) {
    auto *page = find(content);
    if (!page) {
        auto entry = std::make_unique<Page>();
        page = entry.get();
        page->content = content;
        page->title = content->windowTitle();
        page->icon = workbench_icon(WorkbenchIcon::properties);
        content->setWindowFlags(Qt::Widget);
        content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
        if (wrap_in_scroll_area) {
            auto *scroll = new QScrollArea;
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->setWidgetResizable(true);
            scroll->setWidget(content);
            page->host = scroll;
        } else
            page->host = content;
        content->installEventFilter(this);
        connect(content, &QWidget::windowTitleChanged, this, [this, page] { update_title(*page); });
        connect(content, &QDialog::finished, this, [this, content] { close_page(content); });
        pages_.push_back(std::move(entry));
    }
    if (!page->pane)
        append(*page, main_pane());
    activate(*page);
}
void Workspace::tidy(Pane *pane) {
    if (!pane || pane->tabs->count())
        return;
    const auto main_count =
        std::count_if(panes_.begin(), panes_.end(), [](auto *p) { return !p->window; });
    if (!pane->window && main_count == 1)
        return;
    if (active_ == pane)
        active_ = nullptr;
    std::erase(panes_, pane);
    if (pane->window) {
        pane->window->removeEventFilter(this);
        pane->window->hide();
        pane->window->deleteLater();
    } else {
        auto *splitter = qobject_cast<QSplitter *>(pane->parentWidget());
        pane->hide();
        pane->setParent(parking_);
        pane->deleteLater();
        while (splitter && splitter != root_ && splitter->count() == 1) {
            auto *parent = qobject_cast<QSplitter *>(splitter->parentWidget());
            auto *remaining = splitter->widget(0);
            remaining->setParent(parking_);
            parent->replaceWidget(parent->indexOf(splitter), remaining);
            remaining->show();
            splitter->deleteLater();
            splitter = parent;
        }
    }
}
void Workspace::close_page(QWidget *content) {
    auto *page = find(content);
    if (!page || !page->pane)
        return;
    auto *source = page->pane;
    remove(*page);
    tidy(source);
}
void Workspace::move(Page &page, Pane *target, Qt::Edge edge, int insertion) {
    auto *source = page.pane;
    if (!target || (source == target && edge != Qt::Edge{} && target->tabs->count() == 1))
        return;
    if (source == target && edge == Qt::Edge{}) {
        int from = -1;
        for (int i = 0; i < target->tabs->count(); ++i)
            if (target->tabs->tabData(i).value<quintptr>() == reinterpret_cast<quintptr>(&page))
                from = i;
        if (from >= 0 && insertion >= 0) {
            if (insertion > from)
                --insertion;
            target->tabs->moveTab(from, std::clamp(insertion, 0, target->tabs->count() - 1));
        }
        activate(page);
        return;
    }
    if (edge != Qt::Edge{} && !target->window) {
        auto *splitter = qobject_cast<QSplitter *>(target->parentWidget());
        const auto orientation =
            (edge == Qt::LeftEdge || edge == Qt::RightEdge) ? Qt::Horizontal : Qt::Vertical;
        const bool before = edge == Qt::LeftEdge || edge == Qt::TopEdge;
        auto *next = make_pane();
        if (splitter->orientation() == orientation) {
            auto sizes = splitter->sizes();
            const int index = splitter->indexOf(target);
            const int half = std::max(1, sizes[index] / 2);
            sizes[index] = half;
            sizes.insert(index + (before ? 0 : 1), half);
            splitter->insertWidget(index + (before ? 0 : 1), next);
            splitter->setSizes(sizes);
        } else {
            auto *branch = new QSplitter(orientation);
            branch->setChildrenCollapsible(false);
            branch->setHandleWidth(6);
            splitter->replaceWidget(splitter->indexOf(target), branch);
            branch->addWidget(before ? next : target);
            branch->addWidget(before ? target : next);
            branch->setSizes({1, 1});
            target->show();
        }
        target = next;
    }
    remove(page);
    append(page, target, insertion);
    activate(page);
    if (source != target)
        tidy(source);
}
void Workspace::split_page(QWidget *content, QWidget *beside, Qt::Edge edge) {
    auto *page = find(content);
    auto *other = find(beside);
    if (page && other && other->pane)
        move(*page, other->pane, edge);
}
void Workspace::merge_page(QWidget *content, QWidget *beside) {
    auto *page = find(content);
    auto *other = find(beside);
    if (page && other && other->pane)
        move(*page, other->pane);
}
void Workspace::detach_page(QWidget *content) {
    auto *page = find(content);
    if (!page || !page->pane || page->pane->window)
        return;
    auto *window = new QDialog(this, Qt::Window);
    window->setAttribute(Qt::WA_QuitOnClose, false);
    window->setMinimumSize(320, 320);
    auto *layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *pane = make_pane(window);
    layout->addWidget(pane);
    window->installEventFilter(this);
    move(*page, pane);
    // Native view reparenting can recreate the top-level platform window. Apply
    // its initial size after the page has been attached to its new group.
    window->resize(
        QSize(980, 680).boundedTo(window->screen()->availableGeometry().size() - QSize(40, 80)));
}
void Workspace::dock_page(QWidget *content) {
    auto *page = find(content);
    if (page && page->pane && page->pane->window)
        move(*page, main_pane());
}
bool Workspace::contains(QWidget *content) const {
    const auto *page = find(content);
    return page && page->pane;
}
bool Workspace::is_floating(QWidget *content) const {
    const auto *page = find(content);
    return page && page->pane && page->pane->window;
}
QDialog *Workspace::current_document() const {
    const auto *page = current(active_);
    return page ? qobject_cast<QDialog *>(page->content.data()) : nullptr;
}
void Workspace::context_menu(Pane *pane, int tab, const QPoint &global) {
    auto *page = reinterpret_cast<Page *>(pane->tabs->tabData(tab).value<quintptr>());
    if (!page)
        return;
    QMenu menu;
    auto *right = menu.addAction(QStringLiteral("向右分屏"));
    auto *below = menu.addAction(QStringLiteral("向下分屏"));
    right->setEnabled(!pane->window && pane->tabs->count() > 1);
    below->setEnabled(right->isEnabled());
    auto *merge = menu.addAction(QStringLiteral("合并到其他标签组"));
    auto *other = main_pane();
    for (auto *candidate : panes_)
        if (candidate != pane && !candidate->window) {
            other = candidate;
            break;
        }
    merge->setEnabled(other != pane);
    menu.addSeparator();
    auto *detach = menu.addAction(pane->window ? QStringLiteral("放回工作区")
                                               : QStringLiteral("在独立窗口打开"));
    auto *close = menu.addAction(QStringLiteral("关闭页面"));
    auto *chosen = menu.exec(global);
    if (chosen == right || chosen == below)
        move(*page, pane, chosen == right ? Qt::RightEdge : Qt::BottomEdge);
    else if (chosen == merge)
        move(*page, other);
    else if (chosen == detach) {
        if (pane->window)
            dock_page(page->content);
        else
            detach_page(page->content);
    } else if (chosen == close)
        close_page(page->content);
}
Workspace::DropTarget Workspace::drop_target(const QPoint &global) const {
    for (int floating : {1, 0})
        for (auto *pane : panes_) {
            if (!pane->isVisible() || bool(pane->window) != bool(floating))
                continue;
            const auto point = pane->mapFromGlobal(global);
            if (!pane->rect().contains(point))
                continue;
            DropTarget target{pane};
            if (point.y() < 34) {
                const auto tab_point = pane->tabs->mapFromGlobal(global);
                const int index = pane->tabs->tabAt(tab_point);
                target.insertion =
                    index < 0 ? pane->tabs->count()
                              : index + (tab_point.x() > pane->tabs->tabRect(index).center().x());
            } else if (!pane->window) {
                const int x_margin = std::min(80, pane->width() / 4);
                const int y_margin = std::min(80, pane->height() / 4);
                if (point.x() < x_margin)
                    target.edge = Qt::LeftEdge;
                else if (point.x() >= pane->width() - x_margin)
                    target.edge = Qt::RightEdge;
                else if (point.y() < 34 + y_margin)
                    target.edge = Qt::TopEdge;
                else if (point.y() >= pane->height() - y_margin)
                    target.edge = Qt::BottomEdge;
            }
            return target;
        }
    return {};
}
void Workspace::drag_to(Page *page, const QPoint &global) {
    if (!page)
        return;
    const auto target = drop_target(global);
    if (target.pane) {
        QRect rect(target.pane->mapToGlobal(QPoint{0, 0}), target.pane->size());
        if (target.insertion >= 0) {
            const auto *tabs = target.pane->tabs;
            const int x = target.insertion < tabs->count()
                              ? tabs->tabRect(target.insertion).left()
                              : tabs->tabRect(tabs->count() - 1).right() + 1;
            rect = QRect(tabs->mapToGlobal(QPoint{x, 1}), QSize(5, 32));
        } else if (target.edge == Qt::LeftEdge)
            rect.setWidth(rect.width() / 2);
        else if (target.edge == Qt::RightEdge)
            rect.setLeft(rect.center().x());
        else if (target.edge == Qt::TopEdge)
            rect.setHeight(rect.height() / 2);
        else if (target.edge == Qt::BottomEdge)
            rect.setTop(rect.center().y());
        drop_hint_->setGeometry(rect.adjusted(1, 1, -1, -1));
        drop_hint_->show();
        drop_hint_->raise();
    } else {
        drop_hint_->hide();
    }
    drag_label_->setText(page->title);
    drag_label_->adjustSize();
    drag_label_->move(global + QPoint(16, 18));
    drag_label_->show();
    drag_label_->raise();
}
void Workspace::finish_drag(Page *page, const QPoint &global, bool cancelled) {
    drop_hint_->hide();
    drag_label_->hide();
    if (!page || cancelled)
        return;
    const auto target = drop_target(global);
    if (target.pane) {
        move(*page, target.pane, target.edge, target.insertion);
    } else if (!rect().contains(mapFromGlobal(global))) {
        detach_page(page->content);
        if (page->pane && page->pane->window) {
            auto *screen = QGuiApplication::screenAt(global);
            if (!screen)
                screen = this->screen();
            const auto area = screen->availableGeometry();
            auto *window = page->pane->window;
            window->move(std::clamp(global.x() - 40, area.left(),
                                    std::max(area.left(), area.right() - window->width())),
                         std::clamp(global.y() - 16, area.top(),
                                    std::max(area.top(), area.bottom() - window->height())));
        }
    }
}
bool Workspace::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::Close) {
        for (const auto &page : pages_)
            if (page->content == object) {
                close_page(page->content);
                event->ignore();
                return true;
            }
        for (auto *pane : panes_)
            if (pane->window == object) {
                std::vector<QWidget *> closing;
                for (const auto &page : pages_)
                    if (page->pane == pane)
                        closing.push_back(page->content);
                for (auto *content : closing)
                    close_page(content);
                event->ignore();
                return true;
            }
    }
    return QWidget::eventFilter(object, event);
}
} // namespace orbitforge::desktop
