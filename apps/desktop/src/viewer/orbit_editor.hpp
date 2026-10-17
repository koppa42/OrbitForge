#pragma once
#include "display_settings.hpp"
#include "orbit_scene.hpp"
#include <QWidget>
#include <functional>
#include <orbitforge/core/time/time_provider.hpp>
#include <orbitforge/runtime/data_catalog.hpp>

class QLabel;
class QLineEdit;
class QPushButton;
class QComboBox;
class QTabBar;
class QCheckBox;
class QTreeWidget;
class QStackedWidget;
class QPlainTextEdit;
namespace orbitforge::desktop {
class EpochEditor;
class QuantityEditor;
class TrackStyleEditor;
class EopCatalog;
class OrbitEditor final : public QWidget {
  public:
    explicit OrbitEditor(const OrbitInput &input, QWidget *parent = nullptr);
    void set_time_provider(std::shared_ptr<const core::ITimeProvider> time);
    std::function<void(OrbitInput)> apply;
    std::function<void()> cancel;
    std::function<std::vector<runtime::CatalogGravity>()> gravity_models;
    std::function<std::vector<runtime::CatalogEphemeris>()> ephemerides, manage_ephemerides;
    EopCatalog *data_catalog = nullptr;
    std::function<void()> close_requested;
    std::function<void(int, QString, double)> appearance;
    std::function<void(DisplaySettings)> display;
    std::function<core::CartesianState(core::CartesianState, core::FrameId)> transform_state;
    void set_display(const DisplaySettings &settings);
    void set_input(const OrbitInput &input);
    void set_create_mode(bool create);
    void set_state_mode(int mode);
    void set_reference_frame(core::FrameId frame);
    void set_propagator(OrbitPropagator propagator);
    void open_propagator_details();
    void set_appearance(int kind, const QString &path, double diameter_km);
    void submit();
    void set_busy(bool busy);
    void show_result(const QString &message, bool error = false);
    void configure_appearance(int kind, const QString &path, double diameter_km);
    void set_appearance_busy(bool busy);
    void show_appearance_result(const QString &message, bool error = false);

  private:
    void refresh_enabled();
    core::CartesianState read_state() const;
    core::CartesianState in_frame(core::CartesianState state, core::FrameId frame) const;
    void write_cartesian(const core::CartesianState &state);
    void write_elements(const core::CartesianState &state);
    void update_state_labels();
    core::Tle read_tle() const;
    void update_tle_preview();
    std::shared_ptr<const core::ITimeProvider> time_;
    core::CartesianState manual_state_ = OrbitInput{}.initial;
    QPlainTextEdit *tle_text_ = nullptr;
    QLabel *tle_preview_ = nullptr, *epoch_label_ = nullptr;
    core::OriginId body_ = core::OriginId::earth;
    core::FrameId input_frame_ = core::FrameId::gcrf;
    OrbitPropagator propagator_kind_ = OrbitPropagator::two_body;
    core::HpopOptions hpop_;
    bool hpop_initialized_ = false;
    core::MissionPlan mission_;
    [[nodiscard]] bool uses_hpop() const {
        return propagator_kind_ == OrbitPropagator::hpop ||
               (propagator_kind_ == OrbitPropagator::mission &&
                mission_.coast_model == core::MissionCoastModel::hpop);
    }
    QLabel *hpop_summary_ = nullptr;
    QPushButton *model_button_ = nullptr;
    bool create_mode_ = false, orbit_busy_ = false, model_busy_ = false;
    QTreeWidget *navigation_ = nullptr;
    QComboBox *compact_navigation_ = nullptr;
    QTabBar *page_tabs_ = nullptr;
    QLineEdit *object_ = nullptr;
    QComboBox *propagator_ = nullptr, *reference_frame_ = nullptr;
    QLabel *anomaly_label_ = nullptr, *state_title_ = nullptr;
    std::array<QuantityEditor *, 3> position_{}, velocity_{};
    EpochEditor *epoch_ = nullptr, *first_ = nullptr, *last_ = nullptr;
    QComboBox *state_mode_ = nullptr;
    QStackedWidget *state_pages_ = nullptr;
    std::array<QuantityEditor *, 6> elements_{};
    int state_mode_index_ = 0;
    QuantityEditor *step_ = nullptr;
    QPushButton *apply_ = nullptr, *cancel_ = nullptr;
    QLabel *message_ = nullptr;
    QWidget *fields_ = nullptr;
    QWidget *model_fields_ = nullptr;
    QComboBox *model_kind_ = nullptr;
    QLineEdit *model_path_ = nullptr;
    QuantityEditor *model_diameter_ = nullptr;
    QPushButton *apply_model_ = nullptr;
    // Last values handed to the workbench; the 3D page's single Apply submits only changes.
    bool applied_track_3d_ = true;
    int applied_model_kind_ = 0;
    QString applied_model_path_;
    double applied_model_diameter_km_ = 1000;
    void apply_appearance_page();
    QLabel *model_message_ = nullptr;
    QComboBox *track_extent_ = nullptr;
    TrackStyleEditor *track_style_ = nullptr;
    QuantityEditor *past_ = nullptr, *future_ = nullptr;
    QCheckBox *track_2d_ = nullptr, *label_2d_ = nullptr, *track_3d_ = nullptr;
    std::vector<QPushButton *> display_buttons_;
    std::vector<QLabel *> display_messages_;
    void submit_display();
};
} // namespace orbitforge::desktop
