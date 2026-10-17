#include "report_selection.hpp"
#include "workbench_theme.hpp"
#include <QDialogButtonBox>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTextLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>
#include <algorithm>

namespace orbitforge::desktop {
namespace {
constexpr int subtitle_role = Qt::UserRole + 1;
constexpr int unavailable_role = Qt::UserRole + 2;
class PreviewTitle final : public QLabel {
  public:
    using QLabel::QLabel;
    QSize sizeHint() const override { return {220, fontMetrics().height()}; }
    QSize minimumSizeHint() const override { return {0, fontMetrics().height()}; }
    int heightForWidth(int width) const override {
        QTextLayout text_layout(text().simplified(), font());
        arrange(text_layout, width);
        return qCeil(text_layout.boundingRect().height());
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        const auto display = text().simplified();
        QTextLayout text_layout(display, font());
        arrange(text_layout, contentsRect().width());
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        for (int row = 0; row < text_layout.lineCount(); ++row) {
            const auto line = text_layout.lineAt(row);
            if (row == 1 && line.textStart() + line.textLength() < display.size())
                painter.drawText(
                    QPointF(contentsRect().left(), contentsRect().top() + line.y() + line.ascent()),
                    fontMetrics().elidedText(display.mid(line.textStart()), Qt::ElideRight,
                                             contentsRect().width()));
            else
                line.draw(&painter, contentsRect().topLeft());
        }
    }

  private:
    static void arrange(QTextLayout &text, int width) {
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        text.setTextOption(option);
        text.beginLayout();
        qreal y = 0;
        for (int row = 0; row < 2; ++row) {
            auto line = text.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(std::max(1, width));
            line.setPosition({0, y});
            y += line.height();
        }
        text.endLayout();
    }
};
class ReportChoiceDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
        return {120, option.fontMetrics.height() * 2 + 24};
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect row = option.rect.adjusted(2, 2, -2, -2);
        const bool selected = option.state & QStyle::State_Selected;
        const bool unavailable = index.data(unavailable_role).toBool();
        paint_list_row(painter, option, QRectF(row), theme::color(theme::accent));
        const auto icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        icon.paint(painter, QRect(row.left() + 12, row.center().y() - 10, 20, 20), Qt::AlignCenter,
                   unavailable ? QIcon::Disabled : QIcon::Normal);
        QFont name_font = option.font;
        painter->setFont(name_font);
        painter->setPen(theme::color(unavailable ? theme::text_faint
                                     : selected  ? theme::accent_bright
                                                 : theme::text));
        const int x = row.left() + 45, available = std::max(0, row.width() - 64);
        const QFontMetrics name_metrics(name_font);
        painter->drawText(
            QRect(x, row.top() + 9, available, name_metrics.height()), Qt::AlignVCenter,
            name_metrics.elidedText(index.data().toString(), Qt::ElideRight, available));
        QFont subtitle_font = option.font;
        subtitle_font.setPixelSize(std::max(11, option.font.pixelSize() - 1));
        painter->setFont(subtitle_font);
        painter->setPen(theme::color(theme::text_muted));
        const QFontMetrics subtitle_metrics(subtitle_font);
        painter->drawText(
            QRect(x, row.top() + 12 + name_metrics.height(), available, subtitle_metrics.height()),
            Qt::AlignVCenter,
            subtitle_metrics.elidedText(index.data(subtitle_role).toString(), Qt::ElideRight,
                                        available));
        painter->restore();
    }
};
} // namespace
ReportSelectionDialog::ReportSelectionDialog(std::vector<ReportChoice> choices, QString selected,
                                             QWidget *parent)
    : QDialog(parent) {
    setObjectName("reportSelectionDialog");
    setWindowTitle(QStringLiteral("选择报告"));
    setWindowIcon(workbench_icon(WorkbenchIcon::properties));
    resize(840, 580);
    setMinimumSize(360, 440);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme::space::page);
    layout->setSpacing(16);
    auto *heading_layout = new QVBoxLayout;
    heading_layout->setSpacing(6);
    auto *heading = new QLabel(QStringLiteral("选择报告"), this);
    heading->setObjectName("reportSelectionHeading");
    heading_layout->addWidget(heading);
    auto *subtitle = new QLabel(QStringLiteral("查找报告，预览内容与适用范围"), this);
    subtitle->setObjectName("reportSelectionSubtitle");
    heading_layout->addWidget(subtitle);
    layout->addLayout(heading_layout);
    split_ = new QSplitter(this);
    split_->setObjectName("reportSelectionSplit");
    split_->setHandleWidth(20);
    split_->setChildrenCollapsible(false);
    auto *navigation = new QWidget(split_);
    navigation->setMinimumWidth(0);
    auto *navigation_layout = new QVBoxLayout(navigation);
    navigation_layout->setContentsMargins(0, 0, 0, 0);
    navigation_layout->setSpacing(10);
    search_ = new QLineEdit(this);
    search_->setObjectName("reportSelectionSearch");
    search_->setPlaceholderText(QStringLiteral("搜索名称、内容或对象"));
    search_->setAccessibleName(QStringLiteral("搜索报告"));
    search_->setClearButtonEnabled(true);
    search_->addAction(workbench_icon(WorkbenchIcon::search), QLineEdit::LeadingPosition);
    navigation_layout->addWidget(search_);
    source_ = new QTabBar(this);
    source_->setObjectName("reportSelectionSource");
    source_->setAccessibleName(QStringLiteral("报告来源"));
    source_->addTab(QStringLiteral("全部"));
    source_->addTab(QStringLiteral("内置报告"));
    source_->addTab(QStringLiteral("项目模板"));
    source_->setExpanding(false);
    source_->setDrawBase(false);
    navigation_layout->addWidget(source_);
    list_ = new QListWidget(navigation);
    list_->setObjectName("reportSelectionList");
    list_->setAccessibleName(QStringLiteral("报告列表"));
    list_->setTextElideMode(Qt::ElideRight);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setMinimumSize(0, 64);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setMouseTracking(true);
    list_->setItemDelegate(new ReportChoiceDelegate(list_));
    navigation_layout->addWidget(list_, 1);
    auto *empty_layout = new QVBoxLayout(list_->viewport());
    empty_layout->addStretch();
    empty_ = new QLabel(QStringLiteral("没有匹配的报告\n试试其他关键词或来源"), list_->viewport());
    empty_->setObjectName("reportSelectionEmpty");
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    empty_->setAttribute(Qt::WA_TransparentForMouseEvents);
    empty_layout->addWidget(empty_);
    empty_layout->addStretch();
    auto *scroll = new QScrollArea(split_);
    preview_scroll_ = scroll;
    scroll->setObjectName("reportSelectionPreview");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setMinimumSize(0, 100);
    auto *details = new QWidget(scroll);
    details->setObjectName("reportSelectionDetails");
    scroll->setWidget(details);
    auto *preview_layout = new QVBoxLayout(details);
    preview_layout->setContentsMargins(20, 20, 20, 20);
    preview_layout->setSpacing(12);
    const auto label = [&](const char *name) {
        auto *value = new QLabel(details);
        value->setObjectName(name);
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        value->setMinimumWidth(0);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        preview_layout->addWidget(value);
        return value;
    };
    auto *identity = new QHBoxLayout;
    symbol_ = new QLabel(details);
    symbol_->setObjectName("reportSelectionSymbol");
    symbol_->setFixedSize(36, 36);
    symbol_->setAlignment(Qt::AlignCenter);
    identity->addWidget(symbol_);
    identity->setSpacing(12);
    auto *identity_text = new QVBoxLayout;
    identity_text->setSpacing(4);
    category_ = new QLabel(details);
    category_->setObjectName("reportSelectionCategory");
    identity_text->addWidget(category_);
    title_ = new PreviewTitle(details);
    title_->setObjectName("reportSelectionTitle");
    title_->setTextFormat(Qt::PlainText);
    title_->setWordWrap(true);
    title_->setMinimumWidth(0);
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    identity_text->addWidget(title_);
    identity->addLayout(identity_text, 1);
    preview_layout->addLayout(identity);
    preview_layout->addSpacing(4);
    description_ = label("reportSelectionDescription");
    auto *divider = new QFrame(details);
    divider->setObjectName("reportSelectionDivider");
    divider->setFixedHeight(1);
    preview_layout->addSpacing(4);
    preview_layout->addWidget(divider);
    preview_layout->addSpacing(4);
    output_heading_ = label("reportSelectionFieldHeading");
    output_heading_->setText(QStringLiteral("输出形式"));
    output_ = label("reportSelectionOutput");
    preview_layout->addSpacing(4);
    scope_heading_ = label("reportSelectionFieldHeading");
    scope_heading_->setText(QStringLiteral("对象范围"));
    scope_ = label("reportSelectionScope");
    note_ = label("reportSelectionNote");
    warning_ = label("reportSelectionWarning");
    preview_layout->addStretch();
    split_->setSizes({340, 432});
    split_->setStretchFactor(0, 1);
    split_->setStretchFactor(1, 1);
    layout->addWidget(split_, 1);
    auto *footer = new QHBoxLayout;
    count_ = new QLabel(this);
    count_->setObjectName("reportSelectionCount");
    footer->addWidget(count_);
    footer->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    confirm_ = buttons->button(QDialogButtonBox::Ok);
    confirm_->setObjectName("reportSelectionConfirm");
    confirm_->setText(QStringLiteral("使用此报告"));
    confirm_->setProperty("primary", true);
    confirm_->setDefault(true);
    // QDialogButtonBox polishes standard buttons before their role/name is assigned here.
    confirm_->style()->unpolish(confirm_);
    confirm_->style()->polish(confirm_);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
    footer->addWidget(buttons);
    layout->addLayout(footer);
    connect(buttons, &QDialogButtonBox::accepted, this, &ReportSelectionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(search_, &QLineEdit::textChanged, this, [this] { filter(); });
    connect(source_, &QTabBar::currentChanged, this, [this] { filter(); });
    connect(list_, &QListWidget::currentRowChanged, this, [this] { preview(); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { accept(); });
    set_choices(std::move(choices));
    for (int row = 0; row < list_->count(); ++row)
        if (list_->item(row)->data(Qt::UserRole).toString() == selected)
            list_->setCurrentRow(row);
    search_->setFocus();
}

void ReportSelectionDialog::set_choices(std::vector<ReportChoice> choices) {
    const auto selected = selection();
    choices_ = std::move(choices);
    {
        const QSignalBlocker guard(list_);
        list_->clear();
        for (const auto &choice : choices_) {
            auto *item = new QListWidgetItem(
                workbench_icon(choice.icon, theme::color(theme::accent)), choice.name, list_);
            item->setData(Qt::UserRole, choice.key);
            const auto subtitle = choice.saved ? choice.kind + QStringLiteral(" · ") + choice.scope
                                  : choice.output.isEmpty() ? choice.kind
                                                            : choice.output;
            item->setData(subtitle_role, subtitle);
            item->setData(unavailable_role, !choice.unavailable_reason.isEmpty());
            item->setData(Qt::AccessibleDescriptionRole,
                          subtitle + " " + choice.unavailable_reason);
            item->setToolTip(choice.name + "\n" + subtitle + "\n" + choice.scope +
                             (choice.unavailable_reason.isEmpty()
                                  ? QString{}
                                  : "\n" + choice.unavailable_reason));
            if (choice.key == selected)
                list_->setCurrentItem(item);
        }
    }
    filter();
}
QString ReportSelectionDialog::selection() const {
    const auto *item = list_->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString{};
}
void ReportSelectionDialog::filter() {
    const auto query = search_->text().trimmed();
    int visible = 0;
    for (int row = 0; row < list_->count(); ++row) {
        const auto &choice = choices_.at(static_cast<std::size_t>(row));
        const auto searchable = choice.name + " " + choice.kind + " " + choice.description + " " +
                                choice.scope + " " + choice.output;
        const bool matches =
            searchable.contains(query, Qt::CaseInsensitive) &&
            (source_->currentIndex() == 0 || choice.saved == (source_->currentIndex() == 2));
        list_->item(row)->setHidden(!matches);
        visible += matches ? 1 : 0;
    }
    // Filtering must never accept an invisible old selection on Enter.
    if (!list_->currentItem() || list_->currentItem()->isHidden())
        list_->setCurrentRow(-1);
    count_->setText(QStringLiteral("%1 项报告").arg(visible));
    empty_->setVisible(visible == 0);
    preview();
}
void ReportSelectionDialog::preview() {
    const auto found = std::ranges::find(choices_, selection(), &ReportChoice::key);
    const bool valid =
        found != choices_.end() && list_->currentItem() && !list_->currentItem()->isHidden();
    title_->setText(valid ? found->name : QStringLiteral("选择一项报告"));
    title_->setToolTip(title_->text());
    category_->setText(
        valid ? (found->saved ? QStringLiteral("项目模板") : QStringLiteral("内置报告"))
              : QStringLiteral("报告预览"));
    symbol_->setPixmap(workbench_icon(valid ? found->icon : WorkbenchIcon::search,
                                      theme::color(theme::accent_bright))
                           .pixmap(24, 24));
    description_->setText(valid ? found->description
                                : QStringLiteral("从列表选择报告，查看它的内容与适用对象。"));
    output_->setText(valid ? (found->output.isEmpty() ? found->kind : found->output) : QString{});
    output_heading_->setVisible(valid);
    scope_heading_->setVisible(valid);
    scope_->setText(valid ? found->scope : QString{});
    note_->setText(valid ? found->note : QString{});
    note_->setVisible(valid && !found->note.isEmpty());
    warning_->setText(valid ? found->unavailable_reason : QString{});
    warning_->setVisible(valid && !found->unavailable_reason.isEmpty());
    confirm_->setEnabled(valid && found->unavailable_reason.isEmpty());
    preview_scroll_->verticalScrollBar()->setValue(0);
}
void ReportSelectionDialog::accept() {
    if (confirm_->isEnabled())
        QDialog::accept();
}
void ReportSelectionDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (!split_)
        return;
    const auto orientation = event->size().width() < 640 ? Qt::Vertical : Qt::Horizontal;
    const int margin = orientation == Qt::Vertical ? 16 : 24;
    layout()->setContentsMargins(margin, 20, margin, 16);
    if (split_->orientation() != orientation) {
        (split_->orientation() == Qt::Horizontal ? wide_sizes_ : narrow_sizes_) = split_->sizes();
        split_->setOrientation(orientation);
        const auto &saved = orientation == Qt::Horizontal ? wide_sizes_ : narrow_sizes_;
        split_->setSizes(saved.isEmpty() ? QList<int>{2, 1} : saved);
    }
    QTimer::singleShot(0, this, [this] {
        if (list_->currentItem())
            list_->scrollToItem(list_->currentItem());
    });
}
} // namespace orbitforge::desktop
