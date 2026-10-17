#include "quantity_editor.hpp"
#include <QActionGroup>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace orbitforge::desktop {
namespace {
struct Definition {
    Unit unit, base;
    double factor;
    const char *symbol;
    const char *name;
};
constexpr std::array definitions{
    Definition{Unit::one, Unit::one, 1, "", "无量纲"},
    Definition{Unit::metre, Unit::metre, 1, "m", "米"},
    Definition{Unit::kilometre, Unit::metre, 1000, "km", "千米"},
    Definition{Unit::metres_per_second, Unit::metres_per_second, 1, "m/s", "米每秒"},
    Definition{Unit::kilometres_per_second, Unit::metres_per_second, 1000, "km/s", "千米每秒"},
    Definition{Unit::radian, Unit::radian, 1, "rad", "弧度"},
    Definition{Unit::degree, Unit::radian, std::numbers::pi / 180, "°", "度"},
    Definition{Unit::millisecond, Unit::second, 0.001, "ms", "毫秒"},
    Definition{Unit::second, Unit::second, 1, "s", "秒"},
    Definition{Unit::minute, Unit::second, 60, "min", "分钟"},
    Definition{Unit::hour, Unit::second, 3600, "h", "小时"},
    Definition{Unit::day, Unit::second, 86400, "d", "天（86400秒）"},
    Definition{Unit::metres_cubed_per_second_squared, Unit::metres_cubed_per_second_squared, 1,
               "m³/s²", "米³每秒²"},
    Definition{Unit::kilometres_cubed_per_second_squared, Unit::metres_cubed_per_second_squared,
               1e9, "km³/s²", "千米³每秒²"},
    Definition{Unit::square_metres_per_kilogram, Unit::square_metres_per_kilogram, 1, "m²/kg",
               "平方米每千克"},
    Definition{Unit::square_centimetres_per_gram, Unit::square_metres_per_kilogram, 0.1, "cm²/g",
               "平方厘米每克"},
    Definition{Unit::pascal, Unit::pascal, 1, "Pa", "帕斯卡（N/m²）"},
    Definition{Unit::kilogram, Unit::kilogram, 1, "kg", "千克"},
    Definition{Unit::gram, Unit::kilogram, .001, "g", "克"},
    Definition{Unit::tonne, Unit::kilogram, 1000, "t", "吨"}};
const Definition &definition(Unit unit) {
    const auto found = std::find_if(definitions.begin(), definitions.end(),
                                    [unit](const auto &item) { return item.unit == unit; });
    if (found == definitions.end())
        throw std::invalid_argument("Unknown quantity unit");
    return *found;
}
QString formatted(double value) {
    auto locale = QLocale::c();
    locale.setNumberOptions(QLocale::OmitGroupSeparator);
    return locale.toString(value == 0 ? 0 : value, 'g', 15);
}
std::out_of_range invalid_range(const QString &message) {
    return std::out_of_range(message.toStdString());
}
} // namespace
QString unit_symbol(Unit unit) { return QString::fromUtf8(definition(unit).symbol); }
std::vector<Unit> compatible_units(Unit unit) {
    const auto base = definition(unit).base;
    std::vector<Unit> result;
    for (const auto &item : definitions)
        if (item.base == base)
            result.push_back(item.unit);
    return result;
}
double convert_quantity(double value, Unit from, Unit to) {
    const auto &source = definition(from), &target = definition(to);
    if (source.base != target.base)
        throw std::invalid_argument("不能在不同物理量的单位之间转换。");
    if (!std::isfinite(value))
        throw std::invalid_argument("请输入有限数值，支持科学计数法。");
    const double result = value * (source.factor / target.factor);
    if (!std::isfinite(result) || (value != 0 && result == 0))
        throw std::out_of_range("换算结果超出数值精度范围。");
    return result;
}
QuantityEditor::QuantityEditor(Unit display_unit, QWidget *parent)
    : QFrame(parent), unit_(display_unit), base_unit_(definition(display_unit).base) {
    setProperty("quantityField", true);
    setMaximumWidth(276);
    setMinimumWidth(130);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *row = new QHBoxLayout(this);
    // Every single-line input is 28 px tall: the frame border plus the 26 px inner row.
    row->setContentsMargins(0, 0, 1, 0);
    row->setSpacing(0);
    input_ = new QLineEdit;
    input_->setObjectName("quantityValue");
    input_->setMinimumWidth(0);
    input_->setFrame(false);
    row->addWidget(input_, 1);
    // Visibility is configured before layout insertion; an unparented button would show
    // as a top-level window while the surrounding editor is still being constructed.
    unit_button_ = new QToolButton(this);
    unit_button_->setObjectName("quantityUnit");
    unit_button_->setPopupMode(QToolButton::InstantPopup);
    unit_button_->setFocusPolicy(Qt::StrongFocus);
    unit_button_->setToolTip(QStringLiteral("切换单位并换算数值"));
    menu_ = new QMenu(unit_button_);
    menu_->setObjectName("quantityUnitMenu");
    auto *group = new QActionGroup(menu_);
    for (const auto unit : compatible_units(unit_)) {
        const auto &item = definition(unit);
        auto *action = menu_->addAction(QString::fromUtf8(item.symbol) + "  ·  " +
                                        QString::fromUtf8(item.name));
        action->setCheckable(true);
        action->setData(static_cast<int>(unit));
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, unit] {
            try {
                set_unit(unit);
            } catch (const std::exception &error) {
                validate_text();
                QToolTip::showText(unit_button_->mapToGlobal(QPoint(0, unit_button_->height())),
                                   QString::fromUtf8(error.what()), this);
                for (auto *item : menu_->actions())
                    item->setChecked(item->data().toInt() == static_cast<int>(unit_));
            }
        });
    }
    unit_button_->setMenu(menu_);
    fit_unit_button();
    unit_button_->setVisible(unit_ != Unit::one);
    row->addWidget(unit_button_);
    setFocusProxy(input_);
    input_->installEventFilter(this);
    unit_button_->installEventFilter(this);
    connect(input_, &QLineEdit::textChanged, this, [this] {
        dirty_ = true;
        validate_text();
    });
    render();
}
double QuantityEditor::checked_value(double value) const {
    if (!std::isfinite(value))
        throw std::invalid_argument("请输入有限数值，支持科学计数法。");
    if (quantum_ > 0) {
        const double units = value / quantum_;
        const double nearest = std::round(units);
        if (!std::isfinite(units) ||
            std::abs(units - nearest) >
                16 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(units)))
            throw invalid_range(QStringLiteral("数值必须为 %1 %2 的整数倍。")
                                    .arg(formatted(quantum_), unit_symbol(base_unit_)));
        value = nearest * quantum_;
    }
    if (value < minimum_ || value > maximum_)
        throw invalid_range(QStringLiteral("数值范围为 %1 至 %2 %3。")
                                .arg(formatted(convert_quantity(minimum_, base_unit_, unit_)),
                                     formatted(convert_quantity(maximum_, base_unit_, unit_)),
                                     unit_symbol(unit_)));
    return value;
}
double QuantityEditor::base_value() const {
    if (!dirty_)
        return checked_value(stored_);
    auto locale = QLocale::c();
    locale.setNumberOptions(QLocale::RejectGroupSeparator);
    bool ok = false;
    const double entered = locale.toDouble(input_->text().trimmed(), &ok);
    if (!ok || !std::isfinite(entered))
        throw std::invalid_argument("请输入有限数值，支持科学计数法（例如 1.25e-3）。");
    return checked_value(convert_quantity(entered, unit_, base_unit_));
}
double QuantityEditor::value(Unit requested_unit) const {
    return convert_quantity(base_value(), base_unit_, requested_unit);
}
void QuantityEditor::validate_value(double value, Unit supplied_unit) const {
    const auto next = checked_value(convert_quantity(value, supplied_unit, base_unit_));
    (void)convert_quantity(next, base_unit_, unit_);
}
void QuantityEditor::set_value(double value, Unit supplied_unit) {
    const double next = checked_value(convert_quantity(value, supplied_unit, base_unit_));
    (void)convert_quantity(next, base_unit_, unit_);
    stored_ = next;
    render();
    validate_text();
}
void QuantityEditor::set_unit(Unit display_unit) {
    (void)convert_quantity(0, base_unit_, display_unit);
    if (unit_ == display_unit)
        return;
    const auto next = base_value();
    (void)convert_quantity(next, base_unit_, display_unit);
    stored_ = next;
    unit_ = display_unit;
    render();
    emit unitChanged(unit_);
}
void QuantityEditor::set_range(double minimum, double maximum, Unit unit) {
    const auto first = convert_quantity(minimum, unit, base_unit_);
    const auto last = convert_quantity(maximum, unit, base_unit_);
    if (last < first)
        throw std::invalid_argument("Invalid quantity range");
    minimum_ = first;
    maximum_ = last;
    validate_text();
}
void QuantityEditor::set_single_step(double step, Unit unit) {
    const auto converted = convert_quantity(step, unit, base_unit_);
    if (converted <= 0)
        throw std::invalid_argument("Quantity step must be positive");
    step_ = converted;
}
void QuantityEditor::set_quantum(double quantum, Unit unit) {
    const auto converted = convert_quantity(quantum, unit, base_unit_);
    if (converted < 0)
        throw std::invalid_argument("Quantity resolution cannot be negative");
    quantum_ = converted;
    validate_text();
}
void QuantityEditor::set_label(const QString &label) {
    label_ = label;
    setAccessibleName(label);
    input_->setAccessibleName(label + QStringLiteral("数值"));
    unit_button_->setAccessibleName(label + QStringLiteral("单位"));
}
void QuantityEditor::render() {
    const QSignalBlocker block(input_);
    input_->setText(formatted(convert_quantity(stored_, base_unit_, unit_)));
    dirty_ = false;
    unit_button_->setText(unit_symbol(unit_));
    for (auto *action : menu_->actions())
        action->setChecked(action->data().toInt() == static_cast<int>(unit_));
    setProperty("invalid", false);
    input_->setToolTip({});
    refresh_style();
}
void QuantityEditor::set_read_only(bool value) { input_->setReadOnly(value); }
void QuantityEditor::validate_text() {
    bool invalid = false;
    try {
        const auto current = base_value();
        input_->setToolTip({});
        if (current != last_notified_) {
            last_notified_ = current;
            emit valueChanged(current);
        }
    } catch (const std::exception &error) {
        invalid = true;
        input_->setToolTip(QString::fromUtf8(error.what()));
    }
    if (property("invalid").toBool() != invalid) {
        setProperty("invalid", invalid);
        refresh_style();
    }
}
void QuantityEditor::refresh_style() {
    style()->unpolish(this);
    style()->polish(this);
    update();
}
// Sized from the button's own (stylesheet) font so the longest compatible symbol is never
// clipped; refitted when the style or font changes after construction.
void QuantityEditor::fit_unit_button() {
    const auto metrics = unit_button_->fontMetrics();
    int widest = 0;
    for (const auto unit : compatible_units(unit_))
        widest = std::max(widest, metrics.horizontalAdvance(unit_symbol(unit)));
    // Stylesheet padding 7 + 17 (menu chevron), the 1 px divider and breathing room. A shared
    // minimum keeps the unit dividers of common units (s, m, km, m/s) in one column.
    unit_button_->setFixedWidth(std::max(60, widest + 7 + 17 + 1 + 8));
}
bool QuantityEditor::eventFilter(QObject *object, QEvent *event) {
    if (object == unit_button_ &&
        (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange ||
         event->type() == QEvent::Polish))
        fit_unit_button();
    if (object == input_ && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            if (input_->isReadOnly())
                return true;
            try {
                set_value(base_value() + (key == Qt::Key_Up ? step_ : -step_), base_unit_);
            } catch (const std::exception &) {
                validate_text();
            }
            return true;
        }
    }
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
        setProperty("focused", event->type() == QEvent::FocusIn);
        refresh_style();
    }
    return QFrame::eventFilter(object, event);
}
} // namespace orbitforge::desktop
