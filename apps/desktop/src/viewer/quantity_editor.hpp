#pragma once
#include <QFrame>
#include <limits>
#include <vector>

class QLineEdit;
class QToolButton;
class QMenu;
namespace orbitforge::desktop {
enum class Unit {
    one,
    metre,
    kilometre,
    metres_per_second,
    kilometres_per_second,
    radian,
    degree,
    millisecond,
    second,
    minute,
    hour,
    day,
    metres_cubed_per_second_squared,
    kilometres_cubed_per_second_squared,
    square_metres_per_kilogram,
    square_centimetres_per_gram,
    pascal,
    kilogram,
    gram,
    tonne
};
QString unit_symbol(Unit unit);
std::vector<Unit> compatible_units(Unit unit);
// Rejects incompatible dimensions, nonfinite input and conversion overflow.
double convert_quantity(double value, Unit from, Unit to);

class QuantityEditor final : public QFrame {
    Q_OBJECT
  public:
    explicit QuantityEditor(Unit display_unit, QWidget *parent = nullptr);
    [[nodiscard]] Unit unit() const { return unit_; }
    [[nodiscard]] double value(Unit requested_unit) const;
    void validate_value(double value, Unit supplied_unit) const;
    void set_value(double value, Unit supplied_unit);
    void set_unit(Unit display_unit);
    void set_range(double minimum, double maximum, Unit unit);
    void set_single_step(double step, Unit unit);
    // Optional physical resolution, e.g. whole seconds; zero disables quantization.
    void set_quantum(double quantum, Unit unit);
    void set_label(const QString &label);
    // A published constant can still be displayed in another compatible unit.
    void set_read_only(bool value);

  signals:
    // SI value (or dimensionless), only emitted for a valid physical value change.
    void valueChanged(double base_value);
    void unitChanged(orbitforge::desktop::Unit unit);

  protected:
    bool eventFilter(QObject *object, QEvent *event) override;

  private:
    double checked_value(double base_value) const;
    double base_value() const;
    void render();
    void validate_text();
    void refresh_style();
    void fit_unit_button();
    Unit unit_, base_unit_;
    double stored_ = 0, last_notified_ = 0;
    double minimum_ = -std::numeric_limits<double>::max();
    double maximum_ = std::numeric_limits<double>::max();
    double step_ = 1, quantum_ = 0;
    bool dirty_ = false;
    QLineEdit *input_ = nullptr;
    QToolButton *unit_button_ = nullptr;
    QMenu *menu_ = nullptr;
    QString label_;
};
} // namespace orbitforge::desktop
