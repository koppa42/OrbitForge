#include "epoch_editor.hpp"
#include "workbench_spin_box.hpp"
#include "workbench_style.hpp"
#include <QCalendarWidget>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QTextCharFormat>
#include <QTimeZone>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <orbitforge/runtime/time_frames.hpp>

namespace orbitforge::desktop {
namespace {
const QDateTime origin(QDate(2000, 1, 1), QTime(12, 0), QTimeZone::UTC);
QString scale_name(CalendarScale scale) {
    return scale == CalendarScale::utc ? "UTC" : scale == CalendarScale::tai ? "TAI" : "TT";
}
core::UtcDateTime calendar_parts(core::Epoch epoch, CalendarScale scale,
                                 const core::ITimeProvider &time) {
    if (scale == CalendarScale::utc)
        return time.to_utc(epoch);
    auto seconds = epoch.tai_nanoseconds() / 1'000'000'000;
    auto fraction = epoch.tai_nanoseconds() % 1'000'000'000;
    if (fraction < 0) {
        --seconds;
        fraction += 1'000'000'000;
    }
    if (scale == CalendarScale::tt) {
        seconds += 32;
        fraction += 184'000'000;
        if (fraction >= 1'000'000'000) {
            ++seconds;
            fraction -= 1'000'000'000;
        }
    }
    const auto value = origin.addSecs(seconds);
    return {value.date().year(),       value.date().month(),  value.date().day(),
            value.time().hour(),       value.time().minute(), value.time().second(),
            static_cast<int>(fraction)};
}
void validate_date(QDate date) {
    if (!date.isValid() || date < QDate(1972, 1, 1) || date > QDate(2250, 1, 1))
        throw std::out_of_range("日期应在 1972-01-01 至 2250-01-01 范围内。");
}
core::Epoch calendar_epoch(core::UtcDateTime p, CalendarScale scale,
                           const core::ITimeProvider &time) {
    const QDate date(p.year, p.month, p.day);
    validate_date(date);
    if (p.hour < 0 || p.hour > 23 || p.minute < 0 || p.minute > 59 || p.second < 0 ||
        p.second > 60 || p.nanosecond < 0 || p.nanosecond >= 1'000'000'000)
        throw std::invalid_argument("请输入有效的时分秒；UTC 允许有效闰秒 60。");
    if (scale == CalendarScale::utc)
        return time.from_utc(p);
    if (p.second >= 60)
        throw std::invalid_argument("TAI / TT 不含闰秒，秒应小于 60。");
    auto ms = origin.msecsTo(QDateTime(date, QTime(p.hour, p.minute, p.second), QTimeZone::UTC));
    if (scale == CalendarScale::tt)
        ms -= 32184;
    if (ms > (std::numeric_limits<std::int64_t>::max() - p.nanosecond) / 1'000'000 ||
        ms < std::numeric_limits<std::int64_t>::min() / 1'000'000)
        throw std::out_of_range("Calendar outside Epoch range");
    return core::Epoch{ms * 1'000'000 + p.nanosecond};
}
struct ParsedEpoch {
    core::Epoch value;
    CalendarScale scale;
};
ParsedEpoch parse_epoch(const QString &text, const core::ITimeProvider &time) {
    static const QRegularExpression format(
        QStringLiteral("^\\s*(\\d{4})-(\\d{2})-(\\d{2})[ "
                       "T](\\d{1,2}):(\\d{2}):(\\d{2})(?:\\.(\\d{1,9}))?\\s+(UTC|TAI|TT)\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = format.match(text);
    if (!match.hasMatch())
        throw std::invalid_argument("格式示例：2026-10-09 00:00:00 UTC，可附带最多 9 位小数秒。");
    const auto name = match.captured(8).toUpper();
    const auto scale = name == "UTC"   ? CalendarScale::utc
                       : name == "TAI" ? CalendarScale::tai
                                       : CalendarScale::tt;
    return {calendar_epoch({match.captured(1).toInt(), match.captured(2).toInt(),
                            match.captured(3).toInt(), match.captured(4).toInt(),
                            match.captured(5).toInt(), match.captured(6).toInt(),
                            match.captured(7).leftJustified(9, '0').toInt()},
                           scale, time),
            scale};
}
QString clock_text(const core::UtcDateTime &p) {
    return QString("%1:%2:%3")
        .arg(p.hour, 2, 10, QChar('0'))
        .arg(p.minute, 2, 10, QChar('0'))
        .arg(p.second, 2, 10, QChar('0'));
}
QString editor_text(core::UtcDateTime p, CalendarScale scale) {
    QString fraction;
    if (p.nanosecond) {
        fraction = QString(".%1").arg(p.nanosecond, 9, 10, QChar('0'));
        while (fraction.endsWith('0'))
            fraction.chop(1);
    }
    return QDate(p.year, p.month, p.day).toString("yyyy-MM-dd") + " " + clock_text(p) + fraction +
           " " + scale_name(scale);
}
void show_popup(QWidget *popup, QWidget *anchor) {
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->adjustSize();
    auto position = anchor->mapToGlobal(QPoint(0, anchor->height() + 6));
    const auto bounds = anchor->screen()->availableGeometry();
    if (position.y() + popup->height() > bounds.bottom())
        position.setY(anchor->mapToGlobal(QPoint(0, 0)).y() - popup->height() - 6);
    position.setX(std::clamp(position.x(), bounds.left(),
                             std::max(bounds.left(), bounds.right() - popup->width())));
    position.setY(std::max(position.y(), bounds.top()));
    popup->move(position);
    popup->show();
}
} // namespace
QString calendar_text(core::Epoch epoch, CalendarScale scale, bool date,
                      const core::ITimeProvider *time) {
    static const auto builtin = runtime::make_builtin_time_provider();
    if (!time)
        time = builtin.get();
    const auto p = calendar_parts(epoch, scale, *time);
    return date ? QDate(p.year, p.month, p.day).toString("yyyy-MM-dd") + "  " + clock_text(p) +
                      QString(".%1").arg(p.nanosecond / 1'000'000, 3, 10, QChar('0'))
                : clock_text(p);
}
EpochEditor::EpochEditor(QWidget *parent, std::shared_ptr<const core::ITimeProvider> time)
    : QWidget(parent), time_(time ? std::move(time) : runtime::make_builtin_time_provider()) {
    setMaximumWidth(380);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *box = new QFrame;
    box->setObjectName("epochField");
    auto *row = new QHBoxLayout(box);
    row->setContentsMargins(0, 0, 4, 0);
    row->setSpacing(0);
    value_ = new QLineEdit;
    value_->installEventFilter(this);
    value_->setObjectName("epochValue");
    value_->setMinimumWidth(180);
    value_->setAccessibleName(QStringLiteral("历元，日期、时间和 UTC / TAI / TT"));
    value_->setPlaceholderText("yyyy-MM-dd HH:mm:ss UTC");
    value_->setToolTip(QStringLiteral("可直接输入或粘贴完整历元，支持 UTC / TAI / TT 和小数秒。"));
    row->addWidget(value_, 1);
    picker_button_ = new QToolButton;
    picker_button_->setObjectName("openEpochPicker");
    picker_button_->setIcon(workbench_icon(WorkbenchIcon::calendar));
    picker_button_->setToolTip(QStringLiteral("选择日期、时间和时间标准"));
    picker_button_->setFixedSize(24, 24);
    row->addWidget(picker_button_);
    layout->addWidget(box);
    error_ = new QLabel;
    error_->setObjectName("inputError");
    error_->setWordWrap(true);
    error_->hide();
    layout->addWidget(error_);
    connect(value_, &QLineEdit::textChanged, this, [this] {
        if (!updating_) {
            dirty_ = true;
            try {
                current_scale_ = parse_epoch(value_->text(), *time_).scale;
            } catch (const std::exception &) {
                // Incomplete typing keeps the last valid display scale; epoch() validates on use.
            }
        }
    });
    connect(picker_button_, &QToolButton::clicked, this, &EpochEditor::open_picker);
    set_epoch(core::Epoch{0});
}
bool EpochEditor::eventFilter(QObject *watched, QEvent *event) {
    if (watched == value_ &&
        (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        auto *field = value_->parentWidget();
        field->setProperty("focused", event->type() == QEvent::FocusIn);
        field->style()->unpolish(field);
        field->style()->polish(field);
        field->update();
    }
    return QWidget::eventFilter(watched, event);
}
void EpochEditor::set_time_provider(std::shared_ptr<const core::ITimeProvider> time) {
    if (!time)
        throw std::invalid_argument("Missing time provider");
    if (time_->data_info().sha256 == time->data_info().sha256)
        return;
    const auto value = stored_;
    time_ = std::move(time);
    set_epoch(value);
}
core::Epoch EpochEditor::epoch() const {
    const auto value = dirty_ ? parse_epoch(value_->text(), *time_).value : stored_;
    if ((first_ && value.tai_nanoseconds() < first_->tai_nanoseconds()) ||
        (last_ && value.tai_nanoseconds() > last_->tai_nanoseconds()))
        throw std::out_of_range("时间超出可用的分析或数据时段。");
    return value;
}
void EpochEditor::set_epoch(core::Epoch epoch) {
    if ((first_ && epoch.tai_nanoseconds() < first_->tai_nanoseconds()) ||
        (last_ && epoch.tai_nanoseconds() > last_->tai_nanoseconds()))
        throw std::out_of_range("时间超出可用的分析或数据时段。");
    const auto p = calendar_parts(epoch, current_scale_, *time_);
    validate_date(QDate(p.year, p.month, p.day));
    updating_ = true;
    value_->setText(editor_text(p, current_scale_));
    updating_ = false;
    dirty_ = false;
    stored_ = epoch;
    error_->clear();
    error_->hide();
}
void EpochEditor::set_scale(CalendarScale scale) {
    const auto previous = current_scale_;
    try {
        const auto value = epoch();
        current_scale_ = scale;
        set_epoch(value);
    } catch (const std::exception &exception) {
        current_scale_ = previous;
        error_->setText(QString::fromUtf8(exception.what()));
        error_->show();
    }
}
void EpochEditor::set_range(core::Epoch first, core::Epoch last) {
    if (first.tai_nanoseconds() > last.tai_nanoseconds())
        throw std::invalid_argument("Invalid editor time range");
    first_ = first;
    last_ = last;
}
void EpochEditor::set_read_only(bool value) {
    value_->setReadOnly(value);
    value_->parentWidget()->setProperty("readOnly", value);
    value_->parentWidget()->style()->unpolish(value_->parentWidget());
    value_->parentWidget()->style()->polish(value_->parentWidget());
    picker_button_->setVisible(!value);
}
void EpochEditor::adjust_seconds(int seconds) {
    try {
        const auto value = epoch();
        const auto offset = static_cast<std::int64_t>(seconds) * 1'000'000'000;
        if ((offset > 0 &&
             value.tai_nanoseconds() > std::numeric_limits<std::int64_t>::max() - offset) ||
            (offset < 0 &&
             value.tai_nanoseconds() < std::numeric_limits<std::int64_t>::min() - offset))
            throw std::out_of_range("Time adjustment exceeds Epoch range");
        set_epoch(core::Epoch{value.tai_nanoseconds() + offset});
    } catch (const std::exception &exception) {
        error_->setText(QString::fromUtf8(exception.what()));
        error_->show();
    }
}
void EpochEditor::open_picker() {
    if (value_->isReadOnly())
        return;
    ParsedEpoch initial{stored_, current_scale_};
    try {
        initial = parse_epoch(value_->text(), *time_);
    } catch (const std::exception &) { /* Picker can replace a malformed text draft. */
    }
    auto *popup = new QFrame(this, Qt::Popup);
    popup->setObjectName("epochPopup");
    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(theme::space::card);
    layout->setSpacing(8);
    auto *header = new QHBoxLayout;
    header->addWidget(new QLabel(QStringLiteral("日期与时间")), 1);
    auto *scales = new QComboBox;
    scales->setObjectName("pickerScale");
    scales->addItems({"UTC", "TAI", "TT"});
    scales->setCurrentIndex(static_cast<int>(initial.scale));
    header->addWidget(scales);
    layout->addLayout(header);
    auto *calendar = new QCalendarWidget;
    calendar->setObjectName("epochCalendar");
    calendar->setMinimumSize(292, 220);
    calendar->setDateRange(QDate(1972, 1, 1), QDate(2250, 1, 1));
    calendar->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);
    calendar->setHorizontalHeaderFormat(QCalendarWidget::ShortDayNames);
    calendar->setFirstDayOfWeek(Qt::Monday);
    QTextCharFormat weekend;
    weekend.setForeground(theme::color(theme::sakura));
    calendar->setWeekdayTextFormat(Qt::Saturday, weekend);
    calendar->setWeekdayTextFormat(Qt::Sunday, weekend);
    layout->addWidget(calendar);
    auto *row = new QHBoxLayout;
    auto *hours = new WorkbenchSpinBox<QSpinBox>;
    auto *minutes = new WorkbenchSpinBox<QSpinBox>;
    auto *seconds = new WorkbenchSpinBox<QDoubleSpinBox>;
    hours->setObjectName("pickerHour");
    minutes->setObjectName("pickerMinute");
    seconds->setObjectName("pickerSecond");
    hours->setRange(0, 23);
    minutes->setRange(0, 59);
    seconds->setDecimals(3);
    hours->setMinimumWidth(68);
    minutes->setMinimumWidth(68);
    seconds->setMinimumWidth(106);
    const auto add = [&](const QString &label, QAbstractSpinBox *field) {
        field->setProperty("stepButtons", true);
        field->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
        field->setAccelerated(true);
        field->setAccessibleName(label);
        auto *column = new QVBoxLayout;
        auto *caption = new QLabel(label);
        caption->setObjectName("hint");
        column->addWidget(caption);
        column->addWidget(field);
        row->addLayout(column);
    };
    add(QStringLiteral("时"), hours);
    add(QStringLiteral("分"), minutes);
    add(QStringLiteral("秒"), seconds);
    layout->addLayout(row);
    const auto picker_scale = std::make_shared<CalendarScale>(initial.scale);
    const auto picker_value = std::make_shared<core::Epoch>(initial.value);
    const auto dirty = std::make_shared<bool>(false), updating = std::make_shared<bool>(false);
    const auto populate = [this, calendar, hours, minutes, seconds, picker_value, dirty,
                           updating](core::Epoch value, CalendarScale scale) {
        const auto p = calendar_parts(value, scale, *time_);
        validate_date(QDate(p.year, p.month, p.day));
        *updating = true;
        calendar->setSelectedDate(QDate(p.year, p.month, p.day));
        hours->setValue(p.hour);
        minutes->setValue(p.minute);
        seconds->setRange(0, scale == CalendarScale::utc ? 60.999 : 59.999);
        seconds->setValue(p.second + (p.nanosecond / 1'000'000) / 1000.0);
        *picker_value = value;
        *dirty = false;
        *updating = false;
    };
    populate(initial.value, initial.scale);
    const auto changed = [dirty, updating] {
        if (!*updating)
            *dirty = true;
    };
    connect(calendar, &QCalendarWidget::selectionChanged, popup, changed);
    connect(hours, &QSpinBox::valueChanged, popup, changed);
    connect(minutes, &QSpinBox::valueChanged, popup, changed);
    connect(seconds, &QDoubleSpinBox::valueChanged, popup, changed);
    const auto read_draft = [this, calendar, hours, minutes, seconds, picker_scale, picker_value,
                             dirty] {
        if (!*dirty)
            return *picker_value;
        const auto date = calendar->selectedDate();
        const auto ms = qRound64(seconds->value() * 1000);
        return calendar_epoch({date.year(), date.month(), date.day(), hours->value(),
                               minutes->value(), static_cast<int>(ms / 1000),
                               static_cast<int>(ms % 1000) * 1'000'000},
                              *picker_scale, *time_);
    };
    auto *popup_error = new QLabel;
    popup_error->setObjectName("inputError");
    popup_error->setWordWrap(true);
    popup_error->hide();
    layout->addWidget(popup_error);
    connect(scales, &QComboBox::currentIndexChanged, popup,
            [scales, picker_scale, read_draft, populate, popup_error](int index) {
                try {
                    const auto value = read_draft();
                    const auto next = static_cast<CalendarScale>(index);
                    populate(value, next);
                    *picker_scale = next;
                    popup_error->hide();
                } catch (const std::exception &exception) {
                    const QSignalBlocker block(scales);
                    scales->setCurrentIndex(static_cast<int>(*picker_scale));
                    popup_error->setText(QString::fromUtf8(exception.what()));
                    popup_error->show();
                }
            });
    auto *footer = new QHBoxLayout;
    footer->addStretch();
    auto *apply = new QPushButton(QStringLiteral("确定"));
    apply->setObjectName("applyOrbit");
    apply->setAutoDefault(false);
    footer->addWidget(apply);
    layout->addLayout(footer);
    const auto accept = [this, popup, read_draft, picker_scale, popup_error] {
        const auto previous = current_scale_;
        try {
            const auto value = read_draft();
            current_scale_ = *picker_scale;
            set_epoch(value);
            popup->close();
        } catch (const std::exception &exception) {
            current_scale_ = previous;
            popup_error->setText(QString::fromUtf8(exception.what()));
            popup_error->show();
        }
    };
    connect(apply, &QPushButton::clicked, popup, accept);
    connect(calendar, &QCalendarWidget::activated, popup, [accept](QDate) { accept(); });
    auto *enter = new QShortcut(QKeySequence(Qt::Key_Return), popup);
    enter->setContext(Qt::WidgetWithChildrenShortcut);
    connect(enter, &QShortcut::activated, popup, accept);
    show_popup(popup, value_->parentWidget());
}
} // namespace orbitforge::desktop
