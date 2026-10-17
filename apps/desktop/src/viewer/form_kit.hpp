#pragma once
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <vector>

// Inspector-style building blocks shared by parameter windows (properties, HPOP, sensors,
// stations, scenario settings). The conventions keep long forms calm and scannable:
//   * a group is a titled section whose header row carries the title, a muted one-line
//     summary, an info marker holding explanations, and optionally a collapse chevron;
//   * explanations live behind the info marker; only limitations and requirements stay on
//     the page, as cautions;
//   * label columns are aligned across every group of a page (align_form_labels);
//   * optional models are switched on from a toggle-styled check box (make_switch).
namespace orbitforge::desktop {
// A small "i" button whose tooltip and accessible description hold the explanation; a
// click shows it immediately so it also works without hovering.
inline QToolButton *info_marker(QWidget *parent = nullptr) {
    auto *marker = new QToolButton(parent);
    marker->setObjectName("infoMarker");
    marker->setIcon(workbench_icon(WorkbenchIcon::info, theme::color(theme::text_muted)));
    marker->setIconSize(QSize(14, 14));
    marker->setFixedSize(20, 20);
    marker->setAutoRaise(true);
    marker->setFocusPolicy(Qt::TabFocus);
    marker->setAccessibleName(QStringLiteral("说明"));
    QObject::connect(marker, &QToolButton::clicked, marker, [marker] {
        QToolTip::showText(marker->mapToGlobal(QPoint(0, marker->height())), marker->toolTip(),
                           marker);
    });
    marker->hide();
    return marker;
}
inline void append_help(QToolButton *marker, const QString &text) {
    const auto previous = marker->property("helpText").toStringList();
    auto paragraphs = previous;
    paragraphs.append(text);
    marker->setProperty("helpText", paragraphs);
    QString html = QStringLiteral("<div style='max-width:360px'>");
    for (const auto &paragraph : paragraphs)
        html += QStringLiteral("<p style='margin:0 0 6px 0'>%1</p>").arg(paragraph.toHtmlEscaped());
    marker->setToolTip(html + QStringLiteral("</div>"));
    marker->setAccessibleDescription(paragraphs.join('\n'));
    marker->show();
}
// A persistent limitation or requirement line: a quiet callout with a warm leading rule.
inline QLabel *caution_label(const QString &text) {
    auto *label = new QLabel(text);
    label->setObjectName("caution");
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}
// Check boxes that switch a whole model on or off are drawn as switches.
inline void make_switch(QCheckBox *box) {
    box->setProperty("switch", true);
    box->style()->unpolish(box);
    box->style()->polish(box);
}

class FormGroup final : public QFrame {
  public:
    explicit FormGroup(const QString &title, const char *object_name = nullptr,
                       QWidget *parent = nullptr)
        : QFrame(parent) {
        if (object_name)
            setObjectName(object_name);
        setProperty("formGroup", true);
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, theme::space::md, 0, theme::space::md);
        outer->setSpacing(theme::space::sm);
        header_ = new QWidget;
        header_->setObjectName("formGroupHeader");
        auto *row = new QHBoxLayout(header_);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        chevron_ = new QToolButton;
        chevron_->setObjectName("groupChevron");
        chevron_->setAutoRaise(true);
        chevron_->setFixedSize(18, 18);
        chevron_->setIconSize(QSize(12, 12));
        chevron_->hide();
        title_ = new QLabel(title);
        title_->setObjectName("sectionTitle");
        row->addWidget(title_);
        marker_ = info_marker();
        row->addWidget(marker_);
        row->addStretch(1);
        summary_ = new QLabel;
        summary_->setObjectName("groupSummary");
        summary_->setTextFormat(Qt::PlainText);
        summary_->hide();
        row->addWidget(summary_);
        // The chevron sits at the end so every group title keeps the same left edge.
        row->addWidget(chevron_);
        outer->addWidget(header_);
        body_ = new QWidget;
        form_ = new QFormLayout(body_);
        form_->setContentsMargins(0, 0, 0, 0);
        form_->setHorizontalSpacing(theme::space::md);
        form_->setVerticalSpacing(theme::space::row);
        form_->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form_->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        outer->addWidget(body_);
        QObject::connect(chevron_, &QToolButton::clicked, this,
                         [this] { set_expanded(!expanded()); });
        header_->installEventFilter(this);
    }
    [[nodiscard]] QFormLayout *form() const { return form_; }
    [[nodiscard]] QWidget *body() const { return body_; }
    [[nodiscard]] QLabel *title() const { return title_; }
    // Explanations collect behind the header's info marker.
    void add_help(const QString &text) { append_help(marker_, text); }
    // Limitations and data requirements stay visible at the end of the group.
    QLabel *add_caution(const QString &text) {
        auto *label = caution_label(text);
        form_->addRow(label);
        return label;
    }
    // A muted value summary on the right of the header, shown in either state.
    void set_summary(const QString &text) {
        summary_->setText(text);
        summary_->setVisible(!text.isEmpty());
    }
    // Rarely changed groups start folded; the header then reads as a one-line summary.
    void set_collapsible(bool expanded) {
        collapsible_ = true;
        chevron_->show();
        header_->setCursor(Qt::PointingHandCursor);
        set_expanded(expanded);
    }
    void set_expanded(bool expanded) {
        expanded_ = expanded || !collapsible_;
        body_->setVisible(expanded_);
        chevron_->setIcon(
            workbench_icon(expanded_ ? WorkbenchIcon::chevron_down : WorkbenchIcon::chevron_right,
                           theme::color(theme::text_muted)));
        chevron_->setToolTip(expanded_ ? QStringLiteral("收起") : QStringLiteral("展开"));
        chevron_->setAccessibleName(title_->text() + QStringLiteral(" · ") + chevron_->toolTip());
    }
    [[nodiscard]] bool expanded() const { return expanded_; }

  protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == header_ && collapsible_ && event->type() == QEvent::MouseButtonRelease &&
            static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            set_expanded(!expanded_);
            return true;
        }
        return QFrame::eventFilter(object, event);
    }

  private:
    QWidget *header_ = nullptr, *body_ = nullptr;
    QToolButton *chevron_ = nullptr, *marker_ = nullptr;
    QLabel *title_ = nullptr, *summary_ = nullptr;
    QFormLayout *form_ = nullptr;
    bool collapsible_ = false, expanded_ = true;
};

// Expands every folded group that contains `field`, e.g. before focusing a validation error.
inline void reveal(QWidget *field) {
    for (auto *parent = field ? field->parentWidget() : nullptr; parent;
         parent = parent->parentWidget())
        if (auto *group = dynamic_cast<FormGroup *>(parent))
            group->set_expanded(true);
}

// Gives every side-label in `root` the same width (capped at `maximum`), so field edges line
// up across all groups of a page. Wrapped rows (label above field) are unaffected. A group
// that opens its page drops the top rule, which only separates it from content above.
inline void align_form_labels(QWidget *root, int maximum = 120) {
    for (auto *form : root->findChildren<QFormLayout *>())
        if (form->rowCount() > 0)
            if (auto *item = form->itemAt(0, QFormLayout::SpanningRole); item && item->widget())
                if (auto *group = dynamic_cast<FormGroup *>(item->widget())) {
                    group->setProperty("leading", true);
                    group->style()->unpolish(group);
                    group->style()->polish(group);
                }
    std::vector<QWidget *> labels;
    int width = 0;
    for (auto *form : root->findChildren<QFormLayout *>())
        for (int row = 0; row < form->rowCount(); ++row)
            if (auto *item = form->itemAt(row, QFormLayout::LabelRole); item && item->widget()) {
                labels.push_back(item->widget());
                width = std::max(width, item->widget()->sizeHint().width());
            }
    width = std::min(width, maximum);
    for (auto *label : labels)
        label->setMinimumWidth(width);
}
} // namespace orbitforge::desktop
