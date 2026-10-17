#include "graphics_workbench.hpp"
#include "eop_catalog.hpp"
#include "epoch_editor.hpp"
#include "form_kit.hpp"
#include "ground_map.hpp"
#include "ground_station_editor.hpp"
#include "initial_data_setup.hpp"
#include "insert_object_dialog.hpp"
#include "mission_timeline.hpp"
#include "orbit_editor.hpp"
#include "orbit_scene.hpp"
#include "project_scene.hpp"
#include "quantity_editor.hpp"
#include "report_dialog.hpp"
#include "responsive_layout.hpp"
#include "satellite_model.hpp"
#include "scenario_dialog.hpp"
#include "sensor_editor.hpp"
#include "startup_splash.hpp"
#include "welcome_page.hpp"
#include "workbench_profile.hpp"
#include "workbench_style.hpp"
#include "workspace.hpp"
#include <QUuid>
#include <numbers>
#include <orbitforge/runtime/access.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/gravity.hpp>
#include <orbitforge/runtime/ocean_tides.hpp>
#include <orbitforge/runtime/space_weather.hpp>
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
#include "benchmark_gpu.hpp"
#include "benchmark_presentation.hpp"
#include "benchmark_run.hpp"
#include "benchmark_scene.hpp"
#endif
#include <QAction>
#include <QApplication>
#include <QCalendarWidget>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QExposeEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontInfo>
#include <QFormLayout>
#include <QFrame>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimeZone>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <orbitforge/geo_rocky/globe.hpp>
#include <orbitforge/render_vsg/compile_manager.hpp>
#include <orbitforge/render_vsg/labels.hpp>
#include <orbitforge/render_vsg/marker_layer.hpp>
#include <orbitforge/render_vsg/swapchain_capture.hpp>
#include <orbitforge/runtime/demo.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>
#include <tuple>
#include <unordered_map>
#include <vsg/all.h>
#include <vsgQt/Window.h>

namespace orbitforge::desktop {
namespace {
constexpr std::array playback_rates{0.1,  0.5,  1.0,   2.0,   5.0,   10.0,
                                    20.0, 50.0, 100.0, 200.0, 500.0, 1000.0};
double adjacent_playback_rate(double current, bool faster) {
    if (faster) {
        const auto next = std::upper_bound(playback_rates.begin(), playback_rates.end(), current);
        return next == playback_rates.end() ? playback_rates.back() : *next;
    }
    const auto next = std::lower_bound(playback_rates.begin(), playback_rates.end(), current);
    return next == playback_rates.begin() ? playback_rates.front() : *std::prev(next);
}
QString display_tai(core::Epoch epoch, bool date = true) {
    // Display calendar only: TAI has fixed 86400-second days.
    const auto time = QDateTime(QDate(2000, 1, 1), QTime(12, 0), QTimeZone::UTC)
                          .addMSecs(epoch.tai_nanoseconds() / 1'000'000);
    return time.toString(date ? "yyyy-MM-dd  HH:mm:ss.zzz" : "HH:mm:ss");
}
struct ValidationLog {
    std::atomic_uint errors{0}, warnings{0};
    std::mutex mutex;
    QStringList messages;
    vsg::ref_ptr<vsg::Instance> instance;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    static VKAPI_ATTR VkBool32 VKAPI_CALL receive(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                  const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                  void *user) {
        auto &log = *static_cast<ValidationLog *>(user);
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            ++log.errors;
        else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            ++log.warnings;
        std::lock_guard lock(log.mutex);
        log.messages.push_back(QString::fromUtf8(data->pMessage));
        std::cerr << data->pMessage << '\n';
        return VK_FALSE;
    }
    void install(vsg::ref_ptr<vsg::Instance> value) {
        if (instance)
            return;
        instance = std::move(value);
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = receive;
        info.pUserData = this;
        const auto create = instance->getExtensions()->vkCreateDebugUtilsMessengerEXT;
        if (!create || create(instance->vk(), &info, nullptr, &messenger) != VK_SUCCESS) {
            throw std::runtime_error("Cannot install Vulkan validation reporting");
        }
    }
    ~ValidationLog() {
        if (messenger)
            instance->getExtensions()->vkDestroyDebugUtilsMessengerEXT(instance->vk(), messenger,
                                                                       nullptr);
    }
};

// Rebuild our rendering graphs when Qt recreates a native surface during docking.
class EmbeddedWindow final : public vsgQt::Window {
  public:
    EmbeddedWindow(vsg::ref_ptr<vsgQt::Viewer> viewer, vsg::ref_ptr<vsg::WindowTraits> traits)
        : vsgQt::Window(viewer, traits), owner_(viewer) {}
    std::function<void()> attached, detaching;
    std::function<void(QPointF)> pressed;
    std::function<void(const std::string &)> failed;
    std::function<void(const std::function<void()> &)> synchronize;
    void initializeWindow() override {
        if (windowAdapter)
            return;
        // vsgQt::cleanup clears its viewer on native surface destruction.
        viewer = owner_;
        vsgQt::Window::initializeWindow();
        if (!windowAdapter)
            throw std::runtime_error("Vulkan window creation failed");
        if (attached)
            attached();
    }

  protected:
    template <typename Function> void protect(Function function) {
        try {
            function();
        } catch (const std::exception &error) {
            if (failed)
                failed(error.what());
        } catch (const vsg::Exception &error) {
            if (failed)
                failed(error.message);
        }
    }
    void exposeEvent(QExposeEvent *event) override {
        protect([&] { vsgQt::Window::exposeEvent(event); });
    }
    void resizeEvent(QResizeEvent *event) override {
        protect([&] { exclusive([&] { vsgQt::Window::resizeEvent(event); }); });
    }
    void mousePressEvent(QMouseEvent *event) override {
        protect([&] {
            if (event->button() == Qt::LeftButton && pressed)
                pressed(event->position() * devicePixelRatio());
            vsgQt::Window::mousePressEvent(event);
        });
    }
    bool event(QEvent *event) override {
        if (event->type() == QEvent::PlatformSurface) {
            const auto *surface = static_cast<QPlatformSurfaceEvent *>(event);
            if (surface->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
                if (!windowAdapter)
                    return QWindow::event(event);
                bool result = false;
                protect([&] {
                    exclusive([&] {
                        if (detaching)
                            protect(detaching);
                        // Upstream cleanup waits for the device too, so it shares this barrier.
                        result = vsgQt::Window::event(event);
                    });
                });
                return result;
            }
        }
        return vsgQt::Window::event(event);
    }

  private:
    void exclusive(const std::function<void()> &operation) {
        if (synchronize)
            synchronize(operation);
        else
            operation();
    }
    vsg::ref_ptr<vsgQt::Viewer> owner_;
};

// QToolBar recomputes its layout margins from style metrics on every style change. The
// timeline card must span the full toolbar to line up with the panels above, so the margins
// are cleared after each recomputation.
class FlushToolBar final : public QToolBar {
  public:
    using QToolBar::QToolBar;

  protected:
    bool event(QEvent *event) override {
        const bool handled = QToolBar::event(event);
        if ((event->type() == QEvent::StyleChange || event->type() == QEvent::Polish) && layout())
            layout()->setContentsMargins(0, 0, 0, 0);
        return handled;
    }
};

class Workbench final : public QMainWindow {
    struct Station {
        application::ProjectGroundStation config;
        QTreeWidgetItem *item = nullptr;
        [[nodiscard]] QString name() const { return QString::fromStdString(config.name()); }
    };
    struct Satellite {
        std::shared_ptr<const OrbitScene> orbit;
        std::string display_name;
        [[nodiscard]] QString name() const {
            return QString::fromStdString(display_name.empty() ? orbit->input.object_id
                                                               : display_name);
        }
        QTreeWidgetItem *item = nullptr;
        vsg::ref_ptr<vsg::Group> model_host = vsg::Group::create();
        vsg::ref_ptr<vsg::MatrixTransform> model;
        int model_kind = 0;
        double model_diameter_km = 1000;
        QString model_path;
        DisplaySettings display;
        std::shared_ptr<render_vsg::TrackLayer> fixed_track, inertial_track;
        vsg::ref_ptr<vsg::Switch> tracks;
    };

  public:
    Workbench(QApplication &app, ValidationLog &log, const QString &smoke_dir, bool validation,
              const QString &benchmark_manifest, const QString &timing_dir, bool timing_probe,
              bool output_only_probe)
        : app_(app), log_(log), smoke_dir_(smoke_dir), validation_(validation),
          benchmarking_(!benchmark_manifest.isEmpty()), frames_(runtime::make_frame_provider()),
          eop_(runtime::load_eop_snapshot(
              std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) /
              "resources/eop/manifest.json")),
          viewer_(vsgQt::Viewer::create()),
          markers_(std::make_unique<render_vsg::MarkerLayer>(10000)),
          selection_(
              std::make_unique<render_vsg::MarkerLayer>(1, render_vsg::MarkerStyle::selection)) {
        try {
            const auto args = app.arguments();
            const auto profile_index = args.indexOf("--workbench-profile");
            if (profile_index >= 0) {
                if (profile_index + 1 >= args.size() || benchmarking_ || !smoke_dir.isEmpty())
                    throw std::invalid_argument(
                        "Use --workbench-profile <new directory> separately");
                profile_ = std::make_unique<WorkbenchProfile>(args[profile_index + 1]);
                const auto count_index = args.indexOf("--profile-satellites");
                if (count_index >= 0)
                    profile_count_ = args.value(count_index + 1).toInt();
                if (profile_count_ < 1 || profile_count_ > 128)
                    throw std::invalid_argument("Profile satellite count must be in [1, 128]");
            }
            playing_ = benchmarking_;
            eop_history_ = eop_;
            if (!benchmarking_)
                eop_current_ = runtime::load_eop_snapshot(
                    std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) /
                    "resources/eop-current/manifest.json");
            if (!benchmarking_) {
                const auto configured_home = qEnvironmentVariable("ORBITFORGE_DATA_HOME");
                const auto normal_home =
                    configured_home.isEmpty()
                        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                              "/data"
                        : QDir(configured_home).absolutePath();
                const auto store = smoke_dir_.isEmpty() ? normal_home : smoke_dir_ + "/data-store";
                const auto resources =
                    std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) /
                    "resources";
                eop_catalog_ = std::make_unique<EopCatalog>(
                    std::filesystem::path(store.toStdWString()),
                    std::vector<std::filesystem::path>{resources / "eop-current/manifest.json",
                                                       resources / "eop/manifest.json"},
                    smoke_dir_.isEmpty() && configured_home.isEmpty()
                        ? std::filesystem::path((QStandardPaths::writableLocation(
                                                     QStandardPaths::AppLocalDataLocation) +
                                                 "/eop")
                                                    .toStdWString())
                        : std::filesystem::path{},
                    false);
                eop_current_ = eop_catalog_->providers().at(0);
                eop_history_ = eop_catalog_->providers().at(1);
                eop_ = eop_history_;
            }
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            if (benchmarking_)
                benchmark_ = std::make_unique<BenchmarkScene>(benchmark_manifest, *frames_, *eop_);
            if (!timing_dir.isEmpty()) {
                run_ = std::make_unique<BenchmarkRun>(timing_dir, timing_probe, output_only_probe);
                playing_ = false;
            }
#else
            (void)timing_probe;
            (void)output_only_probe;
            (void)timing_dir;
            if (benchmarking_)
                throw std::runtime_error("This build does not enable graphics benchmarks");
#endif
            viewer_->requests.store(0);
            viewer_->continuousUpdate = false;
            hints_ = vsg::ResourceHints::create();
            // Background static uploads must finish before a task/window is replaced.
            hints_->dataTransferHint = vsg::COMPILE_TRAVERSAL_TRANSFER_DATA_AND_WAIT;
            const auto resources =
                std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) /
                "resources";
            globe_ = geo_rocky::make_globe(viewer_, benchmarking_ ? std::filesystem::path{}
                                                                  : resources / "earth/earth.vrt");
            scene_ = vsg::Group::create();
            if (benchmarking_)
                scene_->addChild(globe_->node());
            else {
                earth_transform_ = vsg::MatrixTransform::create();
                earth_transform_->addChild(globe_->node());
                scene_->addChild(earth_transform_);
            }
            scene_->addChild(markers_->node());
            if (!benchmarking_) {
                model_group_ = vsg::Group::create();
                scene_->addChild(model_group_);
                // One title row: brand and menus on the left, the scene in the centre and icon
                // commands on the right. Narrow windows drop the wordmark, then the scene chip;
                // QMenuBar folds overflowing menus into its extension button.
                // Every region is a rounded panel floating on the canvas, 8 px apart.
                setContentsMargins(6, 6, 6, 0);
                auto *title_host = new QWidget;
                auto *title_host_layout = new QVBoxLayout(title_host);
                title_host_layout->setContentsMargins(0, 0, 0, 6);
                auto *title_bar = new QWidget;
                title_bar->setObjectName("titleBar");
                title_bar->setFixedHeight(40);
                title_host_layout->addWidget(title_bar);
                auto *title_layout = new QHBoxLayout(title_bar);
                title_layout->setContentsMargins(10, 0, 6, 0);
                title_layout->setSpacing(6);
                auto *symbol = new QLabel;
                symbol->setObjectName("brandMark");
                symbol->setPixmap(brand_mark(22, devicePixelRatioF()));
                title_layout->addWidget(symbol);
                auto *name = new QLabel("OrbitForge");
                name->setObjectName("brand");
                title_layout->addWidget(name);
                menu_bar_ = new QMenuBar;
                menu_bar_->setObjectName("titleMenus");
                menu_bar_->setNativeMenuBar(false);
                title_layout->addWidget(menu_bar_, 0, Qt::AlignVCenter);
                title_layout->addStretch(1);
                auto *chip = new QFrame;
                chip->setObjectName("workspaceChip");
                chip->setFixedHeight(24);
                auto *chip_layout = new QHBoxLayout(chip);
                chip_layout->setContentsMargins(12, 0, 14, 0);
                chip_layout->setSpacing(8);
                workspace_dot_ = new QLabel;
                workspace_dot_->setObjectName("workspaceDot");
                workspace_dot_->setFixedSize(6, 6);
                chip_layout->addWidget(workspace_dot_);
                workspace_label_ = new QLabel(QStringLiteral("未创建场景"));
                workspace_label_->setObjectName("workspaceName");
                workspace_label_->setMaximumWidth(260);
                chip_layout->addWidget(workspace_label_);
                title_layout->addWidget(chip);
                title_layout->addStretch(1);
                commands_ = new QToolBar(QStringLiteral("场景与图形"));
                commands_->setObjectName("commandBar");
                commands_->setMovable(false);
                commands_->setIconSize(QSize(16, 16));
                commands_->setToolButtonStyle(Qt::ToolButtonIconOnly);
                title_layout->addWidget(commands_);
                setMenuWidget(title_host);
                new WidthObserver(title_bar, [name, chip, wide = WidthMode{},
                                              roomy = WidthMode{}](int width) mutable {
                    name->setVisible(!wide.narrow(width, 1180));
                    chip->setVisible(!roomy.narrow(width, 960));
                });
                scene_menu_ = menu_bar_->addMenu(QStringLiteral("场景"));
                insert_menu_ = menu_bar_->addMenu(QStringLiteral("插入"));
                view_menu_ = menu_bar_->addMenu(QStringLiteral("视图"));
                time_menu_ = menu_bar_->addMenu(QStringLiteral("时间"));
                new_action_ = commands_->addAction(workbench_icon(WorkbenchIcon::new_scenario),
                                                   QStringLiteral("新建场景"));
                new_action_->setShortcut(QKeySequence("Ctrl+N"));
                new_action_->setToolTip(QStringLiteral("新建场景 · Ctrl+N"));
                connect(new_action_, &QAction::triggered, this, [this] { create_scenario(); });
                scene_menu_->addAction(new_action_);
                open_action_ = commands_->addAction(workbench_icon(WorkbenchIcon::folder),
                                                    QStringLiteral("打开"));
                open_action_->setShortcut(QKeySequence::Open);
                open_action_->setObjectName("openProject");
                open_action_->setToolTip(QStringLiteral("打开项目 · Ctrl+O"));
                connect(open_action_, &QAction::triggered, this, [this] { choose_project(); });
                scene_menu_->addAction(open_action_);
                save_action_ = commands_->addAction(workbench_icon(WorkbenchIcon::save),
                                                    QStringLiteral("保存"));
                save_action_->setShortcut(QKeySequence::Save);
                save_action_->setObjectName("saveProject");
                save_action_->setToolTip(QStringLiteral("保存已应用的场景和对象属性 · Ctrl+S"));
                connect(save_action_, &QAction::triggered, this,
                        [this] { (void)save_project_dialog(false); });
                scene_menu_->addAction(save_action_);
                save_as_action_ = scene_menu_->addAction(QStringLiteral("另存为…"));
                save_as_action_->setShortcut(QKeySequence::SaveAs);
                connect(save_as_action_, &QAction::triggered, this,
                        [this] { (void)save_project_dialog(true); });
                example_action_ = scene_menu_->addAction(QStringLiteral("打开二体示例"));
                connect(example_action_, &QAction::triggered, this, [this] { open_example(); });
                auto *data_menu = menu_bar_->addMenu(QStringLiteral("数据"));
                eop_action_ = data_menu->addAction(workbench_icon(WorkbenchIcon::database),
                                                   QStringLiteral("数据中心…"));
                eop_action_->setObjectName("manageEopData");
                connect(eop_action_, &QAction::triggered, this, [this] { manage_eop_data(); });
                auto *analysis_menu = menu_bar_->addMenu(QStringLiteral("分析"));
                report_action_ = analysis_menu->addAction(workbench_icon(WorkbenchIcon::properties),
                                                          QStringLiteral("报告…"));
                report_action_->setObjectName("openReports");
                connect(report_action_, &QAction::triggered, this, [this] { show_reports(); });
                scene_menu_->addSeparator();
                // Analysis and data were only reachable from menus; keep them one click away.
                commands_->addSeparator();
                report_action_->setToolTip(QStringLiteral("报告与曲线"));
                commands_->addAction(report_action_);
                eop_action_->setToolTip(QStringLiteral("数据中心 · 地球定向、引力场与星历"));
                commands_->addAction(eop_action_);
                // Inserting lives with the objects it creates: the explorer's add button,
                // the Insert menu and Ctrl+Shift+I.
                insert_action_ = new QAction(workbench_icon(WorkbenchIcon::add),
                                             QStringLiteral("插入对象…"), this);
                insert_action_->setShortcut(QKeySequence("Ctrl+Shift+I"));
                insert_action_->setToolTip(QStringLiteral("插入卫星或地面站 · Ctrl+Shift+I"));
                connect(insert_action_, &QAction::triggered, this,
                        [this] { show_insert_objects(); });
                insert_menu_->addAction(insert_action_);
            }
            if (benchmarking_) {
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
                for (std::size_t i = 0; i < 100; ++i)
                    scene_->addChild(render_vsg::make_track(benchmark_->cache->track(i * 100)));
#endif
            } else {
                orbit_group_ = vsg::Group::create();
                orbit_switch_ = vsg::Switch::create();
                orbit_switch_->addChild(true, orbit_group_);
                scene_->addChild(orbit_switch_);
            }
            snapshot_ = sample_frame(core::Epoch{0});
            markers_->update(snapshot_);
            initial_position_ = snapshot_.markers.empty() ? std::array<double, 3>{7e6, 0, 0}
                                                          : snapshot_.markers.front().position_m;
            scene_->addChild(selection_->node());
            setWindowTitle(benchmarking_ ? QStringLiteral("OrbitForge — 万星双视图基准场景")
                                         : QStringLiteral("OrbitForge  |  新建场景"));
            resize_to_screen(1600, 960);
            auto *objects = new QTreeWidget;
            objects_ = objects;
            objects->setObjectName("sceneObjects");
            objects->setHeaderLabel(QStringLiteral("场景"));
            if (!benchmarking_)
                objects->setHeaderHidden(true);
            if (benchmarking_) {
                auto *scenario =
                    new QTreeWidgetItem(objects, {QStringLiteral("固定星座 · 10,000 颗")});
                object_item_ =
                    new QTreeWidgetItem(scenario, {QStringLiteral("100 条轨迹 · 每视图 200 标签")});
            }
            objects->expandAll();
            if (!benchmarking_) {
                objects->setIconSize(QSize(18, 18));
                objects->setIndentation(16);
                objects->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                connect(objects, &QTreeWidget::currentItemChanged, this,
                        [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
                            set_selection(object_index(item), false);
                        });
            }
            auto *object_dock = new QDockWidget(QStringLiteral("对象浏览器"), this);
            object_dock_ = object_dock;
            if (benchmarking_) {
                object_dock->setWidget(objects);
            } else {
                auto *explorer = new QWidget;
                explorer->setObjectName("objectExplorer");
                auto *layout = new QVBoxLayout(explorer);
                layout->setContentsMargins(8, 8, 8, 8);
                layout->setSpacing(6);
                auto *search = new QLineEdit;
                search->setObjectName("objectSearch");
                search->setPlaceholderText(QStringLiteral("搜索对象…"));
                search->addAction(workbench_icon(WorkbenchIcon::search),
                                  QLineEdit::LeadingPosition);
                search->setClearButtonEnabled(true);
                connect(search, &QLineEdit::textChanged, objects, [this] { filter_objects(); });
                auto *search_row = new QHBoxLayout;
                search_row->setSpacing(6);
                search_row->addWidget(search, 1);
                auto *add = new QToolButton;
                add->setObjectName("explorerInsert");
                add->setDefaultAction(insert_action_);
                add->setToolButtonStyle(Qt::ToolButtonIconOnly);
                add->setFixedSize(26, 26);
                add->setIconSize(QSize(16, 16));
                add->setAccessibleName(QStringLiteral("插入对象"));
                search_row->addWidget(add);
                layout->addLayout(search_row);
                layout->addWidget(objects, 1);
                object_dock->setWidget(explorer);
                style_dock(object_dock, WorkbenchIcon::orbit);
            }
            addDockWidget(Qt::LeftDockWidgetArea, object_dock);
            object_dock->setMaximumWidth(benchmarking_ ? 220 : 290);
            if (!benchmarking_)
                object_dock->setMinimumWidth(200);
            if (!benchmarking_ && runtime::two_body_available()) {
                editor_ = new OrbitEditor(OrbitInput{});
                editor_->data_catalog = eop_catalog_.get();
                editor_->gravity_models = [this] { return eop_catalog_->data().gravity_models(); };
                editor_->ephemerides = [this] { return eop_catalog_->data().ephemerides(); };
                editor_->manage_ephemerides = [this] {
                    show_data_center(editor_, *eop_catalog_, DataProduct::de440s);
                    return eop_catalog_->data().ephemerides();
                };
                editor_->transform_state = [this](core::CartesianState state, core::FrameId frame) {
                    if (state.frame == frame)
                        return state;
                    auto installed_data = eop_catalog_->providers();
                    installed_data.insert(installed_data.begin(), eop_);
                    for (const auto &source : installed_data) {
                        if (!source)
                            continue;
                        const auto &coverage = source->data_info();
                        if (!core::eop_covers(coverage, state.epoch))
                            continue;
                        const auto transform =
                            frames_->gcrf_to_itrf_state(state.epoch, source->at(state.epoch));
                        return frame == core::FrameId::itrf ? transform.apply(state)
                                                            : transform.inverse(state);
                    }
                    throw std::out_of_range("初始历元超出已安装的地球定向数据范围。");
                };
                properties_dialog_ = new QDialog(this, Qt::Window);
                properties_dialog_->setAttribute(Qt::WA_QuitOnClose, false);
                properties_dialog_->setWindowTitle(QStringLiteral("对象属性"));
                properties_dialog_->resize(
                    980, std::min(660, screen()->availableGeometry().height() - 100));
                properties_dialog_->setMinimumSize(320, 280);
                auto *dialog_layout = new QVBoxLayout(properties_dialog_);
                dialog_layout->setSizeConstraint(QLayout::SetNoConstraint);
                dialog_layout->setContentsMargins(0, 0, 0, 0);
                dialog_layout->setSpacing(0);
                inspector_pages_ = new QStackedWidget;
                inspector_pages_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
                inspector_pages_->addWidget(editor_);
                auto *empty = new QLabel(QStringLiteral("选择卫星"));
                empty->setObjectName("muted");
                empty->setAlignment(Qt::AlignCenter);
                empty->setWordWrap(true);
                inspector_pages_->addWidget(empty);
                dialog_layout->addWidget(inspector_pages_, 1);
                resizeDocks({object_dock}, {224}, Qt::Horizontal);
                editor_->apply = [this](OrbitInput input) { start_orbit(std::move(input)); };
                editor_->close_requested = [this] { properties_dialog_->close(); };
                editor_->display = [this](DisplaySettings settings) {
                    if (!editor_target_ || busy())
                        return;
                    try {
                        auto &satellite = satellites_.at(*editor_target_);
                        (void)display_interval(*satellite.orbit, settings, snapshot_.epoch);
                        satellite.display = settings;
                        satellite.item->setIcon(
                            0, workbench_icon(WorkbenchIcon::satellite,
                                              QColor::fromRgb(settings.line_color)));
                        mark_project_dirty();
                        update_satellite();
                        statusBar()->showMessage(QStringLiteral("已应用显示设置，将按需更新轨迹"),
                                                 5000);
                    } catch (const std::exception &error) {
                        QMessageBox::information(this, QStringLiteral("显示范围无效"),
                                                 QString::fromUtf8(error.what()));
                    }
                };
                editor_->cancel = [this] {
                    if (project_candidate_.valid() && project_orbit_edit_ && project_cancellation_)
                        project_cancellation_->store(true);
                    if (cancellation_)
                        cancellation_->store(true);
                };
                editor_->appearance = [this](int kind, const QString &path, double diameter) {
                    start_model(kind, path, diameter);
                };
                auto *properties = new QAction(workbench_icon(WorkbenchIcon::properties),
                                               QStringLiteral("属性"), this);
                addAction(properties);
                properties_action_ = properties;
                properties->setShortcut(QKeySequence("Ctrl+I"));
                connect(properties, &QAction::triggered, this, [this] { open_properties(); });
                connect(objects, &QTreeWidget::itemDoubleClicked, this,
                        [this](QTreeWidgetItem *item, int) {
                            const auto index = object_index(item);
                            if (!index) {
                                if (item == scenario_item_)
                                    open_scenario_properties();
                                return;
                            }
                            set_selection(index, false);
                            open_properties();
                        });
                scene_menu_->addAction(properties);
                auto *object_menu = menu_bar_->insertMenu(view_menu_->menuAction(),
                                                          new QMenu(QStringLiteral("对象"), this));
                auto *object_commands = object_menu->menu();
                object_commands->addAction(properties);
                rename_action_ = object_commands->addAction(QStringLiteral("重命名…"));
                rename_action_->setObjectName("renameObject");
                rename_action_->setShortcut(QKeySequence("F2"));
                connect(rename_action_, &QAction::triggered, this, [this] { rename_selected(); });
                duplicate_action_ = object_commands->addAction(QStringLiteral("复制对象"));
                duplicate_action_->setObjectName("duplicateObject");
                duplicate_action_->setShortcut(QKeySequence("Ctrl+D"));
                connect(duplicate_action_, &QAction::triggered, this,
                        [this] { duplicate_selected(); });
                delete_action_ = object_commands->addAction(QStringLiteral("删除…"));
                delete_action_->setObjectName("deleteObject");
                delete_action_->setShortcut(QKeySequence::Delete);
                connect(delete_action_, &QAction::triggered, this, [this] { delete_selected(); });
                scene_menu_->addSeparator();
                auto *quit = scene_menu_->addAction(QStringLiteral("退出"));
                connect(quit, &QAction::triggered, this, &QWidget::close);
                view_menu_->addAction(object_dock->toggleViewAction());
                show_orbit_status();
            }
            auto *toolbar = benchmarking_ ? new QToolBar(QStringLiteral("时间"), this)
                                          : new FlushToolBar(QStringLiteral("时间"), this);
            time_toolbar_ = toolbar;
            if (!benchmarking_)
                toolbar->setObjectName("timelineBar");
            addToolBar(benchmarking_ ? Qt::TopToolBarArea : Qt::BottomToolBarArea, toolbar);
            toolbar->setMovable(false);
            toolbar->setIconSize(QSize(18, 18));
            toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            if (run_)
                toolbar->setEnabled(false);
#endif
            auto *play =
                new QAction(play_icon(playing_),
                            playing_ ? QStringLiteral("暂停") : QStringLiteral("播放"), this);
            play_action_ = play;
            connect(play, &QAction::triggered, this, [this] { set_playing(!playing_); });
            if (benchmarking_) {
                toolbar->addAction(play);
                slider_ = new QSlider(Qt::Horizontal);
                slider_->setRange(0, 600);
                slider_->setMinimumWidth(300);
                toolbar->addWidget(slider_);
            } else {
                auto *timeline = new QWidget;
                timeline->setObjectName("timelineCard");
                timeline->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                // Keep the toolbar's cached single-row hint from blocking the narrow layout.
                timeline->setMinimumWidth(560);
                auto *layout = new QVBoxLayout(timeline);
                layout->setContentsMargins(1, 0, 1, 4);
                layout->setSpacing(0);
                auto *control_panel = new QWidget;
                control_panel->setObjectName("timelineControls");
                auto *control_rows = new QHBoxLayout(control_panel);
                control_rows->setContentsMargins(8, 3, 8, 3);
                control_rows->setSpacing(4);
                auto *transport = new QHBoxLayout;
                auto *controls = transport;
                controls->setContentsMargins(0, 0, 0, 0);
                controls->setSpacing(4);
                auto *heading = new QLabel(QStringLiteral("时间轴"));
                heading->setObjectName("timelineTitle");
                heading->setMinimumWidth(60);
                controls->addWidget(heading);
                auto *start = new QAction(workbench_icon(WorkbenchIcon::first),
                                          QStringLiteral("分析起点"), this);
                auto *end = new QAction(workbench_icon(WorkbenchIcon::last),
                                        QStringLiteral("分析终点"), this);
                auto *slower = new QAction(workbench_icon(WorkbenchIcon::slower),
                                           QStringLiteral("减慢播放"), this);
                auto *faster = new QAction(workbench_icon(WorkbenchIcon::faster),
                                           QStringLiteral("加快播放"), this);
                slower->setObjectName("slowerPlayback");
                faster->setObjectName("fasterPlayback");
                slower->setShortcut(QKeySequence("Ctrl+["));
                faster->setShortcut(QKeySequence("Ctrl+]"));
                start->setShortcut(QKeySequence("Ctrl+Home"));
                end->setShortcut(QKeySequence("Ctrl+End"));
                play->setShortcut(QKeySequence("Ctrl+Space"));
                connect(start, &QAction::triggered, this, [this] {
                    set_playing(false);
                    if (scenario_)
                        seek_epoch(scenario_->first());
                });
                connect(end, &QAction::triggered, this, [this] {
                    set_playing(false);
                    if (scenario_)
                        seek_epoch(scenario_->last());
                });
                auto *transport_group = new QFrame;
                transport_group->setObjectName("transportGroup");
                auto *transport_buttons = new QHBoxLayout(transport_group);
                transport_buttons->setContentsMargins(2, 2, 2, 2);
                transport_buttons->setSpacing(1);
                for (auto *action : {start, slower, play, faster, end}) {
                    auto *button = new QToolButton;
                    button->setDefaultAction(action);
                    button->setFixedSize(28, 28);
                    button->setIconSize(QSize(action == play ? 16 : 18, action == play ? 16 : 18));
                    if (action == play)
                        button->setObjectName("transportPlay");
                    button->setAccessibleName(action->text());
                    transport_buttons->addWidget(button);
                    time_menu_->addAction(action);
                }
                controls->addWidget(transport_group);
                time_menu_->addSeparator();
                auto *jump = time_menu_->addAction(QStringLiteral("跳转到指定时间…"));
                jump->setShortcut(QKeySequence("Ctrl+T"));
                jump->setToolTip(QStringLiteral("跳转到指定时间 · Ctrl+T"));
                connect(jump, &QAction::triggered, this, [this] { choose_time(); });
                auto *speed = new QDoubleSpinBox;
                speed->setObjectName("playbackRate");
                speed->setButtonSymbols(QAbstractSpinBox::NoButtons);
                speed->setKeyboardTracking(false);
                speed->setDecimals(1);
                speed->setRange(playback_rates.front(), playback_rates.back());
                speed->setValue(playback_rate_);
                speed->setSuffix(QStringLiteral(" ×"));
                speed->setMinimumWidth(0);
                speed->setToolTip(QStringLiteral("仿真时间相对实际时间的播放倍率"));
                speed->setAccessibleName(QStringLiteral("播放倍率"));
                auto *rate_field = new QFrame;
                rate_field->setObjectName("playbackRateField");
                rate_field->setFixedWidth(110);
                auto *rate_layout = new QHBoxLayout(rate_field);
                rate_layout->setContentsMargins(1, 1, 1, 1);
                rate_layout->setSpacing(0);
                rate_layout->addWidget(speed, 1);
                auto *presets = new QMenu(QStringLiteral("播放倍率"), this);
                presets->setObjectName("playbackRateMenu");
                for (const double rate : playback_rates) {
                    auto *action = presets->addAction(QString::number(rate, 'g', 5) + " ×");
                    action->setCheckable(true);
                    action->setData(rate);
                }
                const auto update_rate = [this, speed, slower, faster, presets] {
                    const double rate = speed->value();
                    playback_rate_ = rate;
                    slower->setEnabled(rate > playback_rates.front());
                    faster->setEnabled(rate < playback_rates.back());
                    slower->setToolTip(QStringLiteral("减慢至 %1× · Ctrl+[")
                                           .arg(adjacent_playback_rate(rate, false), 0, 'g', 5));
                    faster->setToolTip(QStringLiteral("加快至 %1× · Ctrl+]")
                                           .arg(adjacent_playback_rate(rate, true), 0, 'g', 5));
                    for (auto *action : presets->actions())
                        action->setChecked(action->data().toDouble() == rate);
                };
                for (auto *action : presets->actions())
                    connect(action, &QAction::triggered, this, [speed, action, update_rate] {
                        speed->setValue(action->data().toDouble());
                        update_rate();
                    });
                connect(speed, &QDoubleSpinBox::valueChanged, this, update_rate);
                for (const auto &[action, increase] :
                     {std::pair{slower, false}, std::pair{faster, true}})
                    connect(action, &QAction::triggered, this, [speed, increase] {
                        speed->interpretText();
                        speed->setValue(adjacent_playback_rate(speed->value(), increase));
                    });
                connect(presets, &QMenu::aboutToShow, this, [presets, update_rate] {
                    update_rate();
                    for (auto *action : presets->actions())
                        if (action->isChecked())
                            presets->setActiveAction(action);
                });
                auto *preset_button = new QToolButton;
                preset_button->setObjectName("playbackRatePresets");
                preset_button->setIcon(workbench_icon(WorkbenchIcon::chevron_down));
                preset_button->setIconSize(QSize(14, 14));
                preset_button->setFixedSize(24, 26);
                preset_button->setPopupMode(QToolButton::InstantPopup);
                preset_button->setToolTip(QStringLiteral("选择播放倍率"));
                preset_button->setAccessibleName(preset_button->toolTip());
                preset_button->setMenu(presets);
                rate_layout->addWidget(preset_button);
                controls->addWidget(rate_field);
                time_menu_->addMenu(presets);
                update_rate();
                timeline_ = new MissionTimeline;
                slider_ = timeline_;
                time_menu_->addSeparator();
                auto *object_interval = time_menu_->addAction(QStringLiteral("显示所选对象区间"));
                object_interval->setObjectName("showTimelineObject");
                object_interval->setCheckable(true);
                connect(object_interval, &QAction::toggled, this,
                        [this](bool show) { timeline_->set_show_object_interval(show); });
                connect(slider_, &QSlider::sliderPressed, this, [this] { set_playing(false); });
                timeline_->seek_requested = [this](qint64 tai_ms) {
                    if (!scenario_)
                        return;
                    set_playing(false);
                    const auto epoch = tai_ms <= scenario_->first().tai_nanoseconds() / 1'000'000
                                           ? scenario_->first()
                                       : tai_ms >= scenario_->last().tai_nanoseconds() / 1'000'000
                                           ? scenario_->last()
                                           : core::Epoch{tai_ms * 1'000'000};
                    seek_epoch(epoch);
                };
                auto *divider = new QFrame;
                divider->setObjectName("timelineDivider");
                divider->setFrameShape(QFrame::VLine);
                divider->setFixedHeight(18);
                controls->addWidget(divider);
                for (const auto &[icon, text, name, command] :
                     {std::tuple{WorkbenchIcon::zoom_out, QStringLiteral("缩小时间轴 · −"),
                                 "timelineZoomOut",
                                 std::function<void()>{[this] { timeline_->zoom(0.5); }}},
                      std::tuple{WorkbenchIcon::zoom_in, QStringLiteral("放大时间轴 · +"),
                                 "timelineZoomIn",
                                 std::function<void()>{[this] { timeline_->zoom(2); }}},
                      std::tuple{WorkbenchIcon::focus, QStringLiteral("定位当前时间 · C"),
                                 "timelineCenter",
                                 std::function<void()>{[this] { timeline_->center_current(); }}},
                      std::tuple{WorkbenchIcon::time_fit, QStringLiteral("显示全时段 · F"),
                                 "timelineFit",
                                 std::function<void()>{[this] { timeline_->reset_view(); }}}}) {
                    auto *button = new QToolButton;
                    button->setIcon(workbench_icon(icon));
                    button->setObjectName(name);
                    button->setToolTip(text);
                    button->setAccessibleName(text);
                    button->setFixedSize(28, 28);
                    button->setIconSize(QSize(18, 18));
                    connect(button, &QToolButton::clicked, this, command);
                    controls->addWidget(button);
                }
                controls->addStretch();
                control_rows->addLayout(transport, 1);
                controls = new QHBoxLayout;
                controls->setContentsMargins(0, 0, 0, 0);
                controls->setSpacing(4);
                controls->addStretch();
                time_label_ = new QToolButton;
                time_label_->setObjectName("timeReadout");
                time_label_->setDefaultAction(jump);
                time_label_->setToolButtonStyle(Qt::ToolButtonTextOnly);
                controls->addWidget(time_label_);
                auto *time_scale = new QComboBox;
                time_scale->setObjectName("timelineScale");
                time_scale->addItems({"UTC", "TAI", "TT"});
                time_scale->setAccessibleName(QStringLiteral("时间轴显示标准"));
                controls->addWidget(time_scale);
                connect(time_scale, &QComboBox::currentIndexChanged, this, [this](int index) {
                    display_scale_ = static_cast<CalendarScale>(index);
                    if (timeline_)
                        timeline_->set_scale(display_scale_);
                });
                auto *expand = time_menu_->addAction(QStringLiteral("展开时间轴"));
                expand->setObjectName("expandTimeline");
                expand->setCheckable(true);
                expand->setChecked(true);
                expand->setShortcut(QKeySequence("Ctrl+J"));
                auto *collapse = new QToolButton;
                collapse->setObjectName("collapseTimeline");
                collapse->setFixedSize(28, 28);
                collapse->setIconSize(QSize(16, 16));
                collapse->setIcon(workbench_icon(WorkbenchIcon::chevron_down));
                collapse->setToolTip(QStringLiteral("收起时间轴 · Ctrl+J"));
                collapse->setAccessibleName(collapse->toolTip());
                connect(collapse, &QToolButton::clicked, expand, &QAction::toggle);
                connect(expand, &QAction::toggled, this, [this, collapse, timeline](bool expanded) {
                    timeline_->setVisible(expanded);
                    // QToolBar caches the widget action's size hint; refresh both layouts.
                    timeline->layout()->activate();
                    time_toolbar_->layout()->invalidate();
                    time_toolbar_->updateGeometry();
                    this->layout()->invalidate();
                    this->layout()->activate();
                    collapse->setIcon(workbench_icon(expanded ? WorkbenchIcon::chevron_down
                                                              : WorkbenchIcon::chevron_up));
                    collapse->setToolTip(expanded ? QStringLiteral("收起时间轴 · Ctrl+J")
                                                  : QStringLiteral("展开时间轴 · Ctrl+J"));
                    collapse->setAccessibleName(collapse->toolTip());
                });
                controls->addWidget(collapse);
                control_rows->addLayout(controls);
                layout->addWidget(control_panel);
                layout->addWidget(timeline_);
                // The gap above the card lives in a host widget: toolbar padding would also
                // inset the card horizontally and misalign it with the panels above.
                auto *timeline_host = new QWidget;
                timeline_host->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                auto *host_layout = new QVBoxLayout(timeline_host);
                host_layout->setContentsMargins(0, 6, 0, 0);
                host_layout->addWidget(timeline);
                toolbar->addWidget(timeline_host);
                responsive_columns(timeline, control_rows, 1000);
                auto *credit = new QLabel("NASA Blue Marble");
                credit->setObjectName("hint");
                statusBar()->addPermanentWidget(credit);
                show_orbit_status();
            }
            connect(slider_, &QSlider::valueChanged, this, [this](int value) {
                seconds_ = benchmarking_ ? value : duration_seconds() * value / 10000.0;
                precise_epoch_.reset();
                requested_time_ = true;
                clock_.restart();
            });
            build_slot(false);
            build_slot(true);
            if (!benchmarking_) {
                ground_map_ = new GroundMap(QString::fromStdWString(
                    (resources / "earth/land_ocean_ice_2048.jpg").wstring()));
                ground_map_->select = [this](std::optional<std::size_t> index) {
                    set_selection(index);
                };
                map_panel_ = new QWidget;
                auto *map_layout = new QVBoxLayout(map_panel_);
                map_layout->setContentsMargins(0, 0, 0, 0);
                map_layout->setSpacing(0);
                map_layout->addWidget(make_view_controls(std::nullopt));
                map_layout->addWidget(ground_map_, 1);
                workspace_->add_view(map_panel_, QStringLiteral("二维视图"),
                                     workbench_icon(WorkbenchIcon::map));
                map_action_ = new QAction(QStringLiteral("二维视图"), this);
                map_action_->setIcon(workbench_icon(WorkbenchIcon::map));
                map_action_->setShortcut(QKeySequence("Ctrl+3"));
                view_menu_->addAction(map_action_);
                connect(map_action_, &QAction::triggered, this,
                        [this] { workspace_->select_view(1); });
                auto *three_d = view_menu_->addAction(workbench_icon(WorkbenchIcon::earth),
                                                      QStringLiteral("三维视图"));
                three_d->setShortcut(QKeySequence("Ctrl+1"));
                connect(three_d, &QAction::triggered, this, [this] { workspace_->select_view(0); });
                fit_views();
                auto *fit = new QAction(workbench_icon(WorkbenchIcon::fit),
                                        QStringLiteral("适配轨道"), this);
                fit_action_ = fit;
                fit->setShortcut(QKeySequence("Ctrl+0"));
                connect(fit, &QAction::triggered, this, [this] { fit_views(); });
                auto *focus = new QAction(workbench_icon(WorkbenchIcon::focus),
                                          QStringLiteral("定位对象"), this);
                focus_action_ = focus;
                focus->setShortcut(QKeySequence("Ctrl+L"));
                connect(focus, &QAction::triggered, this, [this] {
                    focus_satellite();
                    ground_map_->focus_selection();
                });
                view_menu_->addSeparator();
                view_menu_->addAction(fit);
                view_menu_->addAction(focus);
                objects->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(objects, &QWidget::customContextMenuRequested, this,
                        [this, objects, focus](const QPoint &point) {
                            auto *item = objects->itemAt(point);
                            const auto index = object_index(item);
                            QMenu menu(this);
                            if (index) {
                                objects->setCurrentItem(item);
                                set_selection(index, false);
                                if (properties_action_)
                                    menu.addAction(properties_action_);
                                menu.addAction(focus);
                                auto *report = menu.addAction(QStringLiteral("报告…"));
                                report->setObjectName("openObjectReport");
                                report->setEnabled(!busy());
                                const auto report_id =
                                    selected_sensor() ? selected_sensor()->object_id
                                    : selected_station_
                                        ? stations_.at(*selected_station_).config.object_id
                                        : satellites_.at(*selected_).orbit->input.object_id;
                                const auto report_kind =
                                    selected_sensor()   ? application::ReportKind::access_intervals
                                    : selected_station_ ? application::ReportKind::ground_stations
                                                        : application::ReportKind::cartesian_state;
                                connect(report, &QAction::triggered, this,
                                        [this, report_id, report_kind] {
                                            show_reports();
                                            if (report_dialog_)
                                                report_dialog_->select_object(report_id,
                                                                              report_kind);
                                        });
                                auto *access = menu.addAction(QStringLiteral("可见性分析…"));
                                access->setObjectName("openObjectAccess");
                                access->setEnabled(!busy() && !stations_.empty() &&
                                                   !satellites_.empty());
                                connect(access, &QAction::triggered, this, [this, report_id] {
                                    show_reports();
                                    report_dialog_->select_object(
                                        report_id, application::ReportKind::access_intervals);
                                });
                                if (selected_ && !selected_sensor()) {
                                    auto *sensor = menu.addAction(QStringLiteral("插入传感器…"));
                                    sensor->setObjectName("insertSensor");
                                    sensor->setEnabled(!busy());
                                    connect(sensor, &QAction::triggered, this,
                                            [this] { edit_sensor(true); });
                                }
                                menu.addSeparator();
                                menu.addAction(rename_action_);
                                menu.addAction(duplicate_action_);
                                menu.addAction(delete_action_);
                            } else {
                                if (item && item == scenario_item_) {
                                    auto *properties =
                                        menu.addAction(workbench_icon(WorkbenchIcon::properties),
                                                       QStringLiteral("属性"));
                                    properties->setEnabled(!busy());
                                    connect(properties, &QAction::triggered, this,
                                            [this] { open_scenario_properties(); });
                                    menu.addSeparator();
                                }
                                menu.addAction(new_action_);
                                menu.addAction(insert_action_);
                            }
                            menu.exec(objects->viewport()->mapToGlobal(point));
                        });
                slots_[1]->dock->hide();
                update_actions();
                set_selection(std::nullopt);
            }
            rebuild();
            connect(&timer_, &QTimer::timeout, this, [this] {
                try {
                    frame();
                } catch (const std::exception &error) {
                    fail(error.what());
                } catch (const vsg::Exception &error) {
                    fail(error.message);
                }
            });
            clock_.start();
            timer_.setTimerType(Qt::PreciseTimer);
            timer_.start(16);
            if (!smoke_dir_.isEmpty()) {
                QDir().mkpath(smoke_dir_);
                QTimer::singleShot(45000, this, [this] {
                    if (!finished_)
                        fail("Rendering smoke test timed out");
                });
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }
    ~Workbench() override { shutdown(); }
    bool initialize_data() {
        if (benchmarking_ || !smoke_dir_.isEmpty() || profile_)
            return true;
        if (!prepare_initial_data(this, *eop_catalog_))
            return false;
        if (editor_)
            editor_->set_time_provider(eop_catalog_->time_provider());
        eop_catalog_->set_update_schedule_enabled(true);
        return true;
    }
    void closeEvent(QCloseEvent *event) override {
        if (!benchmarking_ && smoke_dir_.isEmpty() && !profile_) {
            if (busy()) {
                if (project_candidate_.valid() && project_cancellation_)
                    project_cancellation_->store(true);
                event->ignore();
                statusBar()->showMessage(QStringLiteral("请等待当前操作完成，或先取消轨道计算。"),
                                         5000);
                return;
            }
            if (!confirm_replace_scenario()) {
                event->ignore();
                return;
            }
        }
        if (properties_dialog_)
            properties_dialog_->close();
        if (scenario_properties_)
            scenario_properties_->close();
        QMainWindow::closeEvent(event);
    }
    void shutdown() {
        if (stopped_)
            return;
        stopped_ = true;
        timer_.stop();
        if (cancellation_)
            cancellation_->store(true);
        if (display_cancellation_)
            display_cancellation_->store(true);
        if (project_cancellation_)
            project_cancellation_->store(true);
        if (project_candidate_.valid())
            project_candidate_.wait();
        if (candidate_.valid())
            candidate_.wait();
        if (display_candidate_.valid())
            display_candidate_.wait();
        if (model_candidate_.valid())
            model_candidate_.wait();
        if (profile_orbits_.valid())
            profile_orbits_.wait();
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        if (run_)
            run_->stop_inputs();
        if (timing_presentation_)
            timing_metadata_ = timing_presentation_->metadata();
        timing_presentation_.reset();
#endif
        // Wake compiler waiters (including the all-views-closed case) before joining jobs.
        if (viewer_)
            viewer_->status->set(false);
        if (globe_)
            globe_->stop_background_work();
        if (viewer_) {
            viewer_->deviceWaitIdle();
            smoke_render_tasks_.clear();
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            for (const auto &slot : slots_)
                if (slot->gpu)
                    try {
                        slot->gpu->drain_after_device_idle();
                    } catch (const std::exception &error) {
                        finished_ = false;
                        if (failure_.isEmpty())
                            failure_ = QString::fromUtf8(error.what());
                    }
#endif
            viewer_->recordAndSubmitTasks.clear();
            viewer_->presentations.clear();
            viewer_->compileManager = {};
            compiler_ = {};
            viewer_->getEventHandlers().clear();
        }
        for (auto &slot : slots_) {
            if (slot->view && globe_)
                globe_->release_view(*slot->view);
            slot->view = {};
            slot->graph = {};
            slot->render_graph = {};
            slot->capture = {};
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            slot->gpu.reset();
#endif
            slot->trackball = {};
            slot->labels.reset();
            slot->station_markers.reset();
            slot->station_selection.reset();
            if (slot->window) {
                slot->window->attached = {};
                slot->window->detaching = {};
                slot->window->failed = {};
                slot->window->synchronize = {};
                slot->window->pressed = {};
            }
            delete slot->container;
            slot->container = nullptr;
            slot->window = nullptr;
        }
        if (scene_)
            scene_->children.clear();
        scene_ = {};
        orbit_group_ = {};
        smoke_old_track_ = {};
        model_group_ = {};
        model_node_ = {};
        for (auto &satellite : satellites_) {
            satellite.model_host->children.clear();
            satellite.model = {};
        }
        markers_.reset();
        selection_.reset();
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        // Font sharing caches also retain GPU objects. Release them before the
        // validation report, while preserving the CPU-only benchmark metadata.
        if (benchmark_)
            benchmark_->font = {};
#endif
        globe_.reset();
        viewer_ = {};
        shared_device_ref_ = {};
    }
    int report(int code) {
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        if (run_) {
            run_->finish(
                QJsonObject{{"benchmark", benchmark_->report},
                            {"presentation_timing", timing_metadata_},
                            {"gpu_timing",
                             QJsonObject {
                                 { "views",
                                   gpu_metadata_ }
                             }},
                            {"present_timing_capabilities", timing_capabilities_},
                            {"validation_enabled", validation_},
                            {"validation_errors", static_cast<qint64>(log_.errors.load())},
                            {"validation_warnings", static_cast<qint64>(log_.warnings.load())},
                            {"failure", failure_},
                            { "completed",
                              finished_ }});
            if (!finished_ || !failure_.isEmpty() || log_.errors.load())
                return 2;
        }
#endif
        if (!smoke_dir_.isEmpty()) {
            QJsonObject report{{"render_iterations", static_cast<qint64>(frame_count_)},
                               {"qt_version", QString::fromUtf8(qVersion())},
                               {"vsg_version", QString::fromUtf8(vsgGetVersionString())},
                               {"rocky_version", ORBITFORGE_ROCKY_VERSION},
                               {"vsgqt_version", ORBITFORGE_VSGQT_VERSION},
                               {"shared_device", shared_device_},
                               {"shared_scene", shared_scene_},
                               {"independent_cameras", independent_cameras_},
                               {"all_windows_closed_before_shutdown", all_windows_closed_},
                               {"vulkan_validation_enabled", validation_},
                               {"validation_errors", static_cast<qint64>(log_.errors.load())},
                               {"validation_warnings", static_cast<qint64>(log_.warnings.load())},
                               {"views", view_report_},
                               {"failure", failure_},
                               {"scope", "integration-smoke; not the 10k performance acceptance"}};
            QJsonArray messages;
            {
                std::lock_guard lock(log_.mutex);
                for (const auto &message : log_.messages)
                    messages.append(message);
            }
            report.insert("validation_messages", messages);
            if (eop_catalog_)
                report.insert("data_catalog_warnings",
                              QJsonArray::fromStringList(eop_catalog_->warnings()));
            if (!benchmarking_) {
                report.insert("ui_font_family", QFontInfo(font()).family());
                report.insert(
                    "ground_map",
                    QJsonObject{
                        {"projection",
                         "equirectangular; WGS84 ellipsoid; Rocky conversion; display only"},
                        {"tracks", static_cast<qint64>(ground_map_->track_count())},
                        {"epoch_tai_ns", QString::number(ground_map_->epoch().tai_nanoseconds())}});
            }
            if (scenario_) {
                QJsonArray objects;
                for (const auto &satellite : satellites_)
                    objects.append(QJsonObject{
                        {"id", QString::fromStdString(satellite.orbit->input.object_id)},
                        {"revision",
                         static_cast<qint64>(satellite.orbit->session.provenance().input_revision)},
                        {"model_kind", satellite.model_kind}});
                report.insert("scenario",
                              QJsonObject{{"name", QString::fromStdString(scenario_->name())},
                                          {"revision", static_cast<qint64>(scenario_->revision())},
                                          {"objects", objects}});
            }
            if (orbit_) {
                const auto &provenance = orbit_->track.samples().provenance();
                report.insert(
                    "orbit",
                    QJsonObject{
                        {"object_id", QString::fromStdString(provenance.object_id)},
                        {"input_revision", static_cast<qint64>(provenance.input_revision)},
                        {"algorithm", QString::fromStdString(provenance.algorithm)},
                        {"cache_nodes",
                         static_cast<qint64>(orbit_->track.samples().states().size())},
                        {"first_tai_ns", QString::number(orbit_->input.first.tai_nanoseconds())},
                        {"last_tai_ns", QString::number(orbit_->input.last.tai_nanoseconds())},
                        {"step_seconds", orbit_->input.step_seconds},
                        {"max_quarter_segment_check_error_m", orbit_->max_check_error_m},
                        {"max_inertial_check_error_m", orbit_->max_inertial_check_error_m},
                        {"preparation_seconds", orbit_->preparation_seconds},
                        {"display_frame", display_frame_ == core::FrameId::gcrf ? "GCRF" : "ITRF"},
                        {"min_radius_m", orbit_->min_radius_m},
                        {"max_radius_m", orbit_->max_radius_m},
                        {"eop_source_id", QString::fromStdString(orbit_->eop_source_id)},
                        {"display_model",
                         "GCRF and ITRF cubic Hermite; no extrapolation; display only"}});
                report.insert("interactions", interaction_report_);
                report.insert("imagery", "nasa-blue-marble-land-ocean-ice-2002-2048");
                report.insert("appearance", QJsonObject{{"kind", model_kind_},
                                                        {"diameter_km", model_diameter_km_}});
            }
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            if (benchmark_) {
                report.insert("benchmark", benchmark_->report);
                report.insert("interactions", interaction_report_);
                report.insert("scope",
                              "10k workload integration preflight; performance unmeasured");
            }
#endif
            QFile file(smoke_dir_ + "/report.json");
            if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(report).toJson()) < 0)
                return 3;
            if (!finished_ || !shared_device_ || !shared_scene_ || !independent_cameras_ ||
                !all_windows_closed_ || log_.errors.load() != 0 || !failure_.isEmpty())
                return 2;
        }
        return code;
    }

  private:
    bool event(QEvent *event) override {
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        if (run_ && event->type() == BenchmarkInputEvent::event_type()) {
            try {
                benchmark_input(*static_cast<BenchmarkInputEvent *>(event));
            } catch (const std::exception &error) {
                fail(error.what());
            }
            return true;
        }
#endif
        return QMainWindow::event(event);
    }
    void resize_to_screen(int width, int height) {
        const auto available = screen()->availableGeometry();
        resize(std::min(width, available.width() - 40), std::min(height, available.height() - 80));
        move(available.topLeft() + QPoint(20, 40));
    }
    struct Slot {
        ~Slot() {
            // Covers exceptions before this slot joins slots_. The container
            // owns its QWindow; normal shutdown clears both pointers first.
            if (container)
                delete container;
            else
                delete window;
        }
        EmbeddedWindow *window = nullptr;
        QWidget *container = nullptr;
        QDockWidget *dock = nullptr;
        vsg::ref_ptr<vsg::Camera> camera;
        vsg::ref_ptr<vsg::View> view;
        vsg::ref_ptr<vsg::CommandGraph> graph;
        vsg::ref_ptr<vsg::RenderGraph> render_graph;
        vsg::ref_ptr<vsg::Trackball> trackball;
        vsg::ref_ptr<render_vsg::SwapchainCapture> capture;
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        std::shared_ptr<BenchmarkGpu> gpu;
#endif
        std::unique_ptr<render_vsg::LabelLayer> labels;
        std::unique_ptr<render_vsg::MarkerLayer> station_markers, station_selection;
        std::vector<render_vsg::ProjectedMarker> pick_candidates;
        QImage captured_image;
        std::uint64_t creations = 0, destructions = 0, presentations = 0;
    };
    visualization::RenderSnapshot sample_frame(core::Epoch epoch) const {
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        if (benchmark_)
            return benchmark_->cache->sample(epoch);
#endif
        visualization::RenderSnapshot result{epoch, display_frame_, core::OriginId::earth, {}};
        std::optional<core::PositionRotation> rotation;
        for (const auto &satellite : satellites_) {
            if (cache_covers(*satellite.orbit, epoch, epoch))
                result.markers.push_back((display_frame_ == core::FrameId::gcrf
                                              ? satellite.orbit->inertial_track
                                              : satellite.orbit->track)
                                             .sample(epoch)
                                             .markers.front());
            else {
                const auto native =
                    satellite.orbit->session.sample(std::array{epoch}).states().front();
                const auto state =
                    core::transform_state(native, display_frame_, *frames_, eop_->at(epoch));
                result.markers.push_back({satellite.orbit->input.object_id, state.position_m});
            }
        }
        for (const auto &station : stations_) {
            auto location = station.config.location;
            // A display-only surface clearance prevents z-fighting and keeps negative-height
            // stations visible on the reference globe. Numerical sessions retain exact height.
            location.height_m = std::max(location.height_m, 100.0);
            auto position = core::ground_station_position(location);
            if (display_frame_ == core::FrameId::gcrf) {
                if (!rotation)
                    rotation = frames_->gcrf_to_itrf(epoch, eop_->at(epoch).sample);
                position = rotation->inverse_position(position);
            }
            result.markers.push_back({station.config.object_id, position});
        }
        return result;
    }
    double duration_seconds() const {
        return benchmarking_ ? 600
               : scenario_   ? scenario_->last().seconds_since(scenario_->first())
                             : 1;
    }
    bool busy() const {
        return candidate_.valid() || model_candidate_.valid() || project_candidate_.valid();
    }
    void update_actions() {
        if (benchmarking_)
            return;
        const bool active = scenario_.has_value();
        const bool idle = !busy();
        if (station_editor_)
            station_editor_->set_busy(!idle);
        if (sensor_editor_)
            sensor_editor_->setEnabled(idle);
        for (auto *action : {rename_action_, duplicate_action_, delete_action_})
            if (action)
                action->setEnabled(selected_marker_index().has_value() && idle);
        if (eop_action_)
            eop_action_->setEnabled(idle);
        if (report_action_)
            report_action_->setEnabled(active && idle);
        if (cancel_project_button_)
            cancel_project_button_->setVisible(project_candidate_.valid());
        if (scenario_properties_) {
            for (const auto *name :
                 {"scenarioSettingsFields", "applyScenarioSettings", "resetScenarioSettings"})
                if (auto *control = scenario_properties_->findChild<QWidget *>(name))
                    control->setEnabled(idle);
        }
        if (open_action_)
            open_action_->setEnabled(idle);
        if (save_action_)
            save_action_->setEnabled(active && idle);
        if (save_as_action_)
            save_as_action_->setEnabled(active && idle);
        if (new_action_)
            new_action_->setEnabled(idle);
        if (example_action_)
            example_action_->setEnabled(idle && runtime::two_body_available());
        if (insert_action_)
            insert_action_->setEnabled(active && idle && editor_);
        if (insert_menu_)
            insert_menu_->setEnabled(active);
        if (time_toolbar_) {
            time_toolbar_->setEnabled(active);
            time_toolbar_->setVisible(active);
        }
        if (object_dock_) {
            if (!active)
                object_dock_->hide();
            object_dock_->toggleViewAction()->setEnabled(active);
        }
        if (time_menu_)
            time_menu_->setEnabled(active);
        if (fit_action_)
            fit_action_->setEnabled(active);
        if (aux_action_)
            aux_action_->setEnabled(active);
        if (map_action_)
            map_action_->setEnabled(active);
        if (properties_action_)
            properties_action_->setEnabled(
                idle && (selected_marker_index().has_value() ||
                         (scenario_ && objects_->currentItem() == scenario_item_)));
        if (focus_action_)
            focus_action_->setEnabled(selected_marker_index().has_value());
        for (auto *button : view_focus_buttons_)
            button->setEnabled(selected_marker_index().has_value());
    }
    std::optional<std::size_t> object_index(const QTreeWidgetItem *item) const {
        if (item && !item->data(0, Qt::UserRole + 17).toString().isEmpty())
            return object_index(item->parent());
        for (std::size_t i = 0; i < satellites_.size(); ++i)
            if (satellites_[i].item == item)
                return i;
        for (std::size_t i = 0; i < stations_.size(); ++i)
            if (stations_[i].item == item)
                return satellites_.size() + i;
        return std::nullopt;
    }
    void filter_objects() {
        const auto *search = findChild<QLineEdit *>("objectSearch");
        const auto text = search ? search->text() : QString{};
        for (auto &satellite : satellites_) {
            const bool parent_match = satellite.name().contains(text, Qt::CaseInsensitive);
            bool child_match = false;
            for (int i = 0; i < satellite.item->childCount(); ++i) {
                auto *child = satellite.item->child(i);
                const bool match = child->text(0).contains(text, Qt::CaseInsensitive);
                child->setHidden(!parent_match && !match);
                child_match = child_match || match;
            }
            satellite.item->setHidden(!parent_match && !child_match);
            if (child_match && !text.isEmpty()) {
                scenario_item_->setExpanded(true);
                satellite.item->setExpanded(true);
            }
        }
        for (auto &station : stations_)
            station.item->setHidden(!station.name().contains(text, Qt::CaseInsensitive));
    }
    const application::ProjectSensor *selected_sensor() const {
        if (!objects_ || !objects_->currentItem())
            return nullptr;
        const auto id =
            objects_->currentItem()->data(0, Qt::UserRole + 17).toString().toStdString();
        const auto found = std::ranges::find(sensors_, id, &application::ProjectSensor::object_id);
        return found == sensors_.end() ? nullptr : &*found;
    }
    void edit_sensor(bool creating) {
        if (busy() || !selected_)
            return;
        if (!sensor_editor_) {
            sensor_editor_ = new SensorEditor(this);
            sensor_editor_->apply = [this](application::ProjectSensor sensor) {
                if (busy())
                    throw std::runtime_error("场景正在更新，请稍后重试。");
                auto p = project_snapshot(project_path_);
                const auto id = sensor.object_id;
                auto found = std::ranges::find(p.sensors, sensor.object_id,
                                               &application::ProjectSensor::object_id);
                if (found == p.sensors.end())
                    p.sensors.push_back(std::move(sensor));
                else
                    *found = std::move(sensor);
                application::validate_project(p);
                if (start_project(std::move(p), project_path_, true, selected_marker_index()))
                    pending_project_sensor_selection_ = id;
            };
        }
        application::ProjectSensor sensor;
        if (creating) {
            sensor.object_id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            sensor.display_name = "Sensor " + std::to_string(sensors_.size() + 1);
            sensor.parent_satellite_id = satellites_[*selected_].orbit->input.object_id;
        } else {
            if (!selected_sensor())
                return;
            sensor = *selected_sensor();
        }
        sensor_editor_->set_sensor(std::move(sensor), project_snapshot(project_path_), creating);
        workspace_->open(sensor_editor_, false);
    }
    void clear_access_display() {
        access_session_.reset();
        access_ids_.clear();
        access_hash_.clear();
        if (timeline_)
            timeline_->set_access_intervals({}, {});
        if (ground_map_)
            ground_map_->set_access_link({}, false);
    }
    void update_access_display() {
        if (!access_session_)
            return;
        const auto station = std::ranges::find_if(
            stations_, [&](const auto &s) { return s.config.object_id == access_ids_[0]; });
        auto parent = access_ids_[1];
        const auto sensor =
            std::ranges::find(sensors_, parent, &application::ProjectSensor::object_id);
        if (sensor != sensors_.end())
            parent = sensor->parent_satellite_id;
        const auto satellite = std::ranges::find_if(
            satellites_, [&](const auto &s) { return s.orbit->input.object_id == parent; });
        if (station == stations_.end() || satellite == satellites_.end()) {
            clear_access_display();
            return;
        }
        try {
            const bool visible = access_session_->at(snapshot_.epoch).visible;
            ground_map_->set_access_link(
                std::pair{satellites_.size() +
                              static_cast<std::size_t>(station - stations_.begin()),
                          static_cast<std::size_t>(satellite - satellites_.begin())},
                visible);
        } catch (const std::exception &error) {
            clear_access_display();
            statusBar()->showMessage(
                QStringLiteral("可见性联动已停止：%1").arg(QString::fromUtf8(error.what())), 6000);
        }
    }
    std::optional<std::size_t> selected_marker_index() const {
        return selected_station_
                   ? std::optional<std::size_t>{satellites_.size() + *selected_station_}
                   : selected_;
    }
    QString selected_name() const {
        if (const auto *sensor = selected_sensor())
            return QString::fromStdString(sensor->name());
        return selected_           ? satellites_.at(*selected_).name()
               : selected_station_ ? stations_.at(*selected_station_).name()
                                   : QString{};
    }
    bool object_id_exists(const std::string &id) const {
        return std::ranges::any_of(sensors_, [&](const auto &s) { return s.object_id == id; }) ||
               std::any_of(satellites_.begin(), satellites_.end(),
                           [&](const auto &s) { return s.orbit->input.object_id == id; }) ||
               std::any_of(stations_.begin(), stations_.end(),
                           [&](const auto &s) { return s.config.object_id == id; });
    }
    void mark_project_dirty(bool dirty = true) {
        project_dirty_ = dirty;
        if (access_session_ && scenario_ &&
            runtime::report_input_sha256(project_snapshot(project_path_)) != access_hash_)
            clear_access_display();
        if (report_dialog_ && scenario_)
            report_dialog_->update_project(project_snapshot(project_path_), active_time());
        if (scenario_) {
            const auto filename = project_path_.isEmpty() ? QStringLiteral("未保存")
                                                          : QFileInfo(project_path_).fileName();
            setWindowTitle(QString::fromStdString(scenario_->name()) + QStringLiteral(" · ") +
                           filename + QStringLiteral("[*] · OrbitForge"));
        }
        setWindowModified(dirty);
    }
    application::Project project_snapshot(const QString &destination) const {
        if (!scenario_)
            throw application::ProjectError("No active project");
        application::Project project{scenario_->name(), scenario_->first(), scenario_->last()};
        project.current = snapshot_.epoch;
        project.earth_orientation = eop_->data_info();
        project.time_data = standalone_time_data_;
        project.analysis_record = analysis_record_;
        for (const auto &ref : scenario_->data_lock().references())
            if (ref.role.starts_with("gravity/") || ref.role.starts_with("ephemeris/"))
                project.force_data.push_back(ref);
        if (project.data_lock() != scenario_->data_lock())
            throw application::ProjectError(
                "Scenario data lock does not match its active providers");
        const QDir directory(QFileInfo(destination).absolutePath());
        for (const auto &satellite : satellites_) {
            application::ProjectSatellite object;
            object.orbit = satellite.orbit->input;
            object.display_name = satellite.display_name;
            object.display = satellite.display;
            object.model_kind = satellite.model_kind;
            object.model_diameter_m = satellite.model_diameter_km * 1000;
            if (object.model_kind == 2)
                object.model_path =
                    directory.relativeFilePath(QFileInfo(satellite.model_path).absoluteFilePath())
                        .toStdString();
            project.satellites.push_back(std::move(object));
        }
        for (const auto &station : stations_)
            project.ground_stations.push_back(station.config);
        project.reports = reports_;
        project.sensors = sensors_;
        return project;
    }
    std::vector<runtime::CatalogGravity>
    selected_gravity(const application::Project &project) const {
        std::vector<runtime::CatalogGravity> result;
        const auto installed = eop_catalog_->data().gravity_models();
        for (const auto &satellite : project.satellites) {
            const auto &input = satellite.orbit;
            if (!input.uses_hpop() || input.hpop.gravity_model.empty())
                continue;
            if (std::any_of(result.begin(), result.end(),
                            [&](const auto &v) { return v.selection == input.hpop.gravity_model; }))
                continue;
            const auto found = std::find_if(installed.begin(), installed.end(), [&](const auto &v) {
                return v.selection == input.hpop.gravity_model;
            });
            if (found == installed.end())
                throw std::runtime_error("缺少重力场文件，请在数据中心下载或导入：" +
                                         input.hpop.gravity_model);
            result.push_back(*found);
        }
        return result;
    }
    std::vector<runtime::CatalogEphemeris>
    selected_ephemerides(const application::Project &project) const {
        std::vector<runtime::CatalogEphemeris> result;
        const auto installed = eop_catalog_->data().ephemerides();
        for (const auto &satellite : project.satellites) {
            const auto &input = satellite.orbit;
            if (!input.uses_hpop() || !input.hpop.uses_ephemeris())
                continue;
            const auto &id = input.hpop.ephemeris_model;
            if (std::any_of(result.begin(), result.end(),
                            [&](const auto &v) { return v.selection == id; }))
                continue;
            const auto found = std::find_if(installed.begin(), installed.end(),
                                            [&](const auto &v) { return v.selection == id; });
            if (found == installed.end())
                throw std::runtime_error("缺少日月星历，请在数据中心 → 太阳系星历下载：" + id);
            result.push_back(*found);
        }
        return result;
    }
    std::vector<runtime::CatalogSpaceWeather>
    selected_weather(const application::Project &project) const {
        std::vector<runtime::CatalogSpaceWeather> result;
        const auto installed = eop_catalog_->data().weather_sources();
        for (const auto &satellite : project.satellites) {
            const auto &input = satellite.orbit;
            const auto &id = input.hpop.drag.weather_model;
            if (!input.uses_hpop() || !input.hpop.drag.enabled || id.empty())
                continue;
            if (std::any_of(result.begin(), result.end(),
                            [&](const auto &v) { return v.selection == id; }))
                continue;
            const auto found = std::find_if(installed.begin(), installed.end(),
                                            [&](const auto &v) { return v.selection == id; });
            if (found == installed.end())
                throw std::runtime_error("缺少空间天气数据，请在数据中心 → 空间天气下载或导入：" +
                                         id);
            result.push_back(*found);
        }
        return result;
    }
    std::vector<runtime::CatalogOceanTides>
    selected_ocean_tides(const application::Project &project) const {
        std::vector<runtime::CatalogOceanTides> result;
        const auto installed = eop_catalog_->data().ocean_tide_models();
        for (const auto &satellite : project.satellites) {
            const auto &input = satellite.orbit;
            const auto &id = input.hpop.ocean_tides.model;
            if (!input.uses_hpop() || !input.hpop.ocean_tides.active())
                continue;
            if (std::any_of(result.begin(), result.end(),
                            [&](const auto &v) { return v.selection == id; }))
                continue;
            const auto found = std::find_if(installed.begin(), installed.end(),
                                            [&](const auto &v) { return v.selection == id; });
            if (found == installed.end())
                throw std::runtime_error("缺少海潮模型，请在数据中心 → 海潮模型下载或导入：" + id);
            result.push_back(*found);
        }
        return result;
    }
    application::DataLock selected_data_lock(const application::Project &project,
                                             const core::EopDataInfo &eop) const {
        const auto base = application::eop_data_lock(eop);
        std::vector<application::DataReference> refs(base.references().begin(),
                                                     base.references().end());
        for (const auto &model : selected_gravity(project))
            refs.push_back(runtime::gravity_reference(model.selection, model.info));
        for (const auto &model : selected_ephemerides(project))
            refs.push_back(runtime::ephemeris_reference(model.selection, model.info));
        for (const auto &model : selected_weather(project))
            refs.push_back(runtime::space_weather_reference(model.selection, model.info));
        for (const auto &model : selected_ocean_tides(project))
            refs.push_back(runtime::ocean_tide_reference(model.selection, model.info));
        return application::DataLock{std::move(refs)};
    }
    ReportInput report_input() const {
        auto resources = runtime::DataSnapshot::from_providers(active_time(), frames_, eop_);
        for (const auto &[id, model] : gravity_)
            resources = resources.with_gravity(id, model);
        for (const auto &[id, model] : ephemerides_)
            resources = resources.with_ephemeris(id, model);
        for (const auto &[id, model] : weather_)
            resources = resources.with_space_weather(id, model);
        for (const auto &[id, model] : ocean_tides_)
            resources = resources.with_ocean_tides(id, model);
        return {project_snapshot(project_path_), std::move(resources)};
    }
    void show_reports() {
        if (!scenario_ || busy())
            return;
        if (!report_dialog_) {
            report_dialog_ = new ReportDialog(
                project_snapshot(project_path_), active_time(),
                [this](ReportDialog::Ready done) {
                    try {
                        if (busy()) {
                            done({}, QStringLiteral("场景正在更新，请完成后再生成报告。"));
                            return;
                        }
                        const auto [data_first, data_last] =
                            application::calculation_data_interval(project_snapshot(project_path_));
                        const auto current = data_for_interval(data_first, data_last);
                        if (scenario_->data_lock() !=
                            selected_data_lock(project_snapshot(project_path_),
                                               current->data_info())) {
                            if (start_project(project_snapshot(project_path_), project_path_, true,
                                              selected_marker_index()))
                                pending_report_input_ = std::move(done);
                            else
                                done({}, QStringLiteral("未生成报告；场景数据未更新。"));
                        } else
                            done(report_input(), {});
                    } catch (const std::exception &error) {
                        done({}, QString::fromUtf8(error.what()));
                    }
                },
                [this](std::vector<application::ReportDefinition> reports) {
                    if (busy())
                        throw std::runtime_error("场景正在更新，请完成后再保存报告配置。");
                    auto project = project_snapshot(project_path_);
                    project.reports = reports;
                    application::validate_project(project);
                    reports_ = std::move(reports);
                    mark_project_dirty();
                },
                [this](core::Epoch epoch) {
                    set_playing(false);
                    seek_epoch(epoch);
                },
                this);
            report_dialog_->result_published =
                [this](std::shared_ptr<const runtime::ReportResult> result,
                       const runtime::Analysis &analysis) {
                    clear_access_display();
                    if (!application::report_is_access(result->definition.query.kind))
                        return;
                    const auto &ids = result->definition.query.object_ids;
                    // The map/timeline session uses the same confirmed resources as the report.
                    const auto &data = analysis.data();
                    access_session_.emplace(
                        runtime::ReportContext{*analysis.snapshot().project, data.time(),
                                               data.frames(), data.eop(), data.gravity_models(),
                                               data.ephemerides(), data.weather_sources(),
                                               data.ocean_tide_models()},
                        ids[0], ids[1]);
                    access_hash_ = runtime::report_input_sha256(*analysis.snapshot().project);
                    access_ids_ = ids;
                    std::vector<std::pair<qint64, qint64>> windows;
                    if (result->definition.query.kind ==
                        application::ReportKind::access_intervals) {
                        const auto &source =
                            result->source_table ? *result->source_table : result->table;
                        for (const auto &row : source.rows())
                            windows.emplace_back(
                                std::get<core::Epoch>(row[2]).tai_nanoseconds() / 1'000'000,
                                std::get<core::Epoch>(row[3]).tai_nanoseconds() / 1'000'000);
                        timeline_->set_access_intervals(
                            std::move(windows), QString::fromStdString(access_session_->name()));
                    }
                    update_access_display();
                };
        }
        workspace_->open(report_dialog_, false);
    }
    void close_reports() {
        clear_access_display();
        if (report_dialog_) {
            workspace_->close_page(report_dialog_);
            delete report_dialog_;
            report_dialog_ = nullptr;
        }
        reports_.clear();
    }
    void save_project_to(const QString &path) {
        const auto absolute = QFileInfo(path).absoluteFilePath();
        application::save_project(std::filesystem::path(absolute.toStdWString()),
                                  project_snapshot(absolute), true);
        project_path_ = absolute;
        mark_project_dirty(false);
        statusBar()->showMessage(QStringLiteral("已保存：%1").arg(project_path_), 6000);
    }
    bool save_project_dialog(bool save_as) {
        if (!scenario_ || busy())
            return false;
        QString path = project_path_;
        if (save_as || path.isEmpty()) {
            QFileDialog dialog(this, QStringLiteral("保存项目"));
            dialog.setAcceptMode(QFileDialog::AcceptSave);
            dialog.setFileMode(QFileDialog::AnyFile);
            dialog.setNameFilter(QStringLiteral("OrbitForge 项目 (*.orbitforge)"));
            dialog.setDefaultSuffix(QStringLiteral("orbitforge"));
            dialog.selectFile(path.isEmpty() ? QStringLiteral("项目.orbitforge") : path);
            if (dialog.exec() != QDialog::Accepted)
                return false;
            path = dialog.selectedFiles().value(0);
        }
        try {
            save_project_to(path);
            return true;
        } catch (const std::exception &error) {
            QMessageBox::warning(this, QStringLiteral("保存失败"), QString::fromUtf8(error.what()));
            return false;
        }
    }
    bool confirm_replace_scenario() {
        if (!scenario_ || !project_dirty_)
            return true;
        const auto answer = QMessageBox::question(
            this, QStringLiteral("保存当前项目？"),
            QStringLiteral("当前项目有已应用但尚未保存的更改。是否先保存？\n属性页中尚未应用的输入"
                           "不会写入项目。"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Save)
            return save_project_dialog(false);
        return answer == QMessageBox::Discard;
    }
    void choose_project() {
        if (busy())
            return;
        const auto path =
            QFileDialog::getOpenFileName(this, QStringLiteral("打开项目"), project_path_,
                                         QStringLiteral("OrbitForge 项目 (*.orbitforge)"));
        if (path.isEmpty())
            return;
        try {
            // Parse before prompting to replace; an invalid file cannot disturb the current
            // project.
            auto project = application::load_project(std::filesystem::path(path.toStdWString()));
            if (confirm_replace_scenario())
                start_project(std::move(project), path);
        } catch (const std::exception &error) {
            QMessageBox::warning(this, QStringLiteral("无法打开项目"),
                                 QString::fromUtf8(error.what()));
        }
    }
    void show_data_coverage(core::Epoch first, core::Epoch last) {
        const auto label = [this](core::Epoch epoch) {
            try {
                return desktop::calendar_text(epoch, CalendarScale::utc, true,
                                              eop_catalog_->time_provider().get()) +
                       QStringLiteral(" UTC");
            } catch (const std::out_of_range &) {
                return QString::number(epoch.tai_nanoseconds()) + QStringLiteral(" TAI ns");
            }
        };
        QString coverage;
        for (const auto index : eop_catalog_->current_indices()) {
            const auto &info = eop_catalog_->providers().at(index)->data_info();
            coverage += QStringLiteral("\n%1 — %2（允许外推：%3 天）")
                            .arg(label(info.first_epoch), label(info.last_epoch))
                            .arg(eop_catalog_->extrapolation_days(index));
        }
        QMessageBox message(
            QMessageBox::Warning, QStringLiteral("数据覆盖不足"),
            QStringLiteral("场景需要：%1 — "
                           "%2\n当前数据覆盖：%3\n\n请在数据中心更新数据或调整外推设置后重试。UTC "
                           "/ 闰秒有效期不可外推。")
                .arg(label(first), label(last), coverage),
            QMessageBox::Cancel, this);
        message.setObjectName("dataCoverageDialog");
        auto *manage = message.addButton(QStringLiteral("打开数据中心"), QMessageBox::ActionRole);
        message.exec();
        if (message.clickedButton() == manage)
            show_data_center(this, *eop_catalog_);
    }
    QString extrapolation_description(const core::EopDataInfo &info, core::Epoch first,
                                      core::Epoch last) const {
        const auto time = time_for(info.time_data);
        const auto calendar_text = [&](core::Epoch epoch, CalendarScale scale) {
            return desktop::calendar_text(epoch, scale, true, time.get());
        };
        QStringList ranges;
        if (first.tai_nanoseconds() < info.first_epoch.tai_nanoseconds())
            ranges.push_back(
                calendar_text(first, CalendarScale::utc) + QStringLiteral(" — ") +
                calendar_text(core::Epoch{std::min(last.tai_nanoseconds(),
                                                   info.first_epoch.tai_nanoseconds())},
                              CalendarScale::utc));
        if (last.tai_nanoseconds() > info.last_epoch.tai_nanoseconds())
            ranges.push_back(calendar_text(core::Epoch{std::max(first.tai_nanoseconds(),
                                                                info.last_epoch.tai_nanoseconds())},
                                           CalendarScale::utc) +
                             QStringLiteral(" — ") + calendar_text(last, CalendarScale::utc));
        return ranges.empty()
                   ? QStringLiteral("未使用外推")
                   : QStringLiteral("已使用 EOP 外推（端点线性估算，上限 %1 天）\n%2 UTC")
                         .arg(info.extrapolation_days)
                         .arg(ranges.join(QStringLiteral("\n")));
    }
    bool start_project(application::Project project, const QString &path, bool editing = false,
                       std::optional<std::size_t> selection = std::nullopt,
                       std::optional<std::string> edited_object = std::nullopt,
                       std::optional<ScenarioSettings> settings = std::nullopt) {
        if (busy())
            throw std::logic_error("Finish the pending update before opening a project");
        const auto first = settings ? settings->first : project.first;
        const auto last = settings ? settings->last : project.last;
        // Check edits before asking to replace calculation data. The original lock
        // remains available for consent even when its coverage cannot span the new interval.
        if (settings)
            application::validate_project(application::Project{settings->name, first, last});
        auto data_input = project;
        data_input.first = first;
        data_input.last = last;
        const auto [data_first, data_last] = application::calculation_data_interval(data_input);
        EopCatalog::Provider next_data;
        try {
            next_data = data_for_interval(data_first, data_last);
        } catch (const std::out_of_range &) {
            last_project_outcome_ = "rejected";
            show_data_coverage(data_first, data_last);
            return false;
        }
        const auto gravity_files = selected_gravity(project);
        const auto ephemeris_files = selected_ephemerides(project);
        const auto weather_files = selected_weather(project);
        const auto ocean_files = selected_ocean_tides(project);
        const auto selected_lock = selected_data_lock(project, next_data->data_info());
        if (!project.data_lock().references().empty() && project.data_lock() != selected_lock) {
            QMessageBox message(
                QMessageBox::Question, QStringLiteral("场景数据已变化"),
                QStringLiteral("当前数据或使用策略与此场景上次计算不一致。\n是否使用当前数据更新场"
                               "景并继续？\n%1\n\n取消将保留当前场景，不打开或重新计算。")
                    .arg(extrapolation_description(next_data->data_info(), data_first, data_last)),
                QMessageBox::Yes | QMessageBox::Cancel, this);
            message.setObjectName("dataChangeDialog");
            message.button(QMessageBox::Yes)->setText(QStringLiteral("使用当前数据"));
            message.setDefaultButton(QMessageBox::Cancel);
            message.setDetailedText(QStringLiteral("上次数据：%1\n当前数据：%2")
                                        .arg(QString::fromStdString(project.data_lock().sha256()),
                                             QString::fromStdString(selected_lock.sha256())));
            if (message.exec() != QMessageBox::Yes) {
                last_project_outcome_ = "cancelled";
                return false;
            }
        }
        const auto recorded_data = project.data_lock();
        project.force_data.clear();
        for (const auto &file : gravity_files)
            project.force_data.push_back(runtime::gravity_reference(file.selection, file.info));
        for (const auto &file : ephemeris_files)
            project.force_data.push_back(runtime::ephemeris_reference(file.selection, file.info));
        for (const auto &file : weather_files)
            project.force_data.push_back(
                runtime::space_weather_reference(file.selection, file.info));
        for (const auto &file : ocean_files)
            project.force_data.push_back(runtime::ocean_tide_reference(file.selection, file.info));
        if (settings) {
            project.earth_orientation = next_data->data_info();
            project = project.with_settings(settings->name, first, last);
        }
        if (display_cancellation_)
            display_cancellation_->store(true);
        project_edit_ = editing;
        project_settings_edit_ = settings.has_value();
        project_orbit_edit_ =
            edited_object && std::any_of(project.satellites.begin(), project.satellites.end(),
                                         [&](const auto &satellite) {
                                             return satellite.orbit.object_id == *edited_object;
                                         });
        pending_project_selection_ = selection;
        pending_project_sensor_selection_.reset();
        if (editing && selection == selected_marker_index())
            if (const auto *sensor = selected_sensor())
                pending_project_sensor_selection_ = sensor->object_id;
        pending_project_path_ = editing ? project_path_ : QFileInfo(path).absoluteFilePath();
        if (editing)
            set_playing(false);
        if (!cancel_project_button_) {
            cancel_project_button_ = new QPushButton(QStringLiteral("取消打开"), statusBar());
            statusBar()->addPermanentWidget(cancel_project_button_);
            connect(cancel_project_button_, &QPushButton::clicked, this, [this] {
                if (project_cancellation_)
                    project_cancellation_->store(true);
            });
        }
        cancel_project_button_->setText(editing ? QStringLiteral("取消更新")
                                                : QStringLiteral("取消打开"));
        project_cancellation_ = std::make_shared<std::atomic_bool>(false);
        last_project_outcome_.clear();
        project_recorded_data_ = recorded_data;
        // Capture exactly the data the user checked/approved, even if a download
        // completes while the confirmation dialog is open.
        std::vector<EopCatalog::Provider> installed_data{next_data};
        const auto retained = editing ? scenario_ : std::nullopt;
        const auto calculation_time = time_for(next_data->data_info().time_data);
        project_candidate_ = std::async(
            std::launch::async,
            [this, project = std::move(project),
             source = std::filesystem::path(pending_project_path_.toStdWString()), installed_data,
             retained, edited_object, calculation_time, gravity_files, ephemeris_files,
             weather_files, ocean_files, cancel = project_cancellation_]() mutable {
                std::map<std::string, std::shared_ptr<const core::GravityModel>> gravity;
                std::map<std::string, std::shared_ptr<const core::IThirdBodyEphemeris>> ephemerides;
                std::map<std::string, std::shared_ptr<const core::SpaceWeather>> weather;
                std::map<std::string, std::shared_ptr<const core::OceanTideModel>> ocean_tides;
                for (const auto &file : ocean_files) {
                    auto model =
                        runtime::load_ocean_tides(file.file, file.info.source_url, [cancel] {
                            if (*cancel)
                                throw OrbitCancelled{};
                        });
                    if (model->info().sha256 != file.info.sha256)
                        throw std::runtime_error("Ocean tide content changed after selection");
                    ocean_tides.emplace(file.selection, std::move(model));
                }
                for (const auto &file : weather_files) {
                    auto model =
                        runtime::load_space_weather(file.file, file.info.source_url, [cancel] {
                            if (*cancel)
                                throw OrbitCancelled{};
                        });
                    if (model->info().sha256 != file.info.sha256)
                        throw std::runtime_error("Space weather content changed after selection");
                    weather.emplace(file.selection, std::move(model));
                }
                for (const auto &file : ephemeris_files) {
                    if (*cancel)
                        throw OrbitCancelled{};
                    auto model = runtime::make_third_body_ephemeris(runtime::load_spice_snapshot(
                        file.manifest,
                        file.manifest.parent_path().parent_path().parent_path() / "runtime"));
                    if (model->data_info().content_sha256 != file.info.content_sha256)
                        throw std::runtime_error("Ephemeris file content changed after selection");
                    ephemerides.emplace(file.selection, std::move(model));
                }
                for (const auto &file : gravity_files) {
                    auto model = runtime::load_gravity(file.file, file.info.source_url, [cancel] {
                        if (*cancel)
                            throw OrbitCancelled{};
                    });
                    if (model->info().sha256 != file.info.sha256)
                        throw std::runtime_error("Gravity file content changed after selection");
                    gravity.emplace(file.selection, std::move(model));
                }
                return prepare_project_scene(std::move(project), source, frames_, installed_data,
                                             *cancel, retained, edited_object, calculation_time,
                                             gravity, ephemerides, weather, ocean_tides);
            });
        update_actions();
        statusBar()->showMessage(editing ? QStringLiteral("正在更新场景并准备轨迹…")
                                         : QStringLiteral("正在打开项目并准备轨迹…"));
        return true;
    }
    void poll_project() {
        if (!project_candidate_.valid() ||
            project_candidate_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        try {
            auto candidate = project_candidate_.get();
            if (project_cancellation_->load())
                throw OrbitCancelled{};
            const bool root_expanded = !project_edit_ || scenario_item_->isExpanded();
            std::unordered_map<std::string, bool> expanded;
            if (project_edit_)
                for (const auto &satellite : satellites_)
                    if (satellite.item->childCount() > 0)
                        expanded.emplace(satellite.orbit->input.object_id,
                                         satellite.item->isExpanded());
            std::vector<Satellite> satellites;
            std::vector<Station> stations;
            auto root = std::make_unique<QTreeWidgetItem>(
                QStringList{QString::fromStdString(candidate->project.name)});
            root->setIcon(0, workbench_icon(WorkbenchIcon::earth, theme::color(theme::accent)));
            auto tracks = vsg::Group::create(), models = vsg::Group::create();
            for (std::size_t i = 0; i < candidate->orbits.size(); ++i) {
                Satellite satellite;
                const auto &config = candidate->project.satellites[i];
                satellite.orbit = candidate->orbits[i];
                satellite.display_name = config.display_name;
                satellite.display = config.display;
                satellite.model_kind = config.model_kind;
                satellite.model_diameter_km = config.model_diameter_m / 1000;
                satellite.model_path = QString::fromStdString(config.model_path);
                satellite.model = candidate->models[i];
                if (satellite.model)
                    satellite.model_host->addChild(satellite.model);
                models->addChild(satellite.model_host);
                satellite.fixed_track =
                    std::make_shared<render_vsg::TrackLayer>(satellite.orbit->track.nodes());
                satellite.inertial_track = std::make_shared<render_vsg::TrackLayer>(
                    satellite.orbit->inertial_track.nodes());
                satellite.tracks = vsg::Switch::create();
                satellite.tracks->addChild(true, satellite.inertial_track->node());
                satellite.tracks->addChild(true, satellite.fixed_track->node());
                tracks->addChild(satellite.tracks);
                satellite.item = new QTreeWidgetItem(root.get(), {satellite.name()});
                satellite.item->setToolTip(0, QStringLiteral("ID：%1").arg(
                                                  QString::fromStdString(config.orbit.object_id)));
                satellite.item->setIcon(0,
                                        workbench_icon(WorkbenchIcon::satellite,
                                                       QColor::fromRgb(config.display.line_color)));
                satellites.push_back(std::move(satellite));
            }
            for (const auto &sensor : candidate->project.sensors) {
                auto parent = std::ranges::find_if(satellites, [&](const auto &sat) {
                    return sat.orbit->input.object_id == sensor.parent_satellite_id;
                });
                auto *item =
                    new QTreeWidgetItem(parent->item, {QString::fromStdString(sensor.name())});
                item->setData(0, Qt::UserRole + 17, QString::fromStdString(sensor.object_id));
                item->setIcon(0, workbench_icon(WorkbenchIcon::focus, theme::color(theme::sakura)));
                item->setToolTip(0,
                                 QStringLiteral("圆锥传感器 · %1°\nID：%2")
                                     .arg(sensor.half_angle_rad * 180 / std::numbers::pi, 0, 'g', 5)
                                     .arg(QString::fromStdString(sensor.object_id)));
            }
            for (const auto &config : candidate->project.ground_stations) {
                Station station{config};
                station.item = new QTreeWidgetItem(root.get(), {station.name()});
                station.item->setIcon(
                    0, workbench_icon(WorkbenchIcon::ground_station, theme::color(theme::success)));
                station.item->setToolTip(0, QStringLiteral("地面站 · WGS84\nID：%1")
                                                .arg(QString::fromStdString(config.object_id)));
                stations.push_back(std::move(station));
            }
            const auto previous_tracks = orbit_group_->children,
                       previous_models = model_group_->children;
            begin_change();
            try {
                orbit_group_->children = tracks->children;
                model_group_->children = models->children;
                rebuild();
            } catch (...) {
                orbit_group_->children = previous_tracks;
                model_group_->children = previous_models;
                rebuild();
                throw;
            }
            if (properties_dialog_)
                properties_dialog_->close();
            if (scenario_properties_)
                scenario_properties_->close();
            satellites_ = std::move(satellites);
            stations_ = std::move(stations);
            sensors_ = candidate->project.sensors;
            clear_access_display();
            if (sensor_editor_)
                sensor_editor_->close();
            if (station_editor_)
                station_editor_->close();
            scenario_ = std::move(candidate->scenario);
            eop_ = candidate->eop;
            gravity_ = candidate->gravity;
            ephemerides_ = candidate->ephemerides;
            weather_ = candidate->weather;
            ocean_tides_ = candidate->ocean_tides;
            if (!project_edit_)
                close_reports();
            reports_ = candidate->project.reports;
            standalone_time_data_ = candidate->project.time_data;
            analysis_record_ = candidate->project.analysis_record;
            time_label_epoch_.reset();
            sun_epoch_.reset();
            editor_target_.reset();
            pending_orbit_target_.reset();
            {
                const QSignalBlocker block(objects_);
                objects_->clear();
                scenario_item_ = root.release();
                objects_->addTopLevelItem(scenario_item_);
                // Expansion and visibility only take effect after items belong to the tree.
                scenario_item_->setExpanded(root_expanded);
                for (const auto &satellite : satellites_) {
                    const auto previous = expanded.find(satellite.orbit->input.object_id);
                    satellite.item->setExpanded(satellite.item->childCount() > 0 &&
                                                (previous == expanded.end() || previous->second));
                }
                filter_objects();
            }
            orbit_ = satellites_.empty() ? nullptr : satellites_.front().orbit;
            object_item_ = satellites_.empty() ? nullptr : satellites_.front().item;
            model_node_ = satellites_.empty() ? vsg::ref_ptr<vsg::MatrixTransform>{}
                                              : satellites_.front().model;
            model_kind_ = satellites_.empty() ? 0 : satellites_.front().model_kind;
            model_diameter_km_ = satellites_.empty() ? 1000 : satellites_.front().model_diameter_km;
            ground_map_->publish_tracks(std::move(candidate->ground_tracks));
            ground_map_->set_ground_stations(candidate->project.ground_stations);
            if (!project_edit_)
                ground_map_->reset_view();
            set_workspace_name(QString::fromStdString(scenario_->name()));
            set_selection(project_edit_ ? pending_project_selection_ : std::nullopt);
            if (pending_project_sensor_selection_)
                for (const auto &satellite : satellites_)
                    for (int i = 0; i < satellite.item->childCount(); ++i) {
                        auto *item = satellite.item->child(i);
                        if (item->data(0, Qt::UserRole + 17).toString().toStdString() !=
                            *pending_project_sensor_selection_)
                            continue;
                        objects_->setCurrentItem(item);
                        if (!satellite.item->isHidden() && !item->isHidden()) {
                            scenario_item_->setExpanded(true);
                            satellite.item->setExpanded(true);
                            objects_->scrollToItem(item);
                        }
                    }
            set_playing(false);
            seek_epoch(candidate->project.current);
            snapshot_ = sample_frame(candidate->project.current);
            satellite_state_dirty_ = true;
            update_satellite();
            selection_->update({snapshot_.epoch, snapshot_.frame, snapshot_.origin, {}});
            for (auto &slot : slots_)
                slot->pick_candidates.clear();
            workspace_pages_->setCurrentIndex(0);
            if (!project_edit_)
                workspace_->select_view(0);
            object_dock_->show();
            if (!project_edit_)
                fit_pending_ = true;
            show_timeline_status();
            if (timeline_ && !project_edit_)
                timeline_->reset_view();
            project_path_ = pending_project_path_;
            mark_project_dirty(project_edit_ ||
                               candidate->project.data_lock() != project_recorded_data_);
            last_project_outcome_ = "opened";
            statusBar()->showMessage(project_edit_
                                         ? QStringLiteral("已更新场景")
                                         : QStringLiteral("已打开：%1").arg(project_path_),
                                     6000);
        } catch (const OrbitCancelled &) {
            last_project_outcome_ = "cancelled";
            statusBar()->showMessage(QStringLiteral("已取消；继续显示原场景。"), 5000);
        } catch (const std::exception &error) {
            last_project_outcome_ = "rejected";
            const auto title =
                project_edit_ ? QStringLiteral("场景未更新") : QStringLiteral("项目未打开");
            statusBar()->showMessage(title + QStringLiteral("：") +
                                     QString::fromUtf8(error.what()));
            if (smoke_dir_.isEmpty())
                QMessageBox::warning(this, title, QString::fromUtf8(error.what()));
        } catch (const vsg::Exception &error) {
            last_project_outcome_ = "rejected";
            statusBar()->showMessage(
                QStringLiteral("项目图形准备失败：%1").arg(QString::fromStdString(error.message)));
        }
        if (project_orbit_edit_) {
            editor_->set_busy(false);
            last_orbit_outcome_ =
                last_project_outcome_ == "opened" ? "applied" : last_project_outcome_;
            if (last_project_outcome_ != "opened")
                editor_->show_result(QStringLiteral("未应用；原场景保留。"), true);
            if (insert_defaults_pending_ && last_orbit_outcome_ == "rejected")
                show_properties_dialog();
            insert_defaults_pending_ = false;
        }
        if (project_settings_edit_ && last_project_outcome_ != "opened" && scenario_properties_)
            if (auto *result =
                    scenario_properties_->findChild<QLabel *>("scenarioSettingsResult")) {
                result->setText(
                    last_project_outcome_ == "cancelled"
                        ? QStringLiteral("已取消更新，原场景保留。可继续修改后重新应用。")
                        : QStringLiteral("更新失败，原场景保留。请调整设置后重试。"));
                result->show();
            }
        if (station_editor_ && last_project_outcome_ != "opened")
            station_editor_->show_result(QStringLiteral("未应用，原场景保留。请调整后重试。"));
        if (pending_report_input_) {
            auto done = std::move(pending_report_input_);
            pending_report_input_ = {};
            if (last_project_outcome_ == "opened")
                done(report_input(), {});
            else
                done({}, QStringLiteral("未生成报告；场景更新未完成，上次结果保留。"));
        }
        update_actions();
    }
    void rename_selected_to(const QString &name) {
        if (!busy() && selected_sensor()) {
            auto p = project_snapshot(project_path_);
            auto &s = *std::ranges::find(p.sensors, selected_sensor()->object_id,
                                         &application::ProjectSensor::object_id);
            s.display_name = name.trimmed().toStdString();
            if (s.display_name.empty())
                throw std::invalid_argument("请输入传感器名称。");
            start_project(std::move(p), project_path_, true, selected_marker_index());
            return;
        }
        if (!busy() && selected_station_) {
            const auto &station = stations_.at(*selected_station_).config;
            start_project(
                project_snapshot(project_path_)
                    .renamed_ground_station(station.object_id, name.trimmed().toStdString()),
                project_path_, true, selected_marker_index());
            return;
        }
        if (busy() || !selected_)
            return;
        auto &satellite = satellites_.at(*selected_);
        const auto changed =
            project_snapshot(project_path_)
                .renamed_satellite(satellite.orbit->input.object_id, name.trimmed().toStdString());
        satellite.display_name = changed.satellites.at(*selected_).display_name;
        satellite.item->setText(0, satellite.name());
        filter_objects();
        ground_map_->set_name(*selected_, satellite.name());
        if (editor_target_ == selected_)
            properties_dialog_->setWindowTitle(satellite.name() + QStringLiteral(" · 对象属性"));
        show_timeline_status();
        mark_project_dirty();
    }
    void rename_selected() {
        if (busy() || !selected_marker_index())
            return;
        bool accepted = false;
        const auto name =
            QInputDialog::getText(this, QStringLiteral("重命名对象"), QStringLiteral("名称"),
                                  QLineEdit::Normal, selected_name(), &accepted);
        if (accepted) {
            try {
                rename_selected_to(name);
            } catch (const std::exception &error) {
                QMessageBox::warning(this, QStringLiteral("名称无效"),
                                     QString::fromUtf8(error.what()));
            }
        }
    }
    void duplicate_selected() {
        if (busy() || !selected_marker_index())
            return;
        try {
            if (selected_sensor()) {
                auto p = project_snapshot(project_path_);
                auto s = *selected_sensor();
                s.display_name = s.name() + " 副本";
                s.object_id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
                const auto id = s.object_id;
                p.sensors.push_back(std::move(s));
                if (start_project(std::move(p), project_path_, true, selected_marker_index()))
                    pending_project_sensor_selection_ = id;
                return;
            }
            const auto project = project_snapshot(project_path_);
            if (selected_station_) {
                const auto &source = project.ground_stations.at(*selected_station_);
                std::string id;
                unsigned suffix = 1;
                do {
                    id = source.object_id + "-copy-" + std::to_string(suffix++);
                } while (object_id_exists(id));
                start_project(
                    project.with_ground_station_copy(source.object_id, id, source.name() + " 副本"),
                    project_path_, true, satellites_.size() + stations_.size());
                return;
            }
            const auto &source = project.satellites.at(*selected_);
            std::string id;
            unsigned suffix = 1;
            do {
                id = source.orbit.object_id + "-copy-" + std::to_string(suffix++);
            } while (object_id_exists(id));
            start_project(
                project.with_satellite_copy(source.orbit.object_id, id, source.name() + " 副本"),
                project_path_, true, satellites_.size());
        } catch (const std::exception &error) {
            QMessageBox::warning(this, QStringLiteral("复制失败"), QString::fromUtf8(error.what()));
        }
    }
    void delete_selected() {
        if (busy() || !selected_marker_index())
            return;
        const auto index = *selected_marker_index();
        if (QMessageBox::question(
                this, QStringLiteral("删除对象"),
                QStringLiteral("删除“%1”？此操作尚不支持撤销。").arg(selected_name()),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        try {
            const auto current = project_snapshot(project_path_);
            if (selected_sensor()) {
                auto p = current;
                const auto id = selected_sensor()->object_id;
                std::erase_if(p.sensors, [&](const auto &s) { return s.object_id == id; });
                start_project(std::move(p), project_path_, true, selected_marker_index());
                return;
            }
            auto project =
                selected_station_
                    ? current.without_ground_station(
                          stations_.at(*selected_station_).config.object_id)
                    : current.without_satellite(satellites_.at(index).orbit->input.object_id);
            const auto count = project.satellites.size() + project.ground_stations.size();
            const auto selection = count == 0
                                       ? std::optional<std::size_t>{}
                                       : std::optional<std::size_t>{std::min(index, count - 1)};
            start_project(std::move(project), project_path_, true, selection);
        } catch (const std::exception &error) {
            QMessageBox::warning(this, QStringLiteral("删除失败"), QString::fromUtf8(error.what()));
        }
    }
    void manage_eop_data() {
        if (busy())
            return;
        show_data_center(this, *eop_catalog_);
    }
    void create_scenario() {
        if (busy())
            return;
        const auto &coverage =
            eop_catalog_->providers().at(eop_catalog_->preferred_index())->data_info();
        try {
            auto next = new_scenario_dialog(
                this, coverage.first_epoch, coverage.last_epoch,
                [this](core::Epoch first, core::Epoch last) {
                    (void)data_for_interval(first, last);
                },
                eop_catalog_->time_provider());
            if (next && confirm_replace_scenario())
                activate_scenario(std::move(*next));
        } catch (const core::TimeDataRangeError &) {
            QMessageBox::warning(
                this, QStringLiteral("UTC 数据需要更新"),
                QStringLiteral("当前日期超出闰秒数据有效期，请在数据中心更新 UTC / 闰秒后重试。"));
        }
    }
    std::shared_ptr<const core::IEarthOrientationProvider>
    data_for_interval(core::Epoch first, core::Epoch last) const {
        return eop_catalog_->for_interval(first, last);
    }
    std::shared_ptr<const core::ITimeProvider> time_for(const core::TimeDataInfo &info) const {
        if (active_time_ && active_time_->data_info().sha256 == info.sha256)
            return active_time_;
        if (eop_catalog_ && eop_catalog_->time_provider()->data_info().sha256 == info.sha256)
            return eop_catalog_->time_provider();
        auto builtin = runtime::make_builtin_time_provider();
        if (builtin->data_info().sha256 == info.sha256)
            return builtin;
        return runtime::load_time_snapshot(eop_catalog_->data().time_file(info.sha256));
    }
    std::shared_ptr<const core::ITimeProvider> active_time() const {
        active_time_ = time_for(eop_->data_info().time_data);
        return active_time_;
    }
    QString calendar_text(core::Epoch epoch, CalendarScale scale, bool date = true) const {
        return desktop::calendar_text(epoch, scale, date, active_time().get());
    }
    void activate_scenario(application::Scenario next) {
        if (busy())
            throw std::logic_error(
                "Finish the pending object update before replacing the scenario");
        const auto next_eop = data_for_interval(next.first(), next.last());
        if (display_cancellation_)
            display_cancellation_->store(true);
        if (!next.objects().empty())
            throw std::invalid_argument("Create an empty scenario before inserting objects");
        const auto old_tracks = orbit_group_->children;
        const auto old_models = model_group_->children;
        begin_change();
        try {
            orbit_group_->children.clear();
            model_group_->children.clear();
            rebuild();
        } catch (...) {
            orbit_group_->children = old_tracks;
            model_group_->children = old_models;
            rebuild();
            throw;
        }
        if (properties_dialog_)
            properties_dialog_->close();
        satellites_.clear();
        stations_.clear();
        sensors_.clear();
        clear_access_display();
        if (sensor_editor_)
            sensor_editor_->close();
        close_reports();
        if (station_editor_)
            station_editor_->close();
        if (ground_map_) {
            ground_map_->set_orbits({});
            ground_map_->set_ground_stations({});
            ground_map_->reset_view();
        }
        orbit_.reset();
        model_node_ = {};
        editor_target_.reset();
        model_kind_ = 0;
        model_diameter_km_ = 1000;
        const QSignalBlocker block(objects_);
        objects_->clear();
        object_item_ = nullptr;
        scenario_ = std::move(next);
        eop_ = next_eop;
        gravity_.clear();
        ephemerides_.clear();
        weather_.clear();
        ocean_tides_.clear();
        time_label_epoch_.reset();
        scenario_ = scenario_->with_data_lock(application::eop_data_lock(eop_->data_info()));
        scenario_item_ = new QTreeWidgetItem(objects_, {QString::fromStdString(scenario_->name())});
        scenario_item_->setIcon(0,
                                workbench_icon(WorkbenchIcon::earth, theme::color(theme::accent)));
        scenario_item_->setToolTip(
            0, QStringLiteral("Scenario · 中心天体 Earth\n%1 — %2 TAI")
                   .arg(display_tai(scenario_->first()), display_tai(scenario_->last())));
        scenario_item_->setExpanded(true);
        set_workspace_name(QString::fromStdString(scenario_->name()));
        setWindowTitle(QString::fromStdString(scenario_->name()) + QStringLiteral(" · OrbitForge"));
        project_path_.clear();
        standalone_time_data_.reset();
        analysis_record_.reset();
        mark_project_dirty();
        set_selection(std::nullopt);
        set_playing(false);
        seek_epoch(scenario_->first());
        snapshot_ = sample_frame(scenario_->first());
        update_satellite();
        selection_->update(snapshot_);
        for (auto &slot : slots_)
            slot->pick_candidates.clear();
        workspace_pages_->setCurrentIndex(0);
        if (scenario_properties_)
            scenario_properties_->close();
        workspace_->select_view(0);
        object_dock_->show();
        fit_pending_ = true;
        update_actions();
        show_orbit_status();
        if (timeline_)
            timeline_->reset_view();
    }
    void open_example() {
        if (busy() || !confirm_replace_scenario())
            return;
        activate_scenario(application::Scenario("TwoBodyExample", core::Epoch{0},
                                                core::Epoch{7'200'000'000'000}));
        begin_insert_satellite();
        editor_->submit();
        properties_dialog_->close();
    }
    void show_properties_dialog() { workspace_->open(properties_dialog_, false); }
    void show_insert_objects() {
        if (!scenario_ || !editor_ || busy())
            return;
        const auto request = insert_object_dialog(this, QString::fromStdString(scenario_->name()));
        if (!request)
            return;
        switch (request->type) {
        case InsertObjectType::satellite:
            begin_insert_satellite(request->method);
            break;
        case InsertObjectType::ground_station:
            begin_insert_station(request->method);
            break;
        }
    }
    void begin_insert_satellite(InsertObjectMethod method = InsertObjectMethod::define_properties) {
        if (!scenario_ || !editor_ || busy())
            return;
        if (satellites_.size() + stations_.size() >= 10000) {
            QMessageBox::information(this, QStringLiteral("对象数量"),
                                     QStringLiteral("当前场景最多支持 10,000 个对象。"));
            return;
        }
        OrbitInput input;
        std::size_t suffix = satellites_.size() + 1;
        do {
            input.object_id = "Satellite" + std::to_string(suffix++);
        } while (object_id_exists(input.object_id));
        input.first = scenario_->first();
        input.last = scenario_->last();
        input.initial.epoch = scenario_->first();
        editor_target_.reset();
        editor_->set_time_provider(eop_catalog_->time_provider());
        editor_->set_input(input);
        editor_->set_create_mode(true);
        editor_->set_appearance(0, {}, 1000);
        editor_->set_display({});
        editor_->show_appearance_result({});
        editor_->setToolTip({});
        editor_->show_result({});
        inspector_pages_->setCurrentIndex(0);
        properties_dialog_->setWindowTitle(QStringLiteral("插入卫星 · ") +
                                           QString::fromStdString(scenario_->name()));
        insert_defaults_pending_ = method == InsertObjectMethod::defaults;
        if (insert_defaults_pending_) {
            properties_dialog_->close();
            start_orbit(std::move(input));
        } else {
            show_properties_dialog();
        }
    }
    void configure_station_editor(const application::ProjectGroundStation &station, bool creating) {
        if (!station_editor_) {
            station_editor_ = new GroundStationEditor(this);
            station_editor_->apply = [this](application::ProjectGroundStation value) {
                if (busy() || !scenario_)
                    return;
                auto project = project_snapshot(project_path_);
                auto index = stations_.size();
                std::optional<std::string> edited;
                if (station_editor_target_) {
                    const auto found = std::find_if(
                        project.ground_stations.begin(), project.ground_stations.end(),
                        [&](const auto &s) { return s.object_id == *station_editor_target_; });
                    if (found == project.ground_stations.end())
                        throw std::invalid_argument("地面站已不存在，请重新选择对象。");
                    index = static_cast<std::size_t>(found - project.ground_stations.begin());
                    if (found->location == value.location && found->name() == value.name() &&
                        found->minimum_elevation_rad == value.minimum_elevation_rad) {
                        station_editor_->show_result(QStringLiteral("参数未改变。"));
                        return;
                    }
                    if (found->location != value.location)
                        edited = value.object_id;
                    *found = std::move(value);
                } else {
                    project.ground_stations.push_back(std::move(value));
                }
                application::validate_project(project);
                if (start_project(std::move(project), project_path_, true,
                                  satellites_.size() + index, edited))
                    station_editor_->show_result(QStringLiteral("正在应用…"));
                else
                    station_editor_->show_result(QStringLiteral("未应用，原场景保留。"));
            };
        }
        station_editor_target_ =
            creating ? std::nullopt : std::optional<std::string>{station.object_id};
        station_editor_->set_station(station, creating);
    }
    void begin_insert_station(InsertObjectMethod method = InsertObjectMethod::define_properties) {
        if (!scenario_ || busy())
            return;
        if (satellites_.size() + stations_.size() >= 10000) {
            QMessageBox::information(this, QStringLiteral("对象数量"),
                                     QStringLiteral("当前场景最多支持 10,000 个对象。"));
            return;
        }
        application::ProjectGroundStation station;
        auto suffix = stations_.size() + 1;
        do {
            station.object_id = "GroundStation" + std::to_string(suffix++);
        } while (object_id_exists(station.object_id));
        station.display_name = "地面站 " + std::to_string(suffix - 1);
        configure_station_editor(station, true);
        if (method == InsertObjectMethod::defaults) {
            station_editor_->close();
            station_editor_->apply(station);
        } else
            workspace_->open(station_editor_, false);
    }
    void set_playing(bool value) {
        playing_ = value;
        if (value)
            precise_epoch_.reset();
        if (play_action_) {
            play_action_->setText(value ? QStringLiteral("暂停") : QStringLiteral("播放"));
            play_action_->setIcon(play_icon(value));
        }
        clock_.restart();
    }
    void seek_epoch(core::Epoch epoch) {
        if (!scenario_ || epoch.tai_nanoseconds() < scenario_->first().tai_nanoseconds() ||
            epoch.tai_nanoseconds() > scenario_->last().tai_nanoseconds())
            throw std::out_of_range("Requested display epoch lies outside the analysis interval");
        seconds_ = epoch.seconds_since(scenario_->first());
        precise_epoch_ = epoch;
        requested_time_ = true;
        const QSignalBlocker block(slider_);
        slider_->setValue(qRound(seconds_ / duration_seconds() * 10000));
        if (timeline_)
            timeline_->set_current_time(epoch.tai_nanoseconds() / 1'000'000, true);
        clock_.restart();
    }
    void choose_time() {
        if (!scenario_)
            return;
        const bool resume = playing_;
        set_playing(false);
        QDialog dialog(this);
        dialog.setObjectName("jumpTimeDialog");
        dialog.setWindowTitle(QStringLiteral("跳转时间"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *time = new EpochEditor(nullptr, active_time());
        time->set_scale(display_scale_);
        time->set_range(scenario_->first(), scenario_->last());
        time->set_epoch(snapshot_.epoch);
        time->setMinimumWidth(380);
        layout->addWidget(time);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        auto *cancel = new QPushButton(QStringLiteral("取消"));
        auto *apply = new QPushButton(QStringLiteral("跳转"));
        apply->setObjectName("applyOrbit");
        buttons->addWidget(cancel);
        buttons->addWidget(apply);
        layout->addLayout(buttons);
        connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
        connect(apply, &QPushButton::clicked, &dialog, [&dialog, time] {
            try {
                (void)time->epoch();
                dialog.accept();
            } catch (const std::exception &error) {
                QMessageBox::information(&dialog, QStringLiteral("时间无效"),
                                         QString::fromUtf8(error.what()));
            }
        });
        if (dialog.exec() == QDialog::Accepted) {
            const auto epoch = time->epoch();
            if (scenario_ && epoch.tai_nanoseconds() >= scenario_->first().tai_nanoseconds() &&
                epoch.tai_nanoseconds() <= scenario_->last().tai_nanoseconds())
                seek_epoch(epoch);
            else
                QMessageBox::information(
                    this, QStringLiteral("分析时段已更新"),
                    QStringLiteral("新轨道的分析时段已改变，请重新选择跳转时间。"));
        } else
            set_playing(resume);
    }
    void open_properties() {
        if (selected_sensor() && !busy()) {
            edit_sensor(false);
            return;
        }
        if (selected_station_ && !busy()) {
            configure_station_editor(stations_.at(*selected_station_).config, false);
            workspace_->open(station_editor_, false);
            return;
        }
        if (!selected_ && scenario_ && objects_->currentItem() == scenario_item_) {
            open_scenario_properties();
            return;
        }
        if (!properties_dialog_ || !selected_ || busy())
            return;
        editor_target_ = selected_;
        const auto &satellite = satellites_.at(*editor_target_);
        editor_->set_time_provider(eop_catalog_->time_provider());
        editor_->set_input(satellite.orbit->input);
        editor_->set_create_mode(false);
        editor_->set_appearance(satellite.model_kind, satellite.model_path,
                                satellite.model_diameter_km);
        editor_->set_display(satellite.display);
        editor_->show_appearance_result({});
        inspector_pages_->setCurrentIndex(0);
        properties_dialog_->setWindowTitle(satellite.name() + QStringLiteral(" · 对象属性"));
        show_orbit_status();
        show_properties_dialog();
    }
    bool apply_scenario_settings(const ScenarioSettings &settings) {
        if (!scenario_ || busy())
            return false;
        if (settings.first != scenario_->first() || settings.last != scenario_->last())
            return start_project(project_snapshot(project_path_), project_path_, true,
                                 selected_marker_index(), std::nullopt, settings);
        const auto checked = project_snapshot(project_path_)
                                 .with_settings(settings.name, settings.first, settings.last);
        if (checked.name == scenario_->name())
            return false;
        // A rename keeps the current calculation, playback and display caches intact.
        scenario_ = scenario_->with_settings(checked.name, checked.first, checked.last);
        const auto name = QString::fromStdString(checked.name);
        scenario_item_->setText(0, name);
        set_workspace_name(name);
        if (scenario_properties_)
            scenario_properties_->setWindowTitle(name + QStringLiteral(" · 场景属性"));
        show_timeline_status();
        mark_project_dirty();
        return true;
    }
    void open_scenario_properties() {
        if (!scenario_ || busy())
            return;
        if (!scenario_properties_)
            scenario_properties_ = new QDialog(this);
        auto &dialog = *scenario_properties_;
        if (workspace_->contains(&dialog)) {
            if (auto *count = dialog.findChild<QLabel *>("scenarioObjectCount"))
                count->setText(QString::number(satellites_.size() + stations_.size()));
            workspace_->open(&dialog);
            return;
        }
        if (auto *previous = dialog.layout()) {
            while (auto *item = previous->takeAt(0)) {
                if (item->widget())
                    delete item->widget();
                if (item->layout()) {
                    while (auto *field = item->layout()->takeAt(0)) {
                        delete field->widget();
                        delete field;
                    }
                }
                delete item;
            }
            delete previous;
        }
        dialog.setObjectName("scenarioProperties");
        dialog.setWindowTitle(QString::fromStdString(scenario_->name()) +
                              QStringLiteral(" · 场景属性"));
        dialog.setMinimumWidth(620);
        auto *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(theme::space::page);
        layout->setSpacing(16);
        auto *title = new QLabel(QStringLiteral("场景设置"));
        title->setObjectName("panelTitle");
        layout->addWidget(title);
        auto *description =
            new QLabel(QStringLiteral("调整名称与分析时段，保留卫星的初始历元和轨道参数。"));
        description->setObjectName("dialogDescription");
        description->setWordWrap(true);
        layout->addWidget(description);
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *body = new QWidget;
        auto *body_layout = new QVBoxLayout(body);
        body_layout->setContentsMargins(0, 0, 8, 0);
        body_layout->setSpacing(0);
        scroll->setWidget(body);
        layout->addWidget(scroll, 1);
        // Editable settings first, then read-only facts, then the applied data record.
        const auto group = [](const QString &heading) {
            auto *frame = new FormGroup(heading);
            frame->setMaximumWidth(880);
            return std::pair{frame, frame->form()};
        };
        auto [section, form] = group(QStringLiteral("基本设置"));
        auto *name = new QLineEdit(QString::fromStdString(scenario_->name()));
        name->setObjectName("scenarioSettingsName");
        name->setMaximumWidth(380);
        form->addRow(QStringLiteral("场景名称"), name);
        auto *first = new EpochEditor(nullptr, eop_catalog_->time_provider());
        auto *last = new EpochEditor(nullptr, eop_catalog_->time_provider());
        first->setObjectName("scenarioSettingsStart");
        last->setObjectName("scenarioSettingsEnd");
        first->set_epoch(scenario_->first());
        last->set_epoch(scenario_->last());
        for (auto *field : {first, last})
            field->setMaximumWidth(380);
        form->addRow(QStringLiteral("开始时间"), first);
        form->addRow(QStringLiteral("结束时间"), last);
        section->add_help(
            QStringLiteral("修改时段后将重新准备全部轨迹。当前时刻超出新范围时，定位到最近的边界；"
                           "数据不足或更新失败时保留原场景。"));
        auto [facts, facts_form] = group(QStringLiteral("场景概况"));
        facts_form->addRow(QStringLiteral("中心天体"), new QLabel(QStringLiteral("Earth / 地球")));
        auto *count = new QLabel(QString::number(satellites_.size() + stations_.size()));
        count->setObjectName("scenarioObjectCount");
        facts_form->addRow(QStringLiteral("对象数量"), count);
        auto *duration = new QLabel;
        duration->setObjectName("scenarioSettingsDuration");
        facts_form->addRow(QStringLiteral("时长"), duration);
        auto *fields = new QWidget;
        fields->setObjectName("scenarioSettingsFields");
        auto *fields_layout = new QVBoxLayout(fields);
        fields_layout->setContentsMargins(0, 0, 0, 0);
        fields_layout->setSpacing(0);
        fields_layout->addWidget(section);
        fields_layout->addWidget(facts);
        body_layout->addWidget(fields);
        auto [data_section, data_form] = group(QStringLiteral("当前已应用的计算数据"));
        form = data_form;
        body_layout->addWidget(data_section);
        auto *eop_caption = new QLabel(QString::fromStdString(eop_->data_info().id));
        if (eop_->data_info().interpolation.starts_with("finals2000a"))
            eop_caption->setText(eop_caption->text() + QStringLiteral(" · 可能含预测值"));
        eop_caption->setTextFormat(Qt::PlainText);
        eop_caption->setWordWrap(true);
        eop_caption->setToolTip(QString::fromStdString(eop_->data_info().id));
        form->addRow(QStringLiteral("地球定向数据"), eop_caption);
        auto *extrapolated = new QLabel(
            extrapolation_description(eop_->data_info(), scenario_->first(), scenario_->last()));
        extrapolated->setObjectName("scenarioExtrapolation");
        extrapolated->setWordWrap(true);
        form->addRow(QStringLiteral("外推情况"), extrapolated);
        // The 64-digit hash reads best on its own full-width row under its caption.
        auto *locked = new QLineEdit(QString::fromStdString(scenario_->data_lock().sha256()));
        locked->setObjectName("scenarioDataHash");
        locked->setReadOnly(true);
        locked->setAccessibleName(QStringLiteral("本次计算数据 SHA-256"));
        auto *locked_caption = new QLabel(QStringLiteral("本次计算数据 SHA-256"));
        locked_caption->setBuddy(locked);
        form->addRow(locked_caption);
        form->addRow(locked);
        data_section->add_help(
            QStringLiteral("本次计算的数据来源记录；再次打开或计算时检查变化并请求确认。"));
        align_form_labels(body);
        body_layout->addStretch();
        auto *result = new QLabel;
        result->setObjectName("scenarioSettingsResult");
        result->setWordWrap(true);
        result->hide();
        layout->addWidget(result);
        auto *buttons = new QHBoxLayout;
        auto *reset = new QPushButton(QStringLiteral("重置"));
        reset->setObjectName("resetScenarioSettings");
        reset->setAutoDefault(false);
        buttons->addWidget(reset);
        buttons->addStretch();
        auto *close = new QPushButton(QStringLiteral("关闭"));
        close->setAutoDefault(false);
        buttons->addWidget(close);
        auto *apply = new QPushButton(QStringLiteral("应用"));
        apply->setObjectName("applyScenarioSettings");
        apply->setToolTip(QStringLiteral("应用场景名称与分析时段"));
        apply->setProperty("primary", true);
        apply->setDefault(true);
        buttons->addWidget(apply);
        layout->addLayout(buttons);
        const auto preview = [first, last, duration] {
            try {
                const auto seconds = last->epoch().seconds_since(first->epoch());
                duration->setText(seconds > 0 ? QString::number(seconds / 3600, 'g', 10) +
                                                    QStringLiteral(" 小时")
                                              : QStringLiteral("结束时间须晚于开始时间"));
            } catch (const std::exception &) {
                duration->setText(QStringLiteral("请输入有效的起止时间"));
            }
        };
        for (auto *control : {first, last})
            connect(control->findChild<QLineEdit *>("epochValue"), &QLineEdit::textChanged,
                    duration, preview, Qt::QueuedConnection);
        preview();
        connect(reset, &QPushButton::clicked, &dialog, [this, name, first, last, result, preview] {
            name->setText(QString::fromStdString(scenario_->name()));
            first->set_epoch(scenario_->first());
            last->set_epoch(scenario_->last());
            preview();
            result->hide();
        });
        connect(apply, &QPushButton::clicked, &dialog, [this, name, first, last, result] {
            try {
                if (name->text().trimmed().isEmpty())
                    throw std::invalid_argument("请输入场景名称。");
                const ScenarioSettings settings{name->text().trimmed().toStdString(),
                                                first->epoch(), last->epoch()};
                if (settings.last.tai_nanoseconds() <= settings.first.tai_nanoseconds())
                    throw std::invalid_argument("结束时间必须晚于开始时间。");
                const bool unchanged = settings.name == scenario_->name() &&
                                       settings.first == scenario_->first() &&
                                       settings.last == scenario_->last();
                if (unchanged)
                    result->setText(QStringLiteral("设置未变化。"));
                else if (apply_scenario_settings(settings))
                    result->setText(busy() ? QStringLiteral("正在更新，可在状态栏取消。")
                                           : QStringLiteral("场景名称已更新。"));
                else
                    result->setText(QStringLiteral("未应用，原场景保留。"));
            } catch (const std::exception &error) {
                result->setText(QString::fromUtf8(error.what()));
            }
            result->show();
        });
        connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
        workspace_->open(&dialog);
    }
    void set_selection(std::optional<std::size_t> selection, bool synchronize_tree = true) {
        if (benchmarking_) {
            selected_ = selection;
            return;
        }
        if (selection && *selection >= satellites_.size() + stations_.size())
            selection.reset();
        selected_ = selection && *selection < satellites_.size() ? selection : std::nullopt;
        selected_station_ = selection && *selection >= satellites_.size()
                                ? std::optional<std::size_t>{*selection - satellites_.size()}
                                : std::nullopt;
        if (ground_map_)
            ground_map_->set_selection(selection);
        if (synchronize_tree && objects_) {
            const QSignalBlocker block(objects_);
            objects_->clearSelection();
            auto *item = selected_           ? satellites_.at(*selected_).item
                         : selected_station_ ? stations_.at(*selected_station_).item
                                             : nullptr;
            objects_->setCurrentItem(item);
            if (item)
                objects_->scrollToItem(item);
        }
        update_actions();
        show_timeline_status();
    }
    void update_satellite() {
        const ProfileScope profile("satellite_update_ms");
        if (benchmarking_) {
            markers_->update(snapshot_);
            return;
        }
        if (!sun_epoch_ || *sun_epoch_ != snapshot_.epoch) {
            const auto gcrf = runtime::display_sun_direction_gcrf(snapshot_.epoch);
            display_to_itrf_ =
                frames_->gcrf_to_itrf(snapshot_.epoch, eop_->at(snapshot_.epoch).sample);
            sun_direction_ = display_to_itrf_->apply_position(gcrf);
            sun_epoch_ = snapshot_.epoch;
            if (ground_map_)
                ground_map_->set_sun_direction(sun_direction_);
        }
        vsg::dmat4 earth_rotation;
        if (display_frame_ == core::FrameId::gcrf)
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t col = 0; col < 3; ++col)
                    earth_rotation[col][row] =
                        display_to_itrf_->matrix[col][row]; // R transpose, column-major VSG.
        earth_transform_->matrix = earth_rotation;
        visualization::RenderSnapshot fixed{
            snapshot_.epoch, core::FrameId::itrf, core::OriginId::earth, {}};
        visualization::RenderSnapshot points{
            snapshot_.epoch, snapshot_.frame, snapshot_.origin, {}};
        std::vector<std::uint32_t> point_colors;
        const auto format = slots_.empty() || !slots_.front()->window->windowAdapter
                                ? VK_FORMAT_B8G8R8A8_SRGB
                                : slots_.front()->window->windowAdapter->surfaceFormat().format;
        const bool srgb = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
        for (std::size_t i = 0; i < satellites_.size(); ++i) {
            const auto &satellite = satellites_[i];
            const auto &marker = snapshot_.markers.at(i);
            if (cache_covers(*satellite.orbit, snapshot_.epoch, snapshot_.epoch))
                fixed.markers.push_back(
                    satellite.orbit->track.sample(snapshot_.epoch).markers.front());
            else
                fixed.markers.push_back(
                    {marker.object_id, display_frame_ == core::FrameId::gcrf
                                           ? display_to_itrf_->apply_position(marker.position_m)
                                           : marker.position_m});
            const auto [first, last] =
                display_interval(*satellite.orbit, satellite.display, snapshot_.epoch);
            satellite.fixed_track->set_interval(first, last);
            satellite.inertial_track->set_interval(first, last);
            for (const auto &track : {satellite.fixed_track, satellite.inertial_track})
                track->set_appearance(satellite.display.line_color, satellite.display.line_width,
                                      static_cast<unsigned>(satellite.display.line_style),
                                      devicePixelRatioF(), srgb);
            satellite.tracks->setAllChildren(false);
            if (satellite.display.track_3d)
                satellite.tracks->setSingleChildOn(display_frame_ == core::FrameId::gcrf ? 0 : 1);
            if (ground_map_)
                ground_map_->set_display(i, satellite.display, first, last);
            if (satellite.model) {
                const auto &r = marker.position_m;
                satellite.model->matrix = vsg::translate(r[0], r[1], r[2]) * earth_rotation *
                                          vsg::scale(satellite.model_diameter_km * 1000);
            } else {
                points.markers.push_back(marker);
                point_colors.push_back(satellite.display.line_color);
            }
        }
        markers_->update(points, point_colors, srgb);
        for (std::size_t i = 0; i < stations_.size(); ++i) {
            const auto &station = stations_[i].config;
            fixed.markers.push_back(
                {station.object_id, core::ground_station_position(station.location)});
        }
        if (ground_map_)
            ground_map_->set_snapshot(fixed);
        update_access_display();
        globe_->set_illumination(sun_direction_, lighting_action_->isChecked());
        satellite_state_dirty_ = false;
    }
    void start_model(int kind, const QString &path, double diameter_km) {
        if (busy() || !editor_target_)
            return;
        if (kind < 0 || kind > 2 || !std::isfinite(diameter_km) || diameter_km <= 0 ||
            (kind == 2 && path.isEmpty())) {
            editor_->show_appearance_result(QStringLiteral("请选择模型文件和有效的显示尺寸。"),
                                            true);
            return;
        }
        pending_model_kind_ = kind;
        pending_model_target_ = *editor_target_;
        pending_model_path_ = path;
        pending_model_diameter_km_ = diameter_km;
        model_candidate_ = std::async(std::launch::async, [kind, path] {
            if (kind == 0)
                return vsg::ref_ptr<vsg::MatrixTransform>{};
            auto placement = vsg::MatrixTransform::create();
            placement->addChild(make_satellite_model(
                kind == 1 ? std::filesystem::path{} : std::filesystem::path(path.toStdWString())));
            return placement;
        });
        editor_->set_appearance_busy(true);
        update_actions();
    }
    void poll_model() {
        if (!model_candidate_.valid() ||
            model_candidate_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        editor_->set_appearance_busy(false);
        try {
            auto candidate = model_candidate_.get();
            auto &satellite = satellites_.at(pending_model_target_);
            const auto previous = satellite.model_host->children;
            begin_change();
            try {
                satellite.model_host->children.clear();
                if (candidate)
                    satellite.model_host->addChild(candidate);
                rebuild();
            } catch (...) {
                satellite.model_host->children = previous;
                try {
                    rebuild();
                } catch (...) {
                    fail("Cannot restore the previous satellite appearance");
                    return;
                }
                throw;
            }
            satellite.model = std::move(candidate);
            satellite.model_kind = pending_model_kind_;
            satellite.model_diameter_km = pending_model_diameter_km_;
            satellite.model_path = pending_model_path_;
            mark_project_dirty();
            if (pending_model_target_ == 0) {
                model_node_ = satellite.model;
                model_kind_ = satellite.model_kind;
                model_diameter_km_ = satellite.model_diameter_km;
            }
            last_model_outcome_ = "applied";
            update_satellite();
            editor_->show_appearance_result(QStringLiteral("已应用"));
        } catch (const std::exception &error) {
            editor_->show_appearance_result(
                QStringLiteral("未应用；原外观保留。\n") + QString::fromUtf8(error.what()), true);
            last_model_outcome_ = "rejected";
        } catch (const vsg::Exception &error) {
            editor_->show_appearance_result(QStringLiteral("模型编译失败；原外观保留。\n") +
                                                QString::fromStdString(error.message),
                                            true);
            last_model_outcome_ = "rejected";
        }
        update_actions();
    }
    void show_timeline_status() {
        if (timeline_ && scenario_) {
            timeline_->set_time_provider(active_time());
            const auto first_ms = scenario_->first().tai_nanoseconds() / 1'000'000;
            // The ruler resolves milliseconds; keep a drawable span for a valid
            // sub-millisecond scenario. Seeks still clamp to the exact Epoch bounds.
            const auto last_ms =
                std::max(first_ms + 1, scenario_->last().tai_nanoseconds() / 1'000'000);
            timeline_->set_interval(first_ms, last_ms, QString::fromStdString(scenario_->name()),
                                    selected_name());
        }
    }
    void show_orbit_status() {
        show_timeline_status();
        if (!editor_ || !editor_target_ || busy())
            return;
        editor_->show_result({});
    }
    void start_orbit(OrbitInput input) {
        if (busy() || !scenario_)
            return;
        if (display_cancellation_)
            display_cancellation_->store(true);
        pending_orbit_target_ = editor_target_;
        input.first = scenario_->first();
        input.last = scenario_->last();
        if (editor_target_)
            input.object_id = satellites_.at(*editor_target_).orbit->input.object_id;
        else if (object_id_exists(input.object_id)) {
            last_orbit_outcome_ = "rejected";
            editor_->show_result(QStringLiteral("对象 ID 已存在，请使用其他名称。"), true);
            return;
        }
        auto data_first = input.first;
        auto data_last = input.last;
        try {
            auto data_input = project_snapshot(project_path_);
            if (editor_target_)
                data_input.satellites.at(*editor_target_).orbit = input;
            else {
                application::ProjectSatellite added;
                added.orbit = input;
                data_input.satellites.push_back(std::move(added));
            }
            std::tie(data_first, data_last) = application::calculation_data_interval(data_input);
            const auto current_data = data_for_interval(data_first, data_last);
            if (!application::matching_project_data(current_data->data_info(), eop_->data_info()) ||
                !gravity_.empty() || !ephemerides_.empty() || !weather_.empty() ||
                !ocean_tides_.empty() ||
                (input.uses_hpop() &&
                 (!input.hpop.gravity_model.empty() || input.hpop.uses_ephemeris() ||
                  input.hpop.ocean_tides.active() ||
                  (input.hpop.drag.enabled && !input.hpop.drag.weather_model.empty())))) {
                // Publish all tracks and their provider together, never mix EOP revisions.
                auto project = project_snapshot(project_path_);
                const auto id = input.object_id;
                if (editor_target_)
                    project.satellites.at(*editor_target_).orbit = std::move(input);
                else {
                    application::ProjectSatellite added;
                    added.orbit = std::move(input);
                    project.satellites.push_back(std::move(added));
                }
                if (start_project(std::move(project), project_path_, true,
                                  editor_target_.value_or(satellites_.size()), id)) {
                    last_orbit_outcome_.clear();
                    editor_->set_busy(true);
                } else {
                    last_orbit_outcome_ = last_project_outcome_;
                    editor_->show_result(QStringLiteral("未重新计算；原场景保留。"));
                }
                return;
            }
        } catch (const std::out_of_range &) {
            last_orbit_outcome_ = "rejected";
            show_data_coverage(data_first, data_last);
            editor_->show_result(QStringLiteral("数据覆盖不足；原场景保留。"), true);
            return;
        } catch (const std::exception &error) {
            last_orbit_outcome_ = "rejected";
            editor_->show_result(QString::fromUtf8(error.what()), true);
            return;
        }
        cancellation_ = std::make_shared<std::atomic_bool>(false);
        last_orbit_outcome_.clear();
        const auto revision =
            editor_target_
                ? satellites_.at(*editor_target_).orbit->session.provenance().input_revision + 1
                : 1;
        const auto settings =
            editor_target_ ? satellites_.at(*editor_target_).display : DisplaySettings{};
        const auto cache = display_cache_interval(input, settings, scenario_->first());
        const auto eop = eop_;
        const auto calculation_time = time_for(eop->data_info().time_data);
        candidate_ = std::async(std::launch::async, [this, input = std::move(input), revision,
                                                     cache, eop, calculation_time,
                                                     cancel = cancellation_] {
            auto session =
                make_orbit_session(input, revision, calculation_time.get(), {frames_, eop});
            return prepare_orbit_scene(input, std::move(session), *frames_, *eop, *cancel, cache);
        });
        editor_->set_busy(true);
        update_actions();
    }
    void poll_orbit() {
        if (!candidate_.valid() ||
            candidate_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        editor_->set_busy(false);
        try {
            auto candidate = candidate_.get();
            if (cancellation_->load())
                throw OrbitCancelled{};
            auto next_scenario = scenario_->with_object(candidate->session);
            auto ground_tracks = GroundMap::prepare_tracks({candidate});
            auto fixed_track = std::make_shared<render_vsg::TrackLayer>(candidate->track.nodes());
            auto inertial_track =
                std::make_shared<render_vsg::TrackLayer>(candidate->inertial_track.nodes());
            auto track = vsg::Switch::create();
            track->addChild(true, inertial_track->node());
            track->addChild(true, fixed_track->node());
            auto model_host = vsg::Group::create();
            const auto previous = orbit_group_->children;
            const auto previous_models = model_group_->children;
            try {
                publish_static_node(track, [&] {
                    if (pending_orbit_target_)
                        orbit_group_->children.at(*pending_orbit_target_) = track;
                    else {
                        orbit_group_->addChild(track);
                        model_group_->addChild(model_host);
                    }
                });
            } catch (...) {
                orbit_group_->children = previous;
                model_group_->children = previous_models;
                try {
                    rebuild();
                } catch (...) {
                    fail("Cannot restore the previous orbit after scene compilation failed");
                    return;
                }
                throw;
            }
            std::size_t index = pending_orbit_target_.value_or(satellites_.size());
            if (pending_orbit_target_)
                satellites_[index].orbit = std::move(candidate);
            else {
                Satellite satellite;
                satellite.orbit = std::move(candidate);
                satellite.model_host = model_host;
                satellite.item = new QTreeWidgetItem(
                    scenario_item_, {QString::fromStdString(satellite.orbit->input.object_id)});
                satellite.item->setIcon(
                    0, workbench_icon(WorkbenchIcon::satellite,
                                      QColor::fromRgb(satellite.display.line_color)));
                satellite.item->setToolTip(0, QStringLiteral("双击打开对象属性 · 默认显示为点"));
                satellites_.push_back(std::move(satellite));
                scenario_item_->setExpanded(true);
            }
            scenario_ = std::move(next_scenario);
            filter_objects();
            mark_project_dirty();
            if (pending_orbit_target_)
                ground_map_->replace_track(index, std::move(ground_tracks.front()));
            else
                ground_map_->append_track(std::move(ground_tracks.front()));
            ground_map_->set_name(index, satellites_[index].name());
            satellites_[index].fixed_track = std::move(fixed_track);
            satellites_[index].inertial_track = std::move(inertial_track);
            satellites_[index].tracks = track;
            editor_target_ = index;
            editor_->set_create_mode(false);
            properties_dialog_->setWindowTitle(satellites_[index].name() +
                                               QStringLiteral(" · 对象属性"));
            orbit_ = satellites_.front().orbit;
            object_item_ = satellites_.front().item;
            snapshot_ = sample_frame(scenario_->first());
            update_satellite();
            set_selection(index);
            selection_->update({snapshot_.epoch, snapshot_.frame, snapshot_.origin, {}});
            for (auto &slot : slots_)
                slot->pick_candidates.clear();
            seconds_ = 0;
            precise_epoch_.reset();
            requested_time_ = true;
            const QSignalBlocker block(slider_);
            slider_->setValue(0);
            clock_.restart();
            initial_position_ = snapshot_.markers.front().position_m;
            fit_views();
            fit_pending_ = true;
            show_orbit_status();
            last_orbit_outcome_ = "applied";
        } catch (const OrbitCancelled &) {
            editor_->show_result(QStringLiteral("已取消；继续显示原场景。"));
            last_orbit_outcome_ = "cancelled";
        } catch (const std::exception &error) {
            editor_->show_result(
                QStringLiteral("未应用；原场景保留。\n") + QString::fromUtf8(error.what()), true);
            last_orbit_outcome_ = "rejected";
        } catch (const vsg::Exception &error) {
            editor_->show_result(QStringLiteral("轨迹资源编译失败；原场景已恢复。\n") +
                                     QString::fromStdString(error.message),
                                 true);
            last_orbit_outcome_ = "rejected";
        }
        if (insert_defaults_pending_ && last_orbit_outcome_ == "rejected")
            show_properties_dialog();
        insert_defaults_pending_ = false;
        update_actions();
    }
    void request_display_cache() {
        if (benchmarking_ || busy() || display_candidate_.valid() || !scenario_)
            return;
        for (std::size_t i = 0; i < satellites_.size(); ++i) {
            const auto &satellite = satellites_[i];
            if (!satellite.display.track_2d && !satellite.display.track_3d)
                continue;
            const auto wanted =
                display_interval(*satellite.orbit, satellite.display, snapshot_.epoch);
            if (cache_covers(*satellite.orbit, wanted.first, wanted.second))
                continue;
            if (failed_display_source_ == satellite.orbit &&
                failed_display_settings_ == satellite.display)
                continue;
            display_source_ = satellite.orbit;
            display_target_ = i;
            display_settings_ = satellite.display;
            display_cancellation_ = std::make_shared<std::atomic_bool>(false);
            const auto interval =
                display_cache_interval(satellite.orbit->input, satellite.display, snapshot_.epoch);
            display_candidate_ =
                std::async(std::launch::async, [this, source = display_source_, eop = eop_,
                                                interval, cancel = display_cancellation_] {
                    return prepare_orbit_scene(source->input, source->session, *frames_, *eop,
                                               *cancel, interval);
                });
            statusBar()->showMessage(QStringLiteral("正在准备当前时段的显示轨迹…"));
            break;
        }
    }
    void poll_display_cache() {
        if (!display_candidate_.valid() ||
            display_candidate_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        try {
            const ProfileScope profile("cache_publish_ms");
            auto candidate = display_candidate_.get();
            if (display_target_ >= satellites_.size() ||
                satellites_[display_target_].orbit != display_source_ ||
                satellites_[display_target_].display != display_settings_ ||
                display_cancellation_->load())
                return;
            const auto wanted = display_interval(*candidate, display_settings_, snapshot_.epoch);
            if (!cache_covers(*candidate, wanted.first, wanted.second))
                return; // A later seek superseded this request.
            auto fixed = std::make_shared<render_vsg::TrackLayer>(candidate->track.nodes());
            auto inertial =
                std::make_shared<render_vsg::TrackLayer>(candidate->inertial_track.nodes());
            auto tracks = vsg::Switch::create();
            tracks->addChild(true, inertial->node());
            tracks->addChild(true, fixed->node());
            auto prepared_map = GroundMap::prepare_tracks({candidate});
            auto map_track = std::move(prepared_map.front());
            const auto previous = orbit_group_->children.at(display_target_);
            try {
                publish_static_node(tracks,
                                    [&] { orbit_group_->children.at(display_target_) = tracks; });
            } catch (...) {
                orbit_group_->children.at(display_target_) = previous;
                rebuild();
                throw;
            }
            auto &satellite = satellites_[display_target_];
            satellite.orbit = std::move(candidate);
            satellite.fixed_track = std::move(fixed);
            satellite.inertial_track = std::move(inertial);
            satellite.tracks = tracks;
            ground_map_->replace_track(display_target_, std::move(map_track));
            ground_map_->set_name(display_target_, satellite.name());
            satellite_state_dirty_ = true;
            orbit_ = satellites_.front().orbit;
            ++display_cache_publications_;
            statusBar()->showMessage(QStringLiteral("轨迹已更新"), 2000);
        } catch (const OrbitCancelled &) {
        } catch (const std::exception &error) {
            failed_display_source_ = display_source_;
            failed_display_settings_ = display_settings_;
            statusBar()->showMessage(QStringLiteral("显示轨迹未更新：") +
                                     QString::fromUtf8(error.what()));
        } catch (const vsg::Exception &error) {
            failed_display_source_ = display_source_;
            failed_display_settings_ = display_settings_;
            statusBar()->showMessage(QStringLiteral("显示轨迹未更新：") +
                                     QString::fromStdString(error.message));
        }
    }
    void fit_views(std::optional<std::size_t> target = std::nullopt) {
        if (benchmarking_ || !scenario_)
            return;
        double bound = 6378137.0;
        for (const auto &satellite : satellites_)
            bound = std::max(bound, satellite.orbit->max_radius_m);
        bound *= 1.15;
        const auto r = snapshot_.markers.empty()
                           ? std::array<double, 3>{-3e6, 5e6, 2e6}
                           : snapshot_.markers.at(selected_marker_index().value_or(0)).position_m;
        const auto direction = vsg::normalize(vsg::dvec3(r[0], r[1], r[2]));
        const auto reference_up =
            std::abs(direction.z) > 0.9 ? vsg::dvec3(0, 1, 0) : vsg::dvec3(0, 0, 1);
        const auto side = vsg::normalize(vsg::cross(reference_up, direction));
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (target && *target != i)
                continue;
            auto &slot = *slots_[i];
            auto *perspective =
                dynamic_cast<vsg::Perspective *>(slot.camera->projectionMatrix.get());
            const double half_fov = std::atan(std::tan(35.0 * std::acos(-1.0) / 360.0) *
                                              std::min(1.0, perspective->aspectRatio));
            const double distance = bound / std::sin(half_fov);
            const auto view_direction = vsg::normalize(direction + side * (i ? 0.22 : -0.22));
            auto *look = dynamic_cast<vsg::LookAt *>(slot.camera->viewMatrix.get());
            look->eye = view_direction * distance;
            look->center = vsg::dvec3(0, 0, 0);
            look->up = vsg::normalize(vsg::cross(view_direction, side));
            if (slot.trackball)
                slot.trackball->setViewpoint(vsg::LookAt::create(look->eye, look->center, look->up),
                                             0);
            perspective->nearDistance = 6378.137;
            perspective->farDistance = distance + bound * 20;
        }
    }
    void focus_satellite(std::optional<std::size_t> target = std::nullopt) {
        if (!selected_marker_index())
            return;
        const auto &r = snapshot_.markers.at(*selected_marker_index()).position_m;
        const vsg::dvec3 centre(r[0], r[1], r[2]);
        const double distance = selected_ && satellites_.at(*selected_).model
                                    ? satellites_.at(*selected_).model_diameter_km * 3000
                                    : 1e6;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (target && *target != i)
                continue;
            auto &slot = slots_[i];
            auto *look = dynamic_cast<vsg::LookAt *>(slot->camera->viewMatrix.get());
            const auto direction = vsg::normalize(selected_station_ ? centre : look->eye - centre);
            const auto up = selected_station_ ? (std::abs(direction.z) > 0.9 ? vsg::dvec3(0, 1, 0)
                                                                             : vsg::dvec3(0, 0, 1))
                                              : look->up;
            const auto viewpoint = vsg::LookAt::create(centre + direction * distance, centre, up);
            if (slot->trackball)
                slot->trackball->setViewpoint(viewpoint, 0);
            else {
                look->eye = viewpoint->eye;
                look->center = centre;
                look->up = up;
            }
            auto *perspective =
                dynamic_cast<vsg::Perspective *>(slot->camera->projectionMatrix.get());
            perspective->nearDistance = std::max(0.01, distance * 1e-4);
        }
    }
    void fail(const std::string &message) {
        failure_ = QString::fromStdString(message);
        std::cerr << "Graphics failure: " << message << '\n';
        timer_.stop();
        app_.exit(1);
        QTimer::singleShot(0, this, [this] { app_.exit(1); });
    }
    void smoke_set_view_layout(int mode) {
        workspace_->dock_page(view_panel_);
        workspace_->dock_page(map_panel_);
        workspace_->select_view(0);
        if (mode == 2)
            workspace_->split_page(map_panel_, view_panel_, Qt::RightEdge);
        else {
            workspace_->merge_page(map_panel_, view_panel_);
            workspace_->select_view(mode);
        }
    }
    QTabBar *smoke_tabs(const QString &title) const {
        for (auto *tabs : workspace_->findChildren<QTabBar *>())
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->tabText(i) == title)
                    return tabs;
        throw std::runtime_error("Workspace page is missing");
    }
    void smoke_drag_page(const QString &title, const QPoint &global) {
        auto *tabs = smoke_tabs(title);
        int index = 0;
        while (tabs->tabText(index) != title)
            ++index;
        const auto start = tabs->tabRect(index).center();
        QMouseEvent press(QEvent::MouseButtonPress, start, tabs->mapToGlobal(start), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QMouseEvent move(QEvent::MouseMove, tabs->mapFromGlobal(global), global, Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, tabs->mapFromGlobal(global), global,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(tabs, &press);
        QCoreApplication::sendEvent(tabs, &move);
        QCoreApplication::sendEvent(tabs, &release);
    }
    void zoom_view(std::size_t index, double factor) {
        auto &slot = *slots_.at(index);
        auto *look = dynamic_cast<vsg::LookAt *>(slot.camera->viewMatrix.get());
        const auto offset = look->eye - look->center;
        const auto distance =
            std::clamp(vsg::length(offset) * factor,
                       vsg::length(look->center) < 1 ? 6378137.0 * 1.02 : 1.0, 1e11);
        slot.trackball->setViewpoint(
            vsg::LookAt::create(look->center + vsg::normalize(offset) * distance, look->center,
                                look->up),
            0);
        auto *projection = dynamic_cast<vsg::Perspective *>(slot.camera->projectionMatrix.get());
        projection->farDistance = std::max(projection->farDistance, distance * 3);
    }
    void change_display_frame(int index) {
        if (!scenario_ || !display_to_itrf_)
            return;
        const auto next = index == 0 ? core::FrameId::gcrf : core::FrameId::itrf;
        if (display_frame_ == next)
            return;
        const auto convert = [&](vsg::dvec3 value) {
            const std::array<double, 3> input{value.x, value.y, value.z};
            const auto p = next == core::FrameId::itrf ? display_to_itrf_->apply_position(input)
                                                       : display_to_itrf_->inverse_position(input);
            return vsg::dvec3(p[0], p[1], p[2]);
        };
        for (auto &slot : slots_) {
            auto *look = dynamic_cast<vsg::LookAt *>(slot->camera->viewMatrix.get());
            const auto viewpoint =
                vsg::LookAt::create(convert(look->eye), convert(look->center), convert(look->up));
            if (slot->trackball)
                slot->trackball->setViewpoint(viewpoint, 0);
            else
                slot->camera->viewMatrix = viewpoint;
        }
        display_frame_ = next;
        for (auto *choice : frame_choices_) {
            const QSignalBlocker blocker(choice);
            choice->setCurrentIndex(index);
        }
        snapshot_ = sample_frame(snapshot_.epoch);
        update_satellite();
    }
    // The workbench play button is an ice-filled circle; the benchmark bar keeps plain icons.
    [[nodiscard]] QIcon play_icon(bool playing) const {
        return workbench_icon(playing ? WorkbenchIcon::pause : WorkbenchIcon::play,
                              theme::color(benchmarking_ ? theme::text_soft : theme::on_accent));
    }
    void set_workspace_name(const QString &name) {
        const bool extrapolated = eop_->data_info().extrapolation_days > 0;
        workspace_label_->setText(name +
                                  (extrapolated ? QStringLiteral(" · EOP 外推") : QString{}));
        workspace_label_->setToolTip(workspace_label_->text());
        workspace_dot_->setProperty("active", true);
        workspace_dot_->setProperty("warning", extrapolated);
        workspace_dot_->style()->unpolish(workspace_dot_);
        workspace_dot_->style()->polish(workspace_dot_);
    }
    QWidget *make_view_controls(std::optional<std::size_t> slot) {
        auto *header = new QWidget;
        header->setObjectName("viewHeading");
        header->setMinimumHeight(34);
        header->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *rows = new QHBoxLayout(header);
        rows->setContentsMargins(12, 4, 8, 4);
        rows->setSpacing(4);
        auto *bar = new QHBoxLayout;
        bar->setContentsMargins(0, 0, 0, 0);
        bar->setSpacing(4);
        auto *title = new QLabel(slot ? QStringLiteral("地球") : QStringLiteral("地面 · WGS84"));
        title->setObjectName("viewTitle");
        bar->addWidget(title);
        if (slot) {
            auto *choice = new QComboBox;
            choice->setObjectName("viewFrame");
            choice->setAccessibleName(QStringLiteral("三维参考系"));
            choice->addItems({QStringLiteral("GCRF · 惯性"), QStringLiteral("ITRF · 地固")});
            choice->setToolTip(QStringLiteral("三维轨迹参考系；二维始终使用 ITRF"));
            choice->setCurrentIndex(display_frame_ == core::FrameId::gcrf ? 0 : 1);
            connect(choice, &QComboBox::currentIndexChanged, this,
                    [this](int index) { change_display_frame(index); });
            frame_choices_.push_back(choice);
            bar->addWidget(choice);
        }
        rows->addLayout(bar);
        bar = new QHBoxLayout;
        bar->setContentsMargins(0, 0, 0, 0);
        bar->setSpacing(4);
        rows->addLayout(bar, 1);
        auto *tools = new QFrame;
        tools->setObjectName("viewTools");
        auto *tool_layout = new QHBoxLayout(tools);
        tool_layout->setContentsMargins(2, 1, 2, 1);
        tool_layout->setSpacing(1);
        bar->addWidget(tools);
        const auto button = [&](WorkbenchIcon icon, const QString &text, auto command) {
            auto *control = new QToolButton;
            control->setIcon(workbench_icon(icon));
            control->setIconSize(QSize(16, 16));
            control->setFixedSize(26, 26);
            control->setToolTip(text);
            control->setAccessibleName(text);
            connect(control, &QToolButton::clicked, this, command);
            tool_layout->addWidget(control);
            return control;
        };
        button(WorkbenchIcon::fit, slot ? QStringLiteral("适配轨道") : QStringLiteral("显示全球"),
               [this, slot] {
                   if (slot)
                       fit_views(slot);
                   else
                       ground_map_->reset_view();
               });
        view_focus_buttons_.push_back(
            button(WorkbenchIcon::focus, QStringLiteral("定位所选对象"), [this, slot] {
                if (slot)
                    focus_satellite(slot);
                else
                    ground_map_->focus_selection();
            }));
        for (const auto &[icon, factor, text] :
             {std::tuple{WorkbenchIcon::zoom_in, 1.3, QStringLiteral("放大")},
              std::tuple{WorkbenchIcon::zoom_out, 1 / 1.3, QStringLiteral("缩小")}})
            button(icon, text, [this, slot, factor] {
                if (slot)
                    zoom_view(*slot, 1 / factor);
                else
                    ground_map_->zoom(factor, ground_map_->rect().center());
            });
        bar->addStretch();
        auto *settings = new QMenu(header);
        if (!lighting_action_) {
            lighting_action_ = new QAction(QStringLiteral("昼夜光照 · 同步二维 / 三维"), this);
            lighting_action_->setCheckable(true);
            lighting_action_->setChecked(true);
            connect(lighting_action_, &QAction::toggled, this, [this](bool enabled) {
                if (ground_map_)
                    ground_map_->set_lighting(enabled);
                satellite_state_dirty_ = true;
            });
            tracks_action_ = new QAction(QStringLiteral("三维轨迹"), this);
            tracks_action_->setCheckable(true);
            tracks_action_->setChecked(true);
            connect(tracks_action_, &QAction::toggled, this,
                    [this](bool enabled) { orbit_switch_->setAllChildren(enabled); });
        }
        settings->addAction(lighting_action_);
        settings->addSeparator();
        if (slot) {
            settings->addAction(tracks_action_);
        } else {
            for (const auto &[text, method] :
                 {std::pair{QStringLiteral("经纬网"), &GroundMap::set_grid_visible},
                  std::pair{QStringLiteral("地面轨迹"), &GroundMap::set_tracks_visible},
                  std::pair{QStringLiteral("对象名称"), &GroundMap::set_labels_visible},
                  std::pair{QStringLiteral("太阳直射点"), &GroundMap::set_sun_visible}}) {
                auto *action = settings->addAction(text);
                action->setCheckable(true);
                action->setChecked(true);
                connect(action, &QAction::toggled, ground_map_, method);
            }
        }
        auto *display = new QToolButton;
        display->setObjectName(slot ? "display3D" : "display2D");
        display->setIcon(workbench_icon(WorkbenchIcon::properties));
        display->setIconSize(QSize(18, 18));
        display->setToolTip(QStringLiteral("显示设置"));
        display->setAccessibleName(QStringLiteral("显示设置"));
        display->setFixedSize(30, 28);
        display->setMenu(settings);
        display->setPopupMode(QToolButton::InstantPopup);
        bar->addWidget(display);
        responsive_columns(header, rows, 420);
        return header;
    }
    void build_slot(bool secondary) {
        auto slot = std::make_unique<Slot>();
        auto *current = slot.get();
        auto traits = vsg::WindowTraits::create();
        traits->width = 640;
        traits->height = 640;
        traits->deviceTypePreferences = {VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU};
        globe_->prepare_window(*traits, validation_);
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        if (run_)
            prepare_present_timing(*traits);
#endif
        if (!smoke_dir_.isEmpty())
            traits->swapchainPreferences.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        traits->device = shared_device_ref_;
        current->window = new EmbeddedWindow(viewer_, traits);
        current->container = QWidget::createWindowContainer(current->window);
        current->container->setMinimumSize(64, 64);
        if (benchmarking_) {
            const auto ratio = current->window->devicePixelRatio();
            const int width = qRound(1280.0 / ratio), height = qRound(720.0 / ratio);
            if (qRound(width * ratio) != 1280 || qRound(height * ratio) != 720)
                throw std::runtime_error("This DPI cannot represent a 1280x720 native viewport; "
                                         "choose a compatible QT_SCALE_FACTOR");
            current->container->setFixedSize(width, height);
        }
        if (secondary) {
            current->dock = new QDockWidget(QStringLiteral("辅助三维视图"), this);
            current->dock->setWidget(current->container);
            addDockWidget(Qt::RightDockWidgetArea, current->dock);
            if (benchmarking_) {
                resizeDocks({current->dock}, {600}, Qt::Horizontal);
                auto *action = menuBar()->addAction(QStringLiteral("显示视图 2"));
                connect(action, &QAction::triggered, current->dock, &QWidget::show);
            } else {
                style_dock(current->dock, WorkbenchIcon::views);
                auto *action = current->dock->toggleViewAction();
                aux_action_ = action;
                action->setIcon(workbench_icon(WorkbenchIcon::views));
                action->setText(QStringLiteral("辅助视图"));
                action->setShortcut(QKeySequence("Ctrl+2"));
                view_menu_->addAction(action);
                auto *panel = new QWidget;
                auto *layout = new QVBoxLayout(panel);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->setSpacing(0);
                layout->addWidget(make_view_controls(1));
                layout->addWidget(current->container, 1);
                current->dock->setWidget(panel);
                connect(current->dock, &QDockWidget::visibilityChanged, this,
                        [this](bool) { fit_pending_ = true; });
            }
        } else {
            if (benchmarking_) {
                setCentralWidget(current->container);
            } else {
                auto *panel = new QWidget;
                auto *layout = new QVBoxLayout(panel);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->setSpacing(0);
                layout->addWidget(make_view_controls(0));
                layout->addWidget(current->container, 1);
                view_panel_ = panel;
                workspace_ = new Workspace;
                workspace_->add_view(panel, QStringLiteral("三维视图"),
                                     workbench_icon(WorkbenchIcon::earth));
                workspace_pages_ = new QStackedWidget;
                workspace_pages_->addWidget(workspace_);
                auto *welcome = make_welcome_page(new_action_, open_action_, example_action_);
                workspace_pages_->addWidget(welcome);
                workspace_pages_->setCurrentWidget(welcome);
                setCentralWidget(workspace_pages_);
            }
        }
        constexpr double radius = 6378137.0;
        // Two independent cameras that both face the fixed demo's visible hemisphere.
        const double angle =
            std::atan2(initial_position_[1], initial_position_[0]) + (secondary ? 0.35 : -0.35);
        const vsg::dvec3 look(6 * radius * std::cos(angle), 6 * radius * std::sin(angle),
                              2.6 * radius);
        current->camera =
            vsg::Camera::create(vsg::Perspective::create(35.0, 1.0, radius * 0.001, radius * 20),
                                vsg::LookAt::create(look, vsg::dvec3(0, 0, 0), vsg::dvec3(0, 0, 1)),
                                vsg::ViewportState::create(0, 0, 640, 640));
        slots_.push_back(std::move(slot));
        current->window->attached = [this, current] { attach(*current); };
        current->window->detaching = [this, current] { detach(*current); };
        current->window->failed = [this](const std::string &message) { fail(message); };
        current->window->synchronize = [this](const std::function<void()> &operation) {
            run_exclusive(operation);
        };
        current->window->pressed = [this, current](QPointF point) {
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            if (run_ && !injecting_input_)
                return;
#endif
            set_selection(
                render_vsg::pick_marker(current->pick_candidates, point.x(), point.y(), 8));
        };
        current->window->initializeWindow();
    }
    void attach(Slot &slot) {
        begin_change();
        auto adapter = slot.window->windowAdapter;
        viewer_->addWindow(adapter);
        if (validation_)
            log_.install(adapter->getOrCreateInstance());
        // Validation belongs to the instance. Vulkan no longer permits device layers.
        adapter->traits()->requestedLayers.clear();
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        if (run_) {
            const auto capabilities = present_timing_capabilities(*adapter);
            timing_capabilities_.append(capabilities);
            if (!capabilities["present_id2_feature"].toBool() ||
                !capabilities["present_timing_feature"].toBool() ||
                !capabilities["present_id2_surface"].toBool() ||
                !capabilities["present_timing_surface"].toBool() ||
                !capabilities["first_pixel_out"].toBool())
                throw std::runtime_error("This window cannot provide first-pixel-output timing");
        }
#endif
        // Realize the shared device before preparing the second window's traits.
        auto device = adapter->getOrCreateDevice();
        if (shared_device_ref_ && shared_device_ref_ != device)
            throw std::runtime_error("A recreated view changed the shared Vulkan device");
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        if (run_ && !shared_device_ref_)
            run_->configure_video_memory(*device->getPhysicalDevice());
#endif
        shared_device_ref_ = device;
        slot.window->traits->device = device;
        if (!smoke_dir_.isEmpty())
            slot.capture = render_vsg::SwapchainCapture::create(adapter);
        const auto extent = adapter->extent2D();
        slot.camera->viewportState = vsg::ViewportState::create(0, 0, extent.width, extent.height);
        auto *perspective = dynamic_cast<vsg::Perspective *>(slot.camera->projectionMatrix.get());
        perspective->aspectRatio = static_cast<double>(extent.width) / std::max(1u, extent.height);
        // Keep the logical view alive across surface changes: Rocky may still hold
        // its raw identity in a queued CompileResult until the next update.
        if (!slot.view) {
            slot.view = vsg::View::create(slot.camera, scene_);
            if (!benchmarking_) {
                slot.station_markers = std::make_unique<render_vsg::MarkerLayer>(
                    10000, render_vsg::MarkerStyle::ground_station);
                slot.station_selection = std::make_unique<render_vsg::MarkerLayer>(
                    1, render_vsg::MarkerStyle::ground_selection);
                slot.view->addChild(slot.station_markers->node());
                slot.view->addChild(slot.station_selection->node());
            }
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            if (benchmark_) {
                slot.labels = std::make_unique<render_vsg::LabelLayer>(200, benchmark_->font);
                slot.view->addChild(slot.labels->node());
            }
#endif
        }
        globe_->prepare_view(*slot.view);
        auto render_graph = vsg::RenderGraph::create(adapter, slot.view);
        slot.render_graph = render_graph;
        render_graph->setClearValues(benchmarking_
                                         ? VkClearColorValue{{0.018f, 0.025f, 0.045f, 1.0f}}
                                         : VkClearColorValue{{0.0003f, 0.0006f, 0.0012f, 1.0f}});
        slot.graph = vsg::CommandGraph::create(adapter);
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        if (run_) {
            if (slot.gpu)
                throw std::runtime_error("A GPU-timed view was recreated; this run is invalid");
            const auto position =
                std::find_if(slots_.begin(), slots_.end(),
                             [&](const auto &value) { return value.get() == &slot; });
            slot.gpu = std::make_shared<BenchmarkGpu>(
                device, static_cast<std::uint32_t>(slot.graph->queueFamily),
                static_cast<int>(std::distance(slots_.begin(), position)), *run_);
            gpu_metadata_.append(slot.gpu->metadata());
            slot.graph->addChild(slot.gpu->command(true));
        }
#endif
        slot.graph->addChild(render_graph);
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        if (slot.gpu)
            slot.graph->addChild(slot.gpu->command(false));
#endif
        if (slot.capture)
            slot.graph->addChild(slot.capture);
        if (!benchmarking_) {
            slot.trackball = vsg::Trackball::create(slot.camera);
            slot.trackball->addWindow(adapter);
            viewer_->addEventHandler(slot.trackball);
        }
        ++slot.creations;
        if (timer_.isActive())
            rebuild();
    }
    void detach(Slot &slot) {
        if (!slot.graph || stopped_)
            return;
        begin_change();
        auto &handlers = viewer_->getEventHandlers();
        handlers.remove(slot.trackball);
        globe_->release_view(*slot.view);
        slot.trackball = {};
        slot.graph = {};
        slot.render_graph = {};
        slot.pick_candidates.clear();
        ++slot.destructions;
        rebuild();
    }
    void begin_change() {
        if (compiler_)
            compiler_->begin_change(*viewer_);
        viewer_->deviceWaitIdle();
    }
    void publish_static_node(vsg::ref_ptr<vsg::Node> node, const std::function<void()> &publish) {
        // Track buffers are static. Compile only the replacement, retaining the current
        // render tasks, swapchains and all other satellites. An idle barrier protects
        // resources referenced by previous submissions before the old node is released.
        if (compiler_ && !viewer_->recordAndSubmitTasks.empty()) {
            const auto compiled = compiler_->compile(node);
            if (!compiled)
                throw std::runtime_error("Track compilation failed: " + compiled.message);
            run_exclusive([&] {
                viewer_->deviceWaitIdle();
                if (compiled.requiresViewerUpdate(viewer_.get()))
                    vsg::updateViewer(*viewer_, compiled);
                publish();
            });
        } else {
            begin_change();
            publish();
            rebuild();
        }
    }
    void run_exclusive(const std::function<void()> &operation) {
        if (compiler_)
            compiler_->run_exclusive(operation);
        else
            operation();
    }
    void rebuild() {
        begin_change();
        vsg::CommandGraphs graphs;
        for (const auto &slot : slots_)
            if (slot->graph)
                graphs.push_back(slot->graph);
        viewer_->assignRecordAndSubmitTaskAndPresentation(graphs);
        for (const auto &task : viewer_->recordAndSubmitTasks) {
            // A new upload must wait before COPY, rather than at VSG's default BOTTOM_OF_PIPE.
            task->earlyTransferConsumerCompletedSemaphore->pipelineStageFlags() =
                VK_PIPELINE_STAGE_TRANSFER_BIT;
            task->lateTransferConsumerCompletedSemaphore->pipelineStageFlags() =
                VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        if (!graphs.empty()) {
            if (!compiler_) {
                compiler_ = render_vsg::CompileManager::create(*viewer_, hints_);
                viewer_->compileManager = compiler_;
                compiler_->begin_change(*viewer_);
            }
            const auto result = viewer_->compile(hints_);
            if (!result)
                throw std::runtime_error("VSG scene compilation failed: " + result.message);
            compiler_->end_change(*viewer_);
        }
    }
    bool profile_step() {
        if (!profile_)
            return true;
        if (!profile_started_) {
            profile_started_ = true;
            const auto time = runtime::make_builtin_time_provider();
            const auto first = time->from_utc({2026, 10, 9, 0, 0, 0, 0});
            const auto last = time->from_utc({2026, 10, 10, 0, 0, 0, 0});
            activate_scenario(application::Scenario("Interaction profile", first, last));
            cancellation_ = std::make_shared<std::atomic_bool>(false);
            profile_orbits_ = std::async(std::launch::async, [this, first, last,
                                                              cancel = cancellation_] {
                std::vector<std::shared_ptr<const OrbitScene>> orbits;
                for (int i = 0; i < profile_count_; ++i) {
                    OrbitInput input;
                    input.first = input.initial.epoch = first;
                    input.last = last;
                    input.object_id = "Profile-" + std::to_string(i + 1);
                    const auto angle = 6.283185307179586 * i / profile_count_;
                    input.initial.position_m = {7e6 * std::cos(angle), 7e6 * std::sin(angle), 0};
                    input.initial.velocity_mps = {-6200 * std::sin(angle), 6200 * std::cos(angle),
                                                  5300};
                    auto session = make_orbit_session(input, 1);
                    orbits.push_back(
                        prepare_orbit_scene(input, std::move(session), *frames_, *eop_, *cancel));
                }
                return orbits;
            });
        }
        if (profile_orbits_.valid()) {
            if (profile_orbits_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return true;
            auto orbits = profile_orbits_.get();
            begin_change();
            for (const auto &orbit : orbits) {
                Satellite satellite;
                satellite.orbit = orbit;
                satellite.display.extent = 1;
                satellite.fixed_track =
                    std::make_shared<render_vsg::TrackLayer>(orbit->track.nodes());
                satellite.inertial_track =
                    std::make_shared<render_vsg::TrackLayer>(orbit->inertial_track.nodes());
                satellite.tracks = vsg::Switch::create();
                satellite.tracks->addChild(true, satellite.inertial_track->node());
                satellite.tracks->addChild(true, satellite.fixed_track->node());
                satellite.item = new QTreeWidgetItem(
                    scenario_item_, {QString::fromStdString(orbit->input.object_id)});
                orbit_group_->addChild(satellite.tracks);
                model_group_->addChild(satellite.model_host);
                scenario_ = scenario_->with_object(orbit->session);
                satellites_.push_back(std::move(satellite));
            }
            rebuild();
            ground_map_->set_orbits(orbits);
            orbit_ = orbits.front();
            satellite_state_dirty_ = true;
            fit_pending_ = true;
        }
        if (satellites_.empty())
            return true;
        static const QStringList phases{"3d-paused",  "3d-drag", "2d-paused",  "2d-hover",
                                        "2d-pan",     "2d-play", "split-play", "2d-zoom",
                                        "2d-settled", "3d-zoom"};
        constexpr int warmup = 45, measured = 180, length = warmup + measured;
        const int phase = profile_frame_ / length, local = profile_frame_ % length;
        if (phase == phases.size()) {
            profile_->finish(QJsonObject{
                {"satellites", profile_count_},
                {"track_nodes_per_satellite", 1441},
                {"step_seconds", 60},
                {"duration_seconds", 86400},
                {"gpu", QString::fromUtf8(slots_[0]
                                              ->window->windowAdapter->getPhysicalDevice()
                                              ->getProperties()
                                              .deviceName)},
                {"map_width", ground_map_->width()},
                {"map_height", ground_map_->height()},
                {"device_pixel_ratio", devicePixelRatioF()},
                {"map_quality_restored_after_zoom", true},
                {"scope",
                 "GUI CPU and timer cadence; synthetic Qt input, not input-to-photon latency"}});
            finished_ = true;
            timer_.stop();
            app_.exit(0);
            return false;
        }
        profile_->phase(phases[phase], local >= warmup);
        profile_->tick();
        const auto mouse = [](QObject *target, QEvent::Type type, QPointF p, Qt::MouseButton button,
                              Qt::MouseButtons buttons) {
            QMouseEvent event(type, p, p, button, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(target, &event);
        };
        if (local == 0) {
            std::cerr << "Profile phase: " << phases[phase].toStdString() << '\n';
            set_playing(phase == 5 || phase == 6);
            if (phase < 2)
                workspace_->select_view(0);
            else if (phase < 6)
                workspace_->select_view(1);
            else if (phase == 6)
                workspace_->split_page(map_panel_, view_panel_, Qt::RightEdge);
            else if (phase != 8) {
                workspace_->merge_page(map_panel_, view_panel_);
                workspace_->select_view(phase == 7 ? 1 : 0);
            }
            if (phase == 1)
                mouse(slots_[0]->window, QEvent::MouseButtonPress, QPointF(300, 250),
                      Qt::LeftButton, Qt::LeftButton);
            if (phase == 4) {
                ground_map_->zoom(2, ground_map_->rect().center());
                mouse(ground_map_, QEvent::MouseButtonPress, QPointF(300, 250), Qt::LeftButton,
                      Qt::LeftButton);
            }
            if (phase == 5)
                ground_map_->reset_view();
        }
        if (phase == 1 || phase == 3 || phase == 4) {
            const QPointF point(300 + 75 * std::sin(local * 0.045),
                                250 + 45 * std::cos(local * 0.045));
            mouse(phase == 1 ? static_cast<QObject *>(slots_[0]->window) : ground_map_,
                  QEvent::MouseMove, point, Qt::NoButton,
                  phase == 3 ? Qt::NoButton : Qt::LeftButton);
            if (local == length - 1 && phase != 3)
                mouse(phase == 1 ? static_cast<QObject *>(slots_[0]->window) : ground_map_,
                      QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
        }
        if ((phase == 7 || phase == 9) && local % 3 == 0) {
            QWheelEvent wheel(QPointF(300, 250), QPointF(300, 250), {},
                              QPoint(0, local % 60 < 30 ? 120 : -120), Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(
                phase == 7 ? static_cast<QObject *>(ground_map_) : slots_[0]->window, &wheel);
            if (phase == 7 && local % 30 == 0)
                set_selection(static_cast<std::size_t>(local / 30) % satellites_.size());
        }
        // The final idle phase must restore exact-scale quality after the zoom burst.
        // Capture outside a measured interval; the next phase resets the cadence clock.
        if (phase == 8 && local == length - 1) {
            if (!ground_map_->tracks_ready())
                throw std::runtime_error("Map quality did not settle after the zoom profile");
            profile_->capture("2d-settled", ground_map_->grab().toImage());
        }
        ++profile_frame_;
        return true;
    }
    void frame() {
        if (!profile_step())
            return;
        const ProfileScope profile("frame_cpu_ms");
        poll_orbit();
        poll_model();
        poll_project();
        poll_display_cache();
        if (!benchmarking_ && !smoke_dir_.isEmpty())
            scenario_smoke_step();
        if (!benchmarking_ && !scenario_)
            return;
        if (!benchmarking_ && !smoke_dir_.isEmpty() && smoke_bootstrap_ == 3 &&
            frame_count_ == 20) {
            save_workbench_image("workbench-default.png");
            slots_[1]->dock->show();
            open_properties();
        }
        if (!benchmarking_ && !smoke_dir_.isEmpty() && properties_dialog_ &&
            smoke_bootstrap_ == 3) {
            if (frame_count_ == 21 &&
                !properties_dialog_->grab().save(smoke_dir_ + "/object-properties.png"))
                throw std::runtime_error("Cannot save the object properties panel");
            if (frame_count_ == 22)
                properties_dialog_->close();
        }
        if (!smoke_dir_.isEmpty() && !benchmarking_ && editor_)
            orbit_smoke_step();
        if (benchmarking_ && viewer_->windows().empty())
            return;
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        const auto frame_begin = run_ ? BenchmarkRun::qpc() : 0;
        const auto phase = run_ ? run_->phase() : BenchmarkRun::Phase::waiting;
        if (run_) {
            if (phase == BenchmarkRun::Phase::drain)
                run_->begin_drain();
            if (timing_presentation_ &&
                (phase != BenchmarkRun::Phase::drain || !run_->drain_expired()))
                timing_presentation_->poll(*run_);
            if (phase == BenchmarkRun::Phase::drain) {
                const bool complete =
                    timing_presentation_ && timing_presentation_->pending_count() == 0;
                if (complete || run_->drain_expired()) {
                    run_->end_drain(complete);
                    finished_ = true;
                    timer_.stop();
                    app_.exit(0);
                }
                // No more images are acquired or submitted while PE feedback drains.
                return;
            }
            if (phase == BenchmarkRun::Phase::inputs)
                run_->start_inputs(this);
            else if (phase != BenchmarkRun::Phase::drain) {
                seconds_ = run_->animation_seconds();
                slider_->blockSignals(true);
                slider_->setValue(static_cast<int>(seconds_));
                slider_->blockSignals(false);
            }
        }
#endif
        if (!requested_time_ && (playing_ || (benchmarking_ && !smoke_dir_.isEmpty()))) {
            seconds_ =
                !smoke_dir_.isEmpty()
                    ? std::fmod(static_cast<double>(frame_count_), duration_seconds())
                    : std::fmod(seconds_ + static_cast<double>(clock_.restart()) *
                                               (benchmarking_ ? 0.01 : playback_rate_ * 0.001),
                                duration_seconds());
            slider_->blockSignals(true);
            slider_->setValue(benchmarking_ ? static_cast<int>(seconds_)
                                            : qRound(10000 * seconds_ / duration_seconds()));
            slider_->blockSignals(false);
        }
        requested_time_ = false;
        const core::Epoch epoch =
            !benchmarking_ && !playing_ && precise_epoch_
                ? *precise_epoch_
                : core::Epoch((benchmarking_ ? 0 : scenario_->first().tai_nanoseconds()) +
                              static_cast<std::int64_t>(std::llround(seconds_ * 1e9)));
        if (track_pixel_ratio_ != devicePixelRatioF()) {
            track_pixel_ratio_ = devicePixelRatioF();
            satellite_state_dirty_ = true;
        }
        if (benchmarking_ || satellite_state_dirty_ || snapshot_.epoch != epoch) {
            snapshot_ = sample_frame(epoch);
            update_satellite();
        }
        request_display_cache();
        if (time_label_ && (time_label_epoch_ != epoch || time_label_scale_ != display_scale_)) {
            time_label_->setText(calendar_text(snapshot_.epoch, display_scale_));
            time_label_epoch_ = epoch;
            time_label_scale_ = display_scale_;
        }
        if (timeline_ && timeline_->current_time() != epoch.tai_nanoseconds() / 1'000'000)
            timeline_->set_current_time(snapshot_.epoch.tai_nanoseconds() / 1'000'000);
        visualization::RenderSnapshot selected_snapshot{
            snapshot_.epoch, snapshot_.frame, snapshot_.origin, {}};
        if (selected_)
            selected_snapshot.markers.push_back(snapshot_.markers.at(*selected_));
        selection_->update(selected_snapshot);
        // The shared scene clock and 2D snapshot continue when no native view is exposed.
        if (!benchmarking_ && std::none_of(slots_.begin(), slots_.end(), [](const auto &slot) {
                return slot->container->isVisible() && slot->window->isExposed();
            })) {
            ++frame_count_;
            return;
        }
        bool active = false;
        // Image acquisition can recreate an out-of-date swapchain and wait for the device.
        {
            const ProfileScope acquire("acquire_ms");
            run_exclusive([&] { active = viewer_->advanceToNextFrame(seconds_); });
        }
        if (!active)
            throw std::runtime_error("VSG viewer became inactive");
        viewer_->handleEvents();
        // RenderGraph normally adjusts the camera during recording, which is
        // too late for this frame's CPU label/pick projection. Resize once here.
        for (const auto &slot : slots_) {
            if (!slot->render_graph)
                continue;
            const auto extent = slot->render_graph->getExtent();
            const auto previous = slot->render_graph->previous_extent;
            if (extent.width != previous.width || extent.height != previous.height)
                run_exclusive([&] { slot->render_graph->resized(); });
        }
        if (!benchmarking_ && (frame_count_ == 0 || fit_pending_)) {
            fit_views();
            fit_pending_ = false;
        }
        viewer_->update();
        for (const auto &slot : slots_) {
            if (!slot->graph)
                continue;
            const auto extent = slot->window->windowAdapter->extent2D();
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            if (run_ && (extent.width != 1280 || extent.height != 720 ||
                         !slot->window->isExposed() || !slot->container->isVisible()))
                throw std::runtime_error("Timing requires two exposed 1280x720 views");
#endif
            slot->pick_candidates = render_vsg::visible_markers(
                snapshot_, *slot->camera, extent.width, extent.height, 0,
                snapshot_.frame == core::FrameId::gcrf ? &*display_to_itrf_ : nullptr);
            if (slot->station_markers) {
                visualization::RenderSnapshot visible_stations{
                    snapshot_.epoch, snapshot_.frame, snapshot_.origin, {}};
                auto selected_station = visible_stations;
                for (const auto &point : slot->pick_candidates) {
                    if (point.index < satellites_.size())
                        continue;
                    const auto &marker = snapshot_.markers.at(point.index);
                    visible_stations.markers.push_back(marker);
                    if (selected_marker_index() == point.index)
                        selected_station.markers.push_back(marker);
                }
                slot->station_markers->update(visible_stations);
                slot->station_selection->update(selected_station);
            }
            if (slot->labels) {
                const auto eligible = render_vsg::visible_markers(snapshot_, *slot->camera,
                                                                  extent.width, extent.height, 80);
                slot->labels->update(snapshot_, eligible, *slot->camera, extent.height);
                if (slot->labels->size() != 200)
                    throw std::runtime_error(
                        "The fixed benchmark requires 200 eligible labels per view");
            }
        }
        const bool capture =
            !smoke_dir_.isEmpty() &&
            (frame_count_ == 19 || frame_count_ == 189 ||
             (benchmarking_ &&
              (frame_count_ == 30 || frame_count_ == 160 || frame_count_ == 180)) ||
             (!benchmarking_ &&
              (smoke_bootstrap_ == 1 || orbit_capture_ ||
               (orbit_smoke_stage_ == orbit_smoke_complete && frame_count_ >= 189))));
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        const auto update_qpc = run_ ? BenchmarkRun::qpc() : 0;
#endif
        if (capture)
            for (const auto &slot : slots_)
                if (slot->capture && slot->window->isExposed())
                    slot->capture->request();
        // VSG's convenience wrappers discard VkResult. This GUI-thread loop checks them.
        for (const auto &task : viewer_->recordAndSubmitTasks)
            for (const auto &graph : task->commandGraphs)
                graph->reset();
        for (const auto &task : viewer_->recordAndSubmitTasks) {
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            if (run_)
                for (const auto &slot : slots_)
                    if (std::find(task->commandGraphs.begin(), task->commandGraphs.end(),
                                  slot->graph) != task->commandGraphs.end())
                        slot->gpu->prepare(frame_count_, *task);
#endif
            const auto result =
                task->submit(vsg::ref_ptr<vsg::FrameStamp>(viewer_->getFrameStamp()));
            if (result != VK_SUCCESS)
                throw std::runtime_error("Vulkan submission failed: " + std::to_string(result));
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            if (run_)
                for (const auto &slot : slots_)
                    if (std::find(task->commandGraphs.begin(), task->commandGraphs.end(),
                                  slot->graph) != task->commandGraphs.end())
                        slot->gpu->submitted();
#endif
            if (capture)
                for (const auto &slot : slots_)
                    if (slot->window->isExposed() &&
                        std::find(task->commandGraphs.begin(), task->commandGraphs.end(),
                                  slot->graph) != task->commandGraphs.end()) {
                        const auto pixels = slot->capture->read_after_submit(*task->fence());
                        slot->captured_image =
                            QImage(pixels.rgba.data(), static_cast<int>(pixels.width),
                                   static_cast<int>(pixels.height), QImage::Format_RGBA8888)
                                .copy();
                    }
        }
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        const auto submit_qpc = run_ ? BenchmarkRun::qpc() : 0;
#endif
        for (const auto &presentation : viewer_->presentations) {
            VkResult result = VK_SUCCESS;
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
            if (run_) {
                if (viewer_->presentations.size() != 1 || presentation->windows.size() != 2)
                    throw std::runtime_error(
                        "Timing expects one shared-device dual-view presentation batch");
                if (!timing_presentation_) {
                    timing_presentation_ = std::make_unique<BenchmarkPresentation>(
                        std::array<vsg::ref_ptr<vsg::Window>, 2>{slots_[0]->window->windowAdapter,
                                                                 slots_[1]->window->windowAdapter},
                        run_->output_only_probe());
                    timing_metadata_ = timing_presentation_->metadata();
                }
                result = timing_presentation_->present(*presentation, frame_count_, *run_);
                if (result != VK_SUCCESS)
                    throw std::runtime_error(
                        "A timing swapchain changed or failed; this run is invalid");
            } else
#endif
            {
                result = presentation->present();
            }
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR &&
                result != VK_ERROR_OUT_OF_DATE_KHR)
                throw std::runtime_error("Vulkan presentation failed: " + std::to_string(result));
            if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
                for (auto &slot : slots_) {
                    const auto window = slot->window->windowAdapter;
                    if (window && window->visible() && window->imageIndex() < window->numFrames() &&
                        std::find(presentation->windows.begin(), presentation->windows.end(),
                                  window) != presentation->windows.end())
                        ++slot->presentations;
                }
            }
            if (result != VK_SUCCESS)
                run_exclusive([&] {
                    for (const auto &window : presentation->windows)
                        window->resize();
                });
        }
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
        if (run_) {
            run_->frame({frame_count_, frame_begin, update_qpc, submit_qpc, BenchmarkRun::qpc(),
                         run_->active_input_id(), epoch.tai_nanoseconds(),
                         selected_ ? static_cast<std::int64_t>(*selected_) : -1, phase});
            if (!timing_ready_ && frame_count_ >= 5) {
                QJsonArray views;
                for (std::size_t i = 0; i < slots_.size(); ++i) {
                    const auto &slot = slots_[i];
                    const auto device = slot->window->windowAdapter->getPhysicalDevice();
                    views.append(
                        QJsonObject{{"view", static_cast<int>(i)},
                                    {"width_pixels", 1280},
                                    {"height_pixels", 720},
                                    {"native_window", QString::number(slot->window->winId(), 16)},
                                    { "gpu",
                                      QString::fromUtf8(device->getProperties().deviceName) }});
                }
                run_->ready(QJsonObject{{"views", views},
                                        {"present_timing_capabilities", timing_capabilities_},
                                        {"presentation_timing", timing_metadata_},
                                        {"gpu_timing",
                                         QJsonObject {
                                             { "views",
                                               gpu_metadata_ }
                                         }},
                                        { "benchmark",
                                          benchmark_->report }});
                timing_ready_ = true;
            }
        }
#endif
        ++frame_count_;
        statusBar()->showMessage(benchmarking_ ? QStringLiteral("ITRF · 固定双视图基准场景")
                                 : satellites_.empty() && stations_.empty()
                                     ? QStringLiteral("空场景  ·  使用“插入对象”添加第一个对象")
                                     : QStringLiteral("就绪  ·  %1")
                                           .arg(selected_marker_index()
                                                    ? selected_name()
                                                    : QStringLiteral("未选择对象")));
        if (!smoke_dir_.isEmpty() && (benchmarking_ || smoke_bootstrap_ == 3)) {
            if (frame_count_ == 20) {
                for (std::size_t i = 0; i < slots_.size(); ++i)
                    if (slots_[i]->window->isExposed() &&
                        !slots_[i]->captured_image.save(smoke_dir_ +
                                                        QString("/view-%1-initial.png").arg(i)))
                        throw std::runtime_error("Cannot save initial swapchain image");
            }
            if (frame_count_ == 25)
                resize_to_screen(1320, 820);
            if (frame_count_ == 45)
                slots_[1]->dock->setFloating(true);
            if (frame_count_ == 65)
                slots_[1]->dock->setFloating(false);
            if (frame_count_ == 85)
                slots_[1]->dock->hide();
            if (frame_count_ == 100)
                slots_[1]->dock->show();
            if (frame_count_ == 115)
                recreate_window(*slots_[0]);
            if (frame_count_ == 145)
                recreate_window(*slots_[1]);
            if (frame_count_ >= 190 &&
                (benchmarking_ || !editor_ || orbit_smoke_stage_ == orbit_smoke_complete))
                finish_smoke();
            else if (benchmarking_)
                benchmark_smoke_step();
        }
    }
    void inject_click(Slot &slot, const QPointF &pixels) {
        const auto local = pixels / slot.window->devicePixelRatio();
        const auto global = slot.window->mapToGlobal(local);
        QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        injecting_input_ = true;
        QCoreApplication::sendEvent(slot.window, &press);
        QCoreApplication::sendEvent(slot.window, &release);
        injecting_input_ = false;
    }
    void smoke_insert_dialog(std::optional<InsertObjectMethod> method,
                             const QString &screenshot = {}, bool escape = false) {
        timer_.stop();
        QString error;
        bool handled = false;
        QTimer click_timer;
        click_timer.setSingleShot(true);
        connect(&click_timer, &QTimer::timeout, this, [&] {
            handled = true;
            auto *dialog = findChild<QDialog *>("insertObjectsDialog");
            if (!dialog) {
                error = "Insert objects command did not open the chooser";
                return;
            }
            auto *types = dialog->findChild<QTreeWidget *>("insertObjectTypes");
            auto *properties = dialog->findChild<QRadioButton *>("insertDefineProperties");
            auto *defaults = dialog->findChild<QRadioButton *>("insertDefaults");
            auto *insert = dialog->findChild<QPushButton *>("insertObject");
            if (!types || !types->currentItem() || !properties || !defaults || !insert ||
                !properties->isChecked() ||
                types->currentItem()->data(0, Qt::UserRole).toInt() !=
                    static_cast<int>(InsertObjectType::satellite)) {
                error = "Insert objects chooser has no valid initial selection";
                dialog->reject();
                return;
            }
            if (method == InsertObjectMethod::defaults)
                defaults->click();
            if (!screenshot.isEmpty() && !dialog->grab().save(smoke_dir_ + "/" + screenshot))
                error = "Cannot save insert objects chooser";
            if (method) {
                insert->click();
            } else if (escape) {
                QKeyEvent key(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QCoreApplication::sendEvent(dialog, &key);
            } else {
                dialog->findChild<QPushButton *>("closeInsertObjects")->click();
            }
        });
        click_timer.start(0);
        insert_action_->trigger();
        timer_.start();
        if (!handled)
            throw std::runtime_error("Insert objects command did not enter the chooser");
        if (!error.isEmpty())
            throw std::runtime_error(error.toStdString());
    }
    void scenario_smoke_step() {
        const auto require = [](bool condition, const char *message) {
            if (!condition)
                throw std::runtime_error(message);
        };
        const auto record = [&](const char *kind) {
            interaction_report_.append(QJsonObject{{"kind", kind}, {"passed", true}});
        };
        if (smoke_bootstrap_ == 0) {
            require(!scenario_ && satellites_.empty() && !insert_action_->isEnabled() &&
                        !time_toolbar_->isEnabled(),
                    "Startup must require a scenario before inserting objects");
            require(grab().save(smoke_dir_ + "/welcome.png"), "Cannot save welcome screenshot");
            record("startup-without-scenario");
            EpochEditor epoch_control;
            epoch_control.set_epoch(core::Epoch{1});
            for (auto scale : {CalendarScale::tai, CalendarScale::tt, CalendarScale::utc}) {
                epoch_control.set_scale(scale);
                require(epoch_control.epoch() == core::Epoch{1},
                        "Time scale selection changed the physical epoch");
            }
            const auto clock_provider = runtime::make_builtin_time_provider();
            const auto leap = clock_provider->from_utc({2016, 12, 31, 23, 59, 60, 0});
            epoch_control.set_epoch(core::Epoch{leap.tai_nanoseconds() - 1'000'000'000});
            epoch_control.adjust_seconds(1);
            require(epoch_control.epoch() == leap &&
                        epoch_control.findChild<QLineEdit *>("epochValue")->text() ==
                            "2016-12-31 23:59:60 UTC",
                    "UTC shortcut skipped the leap second");
            epoch_control.findChild<QLineEdit *>("epochValue")->setText("2016-12-31 23:59:59 UTC");
            epoch_control.findChild<QLineEdit *>("epochValue")->setText("2016-12-31 23:59:60 UTC");
            require(epoch_control.epoch() == leap, "UTC leap-second calendar input lost its epoch");
            epoch_control.set_scale(CalendarScale::tt);
            require(epoch_control.findChild<QLineEdit *>("epochValue")->text() ==
                        "2017-01-01 00:01:08.184 TT",
                    "TT calendar offset is incorrect at the leap second");
            epoch_control.set_scale(CalendarScale::utc);
            epoch_control.findChild<QLineEdit *>("epochValue")->setText("2016-12-30 23:59:60 UTC");
            bool rejected_leap = false;
            try {
                (void)epoch_control.epoch();
            } catch (const std::exception &) {
                rejected_leap = true;
            }
            require(rejected_leap, "Invalid UTC second 60 was accepted by the editor");
            epoch_control.findChild<QLineEdit *>("epochValue")
                ->setText("2000-01-01 12:00:32.184000001 TT");
            require(epoch_control.epoch() == core::Epoch{1},
                    "Pasted unified TT epoch lost nanoseconds");
            epoch_control.set_epoch(epoch_control.epoch());
            require(epoch_control.findChild<QLineEdit *>("epochValue")->text() ==
                        "2000-01-01 12:00:32.184000001 TT",
                    "Rebinding a pasted epoch lost its explicit time scale");
            epoch_control.open_picker();
            auto *picker = epoch_control.findChild<QFrame *>("epochPopup");
            require(picker, "Unified epoch picker did not open");
            for (const int scale : {0, 1, 2})
                picker->findChild<QComboBox *>("pickerScale")->setCurrentIndex(scale);
            picker->findChild<QPushButton *>("applyOrbit")->click();
            require(epoch_control.epoch() == core::Epoch{1},
                    "Picker scale changes lost nanoseconds");
            EpochEditor tai_control;
            tai_control.findChild<QLineEdit *>("epochValue")
                ->setText("2250-01-01 00:00:00.000000001 TAI");
            tai_control.set_epoch(tai_control.epoch());
            tai_control.adjust_seconds(1);
            require(tai_control.findChild<QLineEdit *>("epochValue")->text() ==
                        "2250-01-01 00:00:01.000000001 TAI",
                    "Pasted TAI binding/adjustment incorrectly required UTC coverage");
            record("time-scales-calendar-and-leap-shortcuts");
            // Exercise the same modal creation path as Ctrl+N without re-entering frame().
            timer_.stop();
            QTimer::singleShot(0, this, [this] {
                auto *dialog = findChild<QDialog *>("newScenarioDialog");
                if (dialog)
                    dialog->reject();
                else
                    fail("Scenario creation dialog did not open");
            });
            create_scenario();
            require(!scenario_, "Cancelling new scenario created one");
            record("cancel-new-scenario");
            QTimer::singleShot(0, this, [this] {
                auto *dialog = findChild<QDialog *>("newScenarioDialog");
                if (!dialog) {
                    fail("Scenario creation dialog did not open");
                    return;
                }
                auto *start =
                    dynamic_cast<EpochEditor *>(dialog->findChild<QWidget *>("scenarioStart"));
                auto *end =
                    dynamic_cast<EpochEditor *>(dialog->findChild<QWidget *>("scenarioEnd"));
                const auto time = runtime::make_builtin_time_provider();
                const auto today = QDateTime::currentDateTimeUtc().date();
                const auto next_day = today.addDays(1);
                if (start->epoch() !=
                        time->from_utc({today.year(), today.month(), today.day(), 0, 0, 0, 0}) ||
                    end->epoch() != time->from_utc({next_day.year(), next_day.month(),
                                                    next_day.day(), 0, 0, 0, 0})) {
                    fail("Scenario defaults must be UTC today's midnight through tomorrow's "
                         "midnight");
                    dialog->reject();
                    return;
                }
                if (!dialog->grab().save(smoke_dir_ + "/new-scenario-default.png"))
                    fail("Cannot save default scenario panel");
                start->open_picker();
                if (auto *popup = start->findChild<QFrame *>("epochPopup")) {
                    if (!popup->grab().save(smoke_dir_ + "/epoch-picker.png"))
                        fail("Cannot save date picker");
                    popup->close();
                } else
                    fail("Calendar did not open");
                interaction_report_.append(QJsonObject{
                    {"kind", "utc-today-one-day-default-and-popovers"}, {"passed", true}});
                dynamic_cast<EpochEditor *>(dialog->findChild<QWidget *>("scenarioStart"))
                    ->set_epoch(core::Epoch{-1'000'000'000'000});
                dynamic_cast<EpochEditor *>(dialog->findChild<QWidget *>("scenarioEnd"))
                    ->set_epoch(core::Epoch{6'600'000'000'000});
                if (!dialog->grab().save(smoke_dir_ + "/new-scenario.png"))
                    fail("Cannot save creation panel");
                dialog->findChild<QPushButton *>("applyOrbit")->click();
            });
            create_scenario();
            timer_.start();
            require(scenario_->objects().empty() && satellites_.empty() &&
                        insert_action_->isEnabled(),
                    "A new scenario must be empty and enable insertion");
            record("create-empty-scenario");
            smoke_bootstrap_ = 1;
        } else if (smoke_bootstrap_ == 1 && frame_count_ >= 3) {
            save_workbench_image("empty-scenario.png");
            smoke_insert_dialog(std::nullopt, "insert-objects.png");
            require(satellites_.empty() && scenario_->objects().empty() && !busy(),
                    "Closing the insertion chooser created an object");
            record("cancel-insert-object-chooser");
            smoke_insert_dialog(InsertObjectMethod::define_properties);
            require(workspace_->current_document() == properties_dialog_ && satellites_.empty(),
                    "Define properties must open an uncommitted workspace draft");
            record("insert-object-define-properties-in-workspace");
            auto *id = editor_->findChild<QLineEdit *>("objectId");
            require(id, "Missing insertion draft object ID");
            id->setText("UncommittedDraft");
            smoke_insert_dialog(std::nullopt, {}, true);
            require(id->text() == "UncommittedDraft" &&
                        workspace_->current_document() == properties_dialog_ && satellites_.empty(),
                    "Escape in the chooser changed the existing properties draft");
            record("escape-insert-chooser-preserves-properties-draft");
            properties_dialog_->close();
            require(satellites_.empty() && scenario_->objects().empty(),
                    "Closing an insertion draft created an object");
            record("close-insert-without-applying");
            smoke_insert_dialog(InsertObjectMethod::defaults, "insert-objects-defaults.png");
            require(workspace_->current_document() != properties_dialog_,
                    "Default insertion unnecessarily opened the properties page");
            smoke_bootstrap_ = 2;
        } else if (smoke_bootstrap_ == 2 && !busy()) {
            require(last_orbit_outcome_ == "applied" && satellites_.size() == 1 &&
                        scenario_->objects().size() == 1 && selected_ == 0,
                    "First inserted satellite was not published into the scenario");
            record("insert-first-satellite");
            require(orbit_->input.object_id == "Satellite1" &&
                        orbit_->input.initial.epoch == scenario_->first() &&
                        orbit_->input.first == scenario_->first() &&
                        orbit_->input.last == scenario_->last() &&
                        orbit_->input.step_seconds == 60 &&
                        orbit_->input.propagator == OrbitPropagator::two_body &&
                        orbit_->input.initial.position_m == OrbitInput{}.initial.position_m,
                    "Default insertion did not use the scenario and satellite defaults");
            record("insert-object-defaults-inherits-scenario");
            properties_dialog_->close();
            smoke_bootstrap_ = 3;
            frame_count_ = 0;
        }
    }
    void orbit_smoke_step() {
        if (orbit_smoke_step_active_ || smoke_bootstrap_ != 3 || frame_count_ < 24 || busy() ||
            orbit_smoke_stage_ == orbit_smoke_complete)
            return;
        // Menus and modal dialogs run nested event loops. A frame arriving there
        // must not execute this same validation step recursively.
        const QScopedValueRollback step_guard(orbit_smoke_step_active_, true);
        const auto require = [](bool condition, const char *message) {
            if (!condition)
                throw std::runtime_error(message);
        };
        const auto choose_report = [this, require](const QString &key) {
            auto *button = report_dialog_->findChild<QPushButton *>("reportLibrary");
            require(button != nullptr, "Report selection button is missing");
            bool selected = false;
            QTimer::singleShot(0, this, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto *list = dialog->findChild<QListWidget *>("reportSelectionList");
                auto *confirm = dialog->findChild<QPushButton *>("reportSelectionConfirm");
                if (list && confirm)
                    for (int i = 0; i < list->count(); ++i)
                        if (list->item(i)->data(Qt::UserRole).toString() == key) {
                            list->setCurrentRow(i);
                            if (confirm->isEnabled()) {
                                selected = true;
                                confirm->click();
                                return;
                            }
                        }
                dialog->reject();
            });
            timer_.stop();
            button->click();
            timer_.start();
            require(selected, "Report selection window did not accept the requested choice");
        };
        const auto record = [&](const char *kind) {
            interaction_report_.append(QJsonObject{
                {"kind", kind},
                {"passed", true},
                {"revision", static_cast<qint64>(
                                 orbit_ ? orbit_->track.samples().provenance().input_revision : 0)},
                {"rendered_tai_ns", QString::number(snapshot_.epoch.tai_nanoseconds())}});
        };
        const auto caches_ready = [&] {
            return std::all_of(satellites_.begin(), satellites_.end(), [&](const auto &satellite) {
                const auto wanted =
                    display_interval(*satellite.orbit, satellite.display, snapshot_.epoch);
                return cache_covers(*satellite.orbit, wanted.first, wanted.second);
            });
        };
        const auto action_in_workspace = [&](const char *name) {
            for (auto *button : editor_->findChildren<QPushButton *>(name))
                if (button->isVisible() &&
                    workspace_->rect().contains(button->mapTo(workspace_, button->rect().center())))
                    return true;
            return false;
        };
        const auto confirm_delete = [&](bool accepted) {
            QTimer::singleShot(0, [accepted] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                    box->button(accepted ? QMessageBox::Yes : QMessageBox::Cancel)->click();
            });
            delete_action_->trigger();
        };
        const auto answer_data = [require](bool accepted, const char *name = "dataChangeDialog") {
            QTimer::singleShot(0, [accepted, name, require] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                require(box && box->objectName() == name, "Missing data approval/coverage dialog");
                box->button(accepted ? QMessageBox::Yes : QMessageBox::Cancel)->click();
            });
        };
        const auto scenario_epoch_control = [this](const char *name) {
            return dynamic_cast<EpochEditor *>(scenario_properties_->findChild<QWidget *>(name));
        };
        auto input = orbit_ ? orbit_->input : OrbitInput{};
        switch (orbit_smoke_stage_) {
        case 0:
            // Start below the south pole to cover the former north-facing camera assumption.
            input.initial.position_m = {0, 0, -7e6};
            input.initial.velocity_mps = {6200, 5300, 0};
            editor_->set_input(input);
            editor_->set_state_mode(0);
            editor_->set_state_mode(1);
            require(editor_->findChild<QComboBox *>("stateRepresentation")->currentIndex() == 1,
                    "Classical element state conversion failed in the editor");
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        case 1:
            require(last_orbit_outcome_ == "applied" && selected_ == 0 &&
                        orbit_->input.object_id == "Satellite1" &&
                        orbit_->max_radius_m - orbit_->min_radius_m > 1e6,
                    "Edited inclined eccentric orbit was not published");
            record("apply-edited-initial-state");
            require(std::abs(orbit_->input.initial.position_m[0]) < 0.001 &&
                        std::abs(orbit_->input.initial.position_m[1]) < 0.001 &&
                        std::abs(orbit_->input.initial.position_m[2] + 7e6) < 0.001,
                    "Changing state representation changed the initial position beyond 1 mm");
            record("apply-keplerian-orbit");
            smoke_old_track_ = orbit_group_->children.front();
            slider_->setValue(10000);
            orbit_capture_ = true;
            ++orbit_smoke_stage_;
            break;
        case 2:
            require(snapshot_.epoch == orbit_->input.last, "Analysis end seek failed");
            record("seek-analysis-end");
            save_interaction_images("orbit-end");
            slider_->setValue(0);
            ++orbit_smoke_stage_;
            break;
        case 3:
            require(snapshot_.epoch == orbit_->input.first, "Analysis start seek failed");
            record("seek-analysis-start");
            save_interaction_images("orbit-start");
            orbit_capture_ = false;
            try {
                activate_scenario(application::Scenario("Invalid", core::Epoch{0},
                                                        core::Epoch{100'000'000'000'000}));
                throw std::runtime_error("Out-of-coverage scenario was accepted");
            } catch (const std::out_of_range &) {
                require(scenario_->name() == "Scenario1" &&
                            orbit_group_->children.front() == smoke_old_track_,
                        "Invalid scenario replaced the live scene");
            }
            record("reject-scenario-eop-coverage");
            input.initial.velocity_mps = {0, 0, 0};
            editor_->set_input(input);
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        case 4: {
            require(last_orbit_outcome_ == "rejected" &&
                        orbit_->track.samples().provenance().input_revision == 2 &&
                        orbit_group_->children.front() == smoke_old_track_,
                    "Invalid edit replaced the live scene");
            record("reject-invalid-initial-state");
            const auto error_message = editor_->findChild<QLabel *>("orbitResult")->text();
            set_selection(std::nullopt);
            require(editor_->findChild<QLabel *>("orbitResult")->text() == error_message,
                    "Changing selection overwrote the property panel's failed update result");
            set_selection(0);
            record("selection-preserves-property-error");
            input.step_seconds = 1;
            editor_->set_input(input);
            editor_->submit();
            cancellation_->store(true);
            orbit_smoke_stage_ = 6;
            break;
        }
        case 6:
            require(last_orbit_outcome_ == "cancelled" &&
                        orbit_->track.samples().provenance().input_revision == 2 &&
                        orbit_group_->children.front() == smoke_old_track_,
                    "Cancelled computation replaced the live scene");
            record("cancel-candidate");
            smoke_old_track_ = {};
            for (auto &slot : slots_) {
                slot->window->hide();
                slot->window->destroy();
            }
            require(viewer_->windows().empty(), "Views remained after destroying both windows");
            editor_->set_input(input);
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        case 7:
            require(last_orbit_outcome_ == "applied" && viewer_->windows().empty() &&
                        orbit_->track.samples().provenance().input_revision == 3,
                    "Orbit could not be applied with all native windows closed");
            record("apply-without-windows");
            for (auto &slot : slots_) {
                slot->window->initializeWindow();
                slot->window->show();
            }
            fit_pending_ = true;
            ++orbit_smoke_stage_;
            break;
        case 8:
            if (slots_[0]->pick_candidates.empty() || slots_[1]->pick_candidates.empty())
                return;
            inject_click(*slots_[0], {slots_[0]->pick_candidates.front().x_pixels,
                                      slots_[0]->pick_candidates.front().y_pixels});
            orbit_capture_ = true;
            ++orbit_smoke_stage_;
            break;
        case 9:
            require(selected_ == std::optional<std::size_t>{0} && selection_->size() == 1,
                    "First view could not select the edited satellite");
            record("select-edited-orbit-view0");
            save_interaction_images("orbit-selected-view0");
            inject_click(*slots_[0], {0, 0});
            ++orbit_smoke_stage_;
            break;
        case 10:
            require(!selected_ && selection_->size() == 0, "Edited orbit selection did not clear");
            require(!slots_[1]->pick_candidates.empty(), "Edited orbit disappeared from view 2");
            record("clear-edited-selection");
            inject_click(*slots_[1], {slots_[1]->pick_candidates.front().x_pixels,
                                      slots_[1]->pick_candidates.front().y_pixels});
            ++orbit_smoke_stage_;
            break;
        case 11:
            require(selected_ == std::optional<std::size_t>{0} && selection_->size() == 1,
                    "Second view could not select the edited satellite");
            record("select-edited-orbit-view1");
            save_interaction_images("orbit-selected-view1");
            orbit_capture_ = false;
            ++orbit_smoke_stage_;
            break;
        case 12:
            editor_->configure_appearance(1, {}, 1000);
            ++orbit_smoke_stage_;
            break;
        case 13:
            require(last_model_outcome_ == "applied" && model_kind_ == 1 && markers_->size() == 0,
                    "Built-in satellite model was not applied");
            record("built-in-satellite-model");
            focus_satellite();
            orbit_capture_ = true;
            ++orbit_smoke_stage_;
            break;
        case 14:
            save_interaction_images("satellite-model");
            editor_->configure_appearance(
                2, QCoreApplication::applicationDirPath() + "/resources/models/satellite_demo.gltf",
                1000);
            ++orbit_smoke_stage_;
            break;
        case 15:
            require(last_model_outcome_ == "applied" && model_kind_ == 2,
                    "Local glTF satellite model was not applied");
            record("local-gltf-satellite-model");
            ++orbit_smoke_stage_;
            break;
        case 16:
            save_interaction_images("satellite-gltf");
            editor_->configure_appearance(2, smoke_dir_ + "/absent-model.glb", 1000);
            ++orbit_smoke_stage_;
            break;
        case 17:
            require(last_model_outcome_ == "rejected" && model_kind_ == 2,
                    "Missing model replaced the existing satellite appearance");
            record("reject-missing-model");
            editor_->configure_appearance(0, {}, 1000);
            ++orbit_smoke_stage_;
            break;
        case 18:
            require(last_model_outcome_ == "applied" && model_kind_ == 0 && markers_->size() == 1,
                    "Point fallback was not restored");
            record("restore-point-marker");
            fit_views();
            orbit_capture_ = false;
            ++orbit_smoke_stage_;
            break;
        case 19:
            smoke_insert_dialog(InsertObjectMethod::define_properties);
            require(editor_->findChild<QLineEdit *>("objectId")->text() == "Satellite2",
                    "Insertion chooser did not generate a unique satellite ID");
            input.object_id = "Satellite1";
            editor_->set_input(input);
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        case 20:
            require(last_orbit_outcome_ == "rejected" && satellites_.size() == 1,
                    "Duplicate insertion changed the scene");
            record("reject-duplicate-object-id");
            input.object_id = "Satellite2";
            input.initial.position_m = {1e6, 0, -7e6};
            editor_->set_input(input);
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        case 21:
            require(
                last_orbit_outcome_ == "applied" && satellites_.size() == 2 &&
                    scenario_->objects().size() == 2 && selected_ == 1 &&
                    orbit_->session.provenance().input_revision == 3 && markers_->size() == 2,
                "Second insertion changed the first satellite or failed to select the new object");
            record("insert-second-satellite");
            input = satellites_[1].orbit->input;
            input.initial.velocity_mps[0] += 10;
            editor_->set_input(input);
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        case 22:
            require(last_orbit_outcome_ == "applied" &&
                        satellites_[1].orbit->session.provenance().input_revision == 2 &&
                        orbit_->session.provenance().input_revision == 3 && selected_ == 1,
                    "Editing satellite 2 affected satellite 1");
            record("independent-object-properties");
            editor_->configure_appearance(1, {}, 500);
            ++orbit_smoke_stage_;
            break;
        case 23:
            require(last_model_outcome_ == "applied" && satellites_[1].model_kind == 1 &&
                        satellites_[0].model_kind == 0 && markers_->size() == 1,
                    "Appearance was not applied only to the chosen satellite");
            record("independent-object-appearance");
            editor_->configure_appearance(0, {}, 500);
            ++orbit_smoke_stage_;
            break;
        case 24:
            require(last_model_outcome_ == "applied" && markers_->size() == 2,
                    "Second point marker was not restored");
            open_properties();
            input = satellites_[1].orbit->input;
            input.initial.position_m[0] += 1e6;
            editor_->set_input(input);
            properties_dialog_->close();
            require(satellites_[1].orbit->input.initial.position_m[0] !=
                        input.initial.position_m[0],
                    "Closing properties applied a draft");
            record("close-properties-without-applying");
            fit_views();
            set_selection(0);
            seek_epoch(scenario_->first());
            orbit_capture_ = true;
            ++orbit_smoke_stage_;
            break;
        case 25:
            save_workbench_image("scenario-two-satellites.png");
            orbit_capture_ = false;
            ++orbit_smoke_stage_;
            break;
        case 26: {
            const auto time = runtime::make_builtin_time_provider();
            const auto today = QDateTime::currentDateTimeUtc().date();
            const auto tomorrow = today.addDays(1);
            activate_scenario(application::Scenario(
                "Today", time->from_utc({today.year(), today.month(), today.day(), 0, 0, 0, 0}),
                time->from_utc({tomorrow.year(), tomorrow.month(), tomorrow.day(), 0, 0, 0, 0})));
            begin_insert_satellite();
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        }
        case 27:
            require(last_orbit_outcome_ == "applied" && satellites_.size() == 1 &&
                        eop_ == eop_current_ && orbit_->input.step_seconds == 60 &&
                        orbit_->track.samples().states().size() < 200,
                    "Current UTC one-day scenario failed to propagate with current EOP data");
            record("current-utc-day-orbit-and-predicted-eop");
            require(!commands_->actions().contains(properties_action_),
                    "Properties action remains in the command bar");
            open_scenario_properties();
            require(workspace_->current_document() == scenario_properties_,
                    "Scenario properties did not open in the workspace");
            require(scenario_properties_->grab().save(smoke_dir_ + "/scenario-properties.png"),
                    "Cannot save scenario properties");
            scenario_properties_->close();
            record("scenario-tree-properties");
            open_properties();
            require(properties_dialog_->grab().save(smoke_dir_ + "/current-object-properties.png"),
                    "Cannot save current object properties");
            properties_dialog_->close();
            slots_[1]->dock->hide();
            fit_views();
            orbit_capture_ = true;
            ++orbit_smoke_stage_;
            break;
        case 28:
            save_workbench_image("current-day-workbench.png");
            workspace_->split_page(map_panel_, view_panel_, Qt::RightEdge);
            ground_map_->reset_view();
            fit_views();
            ++orbit_smoke_stage_;
            break;
        case 29: {
            require(ground_map_->track_count() == satellites_.size() &&
                        ground_map_->epoch() == snapshot_.epoch,
                    "2D map does not share the published scenario and epoch");
            require(QLineF(ground_coordinates({7e6, 0, 0}), QPointF(0, 0)).length() < 1e-9 &&
                        QLineF(ground_coordinates({0, 7e6, 0}), QPointF(90, 0)).length() < 1e-9 &&
                        std::abs(ground_coordinates({0, 0, 7e6}).y() - 90) < 1e-9,
                    "2D geodetic axes are incorrect");
            const double e2 = 6.6943799901413165e-3;
            const double radius = 6378137.0 / std::sqrt(1 - e2 * 0.5);
            const auto latitude45 =
                ground_coordinates({(radius + 500000) / std::sqrt(2.0), 0,
                                    (radius * (1 - e2) + 500000) / std::sqrt(2.0)});
            require(std::abs(latitude45.y() - 45) < 1e-6,
                    "2D latitude is geocentric instead of WGS84 geodetic");
            const auto seam = ground_path({QPointF(179, 10), QPointF(-179, 12)});
            require(seam.elementCount() == 4 && seam.elementAt(1).x == 180 &&
                        seam.elementAt(2).isMoveTo() && seam.elementAt(2).x == -180 &&
                        std::abs(seam.elementAt(1).y - 11) < 1e-12,
                    "Ground track draws a false segment across the antimeridian");
            const auto exact_seam = ground_path({QPointF(180, 10), QPointF(-180, 10)});
            for (int i = 0; i < exact_seam.elementCount(); ++i)
                require(std::isfinite(exact_seam.elementAt(i).x) &&
                            std::isfinite(exact_seam.elementAt(i).y),
                        "Exact antimeridian produced an invalid path");
            for (const int direction : {-1, 1}) {
                const auto vertical =
                    ground_path({QPointF(direction * 180, 10), QPointF(-direction * 180, 12)});
                require(std::abs(vertical.length() - 2.0) < 1e-12,
                        "Ground track dropped latitude changes along the antimeridian");
            }
            save_workbench_image("workbench-2d-3d.png");
            require(ground_map_->grab().save(smoke_dir_ + "/ground-map.png"), "Cannot save 2D map");
            set_selection(std::nullopt);
            const auto point = ground_map_->marker_position(0);
            const auto global = ground_map_->mapToGlobal(point);
            QMouseEvent press(QEvent::MouseButtonPress, point, global, Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, global, Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(ground_map_, &press);
            QCoreApplication::sendEvent(ground_map_, &release);
            require(selected_ == 0 && objects_->currentItem() == satellites_[0].item,
                    "Picking a 2D satellite does not select the same scene object");
            record("2d-geodetic-projection-seam-and-selection");
            seek_epoch(scenario_->last());
            ++orbit_smoke_stage_;
            break;
        }
        case 30: {
            require(ground_map_->epoch() == scenario_->last() &&
                        ground_map_->epoch() == snapshot_.epoch &&
                        QLineF(ground_map_->marker_coordinates(0),
                               ground_coordinates(display_to_itrf_->apply_position(
                                   snapshot_.markers[0].position_m)))
                                .length() < 2e-5,
                    "2D/3D epoch or position diverged after a timeline seek");
            const auto center = QPointF(ground_map_->rect().center());
            QWheelEvent wheel(center, ground_map_->mapToGlobal(center), QPoint{}, QPoint{0, 120},
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(ground_map_, &wheel);
            require(ground_map_->zoom_factor() > 1, "2D wheel zoom did not respond");
            workspace_->detach_page(map_panel_);
            map_panel_->window()->resize(780, 480);
            record("2d-3d-shared-time-and-zoom");
            ++orbit_smoke_stage_;
            break;
        }
        case 31:
            require(workspace_->is_floating(map_panel_) && map_panel_->isVisible(),
                    "2D view cannot float");
            ground_map_->reset_view();
            require(map_panel_->window()->grab().save(smoke_dir_ + "/ground-map-floating.png"),
                    "Cannot save floating 2D view");
            workspace_->dock_page(map_panel_);
            workspace_->select_view(0);
            slots_[1]->dock->show();
            seek_epoch(scenario_->first());
            fit_views();
            record("2d-floating-and-dock-restore");
            ++orbit_smoke_stage_;
            break;
        case 32:
            require(ground_map_->epoch() == snapshot_.epoch,
                    "Hidden 2D view lost its current epoch");
            slots_[1]->dock->hide();
            seek_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 43'200'123'000'000});
            timeline_->zoom(4);
            fit_views();
            ++orbit_smoke_stage_;
            break;
        case 33: {
            if (!view_captures_ready())
                return;
            save_workbench_image("timeline-zoom.png");
            const auto epoch = snapshot_.epoch;
            const auto visible = timeline_->visible_last() - timeline_->visible_first();
            require(visible == 21'600'000, "Timeline zoom did not expose a six-hour interval");
            set_selection(std::nullopt);
            set_selection(0);
            require(timeline_->visible_last() - timeline_->visible_first() == visible &&
                        snapshot_.epoch == epoch,
                    "Changing selected object reset the visible interval or simulation time");
            record("timeline-zoom-and-selection-preserves-range");
            timeline_->zoom(1'000'000);
            const auto plot = timeline_->detail_rect();
            const auto point = QPointF(plot.left() + plot.width() * 0.371, plot.center().y());
            smoke_timeline_epoch_ = timeline_->visible_first() + 371;
            set_playing(true);
            QMouseEvent press(QEvent::MouseButtonPress, point, timeline_->mapToGlobal(point),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, timeline_->mapToGlobal(point),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(timeline_, &press);
            QCoreApplication::sendEvent(timeline_, &release);
            require(!playing_, "Timeline scrubbing did not pause playback");
            ++orbit_smoke_stage_;
            break;
        }
        case 34:
            require(snapshot_.epoch.tai_nanoseconds() == smoke_timeline_epoch_ * 1'000'000 &&
                        timeline_->current_time() == smoke_timeline_epoch_ &&
                        ground_map_->epoch() == snapshot_.epoch,
                    "Zoomed timeline seek lost millisecond precision or 2D/3D synchronization");
            record("timeline-precise-pointer-seek-and-pause");
            findChild<QComboBox *>("timelineScale")->setCurrentIndex(2);
            ++orbit_smoke_stage_;
            break;
        case 35:
            require(snapshot_.epoch.tai_nanoseconds() == smoke_timeline_epoch_ * 1'000'000 &&
                        time_label_->text() == calendar_text(snapshot_.epoch, CalendarScale::tt),
                    "Changing the timeline time standard changed the physical instant");
            record("timeline-scale-preserves-instant");
            require(time_toolbar_->grab().save(smoke_dir_ + "/timeline-precision-tt.png"),
                    "Cannot save the precise TT time ruler");
            findChild<QComboBox *>("timelineScale")->setCurrentIndex(0);
            timeline_->reset_view();
            findChild<QAction *>("showTimelineObject")->setChecked(true);
            ++orbit_smoke_stage_;
            break;
        case 36:
            require(time_toolbar_->grab().save(smoke_dir_ + "/timeline-object-expanded.png"),
                    "Cannot save the optional object interval");
            findChild<QAction *>("showTimelineObject")->setChecked(false);
            smoke_timeline_epoch_ =
                scenario_->first().tai_nanoseconds() / 1'000'000 + 13 * 3'600'000 + 1234;
            timer_.stop();
            QTimer::singleShot(0, this, [this] {
                auto *dialog = findChild<QDialog *>("jumpTimeDialog");
                if (!dialog) {
                    fail("Clicking the time readout did not open precise time input");
                    return;
                }
                dialog->findChild<QLineEdit *>("epochValue")
                    ->setText(calendar_text(core::Epoch{smoke_timeline_epoch_ * 1'000'000},
                                            CalendarScale::utc)
                                  .replace("  ", " ") +
                              " UTC");
                dialog->findChild<QPushButton *>("applyOrbit")->click();
            });
            time_label_->click();
            timer_.start();
            ++orbit_smoke_stage_;
            break;
        case 37:
            if (!view_captures_ready())
                return;
            require(snapshot_.epoch.tai_nanoseconds() == smoke_timeline_epoch_ * 1'000'000 &&
                        ground_map_->epoch() == snapshot_.epoch,
                    "Precise readout jump did not update the shared epoch");
            require(timeline_->sizeHint().height() == 118,
                    "Optional satellite row did not collapse");
            record("timeline-readout-jump-and-optional-object-row");
            {
                auto *rate = findChild<QDoubleSpinBox *>("playbackRate");
                auto *slower = findChild<QAction *>("slowerPlayback");
                auto *faster = findChild<QAction *>("fasterPlayback");
                auto *presets = findChild<QToolButton *>("playbackRatePresets");
                const auto epoch = snapshot_.epoch;
                const auto first = timeline_->visible_first(), last = timeline_->visible_last();
                timer_.stop();
                QTimer::singleShot(0, presets->menu(), [this, presets, require] {
                    require(presets->menu()->grab().save(smoke_dir_ + "/playback-rate-menu.png"),
                            "Cannot capture playback rate presets");
                    for (auto *action : presets->menu()->actions())
                        if (action->data().toDouble() == 100)
                            action->trigger();
                    presets->menu()->close();
                });
                presets->showMenu();
                timer_.start();
                require(rate->value() == 100 && playback_rate_ == 100 && !playing_,
                        "Selecting a playback preset did not update the effective rate");
                slower->trigger();
                require(playback_rate_ == 50, "Slower button did not select the preceding rate");
                faster->trigger();
                require(playback_rate_ == 100, "Faster button did not select the following rate");
                rate->findChild<QLineEdit *>()->setText(QStringLiteral("37.5 ×"));
                rate->interpretText();
                require(playback_rate_ == 37.5, "Custom playback rate was not accepted");
                slower->trigger();
                require(playback_rate_ == 20, "Custom rate did not step down to a preset");
                rate->setValue(37.5);
                faster->trigger();
                require(playback_rate_ == 50, "Custom rate did not step up to a preset");
                rate->setValue(0.1);
                require(!slower->isEnabled(), "Slower button is enabled at the minimum rate");
                rate->setValue(1000);
                require(!faster->isEnabled(), "Faster button is enabled at the maximum rate");
                rate->setValue(20);
                set_playing(true);
                faster->trigger();
                require(playing_ && playback_rate_ == 50,
                        "Changing the rate interrupted active playback");
                set_playing(false);
                rate->setValue(20);
                require(snapshot_.epoch == epoch && timeline_->visible_first() == first &&
                            timeline_->visible_last() == last,
                        "Changing the playback rate sought time or changed the visible interval");
                record("timeline-playback-rate-controls");
            }
            save_workbench_image("timeline-workbench.png");
            require(time_toolbar_->grab().save(smoke_dir_ + "/timeline.png"),
                    "Cannot save the compact timeline");
            // Restore the known visible marker fixture for the final native-window checks.
            slots_[1]->dock->show();
            seek_epoch(scenario_->first());
            fit_views();
            orbit_capture_ = true;
            ++orbit_smoke_stage_;
            break;
        case 38:
            // Qt applies dock layout changes on the next event-loop iteration.
            // Wait for matching native geometry and readback before final validation.
            if (!view_captures_ready())
                return;
            slots_[1]->dock->hide();
            seek_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 6 * 3600'000'000'000LL});
            smoke_set_view_layout(2);
            ++orbit_smoke_stage_;
            break;
        case 39:
            if (!view_captures_ready() || !caches_ready())
                return;
            require(ground_map_->isVisible() && slots_[0]->container->isVisible(),
                    "Parallel view layout did not expose both renderers");
            save_workbench_image("views-parallel.png");
            smoke_view_epoch_ = snapshot_.epoch;
            smoke_sun_ = sun_direction_;
            smoke_set_view_layout(1);
            ++orbit_smoke_stage_;
            break;
        case 40:
            require(ground_map_->isVisible() && !slots_[0]->container->isVisible() &&
                        snapshot_.epoch == smoke_view_epoch_ && selected_ == 0,
                    "2D-only layout lost its content, time or selection");
            save_workbench_image("views-2d.png");
            smoke_map_image_ = ground_map_->grab().toImage();
            lighting_action_->setChecked(false);
            require(ground_map_->grab().toImage() != smoke_map_image_,
                    "The 2D daylight setting did not affect the map");
            lighting_action_->setChecked(true);
            record("view-modes-and-shared-display-settings");
            seek_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 12 * 3600'000'000'000LL});
            ++orbit_smoke_stage_;
            break;
        case 41: {
            const auto dot = smoke_sun_[0] * sun_direction_[0] + smoke_sun_[1] * sun_direction_[1] +
                             smoke_sun_[2] * sun_direction_[2];
            require(ground_map_->epoch() == snapshot_.epoch && dot < 0.1 && dot > -0.1,
                    "2D-only seeking failed to rotate Earth-fixed sunlight over six hours");
            require(ground_map_->grab().toImage() != smoke_map_image_,
                    "2D night overlay did not follow the shared scene time");
            smoke_view_epoch_ = snapshot_.epoch;
            set_playing(true);
            ++orbit_smoke_stage_;
            break;
        }
        case 42:
            require(snapshot_.epoch != smoke_view_epoch_ && ground_map_->epoch() == snapshot_.epoch,
                    "Playback stopped when the 3D window was hidden");
            set_playing(false);
            record("2d-only-playback-and-solar-time-synchronization");
            smoke_set_view_layout(0);
            seek_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 6 * 3600'000'000'000LL});
            fit_views();
            ++orbit_smoke_stage_;
            break;
        case 43:
            if (!view_captures_ready() || !caches_ready())
                return;
            save_workbench_image("views-3d.png");
            smoke_globe_image_ = slots_[0]->captured_image;
            lighting_action_->setChecked(false);
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        case 44:
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            require(slots_[0]->captured_image != smoke_globe_image_,
                    "3D lighting switch did not change the rendered globe");
            record("3d-daylight-toggle-and-restored-native-view");
            lighting_action_->setChecked(true);
            smoke_set_view_layout(2);
            ++orbit_smoke_stage_;
            break;
        case 45:
            if (!view_captures_ready())
                return;
            require(ground_map_->isVisible() && slots_[0]->container->isVisible(),
                    "2D/3D pages did not return to split groups");
            record("map-tab-to-split-roundtrip");
            lighting_action_->setChecked(false);
            smoke_set_view_layout(0);
            slots_[1]->dock->show();
            seek_epoch(scenario_->first());
            fit_views();
            ++orbit_smoke_stage_;
            break;
        case 46:
            if (!view_captures_ready() || !caches_ready())
                return;
            slots_[1]->dock->hide();
            open_properties();
            require(workspace_->current_document() == properties_dialog_,
                    "Object properties did not open in the central workspace");
            require(editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) == 60,
                    "The default display step is not 60 seconds");
            editor_->findChild<QuantityEditor *>("displayStep")->set_value(45, Unit::second);
            workspace_->select_view(1);
            ++orbit_smoke_stage_;
            break;
        case 47:
            require(ground_map_->isVisible() && !workspace_->current_document() &&
                        editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) ==
                            45,
                    "Switching to a view tab discarded the property draft");
            workspace_->open(properties_dialog_, false);
            ++orbit_smoke_stage_;
            break;
        case 48: {
            auto *navigation = editor_->findChild<QTreeWidget *>("propertyNavigation");
            require(navigation && navigation->topLevelItemCount() == 3 &&
                        navigation->topLevelItem(0)->childCount() == 2,
                    "The property navigation does not group orbit and range under Basic");
            editor_->findChild<QuantityEditor *>("displayStep")->set_value(60, Unit::second);
            {
                auto *angle = editor_->findChild<QuantityEditor *>("raan");
                angle->set_value(180, Unit::degree);
                auto *selector = angle->findChild<QToolButton *>("quantityUnit");
                QTimer::singleShot(0, selector->menu(), [this, selector, require] {
                    require(selector->menu()->grab().save(smoke_dir_ + "/quantity-unit-menu.png"),
                            "Cannot capture the quantity unit menu");
                    for (auto *action : selector->menu()->actions())
                        if (action->data().toInt() == static_cast<int>(Unit::radian))
                            action->trigger();
                    selector->menu()->close();
                });
                timer_.stop(); // Keep the scripted stage from re-entering the modal menu.
                selector->showMenu();
                timer_.start();
                require(angle->unit() == Unit::radian &&
                            std::abs(angle->value(Unit::degree) - 180) < 1e-12 &&
                            angle->findChild<QLineEdit *>("quantityValue")
                                ->text()
                                .startsWith("3.14159"),
                        "Clickable angle unit did not convert the number");
                save_workbench_image("workspace-quantity-units.png");
                angle->set_unit(Unit::degree);
                // Unit changes are a draft; restore the actual orbit value before later checks.
                angle->set_value(
                    runtime::to_keplerian(orbit_->input.initial,
                                          core::GravitationalParameter{orbit_->input.mu()})
                        .raan_rad,
                    Unit::radian);
            }
            save_workbench_image("workspace-properties.png");
            require(action_in_workspace("applyOrbit"),
                    "Orbit Apply button is clipped by the central workspace");
            navigation->topLevelItem(0)->setExpanded(false);
            require(!navigation->topLevelItem(0)->isExpanded(), "Property group cannot collapse");
            navigation->topLevelItem(0)->setExpanded(true);
            navigation->setCurrentItem(navigation->topLevelItem(0)->child(1));
            ++orbit_smoke_stage_;
            break;
        }
        case 49:
            save_workbench_image("workspace-display-settings.png");
            require(action_in_workspace("applyDisplay"),
                    "Display Apply button is clipped by the central workspace");
            workspace_->detach_page(properties_dialog_);
            ++orbit_smoke_stage_;
            break;
        case 50:
            require(workspace_->is_floating(properties_dialog_) &&
                        editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) ==
                            60,
                    "Detaching properties changed the field values");
            require(
                properties_dialog_->window()->grab().save(smoke_dir_ + "/properties-detached.png"),
                "Cannot capture detached properties");
            workspace_->dock_page(properties_dialog_);
            require(workspace_->current_document() == properties_dialog_,
                    "Detached properties did not return to the workspace");
            properties_dialog_->close();
            require(isVisible() && !workspace_->contains(properties_dialog_),
                    "Closing properties closed the workbench");
            record("grouped-properties-tabs-draft-and-detach-roundtrip");
            workspace_->select_view(0);
            smoke_cache_ = orbit_;
            smoke_view_epoch_ = snapshot_.epoch;
            require(display_frame_ == core::FrameId::gcrf && snapshot_.frame == core::FrameId::gcrf,
                    "3D did not default to GCRF");
            frame_choices_.front()->setCurrentIndex(1);
            ++orbit_smoke_stage_;
            break;
        case 51: {
            const auto direct =
                orbit_->session.sample(std::array{snapshot_.epoch}).states().front();
            const auto fixed = display_to_itrf_->apply_position(direct.position_m);
            const auto shown = snapshot_.markers.front().position_m;
            require(
                display_frame_ == core::FrameId::itrf && snapshot_.frame == core::FrameId::itrf &&
                    snapshot_.epoch == smoke_view_epoch_ && orbit_ == smoke_cache_ &&
                    std::hypot(fixed[0] - shown[0], fixed[1] - shown[1], fixed[2] - shown[2]) <= 1,
                "Switching the 3D frame changed time/cache or used the wrong coordinates");
            frame_choices_.front()->setCurrentIndex(0);
            const auto inertial = snapshot_.markers.front().position_m;
            require(std::hypot(direct.position_m[0] - inertial[0],
                               direct.position_m[1] - inertial[1],
                               direct.position_m[2] - inertial[2]) <= 1,
                    "Returning to GCRF did not restore the inertial state");
            record("gcrf-itrf-frame-switch-without-propagation");
            const core::Epoch jump{scenario_->first().tai_nanoseconds() + 18 * 3600'000'000'000LL};
            require(!cache_covers(*orbit_, jump, jump), "Seek test unexpectedly already cached");
            const auto immediate = sample_frame(jump).markers.front().position_m;
            const auto exact = orbit_->session.sample(std::array{jump}).states().front().position_m;
            require(immediate == exact,
                    "Uncached time did not query the analysis session directly");
            smoke_publications_ = display_cache_publications_;
            smoke_render_tasks_ = viewer_->recordAndSubmitTasks;
            seek_epoch(jump);
            ++orbit_smoke_stage_;
            break;
        }
        case 52:
            if (!caches_ready())
                return;
            require(display_cache_publications_ > smoke_publications_ && orbit_ != smoke_cache_ &&
                        viewer_->recordAndSubmitTasks == smoke_render_tasks_ &&
                        orbit_->track.samples().states().size() < 200 &&
                        orbit_->session.provenance().input_revision ==
                            smoke_cache_->session.provenance().input_revision,
                    "Seeking did not refill a bounded cache without revising the orbit");
            record("uncached-seek-exact-position-and-asynchronous-period-cache");
            smoke_render_tasks_.clear();
            open_properties();
            editor_->findChild<QComboBox *>("trackExtent")->setCurrentIndex(1);
            editor_->findChild<QPushButton *>("applyDisplay")->click();
            properties_dialog_->close();
            ++orbit_smoke_stage_;
            break;
        case 53:
            if (!caches_ready())
                return;
            require(cache_covers(*orbit_, scenario_->first(), scenario_->last()) &&
                        orbit_->track.samples().states().size() == 1441,
                    "Explicit full-interval request did not prepare the day at 60 seconds");
            record("explicit-full-interval-cache-request");
            smoke_cache_ = orbit_;
            editor_->display({.track_2d = false, .track_3d = false});
            ++orbit_smoke_stage_;
            break;
        case 54:
            require(orbit_ == smoke_cache_ && !display_candidate_.valid(),
                    "Hiding trajectories launched another propagation");
            editor_->display({});
            require(orbit_ == smoke_cache_ && satellites_.front().inertial_track->size() < 160,
                    "One-period display failed to restrict the retained full cache");
            record("display-visibility-and-range-reuse-existing-cache");
            slots_[1]->dock->show();
            seek_epoch(scenario_->first());
            fit_views();
            fit_pending_ = true;
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        case 55:
            // Complete the frame-driven surface lifecycle checks before measuring layout.
            if (frame_count_ < 150 || frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            slots_[1]->dock->hide();
            open_properties();
            ++orbit_smoke_stage_;
            break;
        case 56:
            smoke_workspace_height_ = workspace_->height();
            smoke_timeline_epoch_ = timeline_->current_time();
            smoke_timeline_first_ = timeline_->visible_first();
            smoke_timeline_last_ = timeline_->visible_last();
            findChild<QToolButton *>("collapseTimeline")->click();
            ++orbit_smoke_stage_;
            break;
        case 57:
            save_workbench_image("workspace-collapsed-timeline.png");
            require(!timeline_->isVisible() && time_label_->isVisible() &&
                        play_action_->isEnabled(),
                    "Collapsing the timeline did not keep the transport controls visible");
            require(workspace_->height() >= smoke_workspace_height_ + 100,
                    qPrintable(QString("Collapsing the timeline did not release workspace "
                                       "height: %1 -> %2")
                                   .arg(smoke_workspace_height_)
                                   .arg(workspace_->height())));
            require(timeline_->current_time() == smoke_timeline_epoch_ &&
                        timeline_->visible_first() == smoke_timeline_first_ &&
                        timeline_->visible_last() == smoke_timeline_last_ &&
                        editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) ==
                            60,
                    "Collapsing the timeline changed the instant, range or property draft");
            findChild<QAction *>("expandTimeline")->setChecked(true);
            ++orbit_smoke_stage_;
            break;
        case 58:
            require(timeline_->isVisible() && workspace_->height() == smoke_workspace_height_ &&
                        timeline_->current_time() == smoke_timeline_epoch_ &&
                        timeline_->visible_first() == smoke_timeline_first_ &&
                        timeline_->visible_last() == smoke_timeline_last_,
                    "Restoring the timeline changed its time or browsing range");
            record("timeline-collapse-restores-workspace-and-time-range");
            properties_dialog_->close();
            slots_[1]->dock->show();
            fit_views();
            fit_pending_ = true;
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        case 59:
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            slots_[1]->dock->hide();
            workspace_->select_view(0);
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        case 60: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            smoke_cache_ = orbit_;
            smoke_view_epoch_ = snapshot_.epoch;
            auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
            smoke_camera_ = {look->eye, look->center, look->up};
            require(smoke_tabs(QStringLiteral("三维视图"))->count() == 2,
                    "Workspace still has a special parallel-view tab");
            smoke_drag_page(QStringLiteral("三维视图"),
                            workspace_->mapToGlobal(
                                QPoint(workspace_->width() - 12, workspace_->height() / 2)));
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        }
        case 61: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            require(ground_map_->isVisible() && slots_[0]->container->isVisible() &&
                        smoke_tabs(QStringLiteral("三维视图")) !=
                            smoke_tabs(QStringLiteral("二维视图")),
                    "Dragging the native 3D page did not split the workspace");
            auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
            require(orbit_ == smoke_cache_ && snapshot_.epoch == smoke_view_epoch_ &&
                        look->eye == smoke_camera_[0] && look->center == smoke_camera_[1] &&
                        look->up == smoke_camera_[2],
                    "Dragging 3D changed the camera, current time or orbit cache");
            save_workbench_image("workspace-drag-split.png");
            record("drag-native-3d-tab-to-split-without-resetting-state");
            auto *tabs = smoke_tabs(QStringLiteral("二维视图"));
            smoke_drag_page(QStringLiteral("三维视图"),
                            tabs->mapToGlobal(QPoint(tabs->width() - 8, 16)));
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        }
        case 62:
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            require(smoke_tabs(QStringLiteral("三维视图")) ==
                            smoke_tabs(QStringLiteral("二维视图")) &&
                        !ground_map_->isVisible() && slots_[0]->container->isVisible(),
                    "Dragging 3D back to the tab bar did not merge the groups");
            record("drag-native-3d-tab-to-merge");
            smoke_drag_page(QStringLiteral("三维视图"), workspace_->mapToGlobal(QPoint(-80, 100)));
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        case 63: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            require(workspace_->is_floating(view_panel_) && slots_[0]->container->isVisible(),
                    "Dragging 3D out did not create a floating page");
            auto *window = view_panel_->window();
            require(window->width() >= 480 && window->height() >= 320,
                    qPrintable(QString("Floating 3D page is too small: %1 x %2")
                                   .arg(window->width())
                                   .arg(window->height())));
            auto picture = window->grab();
            QPainter painter(&picture);
            painter.drawImage(
                QRect(slots_[0]->container->mapTo(window, QPoint{}), slots_[0]->container->size()),
                slots_[0]->captured_image);
            painter.end();
            require(picture.save(smoke_dir_ + "/workspace-3d-floating.png"),
                    "Cannot capture floating 3D page");
            record("drag-native-3d-tab-to-floating-window");
            workspace_->dock_page(view_panel_);
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        }
        case 64:
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            require(!workspace_->is_floating(view_panel_), "3D page did not dock again");
            workspace_->close_page(view_panel_);
            require(!workspace_->contains(view_panel_), "3D page could not close");
            workspace_->select_view(0);
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        case 65: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
            require(workspace_->contains(view_panel_) && slots_[0]->container->isVisible() &&
                        orbit_ == smoke_cache_ && snapshot_.epoch == smoke_view_epoch_ &&
                        look->eye == smoke_camera_[0] && look->center == smoke_camera_[1] &&
                        look->up == smoke_camera_[2],
                    "Floating/closing/reopening 3D lost the camera, time or cached orbit");
            save_workbench_image("workspace-pages-restored.png");
            record("native-3d-float-dock-close-reopen-preserves-state");
            smoke_drag_page(QStringLiteral("二维视图"),
                            workspace_->mapToGlobal(
                                QPoint(workspace_->width() / 2, workspace_->height() - 12)));
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        }
        case 66: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            require(ground_map_->isVisible() && slots_[0]->container->isVisible() &&
                        map_panel_->mapTo(workspace_, QPoint{}).y() >
                            view_panel_->mapTo(workspace_, QPoint{}).y(),
                    "Dragging to the bottom edge did not create a vertical split");
            const auto globe_rect = QRect(slots_[0]->container->mapTo(workspace_, QPoint{}),
                                          slots_[0]->container->size());
            const auto map_tabs = smoke_tabs(QStringLiteral("二维视图"));
            require(globe_rect.bottom() < map_tabs->mapTo(workspace_, QPoint{}).y() &&
                        workspace_->rect().contains(
                            QRect(ground_map_->mapTo(workspace_, QPoint{}), ground_map_->size())),
                    "Vertical split clips the map tabs or the map content");
            save_workbench_image("workspace-drag-vertical.png");
            record("drag-pages-to-vertical-split");
            auto *tabs = smoke_tabs(QStringLiteral("三维视图"));
            smoke_drag_page(QStringLiteral("二维视图"),
                            tabs->mapToGlobal(QPoint(tabs->width() - 8, 16)));
            workspace_->select_view(0);
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        }
        case 67: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            auto *look = dynamic_cast<vsg::LookAt *>(slots_[0]->camera->viewMatrix.get());
            require(orbit_ == smoke_cache_ && snapshot_.epoch == smoke_view_epoch_ &&
                        look->eye == smoke_camera_[0] && look->center == smoke_camera_[1] &&
                        look->up == smoke_camera_[2],
                    "Vertical split/merge changed the camera, time or cached orbit");
            slots_[1]->dock->show();
            fit_views();
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        }
        case 68: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            smoke_cache_ = orbit_;
            set_selection(0);
            slots_[1]->dock->hide();
            open_properties();
            auto *frame = editor_->findChild<QComboBox *>("initialReferenceFrame");
            require(frame && !editor_->findChild<QuantityEditor *>("gravitationalParameter"),
                    "Satellite properties must select a frame and inherit the body's mu");
            frame->setCurrentIndex(1);
            const auto &initial = orbit_->input.initial;
            const auto expected =
                frames_->gcrf_to_itrf_state(initial.epoch, eop_->at(initial.epoch)).apply(initial);
            require(frame->currentIndex() == 1 &&
                        std::abs(editor_->findChild<QuantityEditor *>("velocityY")
                                     ->value(Unit::metres_per_second) -
                                 expected.velocity_mps[1]) < 1e-6,
                    "The initial ITRF state did not include the velocity rotation term");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            save_workbench_image("initial-state-itrf.png");
            frame->setCurrentIndex(0);
            editor_->set_state_mode(1);
            editor_->findChild<QComboBox *>("orbitPropagator")->setCurrentIndex(1);
            require(editor_->findChild<QuantityEditor *>("meanAnomaly"),
                    "J2 input must identify the mean anomaly");
            QTimer::singleShot(0, editor_, [this, require] {
                auto *dialog = editor_->findChild<QDialog *>("propagatorDetailsDialog");
                require(dialog && dialog->findChildren<QCheckBox *>().empty() &&
                            dialog->findChildren<QuantityEditor *>().empty(),
                        "Model information must not duplicate sampling or offer partial J2 terms");
                require(dialog->grab().save(smoke_dir_ + "/j2-model.png"),
                        "Cannot capture J2 model information");
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Close)->click();
            });
            timer_.stop();
            editor_->findChild<QPushButton *>("propagatorDetails")->click();
            timer_.start();
            editor_->submit();
            record("initial-state-frame-conversion-and-j2-model-details");
            ++orbit_smoke_stage_;
            break;
        }
        case 69:
            require(last_orbit_outcome_ == "applied" &&
                        orbit_->input.propagator == OrbitPropagator::j2_secular &&
                        orbit_->max_inertial_check_error_m <= 1 &&
                        orbit_->session.provenance().algorithm.find("node=1;apsis=1;mean=1") !=
                            std::string::npos,
                    "J2 propagation did not publish every secular term");
            save_workbench_image("j2-properties.png");
            properties_dialog_->close();
            slots_[1]->dock->show();
            fit_views();
            smoke_wait_frame_ = frame_count_ + 3;
            ++orbit_smoke_stage_;
            break;
        case 70:
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            save_workbench_image("j2-orbit.png");
            record("publish-j2-trajectory-with-validated-display-cache");
            editor_->set_input(smoke_cache_->input);
            editor_->submit();
            ++orbit_smoke_stage_;
            break;
        case 71:
            require(last_orbit_outcome_ == "applied" &&
                        orbit_->input.propagator == OrbitPropagator::two_body,
                    "Could not switch back from J2 to two-body propagation");
            record("switch-propagator-back-to-two-body");
            orbit_capture_ = false;
            begin_insert_satellite();
            input = satellites_.front().orbit->input;
            input.object_id = "ProjectJ2";
            input.propagator = OrbitPropagator::j2_secular;
            start_orbit(input);
            ++orbit_smoke_stage_;
            break;
        case 72:
            require(last_orbit_outcome_ == "applied" && satellites_.size() == 2 &&
                        satellites_[1].orbit->input.propagator == OrbitPropagator::j2_secular,
                    "Project test did not create two different propagators");
            satellites_[1].display.extent = 2;
            satellites_[1].display.past_minutes = 12.5;
            satellites_[1].display.future_minutes = 6.25;
            satellites_[1].display.labels_2d = false;
            start_model(1, {}, 250);
            ++orbit_smoke_stage_;
            break;
        case 73: {
            require(last_model_outcome_ == "applied" && project_dirty_,
                    "Project changes were not marked unsaved");
            set_playing(false);
            const core::Epoch epoch{scenario_->first().tai_nanoseconds() + 123'456'789'123};
            seek_epoch(epoch);
            snapshot_ = sample_frame(epoch);
            update_satellite();
            const auto path = smoke_dir_ + QStringLiteral("/项目.orbitforge");
            const auto recorded = project_snapshot(path);
            analysis_record_ = application::AnalysisRecord{
                application::analysis_input_sha256(recorded), recorded.data_lock().sha256()};
            standalone_time_data_ = eop_->data_info().time_data;
            save_project_to(path);
            require(!project_dirty_, "Successful save did not clear the modified marker");
            smoke_project_bytes_ = application::encode_project(project_snapshot(path));
            auto saved = application::load_project(std::filesystem::path(path.toStdWString()));
            require(saved.current == epoch, "Saved current time lost nanoseconds");
            start_project(std::move(saved), path);
            ++orbit_smoke_stage_;
            break;
        }
        case 74: {
            require(last_project_outcome_ == "opened" && !project_dirty_ &&
                        satellites_.size() == 2 && satellites_[1].model_kind == 1 &&
                        satellites_[1].model_diameter_km == 250 &&
                        !satellites_[1].display.labels_2d &&
                        satellites_[1].orbit->input.propagator == OrbitPropagator::j2_secular,
                    "Saved project did not restore objects and display settings");
            require(application::encode_project(project_snapshot(project_path_)) ==
                        smoke_project_bytes_,
                    "Desktop project roundtrip changed its persisted inputs");
            require(analysis_record_ && standalone_time_data_ &&
                        project_snapshot(project_path_).analysis_status() ==
                            application::AnalysisStatus::current,
                    "Desktop project roundtrip lost API analysis provenance");
            record("project-save-open-two-body-j2-display-nanoseconds");
            save_workbench_image("project-restored.png");
            smoke_cache_ = orbit_;
            const auto smoke_time = runtime::make_builtin_time_provider();
            application::Project uncovered{"Uncovered",
                                           smoke_time->from_utc({2027, 1, 1, 0, 0, 0, 0}),
                                           smoke_time->from_utc({2027, 1, 2, 0, 0, 0, 0})};
            answer_data(false, "dataCoverageDialog");
            require(!start_project(uncovered, project_path_) && orbit_ == smoke_cache_ && !busy() &&
                        !project_dirty_,
                    "Coverage failure replaced the current scene");
            record("project-coverage-gap-preserves-scene");
            auto wrong_data = project_snapshot(project_path_);
            wrong_data.earth_orientation->sha256[0] =
                wrong_data.earth_orientation->sha256[0] == 'a' ? 'b' : 'a';
            answer_data(false);
            require(!start_project(wrong_data, project_path_) && !busy() &&
                        orbit_ == smoke_cache_ && !project_dirty_,
                    "Declining changed data modified the scene");
            record("project-data-change-decline-preserves-scene");
            answer_data(true);
            start_project(std::move(wrong_data), project_path_);
            ++orbit_smoke_stage_;
            break;
        }
        case 75: {
            require(last_project_outcome_ == "opened" && satellites_.size() == 2 &&
                        project_dirty_ &&
                        scenario_->data_lock() == application::eop_data_lock(eop_->data_info()),
                    "Saved data hash prevented the project from using current data");
            record("project-data-change-approval-uses-current-data");
            smoke_cache_ = orbit_;
            auto missing_model = project_snapshot(project_path_);
            missing_model.satellites[1].model_kind = 2;
            missing_model.satellites[1].model_path = "missing-project-model.glb";
            start_project(std::move(missing_model), project_path_);
            ++orbit_smoke_stage_;
            break;
        }
        case 76: {
            require(last_project_outcome_ == "rejected" && orbit_ == smoke_cache_ &&
                        satellites_.size() == 2,
                    "A missing external model replaced the active project");
            record("project-missing-model-preserves-active-scene");
            mark_project_dirty();
            QTimer::singleShot(0, [] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                    box->button(QMessageBox::Cancel)->click();
            });
            require(!confirm_replace_scenario() && project_dirty_,
                    "Cancel discarded unsaved changes");
            QTimer::singleShot(0, [] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                    box->button(QMessageBox::Save)->click();
            });
            require(confirm_replace_scenario() && !project_dirty_, "Save-before-replace failed");
            record("project-unsaved-cancel-and-save");
            auto empty =
                application::Project{"Empty project", scenario_->first(), scenario_->last()};
            empty.earth_orientation = eop_->data_info();
            start_project(std::move(empty), smoke_dir_ + "/empty.orbitforge");
            ++orbit_smoke_stage_;
            break;
        }
        case 77: {
            require(last_project_outcome_ == "opened" && satellites_.empty() &&
                        scenario_->objects().empty() && ground_map_->track_count() == 0,
                    "Empty project did not clear all objects");
            record("project-open-empty-scene");
            const auto args = app_.arguments();
            const auto fixture_index = args.indexOf("--project-smoke-input");
            const auto path = fixture_index >= 0 ? args.value(fixture_index + 1)
                                                 : smoke_dir_ + QStringLiteral("/项目.orbitforge");
            start_project(application::load_project(std::filesystem::path(path.toStdWString())),
                          path);
            ++orbit_smoke_stage_;
            break;
        }
        case 78:
            require(last_project_outcome_ == "opened" && satellites_.size() == 2 &&
                        satellites_[0].orbit->input.propagator == OrbitPropagator::two_body &&
                        satellites_[1].orbit->input.propagator == OrbitPropagator::j2_secular,
                    "Project interoperability input failed to open");
            save_project_to(smoke_dir_ + "/desktop-roundtrip.orbitforge");
            record("project-interoperability-load-and-save");
            smoke_cache_ = orbit_;
            start_project(project_snapshot(project_path_), project_path_);
            project_cancellation_->store(true);
            ++orbit_smoke_stage_;
            break;
        case 79:
            require(last_project_outcome_ == "cancelled" && orbit_ == smoke_cache_ &&
                        !project_dirty_,
                    "Cancelling project preparation changed the current scene");
            record("project-cancel-open-preserves-current-scene");
            ++orbit_smoke_stage_;
            break;
        case 80: {
            set_selection(0);
            smoke_cache_ = orbit_;
            smoke_data_lock_ = scenario_->data_lock();
            QTimer::singleShot(0, [] {
                if (auto *dialog =
                        qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                    dialog->setTextValue(QStringLiteral("观测卫星 A"));
                    dialog->accept();
                }
            });
            rename_action_->trigger();
            require(satellites_[0].name() == QStringLiteral("观测卫星 A") &&
                        orbit_ == smoke_cache_ && project_dirty_ &&
                        scenario_->data_lock() == smoke_data_lock_,
                    "Rename changed the numerical session or failed to update the name");
            save_project_to(smoke_dir_ + "/objects.orbitforge");
            const auto named =
                application::load_project(std::filesystem::path(project_path_.toStdWString()));
            require(named.satellites[0].display_name == "观测卫星 A" &&
                        named.satellites[0].orbit.object_id == smoke_cache_->input.object_id,
                    "Rename did not persist separately from the stable ID");
            record("object-rename-keeps-session-and-stable-id");
            smoke_view_epoch_ = snapshot_.epoch;
            duplicate_action_->trigger();
            ++orbit_smoke_stage_;
            break;
        }
        case 81: {
            require(last_project_outcome_ == "opened" && satellites_.size() == 3 &&
                        selected_ == 2 && project_dirty_ && snapshot_.epoch == smoke_view_epoch_ &&
                        satellites_[0].orbit->session.provenance().input_revision ==
                            smoke_cache_->session.provenance().input_revision &&
                        satellites_[2].orbit->input.object_id !=
                            satellites_[0].orbit->input.object_id &&
                        satellites_[2].display == satellites_[0].display &&
                        satellites_[2].model_kind == satellites_[0].model_kind &&
                        satellites_[2].orbit->input.initial.position_m ==
                            satellites_[0].orbit->input.initial.position_m,
                    "Copy lost settings, changed the original, or reused its identity");
            record("object-copy-preserves-settings-and-current-time");
            open_properties();
            auto copied = satellites_[2].orbit->input;
            copied.initial.position_m[0] *= 1.001;
            start_orbit(std::move(copied));
            ++orbit_smoke_stage_;
            break;
        }
        case 82:
            require(last_orbit_outcome_ == "applied" &&
                        satellites_[2].orbit->session.provenance().input_revision == 2 &&
                        satellites_[0].orbit->input.initial.position_m ==
                            smoke_cache_->input.initial.position_m,
                    "Editing a copy changed the original object");
            record("object-copy-is-independently-editable");
            properties_dialog_->close();
            confirm_delete(false);
            require(satellites_.size() == 3 && !busy(), "Cancel deleted an object");
            record("object-delete-cancel-preserves-scene");
            confirm_delete(true);
            ++orbit_smoke_stage_;
            break;
        case 83:
            require(last_project_outcome_ == "opened" && satellites_.size() == 2 &&
                        selected_ == 1 && ground_map_->track_count() == 2 &&
                        scenario_->objects().size() == 2,
                    "Deleting a copy did not synchronize the scene and selection");
            set_selection(0);
            confirm_delete(true);
            ++orbit_smoke_stage_;
            break;
        case 84:
            require(last_project_outcome_ == "opened" && satellites_.size() == 1 &&
                        selected_ == 0 &&
                        satellites_[0].orbit->input.propagator == OrbitPropagator::j2_secular,
                    "Deleting the first object left stale indices");
            record("object-delete-reindexes-surviving-objects");
            confirm_delete(true);
            ++orbit_smoke_stage_;
            break;
        case 85: {
            require(last_project_outcome_ == "opened" && satellites_.empty() && !selected_ &&
                        scenario_->objects().empty() && ground_map_->track_count() == 0 &&
                        orbit_group_->children.empty() && model_group_->children.empty() &&
                        !delete_action_->isEnabled() && snapshot_.markers.empty(),
                    "Deleting the last object left stale graphical resources");
            record("object-delete-last-produces-valid-empty-scene");
            const auto path = smoke_dir_ + "/objects.orbitforge";
            start_project(application::load_project(std::filesystem::path(path.toStdWString())),
                          path);
            ++orbit_smoke_stage_;
            break;
        }
        case 86: {
            eop_catalog_->set_preferred_product(DataProduct::finals2000a);
            require(last_project_outcome_ == "opened" && satellites_.size() == 2 &&
                        satellites_[0].name() == QStringLiteral("观测卫星 A"),
                    "Object names were not restored from the project");
            const auto resource =
                QCoreApplication::applicationDirPath() +
                (eop_ == eop_current_ ? "/resources/eop-current" : "/resources/eop");
            QFile manifest(resource + "/manifest.json");
            require(manifest.open(QIODevice::ReadOnly), "Cannot open smoke EOP source");
            auto json = QJsonDocument::fromJson(manifest.readAll()).object();
            json["snapshot_id"] = "smoke-imported-" + QString::fromStdString(eop_->data_info().id);
            const auto source = smoke_dir_ + "/import-source";
            require(QDir().mkpath(source), "Cannot create import fixture");
            const auto filename = json["data_file"].toString();
            require(QFile::copy(resource + "/" + filename, source + "/" + filename),
                    "Cannot copy EOP fixture");
            QFile copy(source + "/manifest.json");
            require(copy.open(QIODevice::WriteOnly), "Cannot write EOP fixture");
            copy.write(QJsonDocument(json).toJson());
            copy.close();
            QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
            QTimer::singleShot(0, [&, source] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                require(dialog != nullptr, "Missing EOP dialog");
                auto *details = dialog->findChild<QLabel *>("dataInventoryDetails");
                auto *automatic = dialog->findChild<QCheckBox *>("dataAutoUpdate");
                auto *inventory = dialog->findChild<QTreeWidget *>("dataInventory");
                auto *priority = dialog->findChild<QListWidget *>("eopSourcePriority");
                auto *pages = dialog->findChild<QTabWidget *>("dataPages");
                require(pages && details && automatic && automatic->isChecked() && inventory &&
                            inventory->topLevelItemCount() == 3 && priority &&
                            inventory->topLevelItem(0)->childCount() == 2 &&
                            priority->isVisible() &&
                            !dialog->findChild<QComboBox *>("eopSnapshots") &&
                            !dialog->findChild<QPushButton *>("setDefaultEop") &&
                            !dialog->findChild<QPushButton *>("applyEopSnapshot"),
                        "Data inventory must show current data without version/default controls");
                auto *group = inventory->topLevelItem(0);
                auto *finals = group->child(0);
                auto *c04 = group->child(1);
                auto *utc = inventory->topLevelItem(1);
                require(
                    priority->model()->moveRow({}, 1, {}, 0) &&
                        eop_catalog_->preferred_product() == DataProduct::c04 &&
                        inventory->currentItem() == group &&
                        scenario_->data_lock() == smoke_data_lock_,
                    "Reordering sources must persist priority without changing the active scene");
                require(priority->model()->moveRow({}, 1, {}, 0) &&
                            eop_catalog_->preferred_product() == DataProduct::finals2000a,
                        "Cannot restore default source priority");
                QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
                require(dialog->grab().save(smoke_dir_ + "/data-center-priority.png"),
                        "Cannot save source priority page");
                inventory->setCurrentItem(finals);
                require(dialog->findChild<QLabel *>("dataAcquiredAt")
                            ->text()
                            .contains(QStringLiteral("内置")),
                        "Missing acquisition information");
                require(finals->text(0).contains(QStringLiteral("内置示例")) &&
                            dialog->findChild<QLabel *>("dataLastChecked")
                                ->text()
                                .contains(QStringLiteral("尚未检查")),
                        "Bundled samples must not claim a full download or a successful check");
                auto *frequency = dialog->findChild<QComboBox *>("dataUpdateFrequency");
                require(frequency, "Missing automatic update interval");
                require(!automatic->isVisible(),
                        "Overview must not expose advanced update settings");
                pages->setCurrentIndex(1);
                frequency->setCurrentIndex(frequency->findData(6));
                require(eop_catalog_->update_interval_hours() == 6,
                        "Automatic update frequency was not persisted");
                frequency->setCurrentIndex(frequency->findData(24));
                pages->setCurrentIndex(0);
                QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
                require(dialog->grab().save(smoke_dir_ + "/data-center-sample.png"),
                        "Cannot save sample installation state");
                inventory->setCurrentItem(utc);
                require(
                    dialog->findChild<QLabel *>("dataValidUntil")->text().contains("2027-06-28"),
                    "Missing UTC expiry information");
                auto *allow = dialog->findChild<QCheckBox *>("allowEopExtrapolation");
                auto *days = dialog->findChild<QSpinBox *>("eopExtrapolationDays");
                require(allow && days && !allow->isVisible() && !priority->isVisible(),
                        "UTC must not expose EOP extrapolation controls");
                QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
                require(dialog->grab().save(smoke_dir_ + "/data-center-utc.png"),
                        "Cannot save UTC data details");
                inventory->setCurrentItem(finals);
                pages->setCurrentIndex(1);
                require(!allow->isChecked() && !days->isEnabled(),
                        "Extrapolation should default off");
                allow->setChecked(true);
                days->setValue(2);
                require(eop_catalog_->extrapolation_days(0) == 2,
                        "Extrapolation limit was not saved");
                inventory->setCurrentItem(c04);
                require(!allow->isChecked(), "EOP policies must be independent by product");
                inventory->setCurrentItem(finals);
                pages->setCurrentIndex(1);
                require(allow->isChecked() && days->value() == 2,
                        "Lost extrapolation setting on selection change");
                QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
                require(dialog->grab().save(smoke_dir_ + "/data-center-extrapolation.png"),
                        "Cannot save EOP policy details");
                allow->setChecked(false);
                inventory->setCurrentItem(c04);
                record("data-center-per-product-extrapolation-controls");
                QTimer::singleShot(0, [source] {
                    if (auto *file =
                            qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                        file->selectFile(source + "/manifest.json");
                        QMetaObject::invokeMethod(file, "accept");
                    }
                });
                pages->setCurrentIndex(2);
                dialog->findChild<QPushButton *>("importEopSnapshot")->click();
                require(eop_catalog_->providers().size() == 3, "Snapshot import failed");
                require(eop_->data_info().id.find("smoke-imported-") != 0,
                        "Import implicitly switched the active data");
                const auto imported_row =
                    eop_->data_info().interpolation.starts_with("finals2000a") ? 0 : 1;
                const auto imported =
                    eop_catalog_->inventory()[static_cast<std::size_t>(imported_row)];
                require(inventory->topLevelItemCount() == 3 &&
                            inventory->currentItem() == group->child(imported_row) &&
                            imported.provider_index == 2 && imported.acquired_at &&
                            imported.acquisition == runtime::DataAcquisition::imported &&
                            dialog->findChild<QLabel *>("dataAcquiredAt")->text() ==
                                QDateTime::fromSecsSinceEpoch(*imported.acquired_at)
                                    .toLocalTime()
                                    .toString("yyyy-MM-dd HH:mm"),
                        "Import must select its source with an acquisition timestamp");
                dialog->grab().save(smoke_dir_ + "/data-center-inventory.png");
                pages->setCurrentIndex(2);
                dialog->grab().save(smoke_dir_ + "/eop-data-dialog.png");
                inventory->setCurrentItem(finals);
                automatic->setChecked(false);
                automatic->setChecked(true);
                require(eop_catalog_->automatic_updates() && !eop_catalog_->downloading(),
                        "Update policy was not saved or unexpectedly started a download");
                automatic->setChecked(false);
                require(scenario_->data_lock() == smoke_data_lock_,
                        "Refreshing the data inventory changed the active data lock");
                record("data-inventory-acquisition-expiry-and-no-history-controls");
                dialog->reject();
            });
            eop_action_->trigger();
            answer_data(true);
            start_project(project_snapshot(project_path_), project_path_);
            project_cancellation_->store(true);
            ++orbit_smoke_stage_;
            break;
        }
        case 87:
            require(last_project_outcome_ == "cancelled" && !project_dirty_ &&
                        scenario_->data_lock() == smoke_data_lock_ &&
                        eop_->data_info().id.find("smoke-imported-") != 0,
                    "Cancelled reload changed the current calculation data");
            record("data-update-keeps-active-result-and-cancelled-reload");
            set_selection(0);
            open_properties();
            smoke_cache_ = orbit_;
            {
                auto edited_input = satellites_[0].orbit->input;
                edited_input.initial.position_m[0] *= 1.0001;
                answer_data(false);
                start_orbit(edited_input);
                require(!busy() && orbit_ == smoke_cache_ && last_orbit_outcome_ == "cancelled" &&
                            scenario_->data_lock() == smoke_data_lock_,
                        "Declining recalculation changed the scene");
                record("recalculation-data-change-decline-preserves-scene");
                answer_data(true);
                start_orbit(std::move(edited_input));
            }
            ++orbit_smoke_stage_;
            break;
        case 88: {
            require(
                last_project_outcome_ == "opened" && project_dirty_ &&
                    scenario_->data_lock() != smoke_data_lock_ &&
                    eop_->data_info().id.find("smoke-imported-") == 0 &&
                    last_orbit_outcome_ == "applied" &&
                    orbit_->session.provenance().input_revision ==
                        smoke_cache_->session.provenance().input_revision + 1 &&
                    orbit_->input.initial.position_m != smoke_cache_->input.initial.position_m &&
                    std::all_of(satellites_.begin(), satellites_.end(),
                                [](const auto &satellite) {
                                    return satellite.orbit->eop_source_id.find("smoke-imported-") !=
                                           std::string::npos;
                                }),
                "Recalculation did not use current data consistently for every object");
            record("recalculation-uses-current-data-and-rebuilds-all-tracks");
            save_project_to(smoke_dir_ + "/imported-data.orbitforge");
            // Recreate the catalog as on a fresh process; source files are no longer needed.
            QFile changed(smoke_dir_ + "/import-source/manifest.json");
            require(changed.open(QIODevice::WriteOnly | QIODevice::Truncate),
                    "Cannot change import source");
            changed.write("removed original source");
            changed.close();
            eop_catalog_ = std::make_unique<EopCatalog>(
                eop_catalog_->store(),
                std::vector<std::filesystem::path>{
                    std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) /
                        "resources/eop-current/manifest.json",
                    std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) /
                        "resources/eop/manifest.json"},
                std::filesystem::path{}, false);
            editor_->data_catalog = eop_catalog_.get();
            require(eop_catalog_->providers().size() == 3 && eop_catalog_->warnings().empty() &&
                        eop_catalog_->current_indices().size() == 2 &&
                        std::find(eop_catalog_->current_indices().begin(),
                                  eop_catalog_->current_indices().end(),
                                  2) != eop_catalog_->current_indices().end() &&
                        !eop_catalog_->automatic_updates(),
                    "Imported data did not survive catalog reconstruction");
            start_project(
                application::load_project(std::filesystem::path(project_path_.toStdWString())),
                project_path_);
            ++orbit_smoke_stage_;
            break;
        }
        case 89:
            require(last_project_outcome_ == "opened" && !project_dirty_ &&
                        eop_->data_info().id.find("smoke-imported-") == 0 &&
                        satellites_[0].name() == QStringLiteral("观测卫星 A"),
                    "Reopening could not use the current installed EOP data");
            record("data-catalog-reload-uses-current-data-after-source-removal");
            require(scenario_->data_lock() == application::eop_data_lock(eop_->data_info()) &&
                        scenario_->data_lock() ==
                            application::load_project(
                                std::filesystem::path(project_path_.toStdWString()))
                                .data_lock(),
                    "Restored scenario data lock differs from its file or active providers");
            open_scenario_properties();
            require(findChild<QLineEdit *>("scenarioDataHash")->text().toStdString() ==
                        scenario_->data_lock().sha256(),
                    "Scenario properties did not expose the bound data hash");
            scenario_properties_->close();
            record("calculation-data-provenance-survives-save-and-reopen");
            save_workbench_image("objects-and-data-restored.png");
            ++orbit_smoke_stage_;
            break;
        case 90: {
            const auto index = eop_catalog_->preferred_index();
            eop_catalog_->set_extrapolation_days(index, 2);
            const auto last = eop_catalog_->providers().at(index)->data_info().last_epoch;
            const core::Epoch first{last.tai_nanoseconds() + 3'600'000'000'000};
            application::Project extrapolated{
                "EOP extrapolation", first, core::Epoch{first.tai_nanoseconds() + 600'000'000'000}};
            application::ProjectSatellite satellite;
            satellite.orbit.initial.epoch = first;
            satellite.orbit.first = extrapolated.first;
            satellite.orbit.last = extrapolated.last;
            extrapolated.satellites.push_back(std::move(satellite));
            start_project(std::move(extrapolated), smoke_dir_ + "/extrapolated.orbitforge");
            ++orbit_smoke_stage_;
            break;
        }
        case 91: {
            require(last_project_outcome_ == "opened" && satellites_.size() == 1 &&
                        eop_->data_info().extrapolation_days == 2 &&
                        satellites_[0].orbit->eop_source_id.find(
                            "extrapolation=endpoint-linear-tai-v1") != std::string::npos,
                    "Extrapolated EOP could not prepare an actual orbit scene");
            open_scenario_properties();
            require(findChild<QLabel *>("scenarioExtrapolation")
                        ->text()
                        .contains(QStringLiteral("已使用 EOP 外推")),
                    "Scenario does not identify actual extrapolation");
            scenario_properties_->close();
            record("finite-eop-extrapolation-prepares-orbit-and-displays-provenance");
            save_project_to(smoke_dir_ + "/extrapolated.orbitforge");
            smoke_cache_ = orbit_;
            smoke_data_lock_ = scenario_->data_lock();
            const auto index = eop_catalog_->preferred_index();
            eop_catalog_->set_extrapolation_days(index, 0);
            answer_data(false, "dataCoverageDialog");
            require(!start_project(project_snapshot(project_path_), project_path_) &&
                        orbit_ == smoke_cache_,
                    "Disabled extrapolation reused a stored permission");
            eop_catalog_->set_extrapolation_days(index, 3);
            answer_data(false);
            require(!start_project(project_snapshot(project_path_), project_path_) &&
                        scenario_->data_lock() == smoke_data_lock_,
                    "Declining a changed extrapolation policy changed data");
            record("extrapolation-policy-change-and-disable-recheck-on-open");
            eop_catalog_->set_extrapolation_days(index, 2);
            start_project(
                application::load_project(std::filesystem::path(project_path_.toStdWString())),
                project_path_);
            ++orbit_smoke_stage_;
            break;
        }
        case 92:
            require(last_project_outcome_ == "opened" && !project_dirty_ &&
                        scenario_->data_lock() == smoke_data_lock_ &&
                        eop_->data_info().extrapolation_days == 2,
                    "Extrapolation provenance did not survive save/reopen");
            record("extrapolation-project-roundtrip-keeps-original-coverage-and-policy");
            ++orbit_smoke_stage_;
            break;
        case 93:
            // Closing a property page can queue a native-window resize. Let Qt/VSG
            // settle before comparing swapchain captures to current widget sizes.
            if (++smoke_final_frames_ >= 4) {
                save_workbench_image("extrapolated-scene.png");
                ++orbit_smoke_stage_;
            }
            break;
        case 94:
            start_project(application::load_project(std::filesystem::path(
                              (smoke_dir_ + "/imported-data.orbitforge").toStdWString())),
                          smoke_dir_ + "/imported-data.orbitforge");
            ++orbit_smoke_stage_;
            break;
        case 95: {
            require(last_project_outcome_ == "opened" && satellites_.size() >= 2,
                    "Scenario editing needs the restored multi-object fixture");
            set_selection(satellites_.size() - 1);
            smoke_cache_ = satellites_[0].orbit;
            const auto revision = scenario_->revision();
            open_scenario_properties();
            auto *name = scenario_properties_->findChild<QLineEdit *>("scenarioSettingsName");
            auto *first = scenario_epoch_control("scenarioSettingsStart");
            auto *last = scenario_epoch_control("scenarioSettingsEnd");
            auto *apply = scenario_properties_->findChild<QPushButton *>("applyScenarioSettings");
            auto *reset = scenario_properties_->findChild<QPushButton *>("resetScenarioSettings");
            auto *result = scenario_properties_->findChild<QLabel *>("scenarioSettingsResult");
            require(name && first && last && apply && reset && !name->isReadOnly(),
                    "Missing editable scenario settings");
            apply->click();
            require(!busy() && !project_dirty_ && scenario_->revision() == revision,
                    "Unchanged settings created a revision or dirty project");
            name->setText("  ");
            apply->click();
            require(!busy() && !project_dirty_ && result->isVisible(),
                    "Invalid name did not preserve the scene");
            reset->click();
            first->set_epoch(scenario_->last());
            last->set_epoch(scenario_->first());
            apply->click();
            require(!busy() && !project_dirty_ && scenario_->revision() == revision,
                    "Reversed interval changed the scenario");
            reset->click();
            require(first->epoch() == scenario_->first() && last->epoch() == scenario_->last(),
                    "Reset did not restore applied bounds");
            record("scenario-settings-invalid-noop-and-reset-preserve-state");
            name->setText(QStringLiteral("调整后的分析场景"));
            workspace_->select_view(0);
            open_scenario_properties();
            require(scenario_properties_->findChild<QLineEdit *>("scenarioSettingsName") == name &&
                        name->text() == QStringLiteral("调整后的分析场景"),
                    "Returning to scenario properties discarded an unapplied draft");
            record("scenario-settings-draft-survives-view-switch");
            set_playing(true);
            apply->click();
            require(!busy() && project_dirty_ && playing_ && satellites_[0].orbit == smoke_cache_ &&
                        scenario_->revision() == revision + 1 &&
                        scenario_item_->text(0) == name->text() &&
                        workspace_label_->text().startsWith(name->text()),
                    "Scenario rename rebuilt a cache or lost playback/title state");
            set_playing(false);
            record("scenario-rename-preserves-playback-caches-and-object-revisions");
            save_project_to(smoke_dir_ + "/scenario-edit-base.orbitforge");
            smoke_settings_project_ = project_snapshot(project_path_);
            const auto span =
                scenario_->last().tai_nanoseconds() - scenario_->first().tai_nanoseconds();
            smoke_view_epoch_ = core::Epoch{scenario_->first().tai_nanoseconds() + span / 4 + 123};
            first->set_epoch(smoke_view_epoch_);
            last->set_epoch(core::Epoch{scenario_->last().tai_nanoseconds() - span / 4});
            // The live time is before the candidate range; publication must clamp to its start.
            seek_epoch(scenario_->first());
            snapshot_ = sample_frame(scenario_->first());
            update_satellite();
            apply->click();
            require(busy() && !apply->isEnabled() && !reset->isEnabled(),
                    "Scenario edits did not disable concurrent submissions");
            ++orbit_smoke_stage_;
            break;
        }
        case 96: {
            require(last_project_outcome_ == "opened" && project_dirty_ &&
                        scenario_->first() == smoke_view_epoch_ &&
                        snapshot_.epoch == smoke_view_epoch_,
                    "Changed scenario bounds lost the clamped physical time");
            require(selected_ == satellites_.size() - 1,
                    "Changed scenario bounds lost the selected satellite");
            require(QFileInfo(project_path_) ==
                        QFileInfo(smoke_dir_ + "/scenario-edit-base.orbitforge"),
                    "Changed scenario bounds lost the project file identity");
            const auto &before = *smoke_settings_project_;
            for (std::size_t i = 0; i < satellites_.size(); ++i) {
                const auto &satellite = satellites_[i];
                const auto &settings_input = satellite.orbit->input;
                require(
                    settings_input.first == scenario_->first() &&
                        settings_input.last == scenario_->last() &&
                        settings_input.initial.epoch == before.satellites[i].orbit.initial.epoch &&
                        settings_input.initial.position_m ==
                            before.satellites[i].orbit.initial.position_m &&
                        settings_input.initial.velocity_mps ==
                            before.satellites[i].orbit.initial.velocity_mps &&
                        satellite.display == before.satellites[i].display &&
                        satellite.name().toStdString() == before.satellites[i].name(),
                    "Scenario resizing changed a satellite's physical input or display settings");
                require(scenario_->objects()[i].provenance().input_revision ==
                                satellite.orbit->session.provenance().input_revision &&
                            cache_covers(*satellite.orbit, snapshot_.epoch, snapshot_.epoch),
                        "New bounds did not have consistent sessions and display caches");
            }
            require(satellites_[0].orbit->session.provenance().input_revision ==
                            smoke_cache_->session.provenance().input_revision &&
                        timeline_->current_time() == snapshot_.epoch.tai_nanoseconds() / 1'000'000,
                    "Scenario resizing changed numerical revisions or left a stale timeline");
            record("scenario-resize-rebuilds-all-displays-preserves-inputs-and-clamps-start");
            open_scenario_properties();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
            require(scenario_properties_->grab().save(smoke_dir_ + "/scenario-settings.png"),
                    "Cannot capture scenario settings");
            scenario_properties_->close();
            save_project_to(smoke_dir_ + "/scenario-edited.orbitforge");
            smoke_settings_project_ = project_snapshot(project_path_);
            start_project(*smoke_settings_project_, project_path_);
            ++orbit_smoke_stage_;
            break;
        }
        case 97: {
            require(last_project_outcome_ == "opened" && !project_dirty_ &&
                        application::encode_project(project_snapshot(project_path_)) ==
                            application::encode_project(*smoke_settings_project_),
                    "Scenario settings did not survive reopening");
            record("scenario-edited-project-roundtrip");
            smoke_cache_ = satellites_[0].orbit;
            open_scenario_properties();
            auto *last = scenario_epoch_control("scenarioSettingsEnd");
            auto *apply = scenario_properties_->findChild<QPushButton *>("applyScenarioSettings");
            last->set_epoch(core::Epoch{eop_->data_info().last_epoch.tai_nanoseconds() +
                                        5 * 86'400'000'000'000LL});
            answer_data(false, "dataCoverageDialog");
            apply->click();
            require(!busy() && !project_dirty_ && satellites_[0].orbit == smoke_cache_,
                    "Unavailable data during scenario editing changed the live scene");
            for (auto index : eop_catalog_->current_indices())
                if (eop_catalog_->providers()[index]->data_info().interpolation ==
                    eop_->data_info().interpolation)
                    eop_catalog_->set_extrapolation_days(index, 1);
            last->set_epoch(
                core::Epoch{eop_->data_info().last_epoch.tai_nanoseconds() + 600'000'000'000});
            answer_data(false);
            apply->click();
            require(!busy() && !project_dirty_ && satellites_[0].orbit == smoke_cache_ &&
                        scenario_->data_lock() == smoke_settings_project_->data_lock(),
                    "Declining replacement data during scenario editing changed the live scene");
            record("scenario-edit-coverage-and-data-decline-preserve-original");
            answer_data(true);
            apply->click();
            require(busy(), "Approved scenario edit was not submitted");
            ++orbit_smoke_stage_;
            break;
        }
        case 98: {
            require(last_project_outcome_ == "opened" && project_dirty_ &&
                        eop_->data_info().extrapolation_days == 1 &&
                        scenario_->data_lock() != smoke_settings_project_->data_lock(),
                    "Scenario resizing did not apply the approved data policy");
            for (const auto &satellite : satellites_)
                require(satellite.orbit->input.last == scenario_->last() &&
                            satellite.orbit->eop_source_id.find("limit-days=1") !=
                                std::string::npos,
                        "Scenario editing mixed calculation policies");
            record("scenario-edit-data-consent-publishes-one-consistent-calculation");
            save_project_to(smoke_dir_ + "/scenario-edited.orbitforge");
            smoke_settings_project_ = project_snapshot(project_path_);
            smoke_cache_ = satellites_[0].orbit;
            open_scenario_properties();
            auto *first = scenario_epoch_control("scenarioSettingsStart");
            first->set_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 1'000'000'000});
            scenario_properties_->findChild<QPushButton *>("applyScenarioSettings")->click();
            require(busy() && cancel_project_button_->isVisible(),
                    "Missing scenario update cancellation");
            cancel_project_button_->click();
            ++orbit_smoke_stage_;
            break;
        }
        case 99: {
            require(last_project_outcome_ == "cancelled" && !project_dirty_ &&
                        satellites_[0].orbit == smoke_cache_ &&
                        scenario_->first() == smoke_settings_project_->first &&
                        scenario_properties_->findChild<QPushButton *>("applyScenarioSettings")
                            ->isEnabled(),
                    "Cancelled scenario editing changed state or left the form locked");
            record("scenario-edit-cancellation-preserves-scene-and-restores-form");
            auto oversized = project_snapshot(project_path_);
            oversized.satellites[0].orbit.step_seconds = 1;
            oversized.satellites[0].display.extent = 1;
            start_project(std::move(oversized), project_path_, true, selected_, std::nullopt,
                          ScenarioSettings{scenario_->name(), eop_->data_info().first_epoch,
                                           scenario_->last()});
            ++orbit_smoke_stage_;
            break;
        }
        case 100: {
            require(last_project_outcome_ == "rejected" && !project_dirty_ &&
                        satellites_[0].orbit == smoke_cache_ &&
                        application::encode_project(project_snapshot(project_path_)) ==
                            application::encode_project(*smoke_settings_project_),
                    "A failed scene preparation published part of a scenario edit");
            record("scenario-edit-preparation-failure-retains-all-applied-state");
            scenario_properties_->findChild<QPushButton *>("resetScenarioSettings")->click();
            seek_epoch(scenario_->last());
            snapshot_ = sample_frame(scenario_->last());
            update_satellite();
            smoke_view_epoch_ = core::Epoch{scenario_->last().tai_nanoseconds() - 123'456'789};
            scenario_epoch_control("scenarioSettingsEnd")->set_epoch(smoke_view_epoch_);
            scenario_properties_->findChild<QPushButton *>("applyScenarioSettings")->click();
            ++orbit_smoke_stage_;
            break;
        }
        case 101:
            require(last_project_outcome_ == "opened" && project_dirty_ &&
                        snapshot_.epoch == smoke_view_epoch_ &&
                        scenario_->last() == smoke_view_epoch_,
                    "Scenario shrinking failed to clamp current time to the exact final epoch");
            record("scenario-edit-clamps-end-with-nanosecond-precision");
            save_project_to(smoke_dir_ + "/scenario-edited.orbitforge");
            // A longer interval can put the final marker behind Earth from the
            // retained camera. Frame its current position for the pixel checks.
            fit_views();
            smoke_final_frames_ = 0;
            ++orbit_smoke_stage_;
            break;
        case 102:
            if (++smoke_final_frames_ >= 4) {
                save_workbench_image("scenario-edited-workspace.png");
                ++orbit_smoke_stage_;
            }
            break;
        case 103: {
            timer_.stop();
            QTimer::singleShot(0, this, [this, require] {
                auto *dialog = findChild<QDialog *>("insertObjectsDialog");
                require(dialog, "Missing insertion chooser for ground station");
                auto *types = dialog->findChild<QTreeWidget *>("insertObjectTypes");
                types->setCurrentItem(types->topLevelItem(0)->child(1));
                require(dialog->grab().save(smoke_dir_ + "/insert-ground-station.png"),
                        "Cannot capture station chooser");
                dialog->findChild<QPushButton *>("insertObject")->click();
            });
            show_insert_objects();
            timer_.start();
            require(station_editor_ && workspace_->current_document() == station_editor_ &&
                        stations_.empty(),
                    "Station insertion must open an uncommitted property page");
            station_editor_->findChild<QLineEdit *>("stationName")
                ->setText(QStringLiteral("海岸观测站"));
            station_editor_->findChild<QuantityEditor *>("stationLongitude")
                ->set_value(114.2, Unit::degree);
            station_editor_->findChild<QuantityEditor *>("stationLatitude")
                ->set_value(22.3, Unit::degree);
            station_editor_->findChild<QuantityEditor *>("stationHeight")
                ->set_value(-200, Unit::metre);
            station_editor_->findChild<QPushButton *>("applyGroundStation")->click();
            require(busy(), "Station creation did not start");
            record("ground-station-insert-via-object-chooser");
            ++orbit_smoke_stage_;
            break;
        }
        case 104: {
            orbit_capture_ = true;
            require(last_project_outcome_ == "opened" && stations_.size() == 1 &&
                        selected_station_ == 0 && !selected_ &&
                        stations_[0].config.location.height_m == -200 &&
                        scenario_->objects().size() == satellites_.size() + 1,
                    "Station creation did not publish the model and selection together");
            const auto index = satellites_.size();
            require(QLineF(ground_map_->marker_coordinates(index), QPointF(114.2, 22.3)).length() <
                        1e-8,
                    "Station map location uses incorrect geodetic coordinates");
            change_display_frame(1);
            const auto displayed = snapshot_.markers.at(index).position_m;
            const auto exact = core::ground_station_position(stations_[0].config.location);
            double offset = 0;
            for (std::size_t i = 0; i < 3; ++i)
                offset += std::pow(displayed[i] - exact[i], 2);
            require(std::abs(std::sqrt(offset) - 300) < 1e-6,
                    "Station display clearance changed the numerical height");
            workspace_->select_view(0);
            focus_satellite(0);
            const auto back =
                -vsg::normalize(vsg::dvec3(displayed[0], displayed[1], displayed[2])) * 14e6;
            slots_[1]->trackball->setViewpoint(
                vsg::LookAt::create(back, vsg::dvec3{}, vsg::dvec3(0, 0, 1)), 0);
            set_selection(std::nullopt);
            smoke_final_frames_ = 0;
            record("ground-station-negative-height-keeps-exact-computation");
            ++orbit_smoke_stage_;
            break;
        }
        case 105: {
            if (++smoke_final_frames_ < 4)
                break;
            const auto &candidates = slots_[0]->pick_candidates;
            const auto found =
                std::find_if(candidates.begin(), candidates.end(),
                             [&](const auto &point) { return point.index == satellites_.size(); });
            require(found != candidates.end(), "Front-facing ground station is not pickable in 3D");
            require(slots_[0]->station_markers->size() == 1 &&
                        slots_[1]->station_markers->size() == 0,
                    "Ground station annotations ignored per-view Earth occlusion");
            int teal = 0;
            const auto &pixels = slots_[0]->captured_image;
            require(pixels.save(smoke_dir_ + "/ground-station-glyph.png"),
                    "Cannot capture the station glyph");
            for (int y = static_cast<int>(found->y_pixels) - 5;
                 y <= static_cast<int>(found->y_pixels) + 5; ++y)
                for (int x = static_cast<int>(found->x_pixels) - 5;
                     x <= static_cast<int>(found->x_pixels) + 5; ++x) {
                    if (!pixels.rect().contains(x, y))
                        continue;
                    const auto color = pixels.pixelColor(x, y);
                    if (color.green() > 180 && color.blue() > 120 &&
                        color.red() < color.green() * 0.7)
                        ++teal;
                }
            require(teal >= 6, "Ground station glyph was not rendered above the surface");
            inject_click(*slots_[0], {found->x_pixels, found->y_pixels});
            require(selected_station_ == 0 && objects_->currentItem() == stations_[0].item,
                    "3D ground station pick selected a satellite");
            record("ground-station-visible-glyph-and-3d-pick");
            open_properties();
            smoke_project_bytes_ = application::encode_project(project_snapshot(project_path_));
            auto *latitude = station_editor_->findChild<QuantityEditor *>("stationLatitude");
            latitude->findChild<QLineEdit *>("quantityValue")->setText("95");
            station_editor_->findChild<QPushButton *>("applyGroundStation")->click();
            require(!busy() && smoke_project_bytes_ ==
                                   application::encode_project(project_snapshot(project_path_)),
                    "Invalid station latitude changed the scene");
            station_editor_->findChild<QPushButton *>("resetGroundStation")->click();
            station_editor_->findChild<QuantityEditor *>("stationHeight")
                ->set_value(0.35, Unit::kilometre);
            station_editor_->findChild<QPushButton *>("applyGroundStation")->click();
            record("ground-station-invalid-input-reset-and-unit-edit");
            ++orbit_smoke_stage_;
            break;
        }
        case 106: {
            const auto found = std::find_if(
                scenario_->objects().begin(), scenario_->objects().end(), [&](const auto &session) {
                    return session.provenance().object_id == stations_[0].config.object_id;
                });
            require(
                last_project_outcome_ == "opened" && stations_[0].config.location.height_m == 350 &&
                    found != scenario_->objects().end() && found->provenance().input_revision == 2,
                "Editing station position failed to revise its numerical session");
            rename_selected_to(QStringLiteral("北岸地面站"));
            ++orbit_smoke_stage_;
            break;
        }
        case 107:
            require(last_project_outcome_ == "opened" && stations_[0].config.name() == "北岸地面站",
                    "Station rename failed");
            duplicate_action_->trigger();
            ++orbit_smoke_stage_;
            break;
        case 108:
            require(last_project_outcome_ == "opened" && stations_.size() == 2 &&
                        selected_station_ == 1 &&
                        stations_[0].config.object_id != stations_[1].config.object_id &&
                        stations_[0].config.location == stations_[1].config.location,
                    "Station copy did not preserve location and create a separate identity");
            confirm_delete(false);
            require(!busy() && stations_.size() == 2, "Cancelled deletion removed a station");
            confirm_delete(true);
            ++orbit_smoke_stage_;
            break;
        case 109:
            require(last_project_outcome_ == "opened" && stations_.size() == 1 &&
                        selected_station_ == 0,
                    "Station deletion lost the surviving station selection");
            record("ground-station-rename-copy-and-delete");
            save_project_to(smoke_dir_ + "/ground-stations.orbitforge");
            start_project(
                application::load_project(std::filesystem::path(project_path_.toStdWString())),
                project_path_);
            ++orbit_smoke_stage_;
            break;
        case 110: {
            require(last_project_outcome_ == "opened" && stations_.size() == 1 &&
                        !selected_marker_index() && stations_[0].config.location.height_m == 350 &&
                        stations_[0].config.name() == "北岸地面站",
                    "Station project failed to reopen with its saved coordinates and name");
            auto *search = findChild<QLineEdit *>("objectSearch");
            search->setText(QStringLiteral("北岸"));
            require(!stations_[0].item->isHidden() && satellites_[0].item->isHidden(),
                    "Object search omitted ground stations");
            search->clear();
            workspace_->split_page(map_panel_, view_panel_, Qt::RightEdge);
            ground_map_->reset_view();
            const auto index = satellites_.size();
            const auto point = ground_map_->marker_position(index);
            const auto global = ground_map_->mapToGlobal(point);
            QMouseEvent press(QEvent::MouseButtonPress, point, global, Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, point, global, Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(ground_map_, &press);
            QCoreApplication::sendEvent(ground_map_, &release);
            require(selected_station_ == 0 && objects_->currentItem() == stations_[0].item,
                    "2D station pick failed to synchronize the scene tree");
            const auto geographic = ground_map_->marker_coordinates(index);
            change_display_frame(0);
            seek_epoch(scenario_->first());
            snapshot_ = sample_frame(scenario_->first());
            update_satellite();
            const auto start = snapshot_.markers.at(index).position_m;
            seek_epoch(scenario_->last());
            snapshot_ = sample_frame(scenario_->last());
            update_satellite();
            require(start != snapshot_.markers.at(index).position_m &&
                        QLineF(geographic, ground_map_->marker_coordinates(index)).length() < 1e-8,
                    "Ground station did not rotate in GCRF while staying fixed on the map");
            focus_satellite();
            open_properties();
            record("ground-station-reopen-search-2d-pick-and-earth-rotation");
            smoke_final_frames_ = 0;
            ++orbit_smoke_stage_;
            break;
        }
        case 111: {
            if (++smoke_final_frames_ < 4)
                break;
            save_workbench_image("ground-station-properties.png");
            require(station_editor_->findChild<QPushButton *>("applyGroundStation")->isVisible(),
                    "Station apply button is hidden at this scale");
            station_editor_->close();
            const auto first = core::Epoch{scenario_->first().tai_nanoseconds() + 1'000'000'000};
            require(apply_scenario_settings({scenario_->name(), first, scenario_->last()}),
                    "Scenario interval edit rejected a mixed station project");
            ++orbit_smoke_stage_;
            break;
        }
        case 112: {
            require(last_project_outcome_ == "opened" && selected_station_ == 0 &&
                        stations_.size() == 1,
                    "Scenario interval edit lost its station or selection");
            save_workbench_image("ground-station-map-and-globe.png");
            auto only_stations = project_snapshot(project_path_);
            only_stations.satellites.clear();
            start_project(std::move(only_stations), project_path_, true, 0);
            ++orbit_smoke_stage_;
            break;
        }
        case 113:
            require(last_project_outcome_ == "opened" && !orbit_ && satellites_.empty() &&
                        stations_.size() == 1 && selected_station_ == 0 &&
                        snapshot_.markers.size() == 1,
                    "Station-only scenario incorrectly requires a satellite");
            begin_insert_station(InsertObjectMethod::defaults);
            ++orbit_smoke_stage_;
            break;
        case 114:
            require(last_project_outcome_ == "opened" && stations_.size() == 2 &&
                        selected_station_ == 1 &&
                        stations_[1].config.location == core::GroundStationLocation{},
                    "Default station insertion failed in a station-only scenario");
            begin_insert_satellite(InsertObjectMethod::defaults);
            ++orbit_smoke_stage_;
            break;
        case 115:
            require(last_orbit_outcome_ == "applied" && satellites_.size() == 1 &&
                        stations_.size() == 2 && snapshot_.markers.size() == 3 && selected_ == 0 &&
                        snapshot_.markers[1].object_id == stations_[0].config.object_id &&
                        snapshot_.markers[2].object_id == stations_[1].config.object_id,
                    "Inserting a satellite changed ground station marker identities");
            confirm_delete(true);
            ++orbit_smoke_stage_;
            break;
        case 116:
            require(last_project_outcome_ == "opened" && satellites_.empty() &&
                        stations_.size() == 2 && selected_station_ == 0,
                    "Deleting the last satellite failed to retain station objects");
            confirm_delete(true);
            ++orbit_smoke_stage_;
            break;
        case 117:
            require(last_project_outcome_ == "opened" && stations_.size() == 1 &&
                        selected_station_ == 0,
                    "Station-only deletion lost the surviving object");
            confirm_delete(true);
            ++orbit_smoke_stage_;
            break;
        case 118:
            require(last_project_outcome_ == "opened" && scenario_->objects().empty() &&
                        !selected_marker_index() && snapshot_.markers.empty() &&
                        markers_->size() == 0 && !delete_action_->isEnabled(),
                    "Deleting the last station left a ghost marker or enabled object action");
            record("ground-station-only-defaults-mixed-insertion-and-delete-last");
            start_project(application::load_project(std::filesystem::path(
                              (smoke_dir_ + "/scenario-edited.orbitforge").toStdWString())),
                          smoke_dir_ + "/scenario-edited.orbitforge");
            ++orbit_smoke_stage_;
            break;
        case 119:
            require(last_project_outcome_ == "opened" && stations_.empty() && !satellites_.empty(),
                    "Opening a satellite-only project retained old station objects");
            fit_views();
            smoke_final_frames_ = 0;
            ++orbit_smoke_stage_;
            break;
        case 120:
            if (++smoke_final_frames_ >= 4)
                ++orbit_smoke_stage_;
            break;
        case 121:
            report_action_->trigger();
            require(report_dialog_ && workspace_->contains(report_dialog_),
                    "Report did not open in the workspace");
            choose_report("builtin:2");
            report_dialog_->findChild<QComboBox *>("reportFrame")->setCurrentIndex(1);
            report_dialog_->findChild<QDoubleSpinBox *>("reportStep")->setValue(600);
            QTimer::singleShot(0, [] {
                auto *naming = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                if (!naming)
                    throw std::runtime_error("Report template naming did not open");
                naming->setTextValue(QStringLiteral("轨道位置报告"));
                naming->accept();
            });
            report_dialog_->findChild<QAction *>("saveReport")->trigger();
            require(reports_.size() == 1 && project_dirty_,
                    "Report configuration was not attached to the project");
            smoke_final_frames_ = 0;
            ++orbit_smoke_stage_;
            break;
        case 122:
            if (++smoke_final_frames_ < 4)
                break;
            require(workspace_->rect().contains(
                        report_dialog_->findChild<QPushButton *>("generateReport")
                            ->mapTo(workspace_,
                                    report_dialog_->findChild<QPushButton *>("generateReport")
                                        ->rect()
                                        .bottomRight())),
                    "Report generation action is clipped by the workspace");
            save_workbench_image("report-configuration.png");
            report_dialog_->findChild<QPushButton *>("generateReport")->click();
            ++orbit_smoke_stage_;
            break;
        case 123: {
            if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
                break;
            const auto *table = report_dialog_->findChild<QTableView *>("reportTable");
            require(table->model()->rowCount() > 0, "Report generation did not produce a table");
            save_workbench_image("report-results.png");
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
            QTimer::singleShot(0, this, [this, require] {
                auto *chooser = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
                require(chooser != nullptr, "CSV export did not open a file chooser");
                chooser->selectFile(smoke_dir_ + "/report.csv");
                static_cast<QDialog *>(chooser)->accept();
            });
            report_dialog_->findChild<QPushButton *>("exportReportCsv")->click();
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, false);
            ++orbit_smoke_stage_;
            break;
        }
        case 124: {
            if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
                break;
            QFile csv(smoke_dir_ + "/report.csv");
            require(csv.open(QIODevice::ReadOnly) && csv.readAll().contains("Time (UTC)"),
                    "Report CSV was not exported");
            set_selection(0);
            rename_selected_to(QStringLiteral("报告测试卫星"));
            require(report_dialog_->findChild<QLabel *>("reportResultLabel")
                        ->text()
                        .contains(QStringLiteral("场景已变化")),
                    "Report did not mark changed inputs as stale");
            save_project_to(smoke_dir_ + "/reports.orbitforge");
            const auto restored =
                application::load_project(std::filesystem::path(project_path_.toStdWString()));
            require(restored.reports.size() == 1 &&
                        restored.reports[0].query.step_nanoseconds == 600'000'000'000,
                    "Saved report configuration lost its sampling interval");
            start_project(restored, project_path_);
            record("reports-workspace-generate-csv-stale-and-project-save");
            ++orbit_smoke_stage_;
            break;
        }
        case 125:
            require(last_project_outcome_ == "opened" && reports_.size() == 1,
                    "Project reopen lost report configurations");
            report_action_->trigger();
            choose_report("template:" + QString::fromStdString(reports_.front().id));
            report_dialog_->findChild<QPushButton *>("generateReport")->click();
            ++orbit_smoke_stage_;
            break;
        case 126: {
            auto *generate = report_dialog_->findChild<QPushButton *>("generateReport");
            if (!generate->isEnabled())
                break;
            require(report_dialog_->findChild<QTableView *>("reportTable")->model()->rowCount() > 0,
                    "Reopened report could not regenerate");
            smoke_data_lock_ = scenario_->data_lock();
            for (const auto index : eop_catalog_->current_indices()) {
                const auto &info = eop_catalog_->providers()[index]->data_info();
                if (info.id == eop_->data_info().id && info.sha256 == eop_->data_info().sha256)
                    eop_catalog_->set_extrapolation_days(
                        index, eop_->data_info().extrapolation_days == 30
                                   ? 29
                                   : eop_->data_info().extrapolation_days + 1);
            }
            answer_data(false);
            generate->click();
            require(generate->isEnabled() && scenario_->data_lock() == smoke_data_lock_,
                    "Declined report data update changed the active scenario");
            answer_data(true);
            generate->click();
            ++orbit_smoke_stage_;
            break;
        }
        case 127:
            if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
                break;
            require(last_project_outcome_ == "opened" &&
                        scenario_->data_lock() != smoke_data_lock_ && project_dirty_ &&
                        report_dialog_->findChild<QLabel *>("reportResultLabel")
                            ->text()
                            .contains(QStringLiteral("与当前场景一致")),
                    "Accepted report data update did not rebuild and resume generation");
            record("reports-reopen-and-confirm-current-data-before-regeneration");
            workspace_->close_page(report_dialog_);
            workspace_->select_view(0);
            fit_views();
            smoke_final_frames_ = 0;
            ++orbit_smoke_stage_;
            break;
        case 128:
            if (++smoke_final_frames_ >= 4)
                ++orbit_smoke_stage_;
            break;
        case 129: {
            auto *item = satellites_.at(1).item;
            objects_->scrollToItem(item);
            QTimer::singleShot(0, this, [this, require] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                require(menu != nullptr, "Object report context menu did not open");
                auto *action = menu->findChild<QAction *>("openObjectReport");
                require(action != nullptr, "Object report action is missing");
                // The clicked object's stable ID must survive unrelated selection changes.
                set_selection(0);
                action->trigger();
                menu->close();
            });
            objects_->customContextMenuRequested(objects_->visualItemRect(item).center());
            require(report_dialog_->findChild<QLabel *>("reportScopeLabel")->text() ==
                        item->text(0),
                    "Context report did not bind the right-clicked object");
            choose_report("builtin:1");
            QTimer::singleShot(0, [] {
                auto *naming = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                if (!naming)
                    throw std::runtime_error("Object template naming did not open");
                naming->setTextValue(QStringLiteral("对象报告模板"));
                naming->accept();
            });
            report_dialog_->findChild<QAction *>("saveReport")->trigger();
            require(reports_.back().query.object_ids ==
                        std::vector<std::string>{satellites_.at(1).orbit->input.object_id},
                    "Changing report type widened the object scope");
            report_dialog_->findChild<QPushButton *>("generateReport")->click();
            ++orbit_smoke_stage_;
            break;
        }
        case 130: {
            if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
                break;
            auto *model = report_dialog_->findChild<QTableView *>("reportTable")->model();
            require(model->rowCount() == 1 &&
                        model->data(model->index(0, 0)).toString() ==
                            QString::fromStdString(satellites_.at(1).orbit->input.object_id),
                    "Object report included an unselected object");
            save_workbench_image("object-report-results.png");
            record("object-report-context-scope-survives-template-change");
            choose_report(QStringLiteral("builtin:%1")
                              .arg(static_cast<int>(application::ReportKind::classical_elements)));
            report_dialog_->findChild<QPushButton *>("generateReport")->click();
            ++orbit_smoke_stage_;
            break;
        }
        case 131: {
            if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
                break;
            auto *views = report_dialog_->findChild<QTabBar *>("reportViews");
            require(views->isTabEnabled(1), "Classical elements did not expose a graph");
            views->setCurrentIndex(1);
            ++orbit_smoke_stage_;
            break;
        }
        case 132: {
            auto *plot = report_dialog_->findChild<QWidget *>("reportPlot");
            require(plot && plot->isVisible() && plot->width() > 200 && plot->height() > 150,
                    "Report graph is not usable inside the workbench");
            auto *fields = report_dialog_->findChild<QComboBox *>("reportPlotField");
            require(fields->count() == 6 &&
                        fields->currentData(Qt::UserRole + 1).toString() == "elements.nu",
                    "Classical graph fields or default angle are incorrect");
            save_workbench_image("report-classical-curves.png");
            record("report-classical-elements-and-curves");
            workspace_->close_page(report_dialog_);
            workspace_->select_view(0);
            fit_views();
            smoke_final_frames_ = 0;
            ++orbit_smoke_stage_;
            break;
        }
        case 133:
            if (++smoke_final_frames_ >= 4)
                ++orbit_smoke_stage_;
            break;
        case 134: {
            set_selection(0);
            open_properties();
            auto *navigation = editor_->findChild<QTreeWidget *>("propertyNavigation");
            navigation->setCurrentItem(navigation->topLevelItem(0)->child(1));
            const auto orbit_before = satellites_.front().orbit;
            const auto geometry_before = satellites_.front().fixed_track->node();
            const auto other = satellites_.size() > 1 ? satellites_[1].display : DisplaySettings{};
            auto settings = satellites_.front().display;
            settings.track_2d = settings.track_3d = true;
            editor_->set_display(settings);
            editor_->findChild<QToolButton *>("trackColor2")->click();
            editor_->findChild<QComboBox *>("trackLineStyle")->setCurrentIndex(1);
            editor_->findChild<QDoubleSpinBox *>("trackLineWidth")->setValue(4);
            editor_->findChild<QPushButton *>("applyDisplay")->click();
            require(satellites_.front().display.line_color == 0xe9a2ba &&
                        satellites_.front().display.line_style == application::LineStyle::dashed &&
                        satellites_.front().display.line_width == 4 && project_dirty_ &&
                        satellites_.front().orbit == orbit_before &&
                        satellites_.front().fixed_track->node() == geometry_before &&
                        (satellites_.size() < 2 || satellites_[1].display == other),
                    "Track appearance rebuilt the orbit, lost settings or changed another object");
            save_workbench_image("satellite-style-editor.png");
            workspace_->close_page(properties_dialog_);
            workspace_->select_view(0);
            smoke_final_frames_ = 0;
            record("satellite-style-applied-without-recomputing-orbit");
            ++orbit_smoke_stage_;
            break;
        }
        case 135:
            if (++smoke_final_frames_ < 4 || !ground_map_->tracks_ready())
                break;
            save_workbench_image("satellite-style-globe.png");
            ground_map_->grab().save(smoke_dir_ + "/satellite-style-map.png");
            save_project_to(smoke_dir_ + "/satellite-styles.orbitforge");
            start_project(
                application::load_project(std::filesystem::path(project_path_.toStdWString())),
                project_path_);
            ++orbit_smoke_stage_;
            break;
        case 136:
            require(last_project_outcome_ == "opened" &&
                        satellites_.front().display.line_color == 0xe9a2ba &&
                        satellites_.front().display.line_style == application::LineStyle::dashed &&
                        satellites_.front().display.line_width == 4,
                    "Project reopen lost satellite track appearance");
            record("satellite-style-survives-project-reopen");
            fit_views();
            smoke_final_frames_ = 0;
            ++orbit_smoke_stage_;
            break;
        case 137:
            if (++smoke_final_frames_ >= 4)
                ++orbit_smoke_stage_;
            break;
        case 138: {
            auto p = project_snapshot(project_path_);
            require(!p.satellites.empty(), "Access smoke requires a satellite");
            if (p.ground_stations.empty())
                p.ground_stations.push_back({"access-station", "测控站", {}});
            const std::array epochs{p.first};
            const auto state = core::transform_state(
                runtime::make_orbit_session(p.satellites[0].orbit, 1, active_time().get())
                    .sample(epochs)
                    .states()[0],
                core::FrameId::itrf, *frames_, eop_->at(p.first));
            const auto location = runtime::itrf_to_wgs84(state);
            p.ground_stations[0].location = {location.longitude_rad, location.latitude_rad, 0};
            p.ground_stations[0].minimum_elevation_rad = 5 * std::numbers::pi / 180;
            start_project(std::move(p), project_path_, true, 0);
            ++orbit_smoke_stage_;
            break;
        }
        case 139: {
            if (project_candidate_.valid())
                break;
            if (!sensor_editor_ || workspace_->current_document() != sensor_editor_) {
                set_selection(0);
                edit_sensor(true);
                sensor_editor_->findChild<QLineEdit *>("sensorName")
                    ->setText(QStringLiteral("对地观测仪"));
                sensor_editor_->findChild<QuantityEditor *>("sensorHalfAngle")
                    ->set_value(20, Unit::degree);
                // Inspect the embedded layout on the next frame, after resize events settle.
                break;
            }
            auto *scroll = sensor_editor_->findChild<QScrollArea *>("sensorScroll");
            auto *submit = sensor_editor_->findChild<QPushButton *>("applySensor");
            require(scroll && submit && scroll->horizontalScrollBar()->maximum() == 0,
                    "Embedded sensor editor requires horizontal scrolling");
            require(sensor_editor_->rect().contains(
                        QRect(submit->mapTo(sensor_editor_, QPoint{}), submit->size())),
                    "Sensor action is clipped in the workspace");
            save_workbench_image("sensor-editor.png");
            const auto scroll_details =
                "Sensor horizontal scrollbar is visible after layout: maximum=" +
                std::to_string(scroll->horizontalScrollBar()->maximum()) +
                ", viewport=" + std::to_string(scroll->viewport()->width()) +
                ", contents=" + std::to_string(scroll->widget()->width());
            require(!scroll->horizontalScrollBar()->isVisible(), scroll_details.c_str());
            require(scroll->widget()->width() <= scroll->viewport()->width(),
                    "Sensor form is wider than its workspace viewport");
            record("sensor-editor-embedded-layout");
            submit->click();
            ++orbit_smoke_stage_;
            break;
        }
        case 140: {
            if (project_candidate_.valid())
                break;
            require(sensors_.size() == 1 && satellites_[0].item->childCount() == 1,
                    "Sensor was not attached to its satellite");
            save_workbench_image("sensor-inserted.png");
            require(satellites_[0].item->isExpanded() &&
                        !objects_->visualItemRect(satellites_[0].item->child(0)).isEmpty(),
                    "Inserted sensor is hidden under a collapsed satellite");
            require(selected_sensor() && selected_sensor()->object_id == sensors_[0].object_id &&
                        objects_->currentItem() == satellites_[0].item->child(0),
                    "Inserting a sensor did not select the new child");
            record("sensor-insert-reveals-and-selects-child");
            open_properties();
            require(selected_sensor() && workspace_->current_document() == sensor_editor_,
                    "Sensor properties opened the parent");
            sensor_editor_->findChild<QLineEdit *>("sensorName")->setText("Discarded draft");
            sensor_editor_->close();
            require(sensors_[0].name() != "Discarded draft",
                    "Closing sensor editor applied a draft");
            record("sensor-child-properties-and-cancel");
            show_reports();
            report_dialog_->select_object(sensors_[0].object_id,
                                          application::ReportKind::access_intervals);
            report_dialog_->findChild<QCheckBox *>("reportFollowScenario")->setChecked(false);
            static_cast<EpochEditor *>(report_dialog_->findChild<QWidget *>("reportFirst"))
                ->set_epoch(scenario_->first());
            static_cast<EpochEditor *>(report_dialog_->findChild<QWidget *>("reportLast"))
                ->set_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 600'000'000'000});
            report_dialog_->findChild<QPushButton *>("generateReport")->click();
            ++orbit_smoke_stage_;
            break;
        }
        case 141: {
            if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
                break;
            auto *table = report_dialog_->findChild<QTableView *>("reportTable");
            require(table->model()->rowCount() > 0 && access_session_.has_value(),
                    "Access report produced no overhead pass");
            const auto index = table->model()->index(0, 2);
            table->doubleClicked(index);
            record("access-windows-timeline-and-seek");
            save_workbench_image("access-windows.png");
            choose_report(
                QStringLiteral("builtin:%1").arg(static_cast<int>(application::ReportKind::aer)));
            report_dialog_->findChild<QDoubleSpinBox *>("reportStep")->setValue(10);
            report_dialog_->findChild<QCheckBox *>("reportFollowScenario")->setChecked(false);
            static_cast<EpochEditor *>(report_dialog_->findChild<QWidget *>("reportFirst"))
                ->set_epoch(scenario_->first());
            static_cast<EpochEditor *>(report_dialog_->findChild<QWidget *>("reportLast"))
                ->set_epoch(core::Epoch{scenario_->first().tai_nanoseconds() + 600'000'000'000});
            report_dialog_->findChild<QPushButton *>("generateReport")->click();
            ++orbit_smoke_stage_;
            break;
        }
        case 142:
            if (!report_dialog_->findChild<QPushButton *>("generateReport")->isEnabled())
                break;
            require(snapshot_.epoch == scenario_->first() &&
                        ground_map_->epoch() == scenario_->first(),
                    "Access result seek did not reach both scene and map");
            require(report_dialog_->findChild<QTableView *>("reportTable")->model()->rowCount() > 1,
                    "AER report was not generated");
            report_dialog_->findChild<QTabBar *>("reportViews")->setCurrentIndex(1);
            record("aer-sensor-curve");
            save_workbench_image("access-aer.png");
            save_project_to(smoke_dir_ + "/access-sensors.orbitforge");
            start_project(application::load_project(std::filesystem::path(
                              (smoke_dir_ + "/access-sensors.orbitforge").toStdWString())),
                          smoke_dir_ + "/access-sensors.orbitforge");
            ++orbit_smoke_stage_;
            break;
        case 143:
            if (project_candidate_.valid())
                break;
            require(sensors_.size() == 1 && satellites_[0].item->childCount() == 1 &&
                        std::abs(sensors_[0].half_angle_rad - 20 * std::numbers::pi / 180) < 1e-12,
                    "Reopening lost sensor configuration");
            require(satellites_[0].item->isExpanded() &&
                        !objects_->visualItemRect(satellites_[0].item->child(0)).isEmpty(),
                    "Reopening hid the saved sensor");
            record("sensor-project-reopen");
            findChild<QLineEdit *>("objectSearch")->setText(QStringLiteral("观测仪"));
            objects_->setCurrentItem(satellites_[0].item->child(0));
            open_properties();
            sensor_editor_->findChild<QuantityEditor *>("sensorHalfAngle")
                ->set_value(25, Unit::degree);
            sensor_editor_->findChild<QPushButton *>("applySensor")->click();
            ++orbit_smoke_stage_;
            break;
        case 144:
            require(selected_sensor() && selected_sensor()->object_id == sensors_[0].object_id &&
                        std::abs(selected_sensor()->half_angle_rad - 25 * std::numbers::pi / 180) <
                            1e-12 &&
                        !satellites_[0].item->isHidden() && satellites_[0].item->isExpanded() &&
                        satellites_[1].item->isHidden() && stations_[0].item->isHidden(),
                    "Sensor edit lost the child selection or active search filter");
            record("sensor-edit-preserves-selection-and-search");
            rename_selected_to(QStringLiteral("对地观测仪重命名"));
            ++orbit_smoke_stage_;
            break;
        case 145:
            require(selected_sensor() && selected_sensor()->object_id == sensors_[0].object_id &&
                        selected_name() == QStringLiteral("对地观测仪重命名") &&
                        objects_->currentItem()->text(0) == selected_name(),
                    "Sensor rename did not refresh the selected child");
            record("sensor-rename-refreshes-selected-child");
            duplicate_selected();
            ++orbit_smoke_stage_;
            break;
        case 146:
            require(sensors_.size() == 2 && satellites_[0].item->childCount() == 2 &&
                        selected_sensor() &&
                        selected_sensor()->object_id == sensors_[1].object_id &&
                        selected_sensor()->object_id != sensors_[0].object_id &&
                        !objects_->visualItemRect(objects_->currentItem()).isEmpty() &&
                        satellites_[1].item->isHidden() && stations_[0].item->isHidden(),
                    "Sensor copy was not revealed and selected under the active filter");
            record("sensor-copy-reveals-new-child-under-search");
            findChild<QLineEdit *>("objectSearch")->clear();
            satellites_[0].item->setExpanded(false);
            set_selection(satellites_.size());
            rename_selected_to(QStringLiteral("测控站 · 更新"));
            ++orbit_smoke_stage_;
            break;
        case 147:
            require(!satellites_[0].item->isExpanded() && selected_station_ == 0 &&
                        stations_[0].name() == QStringLiteral("测控站 · 更新"),
                    "Updating a station reset another object's collapsed state");
            findChild<QLineEdit *>("objectSearch")->setText(QStringLiteral("副本"));
            require(satellites_[0].item->isExpanded() && !satellites_[0].item->isHidden() &&
                        satellites_[0].item->child(0)->isHidden() &&
                        !satellites_[0].item->child(1)->isHidden() && stations_[0].item->isHidden(),
                    "Sensor search did not reveal only the matching child and its parent");
            objects_->setCurrentItem(satellites_[0].item->child(1));
            confirm_delete(false);
            require(sensors_.size() == 2 && selected_sensor() && !busy(),
                    "Cancelling sensor deletion changed the tree");
            confirm_delete(true);
            ++orbit_smoke_stage_;
            break;
        case 148:
            require(sensors_.size() == 1 && satellites_[0].item->childCount() == 1 &&
                        !selected_sensor() && selected_ == 0 && satellites_[0].item->isHidden(),
                    "Sensor deletion left a stale child, selection or search result");
            record("sensor-delete-refreshes-tree-and-search");
            findChild<QLineEdit *>("objectSearch")->clear();
            satellites_[0].item->setExpanded(false);
            scenario_item_->setExpanded(false);
            edit_sensor(true);
            sensor_editor_->findChild<QLineEdit *>("sensorName")
                ->setText(QStringLiteral("后续传感器"));
            sensor_editor_->findChild<QPushButton *>("applySensor")->click();
            ++orbit_smoke_stage_;
            break;
        case 149:
            require(sensors_.size() == 2 && selected_sensor() &&
                        selected_name() == QStringLiteral("后续传感器") &&
                        scenario_item_->isExpanded() && satellites_[0].item->isExpanded() &&
                        !objects_->visualItemRect(objects_->currentItem()).isEmpty(),
                    "Inserting another sensor did not reveal it through collapsed ancestors");
            record("sensor-insert-expands-collapsed-ancestors");
            save_workbench_image("sensor-tree-refreshed.png");
            smoke_final_frames_ = 0;
            ++orbit_smoke_stage_;
            break;
        case 150:
            if (++smoke_final_frames_ < 4)
                return;
            smoke_responsive_size_ = size();
            sensor_editor_->close();
            slots_[1]->dock->hide();
            smoke_set_view_layout(0);
            set_selection(0);
            open_properties();
            editor_->findChild<QComboBox *>("propertyPageSelector")->setCurrentIndex(0);
            editor_->findChild<QuantityEditor *>("displayStep")->set_value(17, Unit::second);
            smoke_cache_ = orbit_;
            workspace_->split_page(properties_dialog_, view_panel_, Qt::RightEdge);
            resize(880, 700);
            smoke_wait_frame_ = frame_count_ + 4;
            ++orbit_smoke_stage_;
            break;
        case 151: {
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            save_workbench_image("responsive-workbench.png");
            // Compact navigation is page tabs at inspector widths, a selector when narrower.
            const bool compact_navigation =
                editor_->findChild<QComboBox *>("propertyPageSelector")->isVisible() ||
                editor_->findChild<QTabBar *>("propertyPageTabs")->isVisible();
            require(
                width() <= 880 && compact_navigation &&
                    commands_->toolButtonStyle() == Qt::ToolButtonIconOnly &&
                    findChild<QWidget *>("timelineControls")->height() > 50 &&
                    action_in_workspace("applyOrbit"),
                qPrintable(
                    QString("Narrow workbench: width=%1, propertyWidth=%2, compactNavigation=%3, "
                            "iconStyle=%4, controlsHeight=%5, applyVisible=%6")
                        .arg(width())
                        .arg(editor_->width())
                        .arg(compact_navigation)
                        .arg(commands_->toolButtonStyle())
                        .arg(findChild<QWidget *>("timelineControls")->height())
                        .arg(action_in_workspace("applyOrbit"))));
            require(orbit_ == smoke_cache_, "Resizing the workspace recalculated the orbit");
            record("responsive-workbench-and-native-view-resize");
            workspace_->detach_page(properties_dialog_);
            properties_dialog_->window()->resize(360, 520);
            ++orbit_smoke_stage_;
            break;
        }
        case 152:
            require(properties_dialog_->window()->width() == 360 &&
                        editor_->findChild<QComboBox *>("propertyPageSelector")->isVisible() &&
                        editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) ==
                            17,
                    "Narrow floating properties lost the draft or retained a fixed width");
            require(properties_dialog_->window()->grab().save(
                        smoke_dir_ + "/responsive-floating-properties.png"),
                    "Cannot capture narrow floating properties");
            record("responsive-floating-properties-preserve-draft");
            workspace_->dock_page(properties_dialog_);
            resize(smoke_responsive_size_);
            slots_[1]->dock->show();
            smoke_wait_frame_ = frame_count_ + 4;
            ++orbit_smoke_stage_;
            break;
        case 153:
            if (frame_count_ < smoke_wait_frame_ || !view_captures_ready())
                return;
            require(orbit_ == smoke_cache_ &&
                        editor_->findChild<QuantityEditor *>("displayStep")->value(Unit::second) ==
                            17,
                    "Restoring the workspace discarded the draft or recalculated the orbit");
            record("responsive-workbench-restore-preserves-state");
            editor_->findChild<QuantityEditor *>("displayStep")->set_value(60, Unit::second);
            properties_dialog_->close();
            smoke_set_view_layout(0);
            smoke_final_frames_ = 0;
            ++orbit_smoke_stage_;
            break;
        case 154:
            if (++smoke_final_frames_ >= 4)
                ++orbit_smoke_stage_;
            break;
        }
    }
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
    void benchmark_input(const BenchmarkInputEvent &input) {
        QJsonObject result;
        if (input.seek) {
            const int seconds = 5 + static_cast<int>((input.id * 37 + 23) % 591);
            slider_->setValue(seconds);
            result.insert("target_epoch_ns", static_cast<qint64>(seconds) * 1'000'000'000);
        } else {
            const auto view_index = static_cast<std::size_t>(input.id % 2);
            const auto &slot = *slots_[view_index];
            std::vector<bool> in_other(snapshot_.markers.size());
            for (const auto &point : slots_[1 - view_index]->pick_candidates)
                in_other[point.index] = true;
            std::vector<render_vsg::ProjectedMarker> common;
            for (const auto &point : slot.pick_candidates)
                if (in_other[point.index])
                    common.push_back(point);
            if (common.empty())
                throw std::runtime_error("No shared visible target for timed selection");
            const auto &point = common[(input.id * 131) % common.size()];
            inject_click(*slots_[view_index], {point.x_pixels, point.y_pixels});
            if (!selected_ || *selected_ != point.index)
                throw std::runtime_error("Timed selection did not reach its target");
            result.insert("view", static_cast<int>(view_index));
            result.insert("target_index", static_cast<qint64>(point.index));
            result.insert("target_epoch_ns",
                          static_cast<qint64>(snapshot_.epoch.tai_nanoseconds()));
        }
        run_->input(input, result);
    }
#endif
    void benchmark_smoke_step() {
        if (frame_count_ == 30)
            slider_->setValue(450);
        if (frame_count_ == 31) {
            if (snapshot_.epoch != core::Epoch{450'000'000'000})
                throw std::runtime_error("Cached slider seek did not reach the requested epoch");
            interaction_report_.append(QJsonObject{
                {"kind", "cached-slider-seek"},
                {"target_tai_ns", static_cast<qint64>(450'000'000'000)},
                {"rendered_tai_ns", static_cast<qint64>(snapshot_.epoch.tai_nanoseconds())}});
            save_interaction_images("jump");
        }
        if (frame_count_ == 160 || frame_count_ == 180) {
            const auto view_index = frame_count_ == 160 ? 0u : 1u;
            auto &slot = *slots_[view_index];
            const auto &other = slots_[1u - view_index]->pick_candidates;
            const auto candidate = std::find_if(
                slot.pick_candidates.begin(), slot.pick_candidates.end(), [&](const auto &value) {
                    return std::any_of(other.begin(), other.end(),
                                       [&](const auto &p) { return p.index == value.index; });
                });
            if (candidate == slot.pick_candidates.end())
                throw std::runtime_error("No jointly visible test target for selection");
            expected_selection_ = candidate->index;
            inject_click(slot, {candidate->x_pixels, candidate->y_pixels});
        }
        if (frame_count_ == 161 || frame_count_ == 181) {
            if (selected_ != expected_selection_ || selection_->size() != 1)
                throw std::runtime_error("Synthetic Qt click did not select its visible target");
            interaction_report_.append(QJsonObject{
                {"kind", "synthetic-Qt-click"},
                {"view", frame_count_ == 161 ? 0 : 1},
                {"object_id", QString::fromStdString(snapshot_.markers.at(*selected_).object_id)}});
            save_interaction_images(frame_count_ == 161 ? "selected-view0" : "selected-view1");
        }
        if (frame_count_ == 170)
            inject_click(*slots_[0], {0, 0});
        if (frame_count_ == 171) {
            if (selected_ || selection_->size() != 0)
                throw std::runtime_error("Empty-space click did not clear the selection");
            interaction_report_.append(QJsonObject{{"kind", "clear-selection"}, {"passed", true}});
        }
    }
    void save_interaction_images(const QString &step) {
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (!slots_[i]->captured_image.save(smoke_dir_ +
                                                QString("/%1-view-%2.png").arg(step).arg(i)))
                throw std::runtime_error("Cannot save the interaction swapchain image");
    }
    bool view_captures_ready() const {
        for (const auto &slot : slots_) {
            if (!slot->container->isVisible())
                continue;
            const auto extent = slot->window->windowAdapter->extent2D();
            const auto ratio = slot->window->devicePixelRatio();
            if (slot->captured_image.width() != static_cast<int>(extent.width) ||
                slot->captured_image.height() != static_cast<int>(extent.height) ||
                std::abs(extent.width - slot->container->width() * ratio) > 1 ||
                std::abs(extent.height - slot->container->height() * ratio) > 1 ||
                slot->window->size() != slot->container->size())
                return false;
        }
        return true;
    }
    void save_workbench_image(const QString &name) {
        // QWidget does not paint native Vulkan children. Insert same-frame
        // swapchain readbacks into the actual Qt control capture, without restyling.
        auto workbench = grab();
        QPainter painter(&workbench);
        for (const auto &slot : slots_) {
            if (!slot->container->isVisible() || slot->captured_image.isNull())
                continue;
            const auto position = slot->container->mapTo(this, QPoint{});
            painter.drawImage(QRect(position, slot->container->size()), slot->captured_image);
        }
        painter.end();
        if (!workbench.save(smoke_dir_ + "/" + name))
            throw std::runtime_error("Cannot save workbench controls and view capture");
    }
    void recreate_window(Slot &slot) {
        // QWindow::destroy sends SurfaceAboutToBeDestroyed; winId creates a new native surface.
        slot.window->destroy();
        slot.window->initializeWindow();
        slot.window->show();
    }
    void finish_smoke() {
        run_exclusive([&] { viewer_->deviceWaitIdle(); });
        shared_device_ = slots_[0]->window->windowAdapter->getDevice() ==
                         slots_[1]->window->windowAdapter->getDevice();
        shared_scene_ = slots_[0]->view->children.front() == slots_[1]->view->children.front();
        independent_cameras_ = slots_[0]->camera != slots_[1]->camera &&
                               slots_[0]->view->viewID != slots_[1]->view->viewID;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            auto *window = slots_[i]->window;
            if (slots_[i]->creations < 2 || slots_[i]->destructions < 1 ||
                slots_[i]->presentations < 30)
                throw std::runtime_error("A view did not complete recreation and presentation");
            const auto extent = window->windowAdapter->extent2D();
            if (benchmarking_ &&
                (extent.width != 1280 || extent.height != 720 || markers_->size() != 10000 ||
                 !slots_[i]->labels || slots_[i]->labels->size() != 200))
                throw std::runtime_error(
                    "Actual workload or native viewport differs from the frozen benchmark");
            const auto format = window->windowAdapter->surfaceFormat();
            const auto properties = window->windowAdapter->getPhysicalDevice()->getProperties();
            const auto &image = slots_[i]->captured_image;
            if (image.isNull() || !image.save(smoke_dir_ + QString("/view-%1.png").arg(i))) {
                throw std::runtime_error("Cannot save the application's swapchain image");
            }
            const auto pixels = image.convertToFormat(QImage::Format_RGB32);
            const auto background = pixels.pixelColor(0, 0);
            qint64 foreground = 0, marker = 0, foreground_at_edge = 0, selected_pixels = 0;
            for (int y = 0; y < pixels.height(); ++y)
                for (int x = 0; x < pixels.width(); ++x) {
                    const auto color = pixels.pixelColor(x, y);
                    if (std::abs(color.red() - background.red()) +
                            std::abs(color.green() - background.green()) +
                            std::abs(color.blue() - background.blue()) >
                        90) {
                        ++foreground;
                        if (x == 0 || y == 0 || x == pixels.width() - 1 || y == pixels.height() - 1)
                            ++foreground_at_edge;
                    }
                    // The benchmark retains its fixed marker palette.
                    if (benchmarking_ && color.red() > 180 && color.green() > 110 &&
                        color.blue() < std::min(color.red(), color.green()) * 0.65)
                        ++marker;
                    if (color.red() > 180 && color.blue() > 140 && color.green() < 120)
                        ++selected_pixels;
                }
            if (!benchmarking_) {
                // Satellite colors are user-defined. Check the actual projected markers,
                // accepting UNORM and sRGB framebuffer encodings of their configured RGB.
                for (const auto &point : slots_[i]->pick_candidates) {
                    if (point.index >= satellites_.size() || satellites_[point.index].model)
                        continue;
                    const auto expected =
                        QColor::fromRgb(satellites_[point.index].display.line_color);
                    const auto encode = [](double value) {
                        return qRound(255 * (value <= .0031308
                                                 ? value * 12.92
                                                 : 1.055 * std::pow(value, 1 / 2.4) - .055));
                    };
                    const QColor srgb(encode(expected.redF()), encode(expected.greenF()),
                                      encode(expected.blueF()));
                    const auto close = [](QColor a, QColor b) {
                        return std::abs(a.red() - b.red()) < 12 &&
                               std::abs(a.green() - b.green()) < 12 &&
                               std::abs(a.blue() - b.blue()) < 12;
                    };
                    for (int y = std::max(0, qRound(point.y_pixels) - 5);
                         y < std::min(pixels.height(), qRound(point.y_pixels) + 6); ++y)
                        for (int x = std::max(0, qRound(point.x_pixels) - 5);
                             x < std::min(pixels.width(), qRound(point.x_pixels) + 6); ++x)
                            if (close(pixels.pixelColor(x, y), expected) ||
                                close(pixels.pixelColor(x, y), srgb))
                                ++marker;
                }
            }
            if (foreground < static_cast<qint64>(pixels.width()) * pixels.height() / 100 ||
                marker < 6)
                throw std::runtime_error("Rendered image lacks the globe or satellite marker");
            if (foreground_at_edge)
                throw std::runtime_error("The initial camera clips the globe or demo track");
            if (selected_ && selected_pixels < 20)
                throw std::runtime_error(
                    "The selected satellite ring is missing from the rendered view");
            view_report_.append(QJsonObject{
                {"native_creations", static_cast<qint64>(slots_[i]->creations)},
                {"native_destructions", static_cast<qint64>(slots_[i]->destructions)},
                {"successful_presentations", static_cast<qint64>(slots_[i]->presentations)},
                {"capture_source", "acquired swapchain image; copy in render submission"},
                {"capture_width_pixels", pixels.width()},
                {"capture_height_pixels", pixels.height()},
                {"capture_frame", static_cast<qint64>(frame_count_)},
                {"capture_epoch_tai_ns", static_cast<qint64>(snapshot_.epoch.tai_nanoseconds())},
                {"foreground_pixels", foreground},
                {"foreground_pixels_at_edge", foreground_at_edge},
                {"marker_pixels", marker},
                {"selection_pixels", selected_pixels},
                {"submitted_markers", static_cast<qint64>(markers_->size())},
                {"submitted_labels",
                 static_cast<qint64>(slots_[i]->labels ? slots_[i]->labels->size() : 0)},
                {"visible_pick_candidates", static_cast<qint64>(slots_[i]->pick_candidates.size())},
                {"surface_format", static_cast<int>(format.format)},
                {"color_space", static_cast<int>(format.colorSpace)},
                {"vulkan_api_version", static_cast<qint64>(properties.apiVersion)},
                {"vulkan_driver_version", static_cast<qint64>(properties.driverVersion)},
                {"width_pixels", static_cast<qint64>(extent.width)},
                {"height_pixels", static_cast<qint64>(extent.height)},
                {"width_logical", window->width()},
                {"height_logical", window->height()},
                {"device_pixel_ratio", window->devicePixelRatio()},
                {"gpu", QString::fromUtf8(properties.deviceName)}});
        }
        if (!benchmarking_)
            save_workbench_image("workbench.png");
        timer_.stop();
        // Exercise shutdown while the background compiler has no remaining view contexts.
        for (auto &slot : slots_)
            slot->window->destroy();
        all_windows_closed_ = viewer_->windows().empty();
        finished_ = true;
        app_.exit(0);
    }
    QApplication &app_;
    ValidationLog &log_;
    QString smoke_dir_, failure_;
    bool validation_, stopped_ = false, finished_ = false, playing_ = true;
    bool benchmarking_, requested_time_ = false;
    bool satellite_state_dirty_ = true;
    double track_pixel_ratio_ = 0;
    std::optional<core::Epoch> time_label_epoch_;
    CalendarScale time_label_scale_ = CalendarScale::utc;
    bool injecting_input_ = false;
    bool shared_device_ = false, shared_scene_ = false, independent_cameras_ = false;
    bool all_windows_closed_ = false;
    std::shared_ptr<const OrbitScene> orbit_;
    std::optional<application::Scenario> scenario_;
    QString project_path_, pending_project_path_, last_project_outcome_;
    bool project_dirty_ = false;
    application::DataLock project_recorded_data_;
    std::optional<core::TimeDataInfo> standalone_time_data_;
    std::optional<application::AnalysisRecord> analysis_record_;
    std::optional<core::Epoch> precise_epoch_;
    std::shared_ptr<std::atomic_bool> project_cancellation_;
    std::future<std::shared_ptr<PreparedProject>> project_candidate_;
    QAction *open_action_ = nullptr, *save_action_ = nullptr, *save_as_action_ = nullptr;
    QAction *rename_action_ = nullptr, *duplicate_action_ = nullptr, *delete_action_ = nullptr;
    QAction *eop_action_ = nullptr;
    QAction *report_action_ = nullptr;
    ReportDialog *report_dialog_ = nullptr;
    std::vector<application::ReportDefinition> reports_;
    ReportDialog::Ready pending_report_input_;
    std::unique_ptr<EopCatalog> eop_catalog_;
    std::map<std::string, std::shared_ptr<const core::GravityModel>> gravity_;
    std::map<std::string, std::shared_ptr<const core::IThirdBodyEphemeris>> ephemerides_;
    std::map<std::string, std::shared_ptr<const core::SpaceWeather>> weather_;
    std::map<std::string, std::shared_ptr<const core::OceanTideModel>> ocean_tides_;
    mutable std::shared_ptr<const core::ITimeProvider> active_time_;
    bool project_edit_ = false, project_orbit_edit_ = false, project_settings_edit_ = false;
    std::optional<std::size_t> pending_project_selection_;
    std::optional<std::string> pending_project_sensor_selection_;
    QPushButton *cancel_project_button_ = nullptr;
    std::vector<Satellite> satellites_;
    std::vector<Station> stations_;
    SensorEditor *sensor_editor_ = nullptr;
    std::vector<application::ProjectSensor> sensors_;
    std::optional<runtime::AccessSession> access_session_;
    std::vector<std::string> access_ids_;
    std::string access_hash_;
    GroundStationEditor *station_editor_ = nullptr;
    std::optional<std::string> station_editor_target_;
    std::optional<std::size_t> selected_station_;
    QTreeWidgetItem *scenario_item_ = nullptr;
    std::optional<std::size_t> editor_target_, pending_orbit_target_;
    bool insert_defaults_pending_ = false;
    std::size_t pending_model_target_ = 0;
    QString pending_model_path_;
    QStackedWidget *workspace_pages_ = nullptr;
    QWidget *view_panel_ = nullptr;
    Workspace *workspace_ = nullptr;
    QWidget *map_panel_ = nullptr;
    std::vector<QToolButton *> view_focus_buttons_;
    QAction *lighting_action_ = nullptr, *tracks_action_ = nullptr;
    std::optional<core::Epoch> sun_epoch_;
    std::array<double, 3> sun_direction_{1, 0, 0};
    core::FrameId display_frame_ = core::FrameId::gcrf;
    std::optional<core::PositionRotation> display_to_itrf_;
    std::vector<QComboBox *> frame_choices_;
    vsg::ref_ptr<vsg::MatrixTransform> earth_transform_;
    QLabel *workspace_label_ = nullptr, *workspace_dot_ = nullptr;
    QMenuBar *menu_bar_ = nullptr;
    QAction *new_action_ = nullptr, *example_action_ = nullptr, *insert_action_ = nullptr;
    QAction *fit_action_ = nullptr, *aux_action_ = nullptr, *map_action_ = nullptr;
    GroundMap *ground_map_ = nullptr;
    QToolBar *time_toolbar_ = nullptr;
    QMenu *insert_menu_ = nullptr;
    int smoke_bootstrap_ = 0;
    static constexpr int orbit_smoke_complete = 155;
    int smoke_final_frames_ = 0;
    std::vector<std::uint8_t> smoke_project_bytes_;
    application::DataLock smoke_data_lock_;
    std::array<vsg::dvec3, 3> smoke_camera_;
    std::shared_ptr<const OrbitScene> smoke_cache_;
    std::optional<application::Project> smoke_settings_project_;
    vsg::RecordAndSubmitTasks smoke_render_tasks_;
    std::size_t smoke_publications_ = 0;
    core::Epoch smoke_view_epoch_{0};
    std::array<double, 3> smoke_sun_{};
    QImage smoke_map_image_, smoke_globe_image_;
    std::uint64_t smoke_wait_frame_ = 0;
    qint64 smoke_timeline_epoch_ = 0;
    qint64 smoke_timeline_first_ = 0, smoke_timeline_last_ = 0;
    int smoke_workspace_height_ = 0;
    std::future<std::shared_ptr<const OrbitScene>> candidate_;
    std::future<std::shared_ptr<const OrbitScene>> display_candidate_;
    std::shared_ptr<const OrbitScene> display_source_, failed_display_source_;
    DisplaySettings display_settings_, failed_display_settings_;
    std::shared_ptr<std::atomic_bool> display_cancellation_;
    std::size_t display_target_ = 0, display_cache_publications_ = 0;
    std::shared_ptr<std::atomic_bool> cancellation_;
    OrbitEditor *editor_ = nullptr;
    QTreeWidgetItem *object_item_ = nullptr;
    QTreeWidget *objects_ = nullptr;
    QDockWidget *object_dock_ = nullptr;
    QStackedWidget *inspector_pages_ = nullptr;
    QAction *properties_action_ = nullptr, *focus_action_ = nullptr, *play_action_ = nullptr;
    QString last_orbit_outcome_;
    QString last_model_outcome_;
    std::future<vsg::ref_ptr<vsg::MatrixTransform>> model_candidate_;
    vsg::ref_ptr<vsg::MatrixTransform> model_node_;
    vsg::ref_ptr<vsg::Group> model_group_;
    int model_kind_ = 0, pending_model_kind_ = 0;
    double model_diameter_km_ = 1000, pending_model_diameter_km_ = 1000;
    int orbit_smoke_stage_ = 0;
    bool orbit_smoke_step_active_ = false;
    QSize smoke_responsive_size_;
    bool orbit_capture_ = false, fit_pending_ = false;
    std::shared_ptr<const core::IFrameProvider> frames_;
    std::shared_ptr<const core::IEarthOrientationProvider> eop_, eop_history_, eop_current_;
    vsg::ref_ptr<vsgQt::Viewer> viewer_;
    vsg::ref_ptr<vsg::Device> shared_device_ref_;
    vsg::ref_ptr<vsg::ResourceHints> hints_;
    vsg::ref_ptr<render_vsg::CompileManager> compiler_;
    std::unique_ptr<render_vsg::MarkerLayer> markers_;
    std::unique_ptr<render_vsg::MarkerLayer> selection_;
    std::unique_ptr<render_vsg::IEarthDisplay> globe_;
    vsg::ref_ptr<vsg::Group> scene_;
    vsg::ref_ptr<vsg::Group> orbit_group_;
    vsg::ref_ptr<vsg::Switch> orbit_switch_;
    vsg::ref_ptr<vsg::Node> smoke_old_track_;
    std::vector<std::unique_ptr<Slot>> slots_;
    QSlider *slider_ = nullptr;
    MissionTimeline *timeline_ = nullptr;
    QToolButton *time_label_ = nullptr;
    QDialog *properties_dialog_ = nullptr;
    QDialog *scenario_properties_ = nullptr;
    QToolBar *commands_ = nullptr;
    QMenu *scene_menu_ = nullptr, *view_menu_ = nullptr, *time_menu_ = nullptr;
    double playback_rate_ = 20;
    CalendarScale display_scale_ = CalendarScale::utc;
    QTimer timer_;
    std::unique_ptr<WorkbenchProfile> profile_;
    std::future<std::vector<std::shared_ptr<const OrbitScene>>> profile_orbits_;
    int profile_count_ = 24, profile_frame_ = 0;
    bool profile_started_ = false;
    QElapsedTimer clock_;
    QJsonArray view_report_;
    QJsonArray interaction_report_;
    visualization::RenderSnapshot snapshot_{
        core::Epoch{0}, core::FrameId::itrf, core::OriginId::earth, {}};
    std::optional<std::size_t> selected_, expected_selection_;
#ifdef ORBITFORGE_DESKTOP_BENCHMARK
    std::unique_ptr<BenchmarkScene> benchmark_;
    std::unique_ptr<BenchmarkRun> run_;
    std::unique_ptr<BenchmarkPresentation> timing_presentation_;
    QJsonObject timing_metadata_;
    QJsonArray timing_capabilities_;
    QJsonArray gpu_metadata_;
    bool timing_ready_ = false;
#endif
    std::uint64_t frame_count_ = 0;
    double seconds_ = 0;
    std::array<double, 3> initial_position_{};
};
} // namespace

int run_graphics_workbench(QApplication &app) {
    const auto arguments = app.arguments();
    const auto benchmark_index = arguments.indexOf("--benchmark-scenario");
    if (benchmark_index >= 0 && benchmark_index + 1 >= arguments.size()) {
        std::cerr << "--benchmark-scenario requires the fixed workload manifest\n";
        return 1;
    }
    const auto benchmark_manifest =
        benchmark_index >= 0 ? arguments[benchmark_index + 1] : QString{};
    if (benchmark_manifest.isEmpty())
        apply_workbench_style(app);
    const auto timing_index = arguments.indexOf("--benchmark-run");
    const bool timing_probe = arguments.contains("--benchmark-trace-probe");
    const bool output_only_probe = arguments.contains("--benchmark-output-only-probe");
    if (output_only_probe && !timing_probe) {
        std::cerr << "--benchmark-output-only-probe requires --benchmark-trace-probe\n";
        return 1;
    }
    if (timing_index >= 0 &&
        (timing_index + 1 >= arguments.size() || benchmark_manifest.isEmpty())) {
        std::cerr << "--benchmark-run requires a new output directory and --benchmark-scenario\n";
        return 1;
    }
    const auto timing_dir = timing_index >= 0 ? arguments[timing_index + 1] : QString{};
    const auto portable_smoke_index = arguments.indexOf("--portable-smoke");
    const auto graphics_smoke_index = arguments.indexOf("--graphics-smoke");
    if (portable_smoke_index >= 0 && graphics_smoke_index >= 0) {
        std::cerr << "Select either portable smoke or validation-layer smoke\n";
        return 1;
    }
    const auto smoke_index =
        portable_smoke_index >= 0 ? portable_smoke_index : graphics_smoke_index;
    if (smoke_index >= 0 && smoke_index + 1 >= arguments.size()) {
        std::cerr << "--graphics-smoke requires an output directory\n";
        return 1;
    }
    const QString smoke_dir = smoke_index >= 0 ? arguments[smoke_index + 1] : QString{};
    if ((timing_index >= 0 && smoke_index >= 0) || (timing_probe && timing_index < 0)) {
        std::cerr
            << "Timing and graphics-smoke modes are separate; probe requires --benchmark-run\n";
        return 1;
    }
    const bool validation = graphics_smoke_index >= 0 || arguments.contains("--validation");
    if (validation) {
        std::uint32_t count = 0;
        if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS)
            return 1;
        std::vector<VkLayerProperties> layers(count);
        if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS)
            return 1;
        const bool available = std::any_of(layers.begin(), layers.end(), [](const auto &layer) {
            return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        if (!available) {
            std::cerr << "Vulkan validation layer is required; set VK_LAYER_PATH to its manifest "
                         "directory\n";
            return 1;
        }
    }
    const auto resources_index = arguments.indexOf("--rocky-resources");
    if (resources_index >= 0 && resources_index + 1 >= arguments.size()) {
        std::cerr << "--rocky-resources requires a directory\n";
        return 1;
    }
    const auto resources = resources_index >= 0
                               ? arguments[resources_index + 1]
                               : QCoreApplication::applicationDirPath() + "/resources/rocky";
    for (const auto *shader : {"rocky.terrain.vert", "rocky.terrain.frag"}) {
        if (!QFileInfo(resources + "/shaders/" + shader).isFile()) {
            std::cerr << "Rocky shader resource missing: " << shader << '\n';
            return 1;
        }
    }
    qputenv("ROCKY_FILE_PATH", resources.toUtf8());
    const auto local_resources = QCoreApplication::applicationDirPath() + "/resources";
    if (QFileInfo(local_resources + "/proj/proj.db").isFile()) {
        qputenv("PROJ_DATA", (local_resources + "/proj").toUtf8());
        qputenv("PROJ_NETWORK", "OFF");
    }
    if (QDir(local_resources + "/gdal").exists())
        qputenv("GDAL_DATA", (local_resources + "/gdal").toUtf8());
    ValidationLog log;
    try {
        std::unique_ptr<StartupSplash> splash;
        if (benchmark_manifest.isEmpty()) {
            splash = std::make_unique<StartupSplash>(app.primaryScreen());
            splash->show();
            splash->showMessage(QStringLiteral("正在加载地球与工作台…"));
            app.processEvents(QEventLoop::ExcludeUserInputEvents);
            if (!smoke_dir.isEmpty()) {
                QDir().mkpath(smoke_dir);
                if (!splash->grab().save(smoke_dir + "/startup-splash.png"))
                    throw std::runtime_error("Cannot capture the startup splash");
            }
        }
        Workbench window(app, log, smoke_dir, validation, benchmark_manifest, timing_dir,
                         timing_probe, output_only_probe);
        window.show();
        if (splash) {
            splash->finish(&window);
            if (splash->isVisible())
                throw std::runtime_error(
                    "Startup splash did not close after opening the workbench");
            splash.reset();
        }
        const int code = window.initialize_data() ? app.exec() : 0;
        window.shutdown();
        return window.report(code);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const vsg::Exception &error) {
        std::cerr << "Graphics initialization failed: " << error.message << " (VkResult "
                  << error.result << ")\n";
        return 1;
    }
}
} // namespace orbitforge::desktop
