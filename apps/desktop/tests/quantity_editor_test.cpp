#include "../src/viewer/eop_catalog.hpp"
#include "../src/viewer/epoch_editor.hpp"
#include "../src/viewer/form_kit.hpp"
#include "../src/viewer/ground_station_editor.hpp"
#include "../src/viewer/hpop_settings.hpp"
#include "../src/viewer/mission_editor.hpp"
#include "../src/viewer/object_selection.hpp"
#include "../src/viewer/orbit_editor.hpp"
#include "../src/viewer/quantity_editor.hpp"
#include "../src/viewer/sensor_editor.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "window_show_observer.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <numbers>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/j2.hpp>
#include <orbitforge/runtime/j4.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>

namespace orbitforge::desktop {
class QuantityTest : public ::testing::Test {
  protected:
    void SetUp() override {
        static int argc = 1;
        static char executable[] = "quantity_test";
        static char *argv[] = {executable, nullptr};
        static QApplication app(argc, argv);
    }
    static QLineEdit *text(QuantityEditor &editor) {
        return editor.findChild<QLineEdit *>("quantityValue");
    }
    static void choose(QuantityEditor &editor, Unit unit) {
        auto *menu = editor.findChild<QToolButton *>("quantityUnit")->menu();
        for (auto *action : menu->actions())
            if (action->data().toInt() == static_cast<int>(unit)) {
                action->trigger();
                return;
            }
        FAIL() << "Requested unit was not offered";
    }
    static QuantityEditor &field(OrbitEditor &editor, const char *name) {
        auto *result = editor.findChild<QuantityEditor *>(name);
        if (!result)
            throw std::runtime_error("Missing quantity field");
        return *result;
    }
};
TEST_F(QuantityTest, ConstructingFieldsDoesNotShowTemporaryWindows) {
    WindowShowObserver windows;
    for (const auto unit : {Unit::one, Unit::kilometre, Unit::kilometres_per_second, Unit::degree,
                            Unit::second, Unit::kilometres_cubed_per_second_squared}) {
        QuantityEditor editor(unit);
        EXPECT_FALSE(editor.isVisible());
        QCoreApplication::processEvents();
    }
    EXPECT_TRUE(windows.shown.isEmpty()) << windows.shown.join(", ").toStdString();
}
TEST_F(QuantityTest, PreparingOrbitPropertiesDoesNotShowTemporaryWindows) {
    WindowShowObserver windows;
    OrbitEditor editor(OrbitInput{});
    QCoreApplication::processEvents();
    EXPECT_FALSE(editor.isVisible());
    EXPECT_TRUE(windows.shown.isEmpty()) << windows.shown.join(", ").toStdString();
}
TEST_F(QuantityTest, PreparingGroundStationDoesNotShowTemporaryWindows) {
    WindowShowObserver windows;
    GroundStationEditor editor;
    editor.set_station({"station", "Station", {1.2, 0.6, 350}}, true);
    editor.set_busy(true);
    editor.set_busy(false);
    editor.show_result("Invalid position");
    QCoreApplication::processEvents();
    EXPECT_FALSE(editor.isVisible());
    EXPECT_TRUE(windows.shown.isEmpty()) << windows.shown.join(", ").toStdString();
}
TEST_F(QuantityTest, ResponsivePropertiesPreserveDraftsAndKeepFieldsInsideViewport) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    OrbitEditor editor(OrbitInput{});
    editor.resize(1120, 620);
    editor.show();
    auto *selector = editor.findChild<QComboBox *>("propertyPageSelector");
    auto *tabs = editor.findChild<QTabBar *>("propertyPageTabs");
    auto *navigation = editor.findChild<QTreeWidget *>("propertyNavigation");
    auto *state = editor.findChild<QComboBox *>("stateRepresentation");
    ASSERT_TRUE(selector && tabs && navigation && state);
    state->setCurrentIndex(1);
    auto &axis = field(editor, "semiMajorAxis");
    text(axis)->setText("7123.456");
    editor.findChild<QComboBox *>("trackExtent")->setCurrentIndex(2);
    text(field(editor, "pastDuration"))->setText("37.5");
    const auto settle = [] {
        for (int i = 0; i < 12; ++i)
            QCoreApplication::processEvents();
    };
    const auto capture = qEnvironmentVariable("ORBITFORGE_RESPONSIVE_UI_CAPTURE");
    if (!capture.isEmpty())
        ASSERT_TRUE(QDir().mkpath(capture));
    for (int width : {1120, 320, 560, 840, 320, 1120}) {
        const int page = selector->currentIndex();
        editor.resize(width, 620);
        settle();
        EXPECT_EQ(editor.width(), width);
        EXPECT_EQ(selector->currentIndex(), page);
        // Tree beside wide pages, tabs at inspector widths, a selector when narrower.
        EXPECT_EQ(selector->isVisible(), width < 400);
        EXPECT_EQ(tabs->isVisible(), width >= 400 && width < 680);
        EXPECT_EQ(navigation->isVisible(), width >= 680);
        for (int index = 0; index < 4; ++index) {
            if (tabs->isVisible())
                tabs->setCurrentIndex(index);
            else
                selector->setCurrentIndex(index);
            settle();
            EXPECT_EQ(navigation->currentItem()->data(0, Qt::UserRole).toInt(), index);
            EXPECT_EQ(selector->currentIndex(), index);
            EXPECT_EQ(tabs->currentIndex(), index);
            for (auto *scroll : editor.findChildren<QScrollArea *>())
                if (scroll->isVisible()) {
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0)
                        << "width=" << width << ", page=" << index;
                    EXPECT_LE(scroll->widget()->width(), scroll->viewport()->width())
                        << "width=" << width << ", page=" << index;
                }
            if (!capture.isEmpty() && (width == 320 || width == 1120))
                EXPECT_TRUE(editor.grab().save(
                    capture + QString("/properties-%1-%2.png").arg(width).arg(index)));
        }
        EXPECT_EQ(text(axis)->text(), "7123.456");
        EXPECT_EQ(text(field(editor, "pastDuration"))->text(), "37.5");
    }
    editor.set_create_mode(true);
    editor.resize(320, 480);
    settle();
    EXPECT_EQ(selector->currentIndex(), 0);
    EXPECT_FALSE(selector->isEnabled());
    EXPECT_FALSE(tabs->isEnabled());
    EXPECT_TRUE(editor.findChild<QPushButton *>("applyOrbit")->isVisible());
    editor.set_create_mode(false);
    EXPECT_TRUE(selector->isEnabled());

    GroundStationEditor station;
    station.set_station({"station", "Station", {1.2, 0.6, 350}}, true);
    station.findChild<QLineEdit *>("stationName")->setText(QStringLiteral("未应用的站点名称"));
    station.show();
    for (int width : {480, 320, 640, 320}) {
        station.resize(width, 520);
        settle();
        EXPECT_EQ(station.width(), width);
        auto *scroll = station.findChild<QScrollArea *>("groundStationScroll");
        EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
        auto *submit = station.findChild<QPushButton *>("applyGroundStation");
        EXPECT_TRUE(station.rect().contains(submit->mapTo(&station, submit->rect().bottomRight())));
        EXPECT_EQ(station.findChild<QLineEdit *>("stationName")->text(),
                  QStringLiteral("未应用的站点名称"));
        if (!capture.isEmpty())
            EXPECT_TRUE(station.grab().save(capture + QString("/station-%1.png").arg(width)));
    }
}
TEST_F(QuantityTest, PreparingSensorDoesNotShowTemporaryWindows) {
    WindowShowObserver windows;
    application::Project project("Scenario", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"station", "Station", {1.2, 0.6, 350}});
    application::ProjectSensor sensor;
    sensor.object_id = "sensor";
    sensor.parent_satellite_id = "satellite";
    sensor.half_angle_rad = 0.2;
    SensorEditor editor;
    editor.set_sensor(sensor, project, true);
    sensor.pointing = application::SensorPointing::targeted;
    sensor.target_station_id = "station";
    editor.set_sensor(sensor, project, false);
    QCoreApplication::processEvents();
    EXPECT_FALSE(editor.isVisible());
    EXPECT_TRUE(windows.shown.isEmpty()) << windows.shown.join(", ").toStdString();
}
TEST_F(QuantityTest, SensorPointingChoicesKeepTargetDraftAndSubmitStableIds) {
    application::Project project("Scenario", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"station-a", "Same name", {1.2, 0.6, 350}});
    project.ground_stations.push_back({"station-b", "Same name", {1.3, 0.7, 100}});
    application::ProjectSensor sensor;
    sensor.object_id = "sensor";
    sensor.parent_satellite_id = "satellite";
    SensorEditor editor;
    editor.set_sensor(sensor, project, true);
    auto *name = editor.findChild<QLineEdit *>("sensorName");
    auto *angle = editor.findChild<QuantityEditor *>("sensorHalfAngle");
    auto *nadir = editor.findChild<QPushButton *>("sensorNadir");
    auto *targeted = editor.findChild<QPushButton *>("sensorTargeted");
    auto *target = editor.findChild<ObjectSelectionField *>("sensorTarget");
    auto *row = editor.findChild<QWidget *>("sensorTargetRow");
    auto *submit = editor.findChild<QPushButton *>("applySensor");
    ASSERT_TRUE(name && angle && nadir && targeted && target && row && submit);
    EXPECT_TRUE(nadir->isChecked());
    EXPECT_TRUE(row->isHidden());
    name->setText(QStringLiteral("  对地观测仪  "));
    text(*angle)->setText("30");
    choose(*angle, Unit::radian);
    // The two stations have identical display names; binding must use the stable ID.
    targeted->click();
    QTimer::singleShot(0, [] {
        auto *chooser = qobject_cast<ObjectSelectionDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        chooser->findChild<QLineEdit *>("objectSelectionSearch")->setText("station-b");
        auto *objects = chooser->findChild<QTreeWidget *>("objectSelectionTree");
        ASSERT_EQ(objects->topLevelItemCount(), 2);
        objects->setCurrentItem(objects->topLevelItem(1));
        chooser->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    target->click();
    EXPECT_FALSE(nadir->isChecked());
    EXPECT_FALSE(row->isHidden());
    EXPECT_EQ(editor.sensor().target_station_id, "station-b");
    nadir->click();
    EXPECT_TRUE(row->isHidden());
    EXPECT_TRUE(editor.sensor().target_station_id.empty());
    targeted->click();
    EXPECT_EQ(target->id(), "station-b");
    std::optional<application::ProjectSensor> applied;
    editor.apply = [&](application::ProjectSensor value) { applied = std::move(value); };
    submit->click();
    ASSERT_TRUE(applied);
    EXPECT_EQ(applied->object_id, sensor.object_id);
    EXPECT_EQ(applied->parent_satellite_id, sensor.parent_satellite_id);
    EXPECT_EQ(applied->display_name, "对地观测仪");
    EXPECT_EQ(applied->pointing, application::SensorPointing::targeted);
    EXPECT_EQ(applied->target_station_id, "station-b");
    EXPECT_NEAR(applied->half_angle_rad, std::numbers::pi / 6, 1e-14);
    editor.set_sensor(*applied, project, false);
    EXPECT_EQ(angle->unit(), Unit::radian);
    EXPECT_EQ(target->id(), "station-b");
    EXPECT_EQ(submit->text(), QStringLiteral("应用"));
    applied.reset();
    name->setText("Unsubmitted draft");
    editor.findChild<QPushButton *>("closeSensor")->click();
    EXPECT_FALSE(applied);
    editor.set_sensor(sensor, project, true);
    EXPECT_EQ(name->text(), "sensor");
    EXPECT_TRUE(nadir->isChecked());
    EXPECT_EQ(submit->text(), QStringLiteral("插入传感器"));
}
TEST_F(QuantityTest, SensorRejectsInvalidDraftAndNeverRebindsMissingTarget) {
    application::Project project("Scenario", core::Epoch{0}, core::Epoch{120'000'000'000});
    application::ProjectSensor sensor;
    sensor.object_id = "sensor";
    sensor.parent_satellite_id = "satellite";
    sensor.pointing = application::SensorPointing::targeted;
    sensor.target_station_id = "missing-station";
    SensorEditor editor;
    editor.set_sensor(sensor, project, false);
    auto *target = editor.findChild<ObjectSelectionField *>("sensorTarget");
    auto *submit = editor.findChild<QPushButton *>("applySensor");
    auto *error = editor.findChild<QLabel *>("sensorError");
    ASSERT_TRUE(target && submit && error);
    int applications = 0;
    editor.apply = [&](application::ProjectSensor) { ++applications; };
    EXPECT_TRUE(target->objects().empty());
    submit->click();
    EXPECT_EQ(applications, 0);
    EXPECT_FALSE(error->isHidden());
    EXPECT_TRUE(error->text().contains(QStringLiteral("没有地面站")));
    project.ground_stations.push_back({"other-station", "Station", {1.2, 0.6, 350}});
    editor.set_sensor(sensor, project, false);
    EXPECT_TRUE(error->isHidden());
    EXPECT_TRUE(target->isEnabled());
    EXPECT_EQ(target->id(), "missing-station");
    EXPECT_FALSE(target->contains(target->id()));
    submit->click();
    EXPECT_EQ(applications, 0);
    target->set_id("other-station");
    auto *angle = editor.findChild<QuantityEditor *>("sensorHalfAngle");
    ASSERT_TRUE(angle);
    text(*angle)->setText("91");
    submit->click();
    EXPECT_EQ(applications, 0);
    text(*angle)->setText("30");
    auto *name = editor.findChild<QLineEdit *>("sensorName");
    ASSERT_TRUE(name);
    name->setText(" ");
    submit->click();
    EXPECT_EQ(applications, 0);
    EXPECT_EQ(error->text(), QStringLiteral("请输入传感器名称。"));
    name->setText("Sensor");
    submit->click();
    EXPECT_EQ(applications, 1);
    EXPECT_TRUE(error->isHidden());
}
TEST_F(QuantityTest, SensorLayoutFitsNarrowPanelsAndKeepsActionsVisible) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    application::Project project("Scenario", core::Epoch{0}, core::Epoch{120'000'000'000});
    application::ProjectSatellite satellite;
    satellite.orbit.object_id = "satellite";
    satellite.display_name = "Aurora-01";
    project.satellites.push_back(satellite);
    project.ground_stations.push_back({"station", "Ground Station", {1.2, 0.6, 350}});
    application::ProjectSensor sensor;
    sensor.object_id = "sensor";
    sensor.display_name = "对地观测仪";
    sensor.parent_satellite_id = "satellite";
    sensor.half_angle_rad = std::numbers::pi / 6;
    SensorEditor editor;
    editor.set_sensor(sensor, project, true);
    auto *scroll = editor.findChild<QScrollArea *>("sensorScroll");
    auto *targeted = editor.findChild<QPushButton *>("sensorTargeted");
    auto *submit = editor.findChild<QPushButton *>("applySensor");
    auto *angle = editor.findChild<QuantityEditor *>("sensorHalfAngle");
    auto *full_angle = editor.findChild<QLabel *>("sensorFullAngle");
    ASSERT_TRUE(scroll && targeted && submit && angle && full_angle);
    const auto capture = qEnvironmentVariable("ORBITFORGE_SENSOR_UI_CAPTURE");
    if (!capture.isEmpty())
        ASSERT_TRUE(QDir{}.mkpath(capture));
    const auto screenshot = [&](const QString &name) {
        QApplication::processEvents();
        if (!capture.isEmpty())
            EXPECT_TRUE(editor.grab().save(capture + "/" + name + ".png"));
    };
    editor.resize(480, 640);
    editor.show();
    screenshot("sensor-insert");
    EXPECT_EQ(full_angle->text(), QStringLiteral("全视场 60°"));
    targeted->setFocus();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(targeted, &press);
    QApplication::sendEvent(targeted, &release);
    EXPECT_TRUE(targeted->isChecked());
    EXPECT_EQ(editor.sensor().target_station_id, "station");
    screenshot("sensor-targeted");
    // Long user names must not force the workspace pane wider.
    project.satellites[0].display_name = std::string(200, 'A');
    project.ground_stations[0].display_name = std::string(200, 'S');
    sensor.pointing = application::SensorPointing::targeted;
    sensor.target_station_id = "station";
    editor.set_sensor(sensor, project, false);
    for (const int width : {320, 480, 640}) {
        editor.resize(width, 400);
        QApplication::processEvents();
        EXPECT_EQ(editor.width(), width);
        EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
        EXPECT_FALSE(scroll->horizontalScrollBar()->isVisible());
        EXPECT_LE(scroll->widget()->width(), scroll->viewport()->width());
        scroll->ensureWidgetVisible(editor.findChild<ObjectSelectionField *>("sensorTarget"));
        QApplication::processEvents();
        const QRect action_rect(submit->mapTo(&editor, QPoint{}), submit->size());
        EXPECT_TRUE(editor.rect().contains(action_rect));
        EXPECT_FALSE(scroll->isAncestorOf(submit));
        screenshot(QStringLiteral("sensor-narrow-%1").arg(width));
    }
    editor.resize(360, 640);
    text(*angle)->setText("invalid");
    QApplication::processEvents();
    EXPECT_EQ(full_angle->text(), QStringLiteral("半角范围 0.001–90°"));
    submit->click();
    screenshot("sensor-invalid");
    project.ground_stations.clear();
    editor.set_sensor(sensor, project, false);
    submit->click();
    screenshot("sensor-no-stations");
    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
}
TEST_F(QuantityTest, PreparingEpochAndValidationDoesNotShowTemporaryWindows) {
    WindowShowObserver windows;
    EpochEditor editor;
    editor.set_read_only(true);
    editor.set_read_only(false);
    editor.findChild<QLineEdit *>("epochValue")->setText("invalid epoch");
    editor.set_scale(CalendarScale::tt);
    QCoreApplication::processEvents();
    EXPECT_FALSE(editor.isVisible());
    EXPECT_TRUE(windows.shown.isEmpty()) << windows.shown.join(", ").toStdString();
}
TEST_F(QuantityTest, ConvertsCompatibleDimensionsAndRejectsInvalidConversions) {
    EXPECT_DOUBLE_EQ(convert_quantity(7000, Unit::kilometre, Unit::metre), 7e6);
    EXPECT_DOUBLE_EQ(convert_quantity(7.5, Unit::kilometres_per_second, Unit::metres_per_second),
                     7500);
    EXPECT_DOUBLE_EQ(convert_quantity(180, Unit::degree, Unit::radian), std::numbers::pi);
    EXPECT_DOUBLE_EQ(convert_quantity(2, Unit::day, Unit::hour), 48);
    EXPECT_DOUBLE_EQ(convert_quantity(0.75, Unit::minute, Unit::millisecond), 45000);
    EXPECT_NEAR(convert_quantity(398600.4418, Unit::kilometres_cubed_per_second_squared,
                                 Unit::metres_cubed_per_second_squared),
                3.986004418e14, 0.1);
    EXPECT_THROW(convert_quantity(1, Unit::metre, Unit::second), std::invalid_argument);
    EXPECT_THROW(convert_quantity(1, static_cast<Unit>(999), Unit::second), std::invalid_argument);
    EXPECT_THROW(
        convert_quantity(std::numeric_limits<double>::infinity(), Unit::degree, Unit::radian),
        std::invalid_argument);
    EXPECT_THROW(convert_quantity(1e308, Unit::kilometre, Unit::metre), std::out_of_range);
    EXPECT_THROW(
        convert_quantity(std::numeric_limits<double>::denorm_min(), Unit::metre, Unit::kilometre),
        std::out_of_range);
}
TEST_F(QuantityTest, MenuConvertsDraftAndGetterDoesNotDependOnDisplayUnit) {
    QuantityEditor editor(Unit::degree);
    text(editor)->setText("180");
    choose(editor, Unit::radian);
    EXPECT_EQ(editor.unit(), Unit::radian);
    EXPECT_TRUE(text(editor)->text().startsWith("3.14159265358979"));
    EXPECT_DOUBLE_EQ(editor.value(Unit::radian), std::numbers::pi);
    EXPECT_DOUBLE_EQ(editor.value(Unit::degree), 180);
    text(editor)->setText("0.5");
    choose(editor, Unit::degree);
    EXPECT_DOUBLE_EQ(editor.value(Unit::radian), 0.5);
    EXPECT_NEAR(editor.value(Unit::degree), 28.64788975654116, 1e-12);
}
TEST_F(QuantityTest, RepeatedUnitChangesKeepFullPrecisionAndDoNotChangePhysicalValue) {
    QuantityEditor editor(Unit::radian);
    const double original = 1.2345678901234567;
    editor.set_value(original, Unit::radian);
    int values = 0, units = 0;
    QObject::connect(&editor, &QuantityEditor::valueChanged, [&values] { ++values; });
    QObject::connect(&editor, &QuantityEditor::unitChanged, [&units] { ++units; });
    for (int i = 0; i < 100; ++i) {
        editor.set_unit(Unit::degree);
        EXPECT_DOUBLE_EQ(editor.value(Unit::radian), original);
        editor.set_unit(Unit::radian);
    }
    EXPECT_EQ(values, 0);
    EXPECT_EQ(units, 200);
    editor.set_value(2, Unit::radian);
    EXPECT_EQ(values, 1);
}
TEST_F(QuantityTest, InvalidDraftIsNotDiscardedAndCannotChangeUnitOrBeRead) {
    QuantityEditor editor(Unit::degree);
    editor.set_value(45, Unit::degree);
    for (const QString input : {QString{}, QString("-"), QString("1e"), QString("nan"),
                                QString("inf"), QString("1,000"), QString("1e400")}) {
        text(editor)->setText(input);
        EXPECT_THROW(editor.set_unit(Unit::radian), std::exception);
        EXPECT_THROW((void)editor.value(Unit::radian), std::exception);
        EXPECT_EQ(editor.unit(), Unit::degree);
        EXPECT_EQ(text(editor)->text(), input);
        EXPECT_TRUE(editor.property("invalid").toBool());
    }
    text(editor)->setText("9e1");
    EXPECT_FALSE(editor.property("invalid").toBool());
    choose(editor, Unit::radian);
    EXPECT_DOUBLE_EQ(editor.value(Unit::degree), 90);
}
TEST_F(QuantityTest, ScientificInputAndMixedUnitSetterPreserveSignedValues) {
    QuantityEditor editor(Unit::kilometre);
    text(editor)->setText("-2.5e-3");
    EXPECT_DOUBLE_EQ(editor.value(Unit::metre), -2.5);
    editor.set_unit(Unit::metre);
    EXPECT_EQ(text(editor)->text(), "-2.5");
    editor.set_value(2.25, Unit::kilometre);
    EXPECT_EQ(text(editor)->text(), "2250");
    EXPECT_THROW(editor.set_unit(Unit::radian), std::invalid_argument);
    EXPECT_EQ(editor.unit(), Unit::metre);
    EXPECT_DOUBLE_EQ(editor.value(Unit::metre), 2250);
    EXPECT_THROW((void)editor.value(Unit::second), std::invalid_argument);
}
TEST_F(QuantityTest, PhysicalRangeAndWholeSecondResolutionSurviveUnitChanges) {
    QuantityEditor editor(Unit::second);
    editor.set_range(1, 60, Unit::second);
    editor.set_quantum(1, Unit::second);
    editor.set_value(60, Unit::second);
    choose(editor, Unit::minute);
    EXPECT_EQ(text(editor)->text(), "1");
    text(editor)->setText("0.75");
    EXPECT_DOUBLE_EQ(editor.value(Unit::second), 45);
    text(editor)->setText("0.751");
    EXPECT_THROW((void)editor.value(Unit::second), std::out_of_range);
    EXPECT_THROW(editor.set_unit(Unit::second), std::out_of_range);
    EXPECT_EQ(text(editor)->text(), "0.751");
    editor.set_value(1, Unit::second);
    editor.set_unit(Unit::day);
    const auto rendered = text(editor)->text();
    text(editor)->setText({});
    text(editor)->setText(rendered);
    EXPECT_DOUBLE_EQ(editor.value(Unit::second), 1);
    EXPECT_THROW(editor.set_value(61, Unit::second), std::out_of_range);
    EXPECT_DOUBLE_EQ(editor.value(Unit::second), 1);
}
TEST_F(QuantityTest, KeyboardStepUsesPhysicalIncrementAndDimensionlessHasNoUnitMenu) {
    QuantityEditor editor(Unit::degree);
    editor.set_range(0, 180, Unit::degree);
    editor.set_single_step(1, Unit::degree);
    editor.set_value(90, Unit::degree);
    editor.set_unit(Unit::radian);
    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QCoreApplication::sendEvent(text(editor), &up);
    EXPECT_NEAR(editor.value(Unit::degree), 91, 1e-12);
    QuantityEditor eccentricity(Unit::one);
    EXPECT_TRUE(eccentricity.findChild<QToolButton *>("quantityUnit")->isHidden());
    eccentricity.set_value(0.1, Unit::one);
    EXPECT_THROW(eccentricity.set_unit(Unit::degree), std::invalid_argument);
}
TEST_F(QuantityTest, OrbitSubmissionReadsMixedUnitsAsSIAndKeepsUnitPreferences) {
    OrbitInput input;
    OrbitEditor editor(input);
    editor.set_state_mode(0);
    auto &position = field(editor, "positionX"), &velocity = field(editor, "velocityY");
    position.set_unit(Unit::metre);
    velocity.set_unit(Unit::metres_per_second);
    text(position)->setText("7.2e6");
    text(velocity)->setText("6300");
    EXPECT_EQ(editor.findChild<QuantityEditor *>("gravitationalParameter"), nullptr);
    auto &step = field(editor, "displayStep");
    step.set_unit(Unit::minute);
    text(step)->setText("0.75");
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput result) { submitted = std::move(result); };
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_DOUBLE_EQ(submitted->initial.position_m[0], 7.2e6);
    EXPECT_DOUBLE_EQ(submitted->initial.velocity_mps[1], 6300);
    EXPECT_DOUBLE_EQ(submitted->mu(), input.mu());
    EXPECT_EQ(submitted->step_seconds, 45);
    editor.set_input(*submitted);
    EXPECT_EQ(position.unit(), Unit::metre);
    EXPECT_EQ(velocity.unit(), Unit::metres_per_second);
    EXPECT_EQ(step.unit(), Unit::minute);
    submitted.reset();
    text(step)->setText("0.751");
    editor.submit();
    EXPECT_FALSE(submitted);
}
TEST_F(QuantityTest, KeplerianSubmissionUsesRadiansWithoutDoubleConversion) {
    OrbitInput input;
    OrbitEditor editor(input);
    const std::array<const char *, 6> names{"semiMajorAxis", "eccentricity",        "inclination",
                                            "raan",          "argumentOfPeriapsis", "trueAnomaly"};
    const std::array values{7.8e6, 0.1, 0.6, 1.3, 2.1, -0.2};
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto unit = i == 0 ? Unit::metre : i == 1 ? Unit::one : Unit::radian;
        auto &quantity = field(editor, names[i]);
        quantity.set_unit(unit);
        quantity.set_value(values[i], unit);
    }
    const auto expected =
        runtime::to_cartesian({input.initial.epoch, 7.8e6, 0.1, 0.6, 1.3, 2.1, -0.2},
                              core::GravitationalParameter{input.mu()});
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput result) { submitted = std::move(result); };
    editor.submit();
    ASSERT_TRUE(submitted);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(submitted->initial.position_m[i], expected.position_m[i], 1e-7);
        EXPECT_NEAR(submitted->initial.velocity_mps[i], expected.velocity_mps[i], 1e-10);
    }
}
TEST_F(QuantityTest, DisplayDurationAndModelDiameterKeepExistingStorageUnits) {
    OrbitEditor editor(OrbitInput{});
    editor.findChild<QComboBox *>("trackExtent")->setCurrentIndex(2);
    auto &past = field(editor, "pastDuration");
    past.set_unit(Unit::hour);
    text(past)->setText("0.5");
    auto &future = field(editor, "futureDuration");
    future.set_unit(Unit::second);
    text(future)->setText("900");
    std::optional<DisplaySettings> settings;
    editor.display = [&](DisplaySettings result) { settings = result; };
    editor.findChild<QPushButton *>("applyDisplay")->click();
    ASSERT_TRUE(settings);
    EXPECT_DOUBLE_EQ(settings->past_minutes, 30);
    EXPECT_DOUBLE_EQ(settings->future_minutes, 15);
    editor.set_appearance(1, {}, 12.5);
    auto &diameter = field(editor, "modelDiameter");
    diameter.set_unit(Unit::metre);
    EXPECT_DOUBLE_EQ(diameter.value(Unit::metre), 12500);
    text(diameter)->setText("17000");
    double applied = 0;
    editor.appearance = [&](int, QString, double km) { applied = km; };
    editor.findChild<QPushButton *>("applyAppearance")->click();
    EXPECT_DOUBLE_EQ(applied, 17);
}
TEST_F(QuantityTest, TrackStylePreviewApplyCustomColorAndObjectSwitchStayIndependent) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    OrbitEditor editor(OrbitInput{});
    editor.resize(980, 660);
    auto *navigation = editor.findChild<QTreeWidget *>("propertyNavigation");
    navigation->setCurrentItem(navigation->topLevelItem(0)->child(1));
    DisplaySettings existing;
    existing.line_color = 0x123456;
    existing.line_width = 2.5;
    existing.line_style = application::LineStyle::dash_dot;
    existing.extent = 2;
    existing.past_minutes = 12;
    existing.track_3d = false;
    editor.set_display(existing);
    editor.show();
    QApplication::processEvents();
    auto *style = editor.findChild<QComboBox *>("trackLineStyle");
    auto *width = editor.findChild<QDoubleSpinBox *>("trackLineWidth");
    auto *custom = editor.findChild<QPushButton *>("trackCustomColor");
    ASSERT_TRUE(style && width && custom);
    EXPECT_EQ(style->currentIndex(), 3);
    EXPECT_DOUBLE_EQ(width->value(), 2.5);
    EXPECT_EQ(width->buttonSymbols(), QAbstractSpinBox::UpDownArrows);
    EXPECT_TRUE(custom->text().contains("#123456"));
    std::vector<DisplaySettings> applied;
    editor.display = [&](DisplaySettings result) { applied.push_back(result); };
    const auto before = editor.findChild<QWidget *>("trackStylePreview")->grab().toImage();
    editor.findChild<QToolButton *>("trackColor2")->click();
    style->setCurrentIndex(1);
    width->setValue(4);
    const auto after = editor.findChild<QWidget *>("trackStylePreview")->grab().toImage();
    EXPECT_NE(before, after);
    EXPECT_TRUE(applied.empty());
    editor.findChild<QPushButton *>("applyDisplay")->click();
    ASSERT_EQ(applied.size(), 1);
    EXPECT_EQ(applied[0].line_color, 0xe9a2ba);
    EXPECT_EQ(applied[0].line_style, application::LineStyle::dashed);
    EXPECT_EQ(applied[0].line_width, 4);
    EXPECT_EQ(applied[0].past_minutes, 12);
    EXPECT_FALSE(applied[0].track_3d);
    const auto capture = qEnvironmentVariable("ORBITFORGE_STYLE_UI_CAPTURE");
    if (!capture.isEmpty()) {
        ASSERT_TRUE(QDir{}.mkpath(capture));
        ASSERT_TRUE(editor.grab().save(capture + "/track-style.png"));
        editor.resize(910, 400);
        QApplication::processEvents();
        ASSERT_TRUE(editor.grab().save(capture + "/track-style-compact.png"));
    }
    QTimer::singleShot(0, [] {
        auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->setCurrentColor(QColor("#abcdef"));
        dialog->reject();
    });
    custom->click();
    EXPECT_TRUE(custom->text().contains("#E9A2BA"));
    QTimer::singleShot(0, [] {
        auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->setCurrentColor(QColor("#abcdef"));
        dialog->accept();
    });
    custom->click();
    editor.findChild<QPushButton *>("applyDisplay")->click();
    EXPECT_EQ(applied.back().line_color, 0xabcdef);
    editor.set_display(existing);
    editor.findChild<QPushButton *>("applyDisplay")->click();
    EXPECT_EQ(applied.back(), existing);
}
TEST_F(QuantityTest, FrameSwitchAndItrfInputUseFullPositionVelocityTransform) {
    const auto frames = runtime::make_frame_provider();
    const auto eop = runtime::load_eop_snapshot(ORBITFORGE_TEST_EOP);
    OrbitInput input;
    input.initial.epoch =
        runtime::make_builtin_time_provider()->from_utc({2026, 10, 9, 0, 0, 0, 0});
    OrbitEditor editor(input);
    editor.transform_state = [&](core::CartesianState state, core::FrameId frame) {
        const auto transform = frames->gcrf_to_itrf_state(state.epoch, eop->at(state.epoch));
        return frame == core::FrameId::itrf ? transform.apply(state) : transform.inverse(state);
    };
    editor.findChild<QComboBox *>("initialReferenceFrame")->setCurrentIndex(1);
    EXPECT_EQ(editor.findChild<QComboBox *>("stateRepresentation")->currentIndex(), 0);
    const auto transform =
        frames->gcrf_to_itrf_state(input.initial.epoch, eop->at(input.initial.epoch));
    auto expected_fixed = transform.apply(input.initial);
    EXPECT_NEAR(field(editor, "positionX").value(Unit::metre), expected_fixed.position_m[0], 1e-6);
    EXPECT_NEAR(field(editor, "velocityY").value(Unit::metres_per_second),
                expected_fixed.velocity_mps[1], 1e-8);
    // Editing fixed-frame velocity must include the Earth rotation term on submission.
    field(editor, "velocityY").set_value(1000, Unit::metres_per_second);
    expected_fixed.velocity_mps[1] = 1000;
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput result) { submitted = result; };
    editor.submit();
    ASSERT_TRUE(submitted);
    const auto expected = transform.inverse(expected_fixed);
    EXPECT_EQ(submitted->input_frame, core::FrameId::itrf);
    for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_NEAR(submitted->initial.position_m[k], expected.position_m[k], 1e-6);
        EXPECT_NEAR(submitted->initial.velocity_mps[k], expected.velocity_mps[k], 1e-8);
    }
    editor.set_input(*submitted);
    EXPECT_EQ(editor.findChild<QComboBox *>("initialReferenceFrame")->currentIndex(), 1);
    editor.set_state_mode(1);
    EXPECT_EQ(editor.findChild<QComboBox *>("initialReferenceFrame")->currentIndex(), 0);
    submitted.reset();
    editor.submit();
    ASSERT_TRUE(submitted);
    for (std::size_t k = 0; k < 3; ++k)
        EXPECT_NEAR(submitted->initial.position_m[k], expected.position_m[k], .001);
}
TEST_F(QuantityTest, FailedFrameSwitchRetainsTheOriginalDraft) {
    OrbitEditor editor(OrbitInput{});
    const double original = field(editor, "semiMajorAxis").value(Unit::metre);
    editor.transform_state = [](core::CartesianState, core::FrameId) -> core::CartesianState {
        throw std::out_of_range("No EOP coverage");
    };
    editor.findChild<QComboBox *>("initialReferenceFrame")->setCurrentIndex(1);
    EXPECT_EQ(editor.findChild<QComboBox *>("initialReferenceFrame")->currentIndex(), 0);
    EXPECT_EQ(editor.findChild<QComboBox *>("stateRepresentation")->currentIndex(), 1);
    EXPECT_DOUBLE_EQ(field(editor, "semiMajorAxis").value(Unit::metre), original);
}
TEST_F(QuantityTest, J2MeanAnomalyAndReadOnlyModelDetailsPreserveDraft) {
    OrbitEditor editor(OrbitInput{});
    editor.findChild<QComboBox *>("orbitPropagator")->setCurrentIndex(1);
    field(editor, "meanAnomaly").set_value(90, Unit::degree);
    auto &step = field(editor, "displayStep");
    step.set_unit(Unit::minute);
    step.set_value(30, Unit::second);
    int submissions = 0;
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput result) {
        ++submissions;
        submitted = result;
    };
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("propagatorDetailsDialog");
        ASSERT_NE(dialog, nullptr);
        EXPECT_TRUE(dialog->findChildren<QCheckBox *>().empty());
        EXPECT_TRUE(dialog->findChildren<QuantityEditor *>().empty());
        EXPECT_TRUE(dialog->findChildren<QLineEdit *>().empty());
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Close)->click();
    });
    editor.findChild<QPushButton *>("propagatorDetails")->click();
    EXPECT_EQ(submissions, 0);
    EXPECT_EQ(step.unit(), Unit::minute);
    EXPECT_DOUBLE_EQ(step.value(Unit::minute), 0.5);
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_EQ(submitted->propagator, OrbitPropagator::j2_secular);
    EXPECT_EQ(submitted->step_seconds, 30);
    const auto mean = runtime::j2_mean_ellipse_elements(submitted->initial);
    EXPECT_NEAR(mean.mean_anomaly_rad, std::numbers::pi / 2, 1e-13);
    editor.set_input(*submitted);
    EXPECT_NEAR(field(editor, "meanAnomaly").value(Unit::degree), 90, 1e-10);
    // Inspecting a model must not validate or overwrite a partially edited field.
    text(step)->setText("unfinished");
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("propagatorDetailsDialog");
        ASSERT_NE(dialog, nullptr);
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(dialog, &escape);
    });
    editor.open_propagator_details();
    EXPECT_EQ(submissions, 1);
    EXPECT_EQ(text(step)->text(), "unfinished");
    EXPECT_EQ(step.unit(), Unit::minute);
    step.set_value(30, Unit::second);
    editor.submit();
    EXPECT_EQ(submitted->step_seconds, 30);
    editor.findChild<QComboBox *>("orbitPropagator")->setCurrentIndex(0);
    EXPECT_NE(editor.findChild<QuantityEditor *>("trueAnomaly"), nullptr);
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("propagatorDetailsDialog");
        ASSERT_NE(dialog, nullptr);
        EXPECT_TRUE(dialog->findChildren<QuantityEditor *>().empty());
        EXPECT_TRUE(dialog->findChildren<QCheckBox *>().empty());
        dialog->reject();
    });
    editor.open_propagator_details();
    editor.submit();
    EXPECT_EQ(submitted->propagator, OrbitPropagator::two_body);
    EXPECT_EQ(submitted->step_seconds, 30);
}

TEST_F(QuantityTest, J4AndTleEditingKeepSeparateDraftsAndRejectInvalidInput) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    OrbitEditor editor(OrbitInput{});
    editor.set_time_provider(runtime::make_builtin_time_provider());
    editor.set_propagator(OrbitPropagator::j4_secular);
    field(editor, "meanAnomaly").set_value(45, Unit::degree);
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput value) { submitted = std::move(value); };
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_EQ(submitted->propagator, OrbitPropagator::j4_secular);
    EXPECT_NEAR(runtime::j4_mean_ellipse_elements(submitted->initial).mean_anomaly_rad,
                std::numbers::pi / 4, 1e-10);
    const auto manual = submitted->initial;
    editor.set_propagator(OrbitPropagator::sgp4);
    auto *input = editor.findChild<QPlainTextEdit *>("tleInput");
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(editor.findChild<QComboBox *>("stateRepresentation")->isHidden());
    EXPECT_FALSE(editor.findChild<QComboBox *>("initialReferenceFrame")->isEnabled());
    submitted.reset();
    input->setPlainText("incomplete");
    editor.submit();
    EXPECT_FALSE(submitted);
    input->setPlainText("VANGUARD\n"
                        "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753\n"
                        "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667");
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_EQ(submitted->propagator, OrbitPropagator::sgp4);
    EXPECT_EQ(submitted->tle.line1.size(), 69U);
    EXPECT_EQ(submitted->initial.position_m, manual.position_m);
    const auto saved = *submitted;
    auto fixed_draft = saved;
    fixed_draft.input_frame = core::FrameId::itrf;
    editor.transform_state = [](core::CartesianState, core::FrameId) -> core::CartesianState {
        throw std::runtime_error("Inactive draft epoch is outside current EOP coverage");
    };
    EXPECT_NO_THROW(editor.set_input(fixed_draft));
    editor.submit();
    EXPECT_EQ(submitted->tle, saved.tle);
    EXPECT_EQ(submitted->initial.position_m, saved.initial.position_m);
    editor.set_propagator(OrbitPropagator::j4_secular);
    EXPECT_FALSE(editor.findChild<QComboBox *>("stateRepresentation")->isHidden());
    editor.submit();
    EXPECT_TRUE(submitted->tle.line1.empty());
    EXPECT_NEAR(runtime::j4_mean_ellipse_elements(submitted->initial).mean_anomaly_rad,
                std::numbers::pi / 4, 1e-10);
    editor.set_propagator(OrbitPropagator::sgp4);
    EXPECT_TRUE(input->toPlainText().contains("00005"));
    const auto capture = qEnvironmentVariable("ORBITFORGE_PROPAGATOR_UI_CAPTURE");
    if (!capture.isEmpty()) {
        ASSERT_TRUE(QDir{}.mkpath(capture));
        editor.resize(1120, 720);
        editor.show();
        QApplication::processEvents();
        ASSERT_TRUE(editor.grab().save(capture + "/sgp4-wide.png"));
        editor.resize(840, 560);
        QApplication::processEvents();
        ASSERT_TRUE(editor.grab().save(capture + "/sgp4-compact.png"));
        editor.resize(1120, 720);
        editor.set_propagator(OrbitPropagator::j4_secular);
        QApplication::processEvents();
        ASSERT_TRUE(editor.grab().save(capture + "/j4-wide.png"));
    }
}
TEST_F(QuantityTest, MissionEditorValidatesSequencePreservesDraftAndCancelsWithoutApplying) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    OrbitEditor editor(OrbitInput{});
    editor.resize(1100, 760);
    editor.show();
    editor.set_propagator(OrbitPropagator::mission);
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput input) { submitted = std::move(input); };
    const auto capture = qEnvironmentVariable("ORBITFORGE_MISSION_UI_CAPTURE");
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("missionEditorDialog");
        ASSERT_NE(dialog, nullptr);
        auto *list = dialog->findChild<QListWidget *>("missionSegments");
        ASSERT_EQ(list->count(), 1);
        auto *duration = dialog->findChild<QuantityEditor *>("missionCoastDuration");
        duration->set_value(600, Unit::second);
        dialog->findChild<QPushButton *>("missionAddImpulse")->click();
        dialog->findChild<QLineEdit *>("missionSegmentName")->setText(QStringLiteral("升轨点火"));
        dialog->findChild<QuantityEditor *>("missionDeltaV0")
            ->set_value(120, Unit::metres_per_second);
        dialog->findChild<QComboBox *>("missionManeuverFrame")->setCurrentIndex(1);
        dialog->findChild<QPushButton *>("missionMoveUp")->click();
        EXPECT_EQ(list->currentRow(), 0);
        dialog->findChild<QPushButton *>("missionMoveDown")->click();
        EXPECT_EQ(list->currentRow(), 1);
        dialog->findChild<QPushButton *>("missionAddCoast")->click();
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        ok->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
        text(*duration)->setText("unfinished");
        list->setCurrentRow(1);
        EXPECT_EQ(list->currentRow(), 2);
        duration->set_value(6600, Unit::second);
        list->setCurrentRow(1);
        EXPECT_EQ(list->currentRow(), 1);
        if (!capture.isEmpty()) {
            ASSERT_TRUE(QDir{}.mkpath(capture));
            const auto settled = [] {
                QEventLoop loop;
                QTimer::singleShot(80, &loop, &QEventLoop::quit);
                loop.exec();
            };
            settled();
            EXPECT_TRUE(dialog->grab().save(capture + "/mission-wide.png"));
            dialog->resize(440, 760);
            settled();
            EXPECT_TRUE(dialog->grab().save(capture + "/mission-narrow.png"));
            for (auto *scroll : dialog->findChildren<QScrollArea *>())
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_TRUE(ok->isVisible());
            dialog->resize(900, 690);
            settled();
            EXPECT_DOUBLE_EQ(dialog->findChild<QuantityEditor *>("missionDeltaV0")
                                 ->value(Unit::metres_per_second),
                             120);
        }
        ok->click();
    });
    editor.open_propagator_details();
    EXPECT_FALSE(submitted);
    editor.submit();
    ASSERT_TRUE(submitted);
    ASSERT_EQ(submitted->mission.segments.size(), 3U);
    EXPECT_EQ(submitted->mission.segments[1].delta_v_mps[0], 120);
    const auto saved = *submitted;
    editor.set_input(saved);
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("missionEditorDialog");
        ASSERT_NE(dialog, nullptr);
        EXPECT_EQ(dialog->findChild<QListWidget *>("missionSegments")->count(), 3);
        dialog->findChild<QPushButton *>("missionRemove")->click();
        dialog->reject();
    });
    editor.open_propagator_details();
    editor.submit();
    EXPECT_EQ(submitted->mission, saved.mission);
}
TEST_F(QuantityTest, MissionTimelineUpdatesExactBurnTimesAndRecoversFromIncompleteInput) {
    using namespace std::chrono_literals;
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::MissionPlan plan{
        core::MissionCoastModel::two_body,
        {core::MissionSegment::coast(600s + 1ns), core::MissionSegment::impulse({3, 4, 0}),
         core::MissionSegment::impulse({0, 0, 12}), core::MissionSegment::coast(6600s)}};
    core::HpopOptions hpop;
    QWidget parent;
    QTimer::singleShot(0, &parent, [&] {
        auto *dialog = parent.findChild<QDialog *>("missionEditorDialog");
        ASSERT_NE(dialog, nullptr);
        auto *list = dialog->findChild<QListWidget *>("missionSegments");
        auto *coverage = dialog->findChild<QLabel *>("missionCoverage");
        auto *timing = dialog->findChild<QLabel *>("missionSegmentTiming");
        auto *duration = dialog->findChild<QuantityEditor *>("missionCoastDuration");
        auto *error = dialog->findChild<QLabel *>("errorText");
        EXPECT_FALSE(coverage->property("warning").toBool());
        EXPECT_TRUE(list->item(1)->toolTip().contains("T+00:10:00.000000001"));
        EXPECT_TRUE(list->item(2)->toolTip().contains("T+00:10:00.000000001"));
        duration->set_value(1200, Unit::second);
        EXPECT_TRUE(list->item(1)->toolTip().contains("T+00:20:00"));
        list->setCurrentRow(1);
        EXPECT_TRUE(timing->text().contains("T+00:20:00"));
        EXPECT_TRUE(
            dialog->findChild<QLabel *>("missionImpulseMagnitude")->text().contains("5 m/s"));
        EXPECT_TRUE(dialog->findChild<QLabel *>("missionSummary")->text().contains("17 m/s"));
        dialog->findChild<QComboBox *>("missionManeuverFrame")->setCurrentIndex(2);
        EXPECT_TRUE(list->item(1)->toolTip().contains("LVLH"));
        auto *dv = dialog->findChild<QuantityEditor *>("missionDeltaV0");
        text(*dv)->setText("-");
        EXPECT_FALSE(error->isHidden());
        EXPECT_TRUE(coverage->text().contains(QStringLiteral("输入尚未完成")));
        list->setCurrentRow(2);
        EXPECT_EQ(list->currentRow(), 1);
        // Returning to the previous number emits no valueChanged signal.
        text(*dv)->setText("3");
        EXPECT_TRUE(error->isHidden());
        EXPECT_FALSE(coverage->property("warning").toBool());
        list->setCurrentRow(0);
        duration->set_value(1e-9, Unit::second);
        EXPECT_TRUE(coverage->property("warning").toBool());
        EXPECT_TRUE(coverage->text().contains(QStringLiteral("延长滑行段")));
        duration->set_value(600, Unit::second);
        EXPECT_FALSE(coverage->property("warning").toBool());
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        ok->click();
    });
    ASSERT_TRUE(edit_mission(plan, hpop, core::Epoch{0}, core::Epoch{0},
                             core::Epoch{7'200'000'000'000}, &parent));
    EXPECT_EQ(plan.segments[0].duration_nanoseconds, 600'000'000'000);
    EXPECT_EQ(plan.segments[1].frame, core::ManeuverFrame::lvlh);
}
TEST_F(QuantityTest, MissionCompactPagesPreserveUnitsAndDraftsAndHandleEmptySequence) {
    using namespace std::chrono_literals;
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::MissionPlan plan{core::MissionCoastModel::two_body,
                           {core::MissionSegment::coast(600s),
                            core::MissionSegment::impulse({120, 0, 0}, core::ManeuverFrame::vnc,
                                                          std::string(300, 'A')),
                            core::MissionSegment::coast(6600s)}};
    const auto original = plan;
    core::HpopOptions hpop;
    QWidget parent;
    const auto capture = qEnvironmentVariable("ORBITFORGE_MISSION_UI_CAPTURE");
    QTimer::singleShot(0, &parent, [&] {
        auto *dialog = parent.findChild<QDialog *>("missionEditorDialog");
        ASSERT_NE(dialog, nullptr);
        const auto settled = [] {
            QEventLoop loop;
            QTimer::singleShot(80, &loop, &QEventLoop::quit);
            loop.exec();
        };
        const auto screenshot = [&](const char *name) {
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(dialog->grab().save(capture + '/' + name + ".png"));
            }
        };
        auto *list = dialog->findChild<QListWidget *>("missionSegments");
        auto *navigation = dialog->findChild<QTabBar *>("missionNavigation");
        auto *scroll = dialog->findChild<QScrollArea *>("missionParameters");
        auto *dv = dialog->findChild<QuantityEditor *>("missionDeltaV0");
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        list->setCurrentRow(1);
        dv->set_unit(Unit::kilometres_per_second);
        text(*dv)->setText("1e");
        for (const auto size : {QSize{980, 740}, QSize{400, 520}, QSize{440, 760}}) {
            dialog->resize(size);
            navigation->setCurrentIndex(1);
            settled();
            EXPECT_EQ(list->currentRow(), 1);
            EXPECT_EQ(dv->unit(), Unit::kilometres_per_second);
            EXPECT_EQ(text(*dv)->text(), "1e");
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_GT(scroll->viewport()->height(), 130);
            EXPECT_TRUE(dialog->rect().contains(ok->mapTo(dialog, ok->rect().bottomRight())));
            screenshot(size.width() == 400   ? "mission-short-invalid"
                       : size.width() == 440 ? "mission-narrow-invalid"
                                             : "mission-wide-long-name");
        }
        EXPECT_TRUE(navigation->isVisible());
        navigation->setCurrentIndex(0);
        settled();
        EXPECT_TRUE(list->isVisible());
        EXPECT_FALSE(scroll->isVisible());
        screenshot("mission-narrow-sequence");
        // Save returns to the invalid field rather than losing the hidden draft.
        ok->click();
        EXPECT_EQ(navigation->currentIndex(), 1);
        EXPECT_TRUE(text(*dv)->hasFocus());
        settled();
        EXPECT_TRUE(scroll->viewport()->rect().contains(
            text(*dv)->mapTo(scroll->viewport(), text(*dv)->rect().center())));
        text(*dv)->setText("0.12");
        dialog->resize(980, 740);
        settled();
        EXPECT_FALSE(navigation->isVisible());
        EXPECT_TRUE(list->isVisible());
        EXPECT_TRUE(scroll->isVisible());
        EXPECT_DOUBLE_EQ(dv->value(Unit::metres_per_second), 120);
        screenshot("mission-wide-restored");
        // List shortcuts reorder only the selected segment; Enter opens parameters,
        // without accidentally accepting the whole plan through QDialog's default button.
        const auto key = [&](int value, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            QKeyEvent press(QEvent::KeyPress, value, modifiers);
            QKeyEvent release(QEvent::KeyRelease, value, modifiers);
            QApplication::sendEvent(list, &press);
            QApplication::sendEvent(list, &release);
        };
        list->setFocus();
        key(Qt::Key_Up, Qt::AltModifier);
        EXPECT_EQ(list->currentRow(), 0);
        EXPECT_EQ(list->item(0)->text(), QString(300, 'A'));
        key(Qt::Key_Down, Qt::AltModifier);
        EXPECT_EQ(list->currentRow(), 1);
        navigation->setCurrentIndex(0);
        key(Qt::Key_Return);
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(navigation->currentIndex(), 1);
        key(Qt::Key_Delete);
        EXPECT_EQ(list->count(), 2);
        while (list->count() > 0)
            dialog->findChild<QPushButton *>("missionRemove")->click();
        EXPECT_TRUE(dialog->findChild<QLabel *>("missionEmpty")->isVisible());
        EXPECT_FALSE(dialog->findChild<QLineEdit *>("missionSegmentName")->isVisible());
        EXPECT_FALSE(navigation->isTabEnabled(1));
        EXPECT_FALSE(dialog->findChild<QPushButton *>("missionMoveUp")->isEnabled());
        EXPECT_FALSE(dialog->findChild<QPushButton *>("missionRemove")->isEnabled());
        settled();
        screenshot("mission-empty");
        dialog->findChild<QPushButton *>("missionAddCoast")->click();
        EXPECT_EQ(list->count(), 1);
        EXPECT_TRUE(navigation->isTabEnabled(1));
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isHidden());
        dialog->reject();
    });
    EXPECT_FALSE(edit_mission(plan, hpop, core::Epoch{0}, core::Epoch{0},
                              core::Epoch{7'200'000'000'000}, &parent));
    EXPECT_EQ(plan, original);
}
TEST_F(QuantityTest, HpopSettingsValidateCancelAndRoundTripWithoutApplyingTheOrbit) {
    OrbitInput input;
    input.propagator = OrbitPropagator::hpop;
    OrbitEditor editor(input);
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    editor.resize(1100, 740);
    editor.show();
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput value) { submitted = std::move(value); };
    const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("hpopSettingsDialog");
        ASSERT_NE(dialog, nullptr);
        auto *degree = dialog->findChild<QSpinBox *>("hpopGravityDegree");
        auto *order = dialog->findChild<QSpinBox *>("hpopGravityOrder");
        auto *tabs = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        ASSERT_NE(degree, nullptr);
        ASSERT_NE(order, nullptr);
        degree->setValue(4);
        EXPECT_EQ(order->value(), 4);
        if (!capture.isEmpty()) {
            ASSERT_TRUE(QDir{}.mkpath(capture));
            QApplication::processEvents();
            EXPECT_TRUE(dialog->grab().save(capture + "/hpop-forces.png"));
            tabs->setCurrentIndex(1);
            QApplication::processEvents();
            EXPECT_TRUE(dialog->grab().save(capture + "/hpop-integration.png"));
            dialog->resize(400, 460);
            QApplication::processEvents();
            EXPECT_TRUE(dialog->grab().save(capture + "/hpop-compact.png"));
        }
        auto *minimum = dialog->findChild<QuantityEditor *>("hpopMinStep");
        auto *maximum = dialog->findChild<QuantityEditor *>("hpopMaxStep");
        minimum->set_value(50, Unit::second);
        maximum->set_value(10, Unit::second);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
        minimum->set_value(.01, Unit::second);
        maximum->set_value(30, Unit::second);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    editor.open_propagator_details();
    EXPECT_FALSE(submitted);
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_EQ(submitted->hpop.gravity_degree, 4);
    EXPECT_EQ(submitted->hpop.gravity_order, 4);
    EXPECT_EQ(submitted->hpop.max_step_seconds, 30);
    const auto saved = *submitted;
    editor.set_input(saved);
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("hpopSettingsDialog");
        ASSERT_NE(dialog, nullptr);
        EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopGravityDegree")->value(), 4);
        dialog->findChild<QComboBox *>("hpopIntegrator")->setCurrentIndex(1);
        dialog->reject();
    });
    editor.open_propagator_details();
    editor.submit();
    EXPECT_EQ(submitted->hpop, saved.hpop);
    EXPECT_NEAR(submitted->initial.position_m[0], saved.initial.position_m[0], 1e-7);
    if (!capture.isEmpty()) {
        QApplication::processEvents();
        EXPECT_TRUE(editor.grab().save(capture + "/hpop-editor-wide.png"));
        editor.resize(760, 540);
        QEventLoop settled;
        QTimer::singleShot(50, &settled, &QEventLoop::quit);
        settled.exec(); // Allow queued responsive layouts to settle before the capture.
        EXPECT_TRUE(editor.grab().save(capture + "/hpop-editor-compact.png"));
    }
}
TEST_F(QuantityTest, HpopUsesInstalledGravityLimitsAndPersistsSelectedModel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    runtime::CatalogGravity entry;
    entry.selection = "egm2008";
    entry.info = {"EGM2008", {}, {}, "tide_free", core::egm96_mu, core::egm96_radius_m, 2190, 2159};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *model = dialog->findChild<QComboBox *>("hpopGravityModel");
        model->setCurrentIndex(model->findData(QStringLiteral("egm2008")));
        auto *degree = dialog->findChild<QSpinBox *>("hpopGravityDegree");
        auto *order = dialog->findChild<QSpinBox *>("hpopGravityOrder");
        EXPECT_EQ(degree->maximum(), 2190);
        degree->setValue(2190);
        EXPECT_EQ(order->maximum(), 2159);
        order->setValue(2190);
        EXPECT_EQ(order->value(), 2159);
        degree->setValue(720);
        EXPECT_EQ(order->maximum(), 720);
        EXPECT_EQ(order->value(), 720);
        degree->setValue(2190);
        order->setValue(2159);
        degree->setFocus();
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(degree, &down);
        EXPECT_EQ(degree->value(), 2189);
        QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QApplication::sendEvent(degree, &up);
        EXPECT_EQ(degree->value(), 2190);
        const auto output = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        if (!output.isEmpty()) {
            QDir{}.mkpath(output);
            for (const int width : {720, 430}) {
                dialog->resize(width, 700);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                EXPECT_EQ(degree->value(), 2190);
                EXPECT_EQ(order->value(), 2159);
                EXPECT_TRUE(
                    dialog->grab().save(output + QString("/full-gravity-%1.png").arg(width)));
                std::cout << "Full gravity UI width=" << dialog->width()
                          << " DPR=" << dialog->devicePixelRatioF() << '\n';
            }
        }
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {entry}));
    EXPECT_EQ(options.gravity_model, "egm2008");
    EXPECT_EQ(options.gravity_degree, 2190);
    EXPECT_EQ(options.gravity_order, 2159);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopGravityDegree")->value(), 2190);
        EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopGravityOrder")->value(), 2159);
        dialog->findChild<QSpinBox *>("hpopGravityDegree")->setValue(720);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {entry}));
    EXPECT_EQ(options, saved);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}
TEST_F(QuantityTest, HpopThirdBodiesPreserveDraftsAndValidateManualConstants) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    bool managed = false;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *tabs = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        ASSERT_TRUE(tabs);
        tabs->setCurrentIndex(0);
        auto *sun = dialog->findChild<QCheckBox *>("hpopSunGravity");
        auto *moon = dialog->findChild<QCheckBox *>("hpopMoonGravity");
        auto *source = dialog->findChild<QComboBox *>("hpopSunSource");
        auto *quantity = dialog->findChild<QuantityEditor *>("hpopSunMu");
        auto *mu = quantity->findChild<QLineEdit *>("quantityValue");
        ASSERT_TRUE(sun && moon && source && mu);
        EXPECT_EQ(source->currentIndex(), 1);
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopMoonSource")->currentIndex(), 1);
        EXPECT_FALSE(options.third_body(core::ThirdBody::sun).user_mu ||
                     options.third_body(core::ThirdBody::moon).user_mu);
        EXPECT_FALSE(source->isEnabled());
        sun->setChecked(true);
        moon->setChecked(true);
        EXPECT_TRUE(mu->isReadOnly());
        const auto constant = quantity->value(Unit::metres_cubed_per_second_squared);
        QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QApplication::sendEvent(mu, &up);
        EXPECT_EQ(quantity->value(Unit::metres_cubed_per_second_squared), constant);
        quantity->set_unit(Unit::metres_cubed_per_second_squared);
        EXPECT_EQ(quantity->value(Unit::metres_cubed_per_second_squared), constant);
        quantity->set_unit(Unit::kilometres_cubed_per_second_squared);
        source->setCurrentIndex(2);
        mu->setText("1.3271244e11");
        source->setCurrentIndex(1);
        EXPECT_TRUE(mu->isReadOnly());
        source->setCurrentIndex(2);
        EXPECT_EQ(mu->text(), QStringLiteral("1.3271244e11"));
        dialog->findChild<QToolButton *>("hpopManageEphemerides")->click();
        EXPECT_TRUE(managed);
        EXPECT_EQ(mu->text(), QStringLiteral("1.3271244e11"));
        EXPECT_TRUE(dialog->findChild<QLabel *>("hpopEphemerisStatus")->text().contains("2000"));
        const auto output = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        if (!output.isEmpty()) {
            source->setCurrentIndex(1); // Preview the normal, automatic-GM configuration.
            QDir{}.mkpath(output);
            auto *gravity_scroll = dialog->findChild<QScrollArea *>("hpopGravityPage");
            gravity_scroll->ensureWidgetVisible(
                dialog->findChild<QTableWidget *>("hpopThirdBodyTable"));
            QApplication::processEvents();
            EXPECT_TRUE(dialog->grab().save(output + "/third-body-wide.png"));
            dialog->resize(460, 680);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_TRUE(dialog->grab().save(output + "/third-body-compact.png"));
            source->setCurrentIndex(2);
        }
        mu->setText("0");
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        mu->setText("1.3271244e11");
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {}, [&] {
        managed = true;
        runtime::CatalogEphemeris entry;
        entry.selection = "de440s";
        entry.info.first = core::Epoch{0};
        entry.info.last = core::Epoch{86400000000000};
        return std::vector{entry};
    }));
    EXPECT_TRUE(options.third_body(core::ThirdBody::sun).enabled &&
                options.third_body(core::ThirdBody::moon).enabled);
    ASSERT_TRUE(options.third_body(core::ThirdBody::sun).user_mu);
    EXPECT_EQ(*options.third_body(core::ThirdBody::sun).user_mu, 1.3271244e20);
    EXPECT_FALSE(options.third_body(core::ThirdBody::moon).user_mu);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_TRUE(dialog->findChild<QCheckBox *>("hpopSunGravity")->isChecked());
        dialog->findChild<QCheckBox *>("hpopMoonGravity")->setChecked(false);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}
TEST_F(QuantityTest, HpopPlanetTableSearchAndSourceSwitchingPreserveCbAndManualDrafts) {
    core::HpopOptions options;
    auto &jupiter = options.third_body(core::ThirdBody::jupiter);
    jupiter = {true, core::ThirdBodyGravitySource::cb_file, 1.1e17,
               core::CbGravity{1.26712764e17, "Jupiter.cb", std::string(64, 'a'),
                               core::ThirdBody::jupiter}};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *table = dialog->findChild<QTableWidget *>("hpopThirdBodyTable");
        ASSERT_TRUE(table);
        EXPECT_EQ(table->rowCount(), 10);
        EXPECT_EQ(table->columnCount(), 4);
        auto *search = dialog->findChild<QLineEdit *>("hpopThirdBodySearch");
        search->setText("Jupiter");
        EXPECT_FALSE(table->isRowHidden(2));
        EXPECT_TRUE(table->isRowHidden(0));
        auto *source = dialog->findChild<QComboBox *>("hpopJupiterSource");
        auto *gm = dialog->findChild<QuantityEditor *>("hpopJupiterMu");
        EXPECT_EQ(source->currentIndex(), 0);
        EXPECT_EQ(gm->value(Unit::metres_cubed_per_second_squared), 1.26712764e17);
        source->setCurrentIndex(2);
        auto *input = gm->findChild<QLineEdit *>("quantityValue");
        input->setText("1.25e8");
        source->setCurrentIndex(1);
        EXPECT_NEAR(gm->value(Unit::metres_cubed_per_second_squared), runtime::de440_planet_mu[0],
                    1000);
        source->setCurrentIndex(0);
        EXPECT_EQ(gm->value(Unit::metres_cubed_per_second_squared), 1.26712764e17);
        source->setCurrentIndex(2);
        EXPECT_EQ(input->text(), QStringLiteral("1.25e8"));
        dialog->resize(460, 680);
        EXPECT_EQ(input->text(), QStringLiteral("1.25e8"));
        dialog->findChild<QCheckBox *>("hpopPlutoGravity")->setChecked(true);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(jupiter.source, core::ThirdBodyGravitySource::user_specified);
    EXPECT_EQ(*jupiter.user_mu, 1.25e17);
    ASSERT_TRUE(jupiter.cb);
    EXPECT_EQ(jupiter.cb->mu, 1.26712764e17);
    EXPECT_TRUE(options.third_body(core::ThirdBody::pluto).enabled);
}
TEST_F(QuantityTest, HpopSourceSwitchingKeepsExactSiAndRecoversAnUnavailableCbDisplay) {
    core::HpopOptions options;
    auto &sun = options.third_body(core::ThirdBody::sun);
    const auto exact = std::nextafter(runtime::de440_sun_mu, 0.0);
    sun = {true, core::ThirdBodyGravitySource::user_specified, exact, std::nullopt};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *source = dialog->findChild<QComboBox *>("hpopSunSource");
        auto *gm = dialog->findChild<QuantityEditor *>("hpopSunMu");
        gm->set_unit(Unit::metres_cubed_per_second_squared);
        source->setCurrentIndex(1);
        gm->set_unit(Unit::kilometres_cubed_per_second_squared);
        source->setCurrentIndex(0); // Missing Cb constants display a nonnumeric placeholder.
        source->setCurrentIndex(2);
        EXPECT_EQ(gm->unit(), Unit::metres_cubed_per_second_squared);
        EXPECT_EQ(gm->value(Unit::metres_cubed_per_second_squared), exact);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(*sun.user_mu, exact);
}
TEST_F(QuantityTest, HpopNestedPagesPreserveDraftsAndKeepUnavailableModelsInactive) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        ASSERT_TRUE(outer && forces);
        EXPECT_EQ(outer->count(), 3);
        EXPECT_EQ(outer->tabText(0), QStringLiteral("力模型"));
        EXPECT_EQ(outer->tabText(1), QStringLiteral("积分器"));
        EXPECT_EQ(outer->tabText(2), QStringLiteral("协方差"));
        EXPECT_EQ(forces->count(), 4);
        EXPECT_EQ(forces->tabText(0), QStringLiteral("引力"));
        EXPECT_EQ(forces->tabText(1), QStringLiteral("阻力"));
        EXPECT_EQ(forces->tabText(2), QStringLiteral("太阳辐射压"));
        EXPECT_EQ(forces->tabText(3), QStringLiteral("其他"));
        auto *gravity = dialog->findChild<QScrollArea *>("hpopGravityPage");
        EXPECT_TRUE(
            gravity->isAncestorOf(dialog->findChild<QFrame *>("hpopCentralGravitySection")));
        EXPECT_TRUE(gravity->isAncestorOf(dialog->findChild<QTableWidget *>("hpopThirdBodyTable")));
        auto *integration = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
        auto *altitude = dialog->findChild<QuantityEditor *>("hpopMinimumAltitude");
        EXPECT_TRUE(integration->isAncestorOf(altitude));
        auto *sun = dialog->findChild<QuantityEditor *>("hpopSunMu");
        dialog->findChild<QCheckBox *>("hpopSunGravity")->setChecked(true);
        dialog->findChild<QComboBox *>("hpopSunSource")->setCurrentIndex(2);
        sun->set_unit(Unit::metres_cubed_per_second_squared);
        auto *draft = sun->findChild<QLineEdit *>("quantityValue");
        draft->setText("1.23456789e20");
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        const auto screenshot = [&](const QString &name) {
            QApplication::processEvents();
            if (!capture.isEmpty()) {
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                QDir{}.mkpath(capture);
                EXPECT_TRUE(dialog->grab().save(capture + "/" + name + ".png"));
            }
        };
        screenshot("hpop-gravity");
        for (int index = 1; index < 4; ++index) {
            forces->setCurrentIndex(index);
            if (index == 2) {
                EXPECT_FALSE(dialog->findChild<QCheckBox *>("hpopUseSrp")->isChecked());
                EXPECT_FALSE(dialog->findChild<QuantityEditor *>("hpopSrpCr")->isEnabled());
            } else if (index == 1) {
                EXPECT_FALSE(dialog->findChild<QCheckBox *>("hpopUseDrag")->isChecked());
                EXPECT_FALSE(dialog->findChild<QuantityEditor *>("hpopDragCd")->isEnabled());
            } else {
                EXPECT_EQ(forces->currentWidget()->findChildren<QCheckBox *>().size(), 1);
                EXPECT_FALSE(
                    dialog->findChild<QCheckBox *>("hpopRelativisticAccelerations")->isChecked());
                EXPECT_TRUE(
                    forces->currentWidget()->findChild<QWidget *>("hpopAdditionalPendingSection"));
            }
            screenshot(QStringLiteral("hpop-force-%1").arg(index));
        }
        outer->setCurrentIndex(2);
        EXPECT_TRUE(outer->currentWidget()->findChildren<QCheckBox *>().isEmpty());
        screenshot("hpop-covariance");
        outer->setCurrentIndex(1);
        altitude->set_unit(Unit::kilometre);
        altitude->set_value(10, Unit::kilometre);
        screenshot("hpop-integrator");
        outer->setCurrentIndex(0);
        EXPECT_EQ(forces->currentIndex(), 3);
        forces->setCurrentIndex(0);
        dialog->resize(460, 680);
        screenshot("hpop-gravity-compact");
        EXPECT_EQ(dialog->findChild<QuantityEditor *>("hpopSunMu"), sun);
        EXPECT_EQ(draft->text(), QStringLiteral("1.23456789e20"));
        EXPECT_EQ(sun->unit(), Unit::metres_cubed_per_second_squared);
        EXPECT_EQ(altitude->value(Unit::metre), 10000);
        outer->setCurrentIndex(2);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options.minimum_altitude_m, 10000);
    EXPECT_EQ(*options.third_body(core::ThirdBody::sun).user_mu, 1.23456789e20);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QTabWidget *>("hpopForceModelTabs")->setCurrentIndex(2);
        dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(2);
        dialog->findChild<QSpinBox *>("hpopGravityDegree")->setValue(2);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}
TEST_F(QuantityTest, HpopValidationReturnsToTheOffscreenFieldAndDoesNotCommitTheDraft) {
    core::HpopOptions options;
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        auto *source = dialog->findChild<QComboBox *>("hpopSunSource");
        dialog->findChild<QCheckBox *>("hpopSunGravity")->setChecked(true);
        source->setCurrentIndex(2);
        auto *mu = dialog->findChild<QuantityEditor *>("hpopSunMu")
                       ->findChild<QLineEdit *>("quantityValue");
        mu->setText("0");
        auto *search = dialog->findChild<QLineEdit *>("hpopThirdBodySearch");
        search->setText("Mars");
        forces->setCurrentIndex(2);
        outer->setCurrentIndex(2);
        auto *accept = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        accept->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(options, saved);
        EXPECT_EQ(outer->currentIndex(), 0);
        EXPECT_EQ(forces->currentIndex(), 0);
        EXPECT_TRUE(search->text().isEmpty());
        EXPECT_FALSE(dialog->findChild<QTableWidget *>("hpopThirdBodyTable")->isRowHidden(0));
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
        source->setCurrentIndex(1);
        dialog->findChild<QuantityEditor *>("hpopMinStep")->set_value(50, Unit::second);
        dialog->findChild<QuantityEditor *>("hpopMaxStep")->set_value(10, Unit::second);
        outer->setCurrentIndex(2);
        accept->click();
        EXPECT_EQ(options, saved);
        EXPECT_EQ(outer->currentIndex(), 1);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

namespace {
void settle_hpop_catalog(EopCatalog &catalog) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (catalog.downloading() && elapsed.elapsed() < 5000)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_FALSE(catalog.downloading()) << catalog.status().toStdString();
    QEventLoop settled;
    QTimer::singleShot(150, &settled, &QEventLoop::quit);
    settled.exec();
}
} // namespace
TEST_F(QuantityTest, HpopSrpUnitsShadowAssignmentsResizeAndCancelPreserveExactDrafts) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.srp.reference_distance_m = 149597870700.12347;
    const auto exact_distance = options.srp.reference_distance_m;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    entry.info.first = core::Epoch{0};
    entry.info.last = core::Epoch{86400000000000};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        forces->setCurrentIndex(2);
        auto *use = dialog->findChild<QCheckBox *>("hpopUseSrp");
        auto *area = dialog->findChild<QuantityEditor *>("hpopSrpAreaMassRatio");
        auto *altitude = dialog->findChild<QuantityEditor *>("hpopSrpAtmosphericAltitude");
        auto *shadow = dialog->findChild<QComboBox *>("hpopSrpShadowModel");
        auto *select_bodies = dialog->findChild<QPushButton *>("hpopSrpSelectBodies");
        ASSERT_TRUE(select_bodies);
        auto *sun = dialog->findChild<QComboBox *>("hpopSrpSunMethod");
        ASSERT_TRUE(sun);
        EXPECT_EQ(sun->count(), 3);
        EXPECT_EQ(sun->currentIndex(), 1);
        use->setChecked(true);
        auto *boundary = dialog->findChild<QCheckBox *>("hpopSrpBoundaryMitigation");
        ASSERT_TRUE(boundary);
        EXPECT_FALSE(boundary->isChecked());
        boundary->setChecked(true);
        EXPECT_TRUE(sun->isVisible());
        sun->setFocus();
        QKeyEvent next(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(sun, &next);
        EXPECT_EQ(sun->currentIndex(), 2);
        area->set_unit(Unit::square_centimetres_per_gram);
        area->findChild<QLineEdit *>("quantityValue")->setText("0.3");
        altitude->set_unit(Unit::kilometre);
        altitude->set_value(23, Unit::kilometre);
        shadow->setCurrentIndex(0);
        EXPECT_FALSE(boundary->isEnabled());
        EXPECT_TRUE(boundary->isChecked());
        EXPECT_FALSE(select_bodies->isEnabled());
        EXPECT_FALSE(altitude->isEnabled());
        shadow->setCurrentIndex(2);
        EXPECT_TRUE(boundary->isEnabled());
        QTimer::singleShot(0, [&] {
            auto *selection = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(selection);
            auto *assigned = selection->findChild<QListWidget *>("hpopSrpAssignedBodies");
            ASSERT_TRUE(assigned);
            for (int i = 0; i < assigned->count(); ++i)
                if (assigned->item(i)->data(Qt::UserRole).toInt() == 1)
                    assigned->setCurrentItem(assigned->item(i));
            selection->findChild<QToolButton *>("hpopSrpRemoveBody")->click();
            selection->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        select_bodies->click();
        EXPECT_TRUE(altitude->isEnabled());
        EXPECT_FALSE(dialog->findChild<QuantityEditor *>("hpopSrpMoonRadius")->isEnabled());
        EXPECT_FALSE(dialog->findChild<QLabel *>("hpopSrpEphemerisStatus")
                         ->text()
                         .contains(QStringLiteral("已安装")));
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        std::cout << "HPOP SRP device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_EQ(sun, dialog->findChild<QComboBox *>("hpopSrpSunMethod"));
            EXPECT_EQ(sun->currentIndex(), 2);
            EXPECT_EQ(area, dialog->findChild<QuantityEditor *>("hpopSrpAreaMassRatio"));
            EXPECT_EQ(area->findChild<QLineEdit *>("quantityValue")->text(), "0.3");
            EXPECT_NEAR(area->value(Unit::square_metres_per_kilogram), .03, 1e-16);
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            EXPECT_TRUE(dialog->rect().contains(buttons->geometry()));
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(dialog->grab().save(capture + QString("/hpop-srp-%1.png").arg(width)));
                auto *scroll = dialog->findChild<QScrollArea *>("hpopSrpPage");
                scroll->ensureWidgetVisible(sun);
                QApplication::processEvents();
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-srp-sun-%1.png").arg(width)));
                sun->setCurrentIndex(1);
                QApplication::processEvents();
                EXPECT_TRUE(dialog->grab().save(
                    capture + QString("/hpop-srp-sun-default-%1.png").arg(width)));
                sun->setCurrentIndex(2);
                auto *radius = dialog->findChild<QuantityEditor *>("hpopSrpMoonRadius");
                reveal(radius);
                scroll->ensureWidgetVisible(radius);
                QApplication::processEvents();
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-srp-data-%1.png").arg(width)));
                scroll->verticalScrollBar()->setValue(0);
            }
        }
        outer->setCurrentIndex(1);
        outer->setCurrentIndex(0);
        EXPECT_EQ(forces->currentIndex(), 2);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_TRUE(options.srp.enabled);
    EXPECT_TRUE(options.srp.boundary_mitigation);
    EXPECT_EQ(options.srp.sun_position_method, core::SrpSunPositionMethod::apparent);
    EXPECT_NEAR(options.srp.area_mass_ratio_m2_per_kg, .03, 1e-16);
    EXPECT_EQ(options.srp.atmospheric_altitude_m, 23000);
    EXPECT_FALSE(options.srp.moon_shadow);
    EXPECT_EQ(options.srp.reference_distance_m, exact_distance);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_TRUE(dialog->findChild<QCheckBox *>("hpopUseSrp")->isChecked());
        auto *sun = dialog->findChild<QComboBox *>("hpopSrpSunMethod");
        EXPECT_EQ(sun->currentIndex(), 2);
        sun->setCurrentIndex(0);
        dialog->findChild<QCheckBox *>("hpopUseSrp")->setChecked(false);
        dialog->findChild<QuantityEditor *>("hpopSrpCr")->set_value(2, Unit::one);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopSrpInvalidFieldAndMissingEphemerisReturnToSrpWithoutCommitting) {
    core::HpopOptions options;
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        auto *cr = dialog->findChild<QuantityEditor *>("hpopSrpCr")
                       ->findChild<QLineEdit *>("quantityValue");
        auto *use = dialog->findChild<QCheckBox *>("hpopUseSrp");
        auto *accept = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        use->setChecked(true);
        cr->setText("101");
        outer->setCurrentIndex(2);
        accept->click();
        EXPECT_EQ(options, saved);
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(outer->currentIndex(), 0);
        EXPECT_EQ(forces->currentIndex(), 2);
        cr->setText("1.7");
        outer->setCurrentIndex(2);
        accept->click();
        EXPECT_EQ(options, saved);
        EXPECT_EQ(outer->currentIndex(), 0);
        EXPECT_EQ(forces->currentIndex(), 2);
        EXPECT_TRUE(
            dialog->findChild<QLabel *>("errorText")->text().contains(QStringLiteral("星历")));
        use->setChecked(false);
        cr->setText("invalid"); // Inactive invalid text must not destroy the last valid draft.
        accept->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopDragUnitsModelsResizeSaveReopenAndCancel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    entry.info.first = core::Epoch{0};
    entry.info.last = core::Epoch{86400000000000};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        forces->setCurrentIndex(1);
        auto *use = dialog->findChild<QCheckBox *>("hpopUseDrag");
        auto *area = dialog->findChild<QuantityEditor *>("hpopDragAreaMassRatio");
        auto *blend = dialog->findChild<QuantityEditor *>("hpopDragBlendingRange");
        auto *model = dialog->findChild<QComboBox *>("hpopDragDensityModel");
        auto *low = dialog->findChild<QComboBox *>("hpopDragLowAltitudeModel");
        EXPECT_FALSE(area->isEnabled());
        use->setChecked(true);
        EXPECT_TRUE(dialog->findChild<QCheckBox *>("hpopDragApproximateAltitude")->isVisible());
        area->set_unit(Unit::square_centimetres_per_gram);
        area->findChild<QLineEdit *>("quantityValue")->setText("0.3");
        low->setCurrentIndex(1);
        EXPECT_TRUE(blend->isEnabled());
        blend->set_value(42, Unit::kilometre);
        model->setCurrentIndex(1);
        EXPECT_FALSE(low->isEnabled());
        EXPECT_FALSE(blend->isEnabled());
        model->setCurrentIndex(0);
        EXPECT_EQ(low->currentIndex(), 1);
        EXPECT_EQ(blend->value(Unit::metre), 42000);
        dialog->findChild<QuantityEditor *>("hpopDragCd")->set_value(2.7, Unit::one);
        dialog->findChild<QuantityEditor *>("hpopDragF107")->set_value(180, Unit::one);
        dialog->findChild<QuantityEditor *>("hpopDragF107Average")->set_value(160, Unit::one);
        dialog->findChild<QuantityEditor *>("hpopDragKp")->set_value(4.3, Unit::one);
        dialog->findChild<QCheckBox *>("hpopDragApparentSun")->setChecked(true);
        dialog->findChild<QCheckBox *>("hpopDragApproximateAltitude")->setChecked(true);
        EXPECT_FALSE(dialog->findChild<QLabel *>("hpopDragEphemerisStatus")
                         ->text()
                         .contains(QStringLiteral("已安装")));
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        std::cout << "HPOP drag device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_EQ(area, dialog->findChild<QuantityEditor *>("hpopDragAreaMassRatio"));
            EXPECT_EQ(area->findChild<QLineEdit *>("quantityValue")->text(), "0.3");
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            EXPECT_TRUE(dialog->rect().contains(buttons->geometry()));
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                auto *scroll = dialog->findChild<QScrollArea *>("hpopDragPage");
                scroll->verticalScrollBar()->setValue(0);
                QApplication::processEvents();
                EXPECT_TRUE(dialog->grab().save(capture + QString("/hpop-drag-%1.png").arg(width)));
                auto *approximate = dialog->findChild<QCheckBox *>("hpopDragApproximateAltitude");
                EXPECT_TRUE(approximate->isVisible());
                scroll->ensureWidgetVisible(approximate);
                approximate->setFocus();
                const auto press_space = [&] {
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
                    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
                    QApplication::sendEvent(approximate, &press);
                    QApplication::sendEvent(approximate, &release);
                };
                press_space();
                EXPECT_FALSE(approximate->isChecked());
                press_space();
                EXPECT_TRUE(approximate->isChecked());
                QApplication::processEvents();
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-drag-data-%1.png").arg(width)));
            }
        }
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_TRUE(options.drag.enabled);
    EXPECT_EQ(options.drag.cd, 2.7);
    EXPECT_NEAR(options.drag.area_mass_ratio_m2_per_kg, .03, 1e-16);
    EXPECT_EQ(options.drag.blending_range_m, 42000);
    EXPECT_EQ(options.drag.f107, 180);
    EXPECT_EQ(options.drag.f107_average, 160);
    EXPECT_EQ(options.drag.kp, 4.3);
    EXPECT_TRUE(options.drag.use_apparent_sun_position);
    EXPECT_TRUE(options.drag.use_approximate_altitude);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_TRUE(dialog->findChild<QCheckBox *>("hpopUseDrag")->isChecked());
        EXPECT_TRUE(dialog->findChild<QCheckBox *>("hpopDragApproximateAltitude")->isChecked());
        dialog->findChild<QCheckBox *>("hpopDragApproximateAltitude")->setChecked(false);
        EXPECT_EQ(dialog->findChild<QuantityEditor *>("hpopDragF107")->value(Unit::one), 180);
        dialog->findChild<QCheckBox *>("hpopUseDrag")->setChecked(false);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopDragErrorsReturnToDragAndDoNotCommitInvalidOrMissingData) {
    core::HpopOptions options;
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        auto *kp = dialog->findChild<QuantityEditor *>("hpopDragKp")
                       ->findChild<QLineEdit *>("quantityValue");
        auto *use = dialog->findChild<QCheckBox *>("hpopUseDrag");
        auto *accept = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        use->setChecked(true);
        kp->setText("10");
        outer->setCurrentIndex(2);
        accept->click();
        EXPECT_EQ(options, saved);
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(outer->currentIndex(), 0);
        EXPECT_EQ(forces->currentIndex(), 1);
        kp->setText("3");
        outer->setCurrentIndex(2);
        accept->click();
        EXPECT_EQ(options, saved);
        EXPECT_EQ(forces->currentIndex(), 1);
        EXPECT_TRUE(
            dialog->findChild<QLabel *>("errorText")->text().contains(QStringLiteral("星历")));
        use->setChecked(false);
        kp->setText("invalid");
        accept->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopNewDesktopDefaultsUseEgm2008AndKeepExplicitModelChoices) {
    OrbitEditor editor(OrbitInput{});
    runtime::CatalogGravity entry;
    entry.selection = "egm2008";
    entry.info = {"EGM2008", {}, {}, "tide_free", core::egm96_mu, 6378137, 2190, 2159};
    editor.gravity_models = [entry] { return std::vector{entry}; };
    editor.set_propagator(OrbitPropagator::hpop);
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("hpopSettingsDialog");
        ASSERT_TRUE(dialog);
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopGravityModel")->currentData().toString(),
                  "egm2008");
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopGravityModel")->currentText(), "EGM2008");
        EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopGravityDegree")->value(), 21);
        EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopGravityOrder")->value(), 21);
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        if (!capture.isEmpty()) {
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_TRUE(QDir{}.mkpath(capture));
            EXPECT_TRUE(dialog->grab().save(capture + "/hpop-egm2008.png"));
        }
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    editor.open_propagator_details();
    std::optional<OrbitInput> saved;
    editor.apply = [&](OrbitInput input) { saved = input; };
    editor.submit();
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->hpop.gravity_model, "egm2008");
    OrbitInput explicit_builtin;
    explicit_builtin.propagator = OrbitPropagator::hpop;
    editor.set_input(explicit_builtin);
    editor.set_propagator(OrbitPropagator::two_body);
    editor.set_propagator(OrbitPropagator::hpop);
    editor.submit();
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->hpop, explicit_builtin.hpop);
}
TEST_F(QuantityTest, HpopMissingGravityStartsOwnedDownloadAndCancelCloseCanRetryWithoutCommitting) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    auto options = new_hpop_options();
    const auto original = options;
    for (int attempt = 0; attempt < 2; ++attempt) {
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            auto *model = dialog->findChild<QComboBox *>("hpopGravityModel");
            EXPECT_EQ(model->currentData().toString(), "egm2008");
            EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopGravityDegree")->value(), 21);
            // Trigger the same request before processing network events: this test is offline.
            dialog->findChild<QPushButton *>("hpopDownloadGravity")->click();
            EXPECT_TRUE(catalog.downloading());
            EXPECT_EQ(catalog.active_product(), DataProduct::egm2008);
            EXPECT_TRUE(dialog->findChild<QProgressBar *>("hpopGravityProgress")->isVisible());
            EXPECT_FALSE(
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
            if (attempt == 1)
                dialog->findChild<QPushButton *>("hpopCancelGravityDownload")->click();
            dialog->findChild<QSpinBox *>("hpopGravityDegree")->setValue(30);
            dialog->reject();
        });
        EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
        EXPECT_EQ(options, original);
        settle_hpop_catalog(catalog);
        EXPECT_EQ(catalog.update_status(DataProduct::egm2008).state, DataUpdateState::cancelled);
        EXPECT_TRUE(catalog.data().gravity_models().empty());
    }
}
TEST_F(QuantityTest, HpopGravityFailureIsVisibleAndNeverRetriesOrFallsBackAutomatically) {
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    QTcpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost));
    int requests = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            socket->readAll();
            ++requests;
            socket->write(
                "HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\ninvalid");
            socket->disconnectFromHost();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    });
    catalog.start_download(
        QUrl(QStringLiteral("http://127.0.0.1:%1/gravity").arg(server.serverPort())),
        DataProduct::egm2008);
    auto options = new_hpop_options();
    const auto original = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        // It observes the shared download without owning or cancelling it.
        EXPECT_TRUE(dialog->findChild<QPushButton *>("hpopCancelGravityDownload")->isHidden());
        settle_hpop_catalog(catalog);
        EXPECT_EQ(catalog.update_status(DataProduct::egm2008).state, DataUpdateState::failed);
        EXPECT_EQ(requests, 1);
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopGravityModel")->currentData().toString(),
                  "egm2008");
        EXPECT_TRUE(dialog->findChild<QLabel *>("hpopGravityStatus")
                        ->text()
                        .contains(QStringLiteral("重试")));
        EXPECT_TRUE(dialog->findChild<QPushButton *>("hpopDownloadGravity")->isVisible());
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(options, original);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_EQ(options, original);
    EXPECT_EQ(requests, 1);
}
TEST_F(QuantityTest, HpopQueuedGravityCanBeCancelledWithoutCancellingAnUnrelatedDownload) {
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    QTcpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost));
    catalog.start_download(
        QUrl(QStringLiteral("http://127.0.0.1:%1/pending").arg(server.serverPort())));
    auto options = new_hpop_options();
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QPushButton *>("hpopDownloadGravity")->click();
        EXPECT_TRUE(dialog->findChild<QLabel *>("hpopGravityStatus")
                        ->text()
                        .contains(QStringLiteral("等待")));
        dialog->findChild<QPushButton *>("hpopCancelGravityDownload")->click();
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_TRUE(catalog.downloading());
    EXPECT_EQ(catalog.active_product(), DataProduct::finals2000a);
    catalog.cancel_download();
    settle_hpop_catalog(catalog);
}
TEST_F(QuantityTest,
       HpopBackgroundGravityPublicationRefreshesLimitsAndCachedReopenDoesNotDownload) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    EopCatalog catalog(root / "data", {}, {}, false);
    QFile file(temporary.path() + "/local.grv");
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("stk.v.6.0\nModel Local\nDegree 21\nOrder 12\nGm 3.95e14\nRefDistance 6378137\n"
               "Normalized Yes\nIncludesPermTide No\nBEGIN Coefficients\n2 0 -0.000484 0\nEND "
               "Coefficients\n");
    file.close();
    catalog.import_raw(root / "local.grv", DataProduct::custom_gravity);
    core::HpopOptions options;
    options.gravity_model = "custom";
    const auto original = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        settle_hpop_catalog(catalog);
        auto *degree = dialog->findChild<QSpinBox *>("hpopGravityDegree");
        EXPECT_EQ(degree->maximum(), 21);
        degree->setValue(21);
        dialog->findChild<QSpinBox *>("hpopGravityOrder")->setValue(12);
        EXPECT_TRUE(dialog->findChild<QLabel *>("hpopGravityStatus")->text().contains("21"));
        EXPECT_EQ(options, original);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_EQ(options, original);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_FALSE(catalog.downloading());
        EXPECT_TRUE(dialog->findChild<QPushButton *>("hpopDownloadGravity")->isHidden());
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopGravityModel")->currentText(),
                  QStringLiteral("Local · 外部文件"));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_EQ(options.gravity_mu, 3.95e14);
    EXPECT_FALSE(catalog.downloading());
}
TEST_F(QuantityTest, HpopWeatherFileSwitchesFieldsRetainsDraftsAndReopens) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    EopCatalog catalog(root / "data", {}, {}, false);
    QFile file(temporary.path() + "/weather.csv");
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(
        "DATE,BSRN,ND,KP1,KP2,KP3,KP4,KP5,KP6,KP7,KP8,KP_SUM,"
        "AP1,AP2,AP3,AP4,AP5,AP6,AP7,AP8,AP_AVG,CP,C9,ISN,F10.7_OBS,F10.7_ADJ,"
        "F10.7_DATA_TYPE,F10.7_OBS_CENTER81,F10.7_OBS_LAST81,F10.7_ADJ_CENTER81,F10.7_ADJ_LAST81\n"
        "2000-01-01,1,1,30,30,30,30,30,30,30,30,240,15,15,15,15,15,15,15,15,15,0,0,0,150,150,OBS,"
        "150,150,150,150\n");
    file.close();
    catalog.data().install_space_weather(root / "weather.csv");
    std::ofstream cssi(root / "cssi.dat");
    cssi << "2000 1 2000 2 1999 12 2\n2000 1 170 150 20 15\n2000 2 180 160 22 16\n";
    cssi.close();
    catalog.data().install_space_weather(root / "cssi.dat", "cssi");
    core::HpopOptions options;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QTabWidget *>("hpopForceModelTabs")->setCurrentIndex(1);
        dialog->findChild<QCheckBox *>("hpopUseDrag")->setChecked(true);
        dialog->findChild<QPushButton *>("hpopDragManageEphemerides")->click();
        QTimer::singleShot(5000, dialog, &QDialog::reject);
        auto *source = dialog->findChild<QComboBox *>("hpopDragWeatherSource");
        auto *f107 = dialog->findChild<QuantityEditor *>("hpopDragF107");
        auto *selection = dialog->findChild<QComboBox *>("hpopDragWeatherFile");
        f107->set_value(183, Unit::one);
        EXPECT_TRUE(f107->isEnabled());
        EXPECT_FALSE(selection->isEnabled());
        source->setCurrentIndex(1);
        EXPECT_FALSE(f107->isEnabled());
        EXPECT_TRUE(selection->isEnabled());
        EXPECT_EQ(selection->currentData().toString(), "celestrak");
        EXPECT_FALSE(selection->currentText().contains(QStringLiteral("已安装")));
        auto *update = dialog->findChild<QComboBox *>("hpopDragGeomagneticUpdate");
        update->setFocus();
        QKeyEvent key(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
        QApplication::sendEvent(update, &key);
        EXPECT_EQ(update->count(), 4);
        EXPECT_EQ(update->currentIndex(), 3);
        QKeyEvent previous(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QApplication::sendEvent(update, &previous);
        EXPECT_EQ(update->currentIndex(), 2);
        EXPECT_TRUE(update->currentText().contains("Interp"));
        auto *geomagnetic_source = dialog->findChild<QComboBox *>("hpopDragGeomagneticSource");
        geomagnetic_source->setCurrentIndex(1);
        selection->setCurrentIndex(selection->findData("cssi"));
        EXPECT_EQ(geomagnetic_source->currentIndex(), 0);
        EXPECT_FALSE(geomagnetic_source->isEnabled());
        source->setCurrentIndex(0);
        EXPECT_EQ(f107->value(Unit::one), 183);
        source->setCurrentIndex(1);
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        std::cout << "HPOP weather device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (const int width : {720, 430}) {
            dialog->resize(width, 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_EQ(selection, dialog->findChild<QComboBox *>("hpopDragWeatherFile"));
            EXPECT_EQ(selection->currentData().toString(), "cssi");
            auto *scroll = dialog->findChild<QScrollArea *>("hpopDragPage");
            scroll->ensureWidgetVisible(geomagnetic_source);
            QApplication::processEvents();
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_TRUE(
                dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-weather-%1.png").arg(width)));
            }
        }
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    ASSERT_TRUE(edit_hpop_settings(
        options, nullptr, {}, {entry}, [entry] { return std::vector{entry}; }, &catalog));
    EXPECT_EQ(options.drag.weather_model, "cssi");
    EXPECT_EQ(options.drag.geomagnetic_update, core::GeomagneticUpdate::three_hourly_interpolated);
    EXPECT_EQ(options.drag.geomagnetic_source, core::GeomagneticSource::ap);
    EXPECT_EQ(options.drag.f107, 183);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopDragWeatherSource")->currentIndex(), 1);
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopDragGeomagneticUpdate")->currentIndex(), 2);
        auto *geomagnetic_source = dialog->findChild<QComboBox *>("hpopDragGeomagneticSource");
        EXPECT_EQ(geomagnetic_source->currentIndex(), 0);
        EXPECT_FALSE(geomagnetic_source->isEnabled());
        auto *file = dialog->findChild<QComboBox *>("hpopDragWeatherFile");
        file->setCurrentIndex(file->findData("celestrak"));
        EXPECT_TRUE(geomagnetic_source->isEnabled());
        geomagnetic_source->setCurrentIndex(1);
        dialog->findChild<QComboBox *>("hpopDragWeatherSource")->setCurrentIndex(0);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(
        options, nullptr, {}, {entry}, [entry] { return std::vector{entry}; }, &catalog));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopSolidTidesModesTruncationResizeReopenAndCancel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    entry.info.first = core::Epoch{0};
    entry.info.last = core::Epoch{86400000000000};
    EXPECT_EQ(new_hpop_options().solid_tides.mode, core::SolidTideMode::permanent);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *mode = dialog->findChild<QComboBox *>("hpopSolidTideMode");
        auto *truncate = dialog->findChild<QCheckBox *>("hpopSolidTideTruncate");
        auto *time = dialog->findChild<QCheckBox *>("hpopSolidTideTimeDependent");
        ASSERT_TRUE(time);
        ASSERT_TRUE(mode);
        ASSERT_TRUE(truncate);
        EXPECT_FALSE(time->isEnabled());
        EXPECT_FALSE(time->isChecked());
        EXPECT_EQ(mode->count(), 3);
        EXPECT_EQ(mode->currentIndex(), 0);
        EXPECT_FALSE(truncate->isEnabled());
        reveal(mode);
        mode->setFocus();
        QKeyEvent next(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(mode, &next);
        EXPECT_EQ(mode->currentIndex(), 1);
        EXPECT_TRUE(truncate->isEnabled());
        EXPECT_FALSE(time->isEnabled());
        mode->setCurrentIndex(2);
        EXPECT_TRUE(time->isEnabled());
        time->setChecked(true);
        truncate->setChecked(false);
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        std::cout << "HPOP solid tide device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            auto *scroll = dialog->findChild<QScrollArea *>("hpopGravityPage");
            scroll->ensureWidgetVisible(time);
            EXPECT_TRUE(time->isChecked());
            QApplication::processEvents();
            EXPECT_EQ(mode, dialog->findChild<QComboBox *>("hpopSolidTideMode"));
            EXPECT_EQ(mode->currentIndex(), 2);
            EXPECT_FALSE(truncate->isChecked());
            EXPECT_TRUE(mode->isVisible());
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            EXPECT_TRUE(dialog->rect().contains(buttons->geometry()));
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-solid-tides-%1.png").arg(width)));
            }
        }
        mode->setCurrentIndex(0);
        EXPECT_FALSE(time->isEnabled());
        EXPECT_TRUE(time->isChecked());
        EXPECT_FALSE(truncate->isEnabled());
        EXPECT_FALSE(truncate->isChecked());
        mode->setCurrentIndex(2);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options.solid_tides.mode, core::SolidTideMode::full);
    EXPECT_FALSE(options.solid_tides.truncate_to_gravity);
    EXPECT_TRUE(options.solid_tides.include_time_dependent_terms);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *mode = dialog->findChild<QComboBox *>("hpopSolidTideMode");
        auto *truncate = dialog->findChild<QCheckBox *>("hpopSolidTideTruncate");
        auto *time = dialog->findChild<QCheckBox *>("hpopSolidTideTimeDependent");
        ASSERT_TRUE(time);
        EXPECT_EQ(mode->currentIndex(), 2);
        EXPECT_FALSE(truncate->isChecked());
        EXPECT_TRUE(time->isChecked());
        mode->setCurrentIndex(1);
        EXPECT_FALSE(time->isEnabled());
        time->setChecked(false);
        truncate->setChecked(true);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopSolidTidesMissingEphemerisReturnsToGravityAndTruncatedPointMassNeedsNone) {
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *mode = dialog->findChild<QComboBox *>("hpopSolidTideMode");
        auto *truncate = dialog->findChild<QCheckBox *>("hpopSolidTideTruncate");
        auto *time = dialog->findChild<QCheckBox *>("hpopSolidTideTimeDependent");
        ASSERT_TRUE(time);
        EXPECT_FALSE(time->isEnabled());
        mode->setCurrentIndex(2);
        EXPECT_TRUE(time->isEnabled());
        time->setChecked(true);
        truncate->setChecked(false);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        outer->setCurrentIndex(2);
        auto *accept = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        accept->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(options, saved);
        EXPECT_EQ(outer->currentIndex(), 0);
        EXPECT_EQ(forces->currentIndex(), 0);
        EXPECT_TRUE(
            dialog->findChild<QLabel *>("errorText")->text().contains(QStringLiteral("固体潮")));
        truncate->setChecked(true);
        accept->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options.solid_tides.mode, core::SolidTideMode::full);
    EXPECT_TRUE(options.solid_tides.truncate_to_gravity);
    EXPECT_FALSE(options.uses_full_solid_tides());
    EXPECT_TRUE(options.solid_tides.include_time_dependent_terms);
    EXPECT_FALSE(options.uses_utc_force());
}

TEST_F(QuantityTest, HpopRelativityKeyboardResizeReopenAndCancel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    entry.info.first = core::Epoch{0};
    entry.info.last = core::Epoch{86400000000000};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        auto *relativity = dialog->findChild<QCheckBox *>("hpopRelativisticAccelerations");
        ASSERT_TRUE(relativity);
        forces->setCurrentIndex(3);
        reveal(relativity);
        relativity->setFocus();
        EXPECT_FALSE(relativity->isChecked());
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier),
            release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(relativity, &press);
        QApplication::sendEvent(relativity, &release);
        EXPECT_TRUE(relativity->isChecked());
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        std::cout << "HPOP relativity device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            auto *scroll = dialog->findChild<QScrollArea *>("hpopAdditionalPage");
            scroll->ensureWidgetVisible(relativity);
            QApplication::processEvents();
            EXPECT_TRUE(relativity->isVisible());
            EXPECT_TRUE(relativity->isChecked());
            EXPECT_EQ(relativity, dialog->findChild<QCheckBox *>("hpopRelativisticAccelerations"));
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_TRUE(
                dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-relativity-%1.png").arg(width)));
            }
        }
        outer->setCurrentIndex(1);
        outer->setCurrentIndex(0);
        EXPECT_EQ(forces->currentIndex(), 3);
        EXPECT_TRUE(relativity->isChecked());
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_TRUE(options.include_relativistic_accelerations);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *relativity = dialog->findChild<QCheckBox *>("hpopRelativisticAccelerations");
        ASSERT_TRUE(relativity);
        EXPECT_TRUE(relativity->isChecked());
        relativity->setChecked(false);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options, saved);
}
TEST_F(QuantityTest, HpopRelativityMissingEphemerisReturnsToAdditionalWithoutPublishingDraft) {
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *relativity = dialog->findChild<QCheckBox *>("hpopRelativisticAccelerations");
        ASSERT_TRUE(relativity);
        relativity->setChecked(true);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        outer->setCurrentIndex(2);
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        ok->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(options, saved);
        EXPECT_EQ(outer->currentIndex(), 0);
        EXPECT_EQ(dialog->findChild<QTabWidget *>("hpopForceModelTabs")->currentIndex(), 3);
        EXPECT_TRUE(
            dialog->findChild<QLabel *>("errorText")->text().contains(QStringLiteral("相对论")));
        EXPECT_TRUE(relativity->isVisible());
        relativity->setChecked(false);
        ok->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopSatelliteMassUnitsKeyboardResizeReopenAndCancel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.drag.enabled = options.srp.enabled = true;
    options.srp.area_mass_ratio_m2_per_kg = .03;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    entry.info.first = core::Epoch{0};
    entry.info.last = core::Epoch{86400000000000};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *mass = dialog->findChild<QuantityEditor *>("hpopSatelliteMass");
        auto *drag_area = dialog->findChild<QLabel *>("hpopDragCompatibleArea");
        auto *srp_area = dialog->findChild<QLabel *>("hpopSrpCompatibleArea");
        ASSERT_TRUE(mass && drag_area && srp_area);
        EXPECT_EQ(mass->value(Unit::kilogram), 1000);
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        forces->setCurrentIndex(3);
        reveal(mass);
        choose(*mass, Unit::tonne);
        text(*mass)->setFocus();
        text(*mass)->selectAll();
        QKeyEvent typing(QEvent::KeyPress, Qt::Key_1, Qt::NoModifier, QStringLiteral("1.25"));
        QApplication::sendEvent(text(*mass), &typing);
        EXPECT_EQ(mass->value(Unit::kilogram), 1250);
        choose(*mass, Unit::gram);
        EXPECT_EQ(mass->value(Unit::gram), 1250000);
        choose(*mass, Unit::tonne);
        EXPECT_EQ(text(*mass)->text(), QStringLiteral("1.25"));
        EXPECT_EQ(drag_area->text(), QStringLiteral("25 m²"));
        EXPECT_EQ(srp_area->text(), QStringLiteral("37.5 m²"));
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        std::cout << "HPOP mass device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            for (const int page_index : {3, 1, 2}) {
                forces->setCurrentIndex(page_index);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                auto *scroll = qobject_cast<QScrollArea *>(forces->currentWidget());
                ASSERT_TRUE(scroll);
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                EXPECT_EQ(mass, dialog->findChild<QuantityEditor *>("hpopSatelliteMass"));
                EXPECT_EQ(mass->value(Unit::kilogram), 1250);
                EXPECT_EQ(mass->unit(), Unit::tonne);
                EXPECT_TRUE(
                    dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
                if (!capture.isEmpty()) {
                    EXPECT_TRUE(QDir{}.mkpath(capture));
                    EXPECT_TRUE(dialog->grab().save(
                        capture + QString("/hpop-mass-%1-%2.png").arg(page_index).arg(width)));
                }
            }
        }
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options.satellite_mass_kg, 1250);
    EXPECT_EQ(options.drag.area_mass_ratio_m2_per_kg, .02);
    EXPECT_EQ(options.srp.area_mass_ratio_m2_per_kg, .03);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *mass = dialog->findChild<QuantityEditor *>("hpopSatelliteMass");
        ASSERT_TRUE(mass);
        EXPECT_EQ(mass->value(Unit::kilogram), 1250);
        text(*mass)->setText("900");
        EXPECT_EQ(dialog->findChild<QLabel *>("hpopDragCompatibleArea")->text(),
                  QStringLiteral("18 m²"));
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options, saved);
}
TEST_F(QuantityTest, HpopSatelliteMassInvalidValueReturnsToAdditionalWithoutCommitting) {
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.srp.area_mass_ratio_m2_per_kg = 9999;
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *mass = dialog->findChild<QuantityEditor *>("hpopSatelliteMass");
        ASSERT_TRUE(mass);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        for (const char *invalid : {"0", "-1", "1e309", "1e308", "5e-324"}) {
            text(*mass)->setText(invalid);
            outer->setCurrentIndex(2);
            ok->click();
            EXPECT_TRUE(dialog->isVisible());
            EXPECT_EQ(options, saved);
            EXPECT_EQ(outer->currentIndex(), 0);
            EXPECT_EQ(dialog->findChild<QTabWidget *>("hpopForceModelTabs")->currentIndex(), 3);
            EXPECT_FALSE(dialog->findChild<QLabel *>("errorText")->text().isEmpty());
            EXPECT_TRUE(mass->isVisible());
        }
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopLoveNumberFollowsGravityFileAndCancelPreservesSelection) {
    core::HpopOptions options;
    runtime::CatalogGravity custom;
    custom.selection = "custom";
    custom.info = {"Custom", {}, {}, "tide_free", core::egm96_mu, core::egm96_radius_m, 21, 21, .3};
    const auto original = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *love = dialog->findChild<QLabel *>("hpopSolidTideK20");
        auto *model = dialog->findChild<QComboBox *>("hpopGravityModel");
        ASSERT_TRUE(love);
        ASSERT_TRUE(model);
        EXPECT_DOUBLE_EQ(love->text().toDouble(), .30190);
        model->setCurrentIndex(model->findData(QStringLiteral("custom")));
        EXPECT_DOUBLE_EQ(love->text().toDouble(), .3);
        auto *mode = dialog->findChild<QComboBox *>("hpopSolidTideMode");
        mode->setCurrentIndex(1);
        dialog->resize(430, 620);
        QApplication::processEvents();
        EXPECT_EQ(love, dialog->findChild<QLabel *>("hpopSolidTideK20"));
        EXPECT_DOUBLE_EQ(love->text().toDouble(), .3);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {custom}, {}));
    EXPECT_EQ(options, original);
    options.gravity_model = "custom";
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_DOUBLE_EQ(dialog->findChild<QLabel *>("hpopSolidTideK20")->text().toDouble(), .3);
        dialog->findChild<QComboBox *>("hpopSolidTideMode")->setCurrentIndex(1);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {custom}, {}));
    EXPECT_EQ(options.gravity_model, "custom");
    EXPECT_EQ(options.solid_tides.mode, core::SolidTideMode::permanent);
}

TEST_F(QuantityTest, HpopOceanIndependentTruncationKeyboardResizeReopenAndCancel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    QTemporaryDir temporary;
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    EopCatalog catalog(root / "data", {}, {}, false);
    QFile file(temporary.path() + "/test.pot");
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(
        "Coefficients to compute variations in normalized Stokes coefficients (unit = 10^-11)\n"
        "Ocean tide model: Test up to (4,2)\n(test)\nDoodson Darw l m DelC+ DelS+ DelC- DelS-\n");
    for (int n = 2; n <= 4; ++n)
        for (int m = 0; m <= 2; ++m)
            file.write(QString("55.555 Test %1 %2 1 2 3 4\n").arg(n).arg(m).toUtf8());
    file.close();
    catalog.data().install_ocean_tides(root / "test.pot", "custom-ocean");
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.ocean_tides = {false, "custom-ocean", 4, 2};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *use = dialog->findChild<QCheckBox *>("hpopUseOceanTides");
        auto *degree = dialog->findChild<QSpinBox *>("hpopOceanTideDegree");
        auto *order = dialog->findChild<QSpinBox *>("hpopOceanTideOrder");
        ASSERT_TRUE(use);
        ASSERT_TRUE(degree);
        ASSERT_TRUE(order);
        EXPECT_FALSE(degree->isEnabled());
        reveal(use);
        use->setFocus();
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(use, &press);
        QApplication::sendEvent(use, &release);
        EXPECT_TRUE(use->isChecked());
        EXPECT_TRUE(degree->isEnabled());
        auto *accept = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        degree->setValue(5);
        accept->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_FALSE(options.ocean_tides.enabled);
        EXPECT_TRUE(
            dialog->findChild<QLabel *>("errorText")->text().contains(QStringLiteral("可用范围")));
        degree->setValue(4);
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        std::cout << "HPOP ocean tide device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            auto *scroll = dialog->findChild<QScrollArea *>("hpopGravityPage");
            scroll->ensureWidgetVisible(order);
            QApplication::processEvents();
            EXPECT_EQ(degree, dialog->findChild<QSpinBox *>("hpopOceanTideDegree"));
            EXPECT_EQ(degree->value(), 4);
            EXPECT_EQ(order->value(), 2);
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_FALSE(dialog->findChild<QLabel *>("hpopOceanTideStatus")
                             ->text()
                             .contains(QStringLiteral("已安装")));
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-ocean-%1.png").arg(width)));
            }
        }
        // An unrelated official-model download must not block this available custom model.
        catalog.start_download(QUrl("http://127.0.0.1:1"), DataProduct::fes2014bv1);
        use->setChecked(false);
        EXPECT_FALSE(degree->isEnabled());
        EXPECT_EQ(order->value(), 2);
        use->setChecked(true);
        EXPECT_TRUE(accept->isEnabled());
        EXPECT_TRUE(dialog->findChild<QProgressBar *>("hpopOceanTideProgress")->isHidden());
        accept->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_TRUE(catalog.downloading());
    catalog.cancel_download();
    settle_hpop_catalog(catalog);
    EXPECT_EQ(options.gravity_degree, 0);
    EXPECT_EQ(options.ocean_tides, (core::OceanTideOptions{true, "custom-ocean", 4, 2}));
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_TRUE(dialog->findChild<QCheckBox *>("hpopUseOceanTides")->isChecked());
        EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopOceanTideDegree")->value(), 4);
        dialog->findChild<QSpinBox *>("hpopOceanTideOrder")->setValue(0);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_EQ(options, saved);
}
TEST_F(QuantityTest, HpopMissingOceanDownloadsAreOwnedCancellableAndRetryOnReopen) {
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    core::HpopOptions options;
    options.ocean_tides.enabled = true;
    const auto saved = options;
    for (int attempt = 0; attempt < 2; ++attempt) {
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QPushButton *>("hpopDownloadOceanTides")->click();
            EXPECT_TRUE(catalog.downloading());
            EXPECT_EQ(catalog.active_product(), DataProduct::fes2014bv1);
            EXPECT_FALSE(
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
            if (attempt)
                dialog->findChild<QPushButton *>("hpopCancelOceanDownload")->click();
            dialog->reject();
        });
        EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
        settle_hpop_catalog(catalog);
        EXPECT_EQ(options, saved);
        EXPECT_EQ(catalog.update_status(DataProduct::fes2014bv1).state, DataUpdateState::cancelled);
    }
}
TEST_F(QuantityTest, HpopQueuedOceanCancellationPreservesUnrelatedDownloadAndDisabledDraft) {
    QTemporaryDir temporary;
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    catalog.start_download(QUrl("http://127.0.0.1:1"), DataProduct::space_weather);
    core::HpopOptions options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *use = dialog->findChild<QCheckBox *>("hpopUseOceanTides");
        use->setChecked(true);
        EXPECT_TRUE(dialog->findChild<QLabel *>("hpopOceanTideStatus")
                        ->text()
                        .contains(QStringLiteral("等待")));
        dialog->findChild<QPushButton *>("hpopCancelOceanDownload")->click();
        EXPECT_TRUE(catalog.downloading());
        EXPECT_EQ(catalog.active_product(), DataProduct::space_weather);
        use->setChecked(false);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {}, {}, &catalog));
    EXPECT_FALSE(options.ocean_tides.enabled);
    EXPECT_EQ(options.ocean_tides.degree, 4);
    catalog.cancel_download();
    settle_hpop_catalog(catalog);
}
TEST_F(QuantityTest, HpopSecularFileCapabilityKeyboardDraftAndValidation) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    runtime::CatalogGravity custom;
    custom.selection = "custom";
    custom.info = {"Secular", {}, {}, "tide_free", core::egm96_mu, core::egm96_radius_m, 8, 4};
    custom.info.secular_variations = core::GravitySecularVariations{
        core::Epoch{-32184000000LL}, {{2, 0, 1.162755e-11, 0}, {2, 1, -.337e-11, 1.606e-11}}};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *use = dialog->findChild<QCheckBox *>("hpopSecularVariations");
        auto *model = dialog->findChild<QComboBox *>("hpopGravityModel");
        auto *status = dialog->findChild<QLabel *>("hpopSecularStatus");
        ASSERT_TRUE(use);
        ASSERT_TRUE(model);
        ASSERT_TRUE(status);
        EXPECT_FALSE(use->isEnabled());
        model->setCurrentIndex(model->findData(QStringLiteral("custom")));
        EXPECT_TRUE(use->isEnabled());
        EXPECT_TRUE(status->text().contains("2451545.000000000"));
        use->setFocus();
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(use, &press);
        QApplication::sendEvent(use, &release);
        EXPECT_TRUE(use->isChecked());
        std::cout << "HPOP secular device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_EQ(use, dialog->findChild<QCheckBox *>("hpopSecularVariations"));
            EXPECT_TRUE(use->isChecked());
            auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
            auto *scroll = qobject_cast<QScrollArea *>(forces->currentWidget());
            ASSERT_TRUE(scroll);
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_TRUE(
                dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
            const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-secular-%1.png").arg(width)));
            }
        }
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {custom}, {}));
    EXPECT_TRUE(options.include_secular_variations);
    EXPECT_EQ(options.gravity_model, "custom");
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *use = dialog->findChild<QCheckBox *>("hpopSecularVariations");
        auto *model = dialog->findChild<QComboBox *>("hpopGravityModel");
        EXPECT_TRUE(use->isChecked());
        model->setCurrentIndex(0);
        EXPECT_TRUE(use->isEnabled()); // A stale checked draft can always be switched off.
        dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(2);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_FALSE(dialog->findChild<QLabel *>("errorText")->text().isEmpty());
        EXPECT_EQ(dialog->findChild<QTabWidget *>("hpopSettingsTabs")->currentIndex(), 0);
        EXPECT_EQ(options, saved);
        use->setChecked(false);
        EXPECT_FALSE(use->isEnabled());
        model->setCurrentIndex(model->findData(QStringLiteral("custom")));
        EXPECT_TRUE(use->isEnabled());
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {custom}, {}));
    EXPECT_EQ(options, saved);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QComboBox *>("hpopGravityModel")->setCurrentIndex(1);
        auto *use = dialog->findChild<QCheckBox *>("hpopSecularVariations");
        EXPECT_FALSE(use->isEnabled());
        EXPECT_TRUE(use->isChecked());
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {custom}, {}));
    EXPECT_EQ(options.gravity_degree, 0);
    EXPECT_TRUE(options.include_secular_variations);
}

TEST_F(QuantityTest, HpopAdaptiveIntegratorsValidatePersistAndPreserveCancelledDrafts) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    for (const auto integrator : {core::HpopIntegrator::rkf78, core::HpopIntegrator::rkv89,
                                  core::HpopIntegrator::bulirsch_stoer}) {
        const int method_index = static_cast<int>(integrator);
        const auto label = method_index == 0   ? QStringLiteral("RKF 7(8)")
                           : method_index == 2 ? QStringLiteral("RKV 8(9) Efficient")
                                               : QStringLiteral("Bulirsch–Stoer");
        core::HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.fixed_step_seconds = 12;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *method = dialog->findChild<QComboBox *>("hpopIntegrator");
            auto *minimum = dialog->findChild<QuantityEditor *>("hpopMinStep");
            auto *maximum = dialog->findChild<QuantityEditor *>("hpopMaxStep");
            auto *fixed = dialog->findChild<QuantityEditor *>("hpopFixedStep");
            ASSERT_EQ(method->count(), 5);
            method->setCurrentIndex(1);
            EXPECT_FALSE(maximum->isVisible());
            EXPECT_TRUE(fixed->isVisible());
            method->setFocus();
            QKeyEvent end_key(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
            QApplication::sendEvent(method, &end_key);
            for (int index = 4; index > method_index; --index) {
                QKeyEvent up_key(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
                QApplication::sendEvent(method, &up_key);
            }
            EXPECT_EQ(method->currentIndex(), method_index);
            EXPECT_TRUE(method->currentText().contains(label));
            auto *control = dialog->findChild<QComboBox *>("hpopStepControl");
            ASSERT_TRUE(control);
            EXPECT_TRUE(control->isEnabled());
            control->setCurrentIndex(1);
            EXPECT_TRUE(fixed->isVisible());
            EXPECT_FALSE(maximum->isVisible());
            method->setCurrentIndex(4);
            EXPECT_FALSE(control->isEnabled());
            EXPECT_EQ(control->currentIndex(), 1);
            method->setCurrentIndex(method_index);
            EXPECT_TRUE(control->isEnabled());
            EXPECT_EQ(control->currentIndex(), 1);
            control->setCurrentIndex(0);
            method->setCurrentIndex(1);
            EXPECT_FALSE(control->isEnabled());
            EXPECT_EQ(control->currentIndex(), 1);
            method->setCurrentIndex(method_index);
            EXPECT_EQ(control->currentIndex(), 0);
            EXPECT_TRUE(maximum->isVisible());
            EXPECT_FALSE(fixed->isVisible());
            minimum->set_value(50, Unit::second);
            maximum->set_value(10, Unit::second);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            EXPECT_TRUE(dialog->isVisible());
            EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
            minimum->set_value(.01, Unit::second);
            text(*maximum)->setText("86401");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            EXPECT_TRUE(dialog->isVisible());
            EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
            maximum->set_value(86400, Unit::second);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        ASSERT_TRUE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options.integrator, integrator);
        EXPECT_EQ(options.min_step_seconds, .01);
        EXPECT_EQ(options.max_step_seconds, 86400);
        EXPECT_EQ(options.fixed_step_seconds, 12);
        OrbitInput input;
        input.propagator = OrbitPropagator::hpop;
        input.hpop = options;
        OrbitEditor editor(input);
        bool summary_matches = false;
        for (const auto *text_label : editor.findChildren<QLabel *>())
            summary_matches |= text_label->text().contains(label);
        EXPECT_TRUE(summary_matches);
        const auto saved = options;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            EXPECT_EQ(dialog->findChild<QComboBox *>("hpopIntegrator")->currentIndex(),
                      method_index);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *method = dialog->findChild<QComboBox *>("hpopIntegrator");
            auto *maximum = dialog->findChild<QuantityEditor *>("hpopMaxStep");
            const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
            std::cout << "HPOP " << method_index
                      << " device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
            for (const int width : {720, 430}) {
                dialog->resize(width, 620);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                EXPECT_EQ(method, dialog->findChild<QComboBox *>("hpopIntegrator"));
                EXPECT_EQ(maximum->value(Unit::second), 86400);
                auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                scroll->ensureWidgetVisible(method);
                QApplication::processEvents();
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                EXPECT_TRUE(
                    dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
                if (!capture.isEmpty()) {
                    EXPECT_TRUE(QDir{}.mkpath(capture));
                    EXPECT_TRUE(dialog->grab().save(
                        capture +
                        QString("/hpop-adaptive-%1-%2.png").arg(method_index).arg(width)));
                }
            }
            dialog->findChild<QComboBox *>("hpopIntegrator")->setCurrentIndex(1);
            dialog->findChild<QuantityEditor *>("hpopFixedStep")->set_value(7, Unit::second);
            dialog->reject();
        });
        EXPECT_FALSE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options, saved);
    }
}

TEST_F(QuantityTest, HpopFixedStepControlAppliesAndRestoresDrafts) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    for (const auto method_kind : {core::HpopIntegrator::rkf78, core::HpopIntegrator::rkv89,
                                   core::HpopIntegrator::bulirsch_stoer}) {
        core::HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.integrator = method_kind;
        const int method_index = static_cast<int>(method_kind);
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *control = dialog->findChild<QComboBox *>("hpopStepControl");
            control->setCurrentIndex(1);
            auto *method = dialog->findChild<QComboBox *>("hpopIntegrator");
            method->setCurrentIndex(4);
            EXPECT_FALSE(control->isEnabled());
            method->setCurrentIndex(method_index);
            EXPECT_TRUE(control->isEnabled());
            EXPECT_EQ(control->currentIndex(), 1);
            auto *fixed = dialog->findChild<QuantityEditor *>("hpopFixedStep");
            fixed->set_value(7.1, Unit::second);
            EXPECT_TRUE(fixed->isVisible());
            auto *maximum = dialog->findChild<QuantityEditor *>("hpopMaxStep");
            EXPECT_FALSE(maximum->isVisible());
            dialog->findChild<QuantityEditor *>("hpopMinStep")->set_value(50, Unit::second);
            maximum->set_value(10, Unit::second);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        ASSERT_TRUE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options.integrator, method_kind);
        EXPECT_EQ(options.step_control, core::HpopStepControl::fixed);
        EXPECT_EQ(options.fixed_step_seconds, 7.1);
        EXPECT_EQ(options.max_step_seconds, 60);
        const auto saved = options;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *control = dialog->findChild<QComboBox *>("hpopStepControl");
            EXPECT_EQ(control->currentIndex(), 1);
            const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
            for (int width : {430, 720}) {
                dialog->resize(width, 620);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                scroll->ensureWidgetVisible(control);
                QApplication::processEvents();
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                EXPECT_TRUE(
                    dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
                if (!capture.isEmpty()) {
                    EXPECT_TRUE(QDir{}.mkpath(capture));
                    EXPECT_TRUE(dialog->grab().save(
                        capture + QString("/hpop-fixed-%1-%2.png").arg(method_index).arg(width)));
                }
            }
            control->setCurrentIndex(0);
            dialog->reject();
        });
        EXPECT_FALSE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options, saved);
    }
}

TEST_F(QuantityTest, HpopGaussJacksonCorrectionControlsValidatePersistAndCancel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    for (int scheme : {0, 1}) {
        core::HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *method = dialog->findChild<QComboBox *>("hpopIntegrator");
            auto *correction = dialog->findChild<QComboBox *>("hpopCorrection");
            auto *fixed = dialog->findChild<QuantityEditor *>("hpopFixedStep");
            auto *maximum = dialog->findChild<QuantityEditor *>("hpopMaxStep");
            ASSERT_TRUE(correction);
            EXPECT_FALSE(correction->isVisible());
            method->setFocus();
            QKeyEvent end_key(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
            QApplication::sendEvent(method, &end_key);
            EXPECT_EQ(method->currentIndex(), 4);
            EXPECT_FALSE(maximum->isVisible());
            EXPECT_TRUE(fixed->isVisible());
            EXPECT_TRUE(correction->isVisible());
            correction->setCurrentIndex(scheme);
            fixed->findChild<QLineEdit *>("quantityValue")->setText("0");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            EXPECT_TRUE(dialog->isVisible());
            EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
            fixed->set_value(30, Unit::second);
            method->setCurrentIndex(0);
            EXPECT_FALSE(correction->isVisible());
            method->setCurrentIndex(4);
            EXPECT_EQ(correction->currentIndex(), scheme);
            EXPECT_EQ(fixed->value(Unit::second), 30);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        ASSERT_TRUE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options.integrator, core::HpopIntegrator::gauss_jackson);
        EXPECT_EQ(options.correction, static_cast<core::HpopCorrection>(scheme));
        EXPECT_EQ(options.fixed_step_seconds, 30);
        const auto saved = options;
        OrbitInput input;
        input.propagator = OrbitPropagator::hpop;
        input.hpop = options;
        OrbitEditor editor(input);
        bool summary_matches = false;
        for (const auto *text_label : editor.findChildren<QLabel *>())
            summary_matches |= text_label->text().contains("Gauss–Jackson 12");
        EXPECT_TRUE(summary_matches);
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *correction = dialog->findChild<QComboBox *>("hpopCorrection");
            auto *fixed = dialog->findChild<QuantityEditor *>("hpopFixedStep");
            EXPECT_EQ(correction->currentIndex(), scheme);
            EXPECT_EQ(fixed->value(Unit::second), 30);
            auto *navigation = dialog->findChild<QListWidget *>("hpopNavigation");
            ASSERT_TRUE(navigation);
            bool fixed_summary = false;
            for (int row = 0; row < navigation->count(); ++row) {
                const auto summary = navigation->item(row)->data(Qt::UserRole + 1).toString();
                if (summary.contains("Gauss–Jackson")) {
                    EXPECT_TRUE(summary.contains(QStringLiteral("步长 30 s")));
                    EXPECT_FALSE(summary.contains(QStringLiteral("相对容差")));
                    fixed_summary = true;
                }
            }
            EXPECT_TRUE(fixed_summary);
            const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
            std::cout << "HPOP Gauss-Jackson " << scheme << " DPR " << dialog->devicePixelRatioF()
                      << '\n';
            for (int width : {720, 430}) {
                dialog->resize(width, 620);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                scroll->ensureWidgetVisible(correction);
                QApplication::processEvents();
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                EXPECT_EQ(correction->currentIndex(), scheme);
                EXPECT_TRUE(
                    dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
                if (!capture.isEmpty()) {
                    EXPECT_TRUE(QDir{}.mkpath(capture));
                    EXPECT_TRUE(dialog->grab().save(
                        capture + QString("/hpop-gj-%1-%2.png").arg(scheme).arg(width)));
                }
            }
            correction->setCurrentIndex(1 - scheme);
            fixed->set_value(7, Unit::second);
            dialog->reject();
        });
        EXPECT_FALSE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options, saved);
    }
}

TEST_F(QuantityTest, HpopInterpolationValidatesPreservesDraftsAndReopensAtNarrowWidths) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
        auto *method = dialog->findChild<QComboBox *>("hpopInterpolation");
        auto *order = dialog->findChild<QSpinBox *>("hpopInterpolationOrder");
        auto *fixed = dialog->findChild<QCheckBox *>("hpopFixedEphemeris");
        ASSERT_TRUE(method);
        ASSERT_TRUE(order);
        ASSERT_TRUE(fixed);
        EXPECT_EQ(method->currentIndex(), 1);
        EXPECT_EQ(order->value(), 7);
        order->setValue(8);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
        order->setValue(9);
        method->setCurrentIndex(0);
        order->setValue(6);
        method->setCurrentIndex(1);
        EXPECT_EQ(order->value(), 9);
        method->setCurrentIndex(0);
        EXPECT_EQ(order->value(), 6);
        fixed->setChecked(true);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    ASSERT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options.interpolation.method, core::OrbitInterpolationMethod::lagrange);
    EXPECT_EQ(options.interpolation.order, 6);
    EXPECT_TRUE(options.report_ephemeris_on_fixed_step);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
        auto *method = dialog->findChild<QComboBox *>("hpopInterpolation");
        auto *order = dialog->findChild<QSpinBox *>("hpopInterpolationOrder");
        auto *fixed = dialog->findChild<QCheckBox *>("hpopFixedEphemeris");
        EXPECT_EQ(method->currentIndex(), 0);
        EXPECT_EQ(order->value(), 6);
        EXPECT_TRUE(fixed->isChecked());
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        std::cout << "HPOP interpolation DPR " << dialog->devicePixelRatioF() << '\n';
        for (int selected : {0, 1}) {
            method->setCurrentIndex(selected);
            if (selected == 1) {
                order->setFocus();
                QKeyEvent key(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
                QApplication::sendEvent(order, &key);
                EXPECT_EQ(order->value(), 9);
            }
            for (int width : {430, 720}) {
                dialog->resize(width, 620);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                scroll->ensureWidgetVisible(order);
                QApplication::processEvents();
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                EXPECT_EQ(order->value(), selected == 0 ? 6 : 9);
                EXPECT_TRUE(
                    dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
                if (!capture.isEmpty()) {
                    EXPECT_TRUE(QDir{}.mkpath(capture));
                    EXPECT_TRUE(dialog->grab().save(
                        capture +
                        QString("/hpop-interpolation-%1-%2.png").arg(selected).arg(width)));
                }
            }
        }
        fixed->setChecked(false);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}
TEST_F(QuantityTest, HpopRegularizationControlsPreserveDraftsCancelAndNarrowLayouts) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    for (int selected = 0; selected < 5; ++selected) {
        core::HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.integrator = static_cast<core::HpopIntegrator>(selected);
        options.step_control = core::HpopStepControl::fixed;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *enabled = dialog->findChild<QCheckBox *>("hpopTimeRegularization");
            auto *exponent = dialog->findChild<QuantityEditor *>("hpopRegularizationExponent");
            auto *count = dialog->findChild<QSpinBox *>("hpopStepsPerOrbit");
            auto *fixed = dialog->findChild<QuantityEditor *>("hpopFixedStep");
            auto *control = dialog->findChild<QComboBox *>("hpopStepControl");
            ASSERT_TRUE(enabled);
            ASSERT_TRUE(exponent);
            ASSERT_TRUE(count);
            EXPECT_FALSE(enabled->isChecked());
            EXPECT_FALSE(exponent->isEnabled());
            EXPECT_FALSE(count->isEnabled());
            EXPECT_TRUE(fixed->isVisible());
            enabled->setChecked(true);
            EXPECT_TRUE(exponent->isEnabled());
            EXPECT_TRUE(count->isEnabled());
            EXPECT_FALSE(fixed->isVisible());
            exponent->set_value(1.75, Unit::one);
            count->setValue(1024);
            enabled->setChecked(false);
            EXPECT_TRUE(fixed->isVisible());
            enabled->setChecked(true);
            EXPECT_EQ(count->value(), 1024);
            if (control->isEnabled()) {
                control->setCurrentIndex(0);
                EXPECT_TRUE(dialog->findChild<QuantityEditor *>("hpopMaxStep")->isVisible());
                EXPECT_FALSE(fixed->isVisible());
                control->setCurrentIndex(1);
            }
            // An inactive ordinary-step draft must not block a valid regularized edit.
            fixed->findChild<QLineEdit *>()->setText("invalid inactive draft");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        ASSERT_TRUE(edit_hpop_settings(options, nullptr));
        EXPECT_TRUE(options.time_regularization);
        EXPECT_EQ(options.regularization_exponent, 1.75);
        EXPECT_EQ(options.steps_per_orbit, 1024);
        EXPECT_EQ(options.fixed_step_seconds, 10);
        const auto saved = options;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *enabled = dialog->findChild<QCheckBox *>("hpopTimeRegularization");
            auto *count = dialog->findChild<QSpinBox *>("hpopStepsPerOrbit");
            EXPECT_TRUE(enabled->isChecked());
            EXPECT_EQ(count->value(), 1024);
            count->setFocus();
            QKeyEvent key(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
            QApplication::sendEvent(count, &key);
            EXPECT_EQ(count->value(), 1025);
            const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
            std::cout << "HPOP regularization DPR " << dialog->devicePixelRatioF() << '\n';
            for (int width : {430, 720}) {
                dialog->resize(width, 620);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                scroll->ensureWidgetVisible(count);
                QApplication::processEvents();
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                EXPECT_EQ(count->value(), 1025);
                EXPECT_TRUE(
                    dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
                if (!capture.isEmpty() && selected == 4) {
                    EXPECT_TRUE(QDir{}.mkpath(capture));
                    EXPECT_TRUE(dialog->grab().save(
                        capture + QString("/hpop-regularization-%1.png").arg(width)));
                }
            }
            enabled->setChecked(false);
            dialog->reject();
        });
        EXPECT_FALSE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options, saved);
    }
}
} // namespace orbitforge::desktop

namespace orbitforge::desktop {
TEST_F(QuantityTest, HpopSrpBodyAssignmentSearchRadiusDraftsNestedCancelAndNarrowLayout) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.srp.enabled = true;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    const auto visit = [&](bool accept_main) {
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopForceModelTabs")->setCurrentIndex(2);
            auto *select = dialog->findChild<QPushButton *>("hpopSrpSelectBodies");
            const auto summary = dialog->findChild<QLabel *>("hpopSrpEclipsingSummary")->text();
            QTimer::singleShot(0, [&] {
                auto *selection = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                ASSERT_TRUE(selection);
                auto *available = selection->findChild<QListWidget *>("hpopSrpAvailableBodies");
                auto *assigned = selection->findChild<QListWidget *>("hpopSrpAssignedBodies");
                auto *search = selection->findChild<QLineEdit *>("hpopSrpBodySearch");
                ASSERT_TRUE(available && assigned && search);
                search->setText("vEnUs");
                for (int i = 0; i < available->count(); ++i)
                    if (!available->item(i)->isHidden()) {
                        EXPECT_EQ(available->item(i)->data(Qt::UserRole).toInt(), 3);
                        available->setCurrentItem(available->item(i));
                    }
                selection->findChild<QToolButton *>("hpopSrpAddBody")->click();
                ASSERT_EQ(assigned->count(), 3);
                auto *radius = selection->findChild<QuantityEditor *>("hpopSrpRadius3");
                ASSERT_TRUE(radius);
                radius->set_unit(Unit::kilometre);
                radius->findChild<QLineEdit *>("quantityValue")->setText("6000.125");
                const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
                std::cout << "HPOP SRP bodies DPR: " << selection->devicePixelRatioF() << '\n';
                for (int width : {430, 720}) {
                    selection->resize(width, 520);
                    QEventLoop settled;
                    QTimer::singleShot(60, &settled, &QEventLoop::quit);
                    settled.exec();
                    EXPECT_EQ(radius->findChild<QLineEdit *>("quantityValue")->text(), "6000.125");
                    EXPECT_TRUE(selection->rect().contains(
                        selection->findChild<QDialogButtonBox *>()->geometry()));
                    EXPECT_EQ(available->horizontalScrollBar()->maximum(), 0);
                    EXPECT_EQ(assigned->horizontalScrollBar()->maximum(), 0);
                    if (!capture.isEmpty()) {
                        EXPECT_TRUE(QDir{}.mkpath(capture));
                        EXPECT_TRUE(selection->grab().save(
                            capture + QString("/hpop-srp-bodies-%1.png").arg(width)));
                    }
                }
                radius->findChild<QLineEdit *>("quantityValue")->setText("-1");
                selection->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
                EXPECT_TRUE(selection->isVisible());
                EXPECT_FALSE(
                    selection->findChild<QLabel *>("hpopSrpBodiesError")->text().isEmpty());
                radius->findChild<QLineEdit *>("quantityValue")->setText("6000.125");
                selection->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            select->click();
            EXPECT_NE(dialog->findChild<QLabel *>("hpopSrpEclipsingSummary")->text(), summary);
            QTimer::singleShot(0, [&] {
                auto *selection = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                ASSERT_TRUE(selection);
                auto *assigned = selection->findChild<QListWidget *>("hpopSrpAssignedBodies");
                ASSERT_EQ(assigned->count(), 3);
                for (int i = 0; i < assigned->count(); ++i)
                    if (assigned->item(i)->data(Qt::UserRole).toInt() == 3)
                        assigned->setCurrentItem(assigned->item(i));
                EXPECT_EQ(
                    selection->findChild<QuantityEditor *>("hpopSrpRadius3")->value(Unit::metre),
                    6000125);
                selection->findChild<QToolButton *>("hpopSrpRemoveBody")->click();
                selection->reject();
            });
            select->click();
            EXPECT_TRUE(
                dialog->findChild<QLabel *>("hpopSrpEclipsingSummary")->text().contains("Venus"));
            if (accept_main)
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            else
                dialog->reject();
        });
        return edit_hpop_settings(options, nullptr, {}, {entry});
    };
    const auto before = options;
    EXPECT_FALSE(visit(false));
    EXPECT_EQ(options, before);
    EXPECT_TRUE(visit(true));
    ASSERT_EQ(options.srp.additional_eclipsing_bodies.size(), 1U);
    EXPECT_EQ(options.srp.additional_eclipsing_bodies[0],
              (core::SrpEclipsingBody{core::ThirdBody::venus, 6000125}));
}

TEST_F(QuantityTest, HpopVopInterpolationMuDraftsUnitsValidationAndNarrowLayouts) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    for (int pass = 0; pass < 3; ++pass) {
        const auto saved = options;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *method = dialog->findChild<QComboBox *>("hpopInterpolation");
            auto *order = dialog->findChild<QSpinBox *>("hpopInterpolationOrder");
            auto *mu = dialog->findChild<QuantityEditor *>("hpopInterpolationMu");
            ASSERT_TRUE(mu);
            if (pass == 0) {
                EXPECT_FALSE(mu->isEnabled());
                method->setCurrentIndex(2);
                EXPECT_TRUE(mu->isEnabled());
                order->setValue(8);
                mu->findChild<QLineEdit *>("quantityValue")->setText("0");
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
                EXPECT_TRUE(dialog->isVisible());
                EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
                mu->set_value(4.1e14, Unit::metres_cubed_per_second_squared);
                method->setCurrentIndex(1);
                EXPECT_FALSE(mu->isEnabled());
                EXPECT_EQ(order->value(), 7);
                method->setCurrentIndex(2);
                EXPECT_TRUE(mu->isEnabled());
                EXPECT_EQ(order->value(), 8);
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            } else {
                EXPECT_EQ(method->currentIndex(), 2);
                EXPECT_EQ(order->value(), 8);
                EXPECT_DOUBLE_EQ(mu->value(Unit::metres_cubed_per_second_squared), 4.1e14);
                mu->set_unit(Unit::metres_cubed_per_second_squared);
                EXPECT_DOUBLE_EQ(mu->value(Unit::metres_cubed_per_second_squared), 4.1e14);
                mu->set_unit(Unit::kilometres_cubed_per_second_squared);
                for (int width : {430, 720}) {
                    dialog->resize(width, 620);
                    QEventLoop settled;
                    QTimer::singleShot(60, &settled, &QEventLoop::quit);
                    settled.exec();
                    auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                    scroll->ensureWidgetVisible(mu);
                    QApplication::processEvents();
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                    EXPECT_TRUE(dialog->rect().contains(
                        dialog->findChild<QDialogButtonBox *>()->geometry()));
                    const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
                    if (!capture.isEmpty()) {
                        EXPECT_TRUE(QDir{}.mkpath(capture));
                        EXPECT_TRUE(
                            dialog->grab().save(capture + QString("/hpop-vop-%1.png").arg(width)));
                    }
                }
                std::cout << "HPOP VOP DPR " << dialog->devicePixelRatioF() << '\n';
                if (pass == 1) {
                    mu->set_value(4.2e14, Unit::metres_cubed_per_second_squared);
                    dialog->reject();
                } else {
                    method->setCurrentIndex(0);
                    dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
                }
            }
        });
        EXPECT_EQ(edit_hpop_settings(options, nullptr), pass != 1);
        if (pass == 1)
            EXPECT_EQ(options, saved);
        else
            EXPECT_DOUBLE_EQ(options.interpolation.vop_mu, 4.1e14);
        if (pass == 0)
            EXPECT_EQ(options.interpolation.method, core::OrbitInterpolationMethod::vop);
        if (pass == 2)
            EXPECT_EQ(options.interpolation.method, core::OrbitInterpolationMethod::lagrange);
    }
}

TEST_F(QuantityTest, HpopVopIntegrationSupportsOnlyRkfBsPreservesDraftAndCancels) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    for (int pass = 0; pass < 4; ++pass) {
        const auto saved = options;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *method = dialog->findChild<QComboBox *>("hpopIntegrator");
            auto *vop = dialog->findChild<QCheckBox *>("hpopVop");
            auto *interpolation = dialog->findChild<QComboBox *>("hpopInterpolation");
            ASSERT_TRUE(vop);
            EXPECT_EQ(interpolation->currentIndex(), 1);
            if (pass == 0) {
                EXPECT_FALSE(vop->isChecked());
                EXPECT_TRUE(vop->isEnabled());
                vop->setFocus();
                QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
                QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
                QApplication::sendEvent(vop, &press);
                QApplication::sendEvent(vop, &release);
                EXPECT_TRUE(vop->isChecked());
                for (int selected : {1, 2, 4, 0, 3}) {
                    method->setCurrentIndex(selected);
                    EXPECT_EQ(vop->isEnabled(), selected == 0 || selected == 3);
                    EXPECT_TRUE(vop->isChecked());
                }
            } else {
                EXPECT_TRUE(vop->isChecked());
                EXPECT_EQ(method->currentIndex(), pass == 3 ? 2 : 3);
                EXPECT_EQ(vop->isEnabled(), pass != 3);
                for (int width : {430, 720}) {
                    dialog->resize(width, 620);
                    QEventLoop settled;
                    QTimer::singleShot(60, &settled, &QEventLoop::quit);
                    settled.exec();
                    auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                    scroll->ensureWidgetVisible(vop);
                    QApplication::processEvents();
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                    EXPECT_TRUE(dialog->rect().contains(
                        dialog->findChild<QDialogButtonBox *>()->geometry()));
                    const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
                    if (!capture.isEmpty() && pass == 1) {
                        EXPECT_TRUE(QDir{}.mkpath(capture));
                        EXPECT_TRUE(dialog->grab().save(
                            capture + QString("/hpop-vop-integration-%1.png").arg(width)));
                    }
                }
                RecordProperty("device_pixel_ratio", dialog->devicePixelRatioF());
                if (pass == 1) {
                    vop->setChecked(false);
                    dialog->reject();
                    return;
                }
                method->setCurrentIndex(pass == 2 ? 2 : 0);
            }
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        EXPECT_EQ(edit_hpop_settings(options, nullptr), pass != 1);
        if (pass == 1) {
            EXPECT_EQ(options, saved);
        }
        EXPECT_TRUE(options.vop);
        EXPECT_EQ(options.uses_vop(), pass != 2);
        EXPECT_EQ(options.interpolation.method, core::OrbitInterpolationMethod::hermite);
    }
}

} // namespace orbitforge::desktop
