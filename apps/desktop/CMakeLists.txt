find_package(Qt6 6.8 CONFIG REQUIRED COMPONENTS Widgets)
add_executable(orbitforge_desktop WIN32 src/main.cpp)
target_link_libraries(orbitforge_desktop PRIVATE
    OrbitForge::Runtime OrbitForge::Visualization Qt6::Widgets)
orbitforge_warnings(orbitforge_desktop)
if(ORBITFORGE_WITH_VSG)
    if(NOT ORBITFORGE_WITH_DATA_CATALOG)
        message(FATAL_ERROR "The graphical desktop requires ORBITFORGE_WITH_DATA_CATALOG")
    endif()
    find_package(Qt6 6.8 CONFIG REQUIRED COMPONENTS Network)
    if(NOT QT_FEATURE_imageformat_png OR NOT QT_FEATURE_imageformat_jpeg)
        message(FATAL_ERROR
            "The graphical desktop requires Qt PNG and JPEG support for artwork, "
            "report exports and Earth imagery. Enable qtbase[png,jpeg] in vcpkg.")
    endif()
    if(BUILD_TESTING AND NOT TARGET Qt6::QOffscreenIntegrationPlugin)
        message(FATAL_ERROR
            "Desktop UI tests require Qt's offscreen platform plugin. "
            "Enable qtbase[freetype] in vcpkg and allow Qt plugin target imports.")
    endif()
    target_link_libraries(orbitforge_desktop PRIVATE Qt6::Network)
    # Original chevrons are pre-rendered at high resolution; no optional QtSvg plugin needed.
    qt_add_resources(orbitforge_desktop "workbench_icons" PREFIX "/orbitforge"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
        FILES resources/icons/chevron-down.png
              resources/icons/chevron-left.png resources/icons/chevron-right.png
              resources/startup/star-gazing.png
              resources/artwork/orbital-welcome.png resources/artwork/data-preparation.png)
    find_package(vsgQt 0.4.0 EXACT CONFIG REQUIRED)
    find_package(rocky 1.1.0 EXACT CONFIG REQUIRED)
    find_package(vsgXchange 1.1.13 EXACT CONFIG REQUIRED)
    target_sources(orbitforge_desktop PRIVATE src/viewer/graphics_workbench.cpp
        src/viewer/report_dialog.cpp src/viewer/report_plot.cpp src/viewer/report_selection.cpp
        src/viewer/project_scene.cpp src/viewer/eop_catalog.cpp src/viewer/data_center_dialog.cpp
        src/viewer/orbit_scene.cpp src/viewer/orbit_editor.cpp src/viewer/hpop_settings.cpp src/viewer/mission_editor.cpp src/viewer/track_style_editor.cpp src/viewer/workbench_style.cpp
        src/viewer/satellite_model.cpp src/viewer/mission_timeline.cpp src/viewer/scenario_dialog.cpp
        src/viewer/insert_object_dialog.cpp src/viewer/insert_object_dialog.hpp
        src/viewer/ground_station_editor.cpp src/viewer/ground_station_editor.hpp
        src/viewer/sensor_editor.cpp src/viewer/sensor_editor.hpp
        src/viewer/object_selection.cpp src/viewer/object_selection.hpp
        src/viewer/epoch_editor.cpp src/viewer/ground_map.cpp src/viewer/workspace.cpp
        src/viewer/workbench_profile.cpp src/viewer/quantity_editor.cpp src/viewer/quantity_editor.hpp
        src/viewer/startup_splash.cpp src/viewer/startup_splash.hpp
        src/viewer/initial_data_setup.cpp src/viewer/initial_data_setup.hpp
        src/viewer/welcome_page.cpp src/viewer/welcome_page.hpp)
    set_target_properties(orbitforge_desktop PROPERTIES AUTOMOC ON)
    target_link_libraries(orbitforge_desktop PRIVATE OrbitForge::GeoRocky vsgQt::vsgQt
        vsgXchange::vsgXchange rocky::rocky)
    target_compile_definitions(orbitforge_desktop PRIVATE
        ORBITFORGE_DESKTOP_GRAPHICS
        ORBITFORGE_ROCKY_VERSION="${rocky_VERSION}"
        ORBITFORGE_VSGQT_VERSION="${vsgQt_VERSION}")
    if(BUILD_TESTING)
        add_executable(orbitforge_object_selection_tests tests/object_selection_test.cpp
            src/viewer/object_selection.cpp src/viewer/object_selection.hpp src/viewer/workbench_style.cpp)
        set_target_properties(orbitforge_object_selection_tests PROPERTIES AUTOMOC ON)
        qt_add_resources(orbitforge_object_selection_tests "object_selection_icons" PREFIX "/orbitforge"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
            FILES resources/icons/chevron-down.png resources/icons/chevron-right.png)
        target_link_libraries(orbitforge_object_selection_tests PRIVATE OrbitForge::Application Qt6::Widgets GTest::gtest_main)
        orbitforge_warnings(orbitforge_object_selection_tests)
        orbitforge_copy_runtime_dlls(orbitforge_object_selection_tests)
        gtest_discover_tests(orbitforge_object_selection_tests DISCOVERY_MODE PRE_TEST
            TEST_PREFIX "desktop." PROPERTIES TIMEOUT 20
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION "QT_PLUGIN_PATH=set:$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>/..")
        add_executable(orbitforge_welcome_tests tests/welcome_page_test.cpp
            src/viewer/welcome_page.cpp src/viewer/workbench_style.cpp)
        qt_add_resources(orbitforge_welcome_tests "welcome_resources" PREFIX "/orbitforge"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources" FILES resources/artwork/orbital-welcome.png)
        target_link_libraries(orbitforge_welcome_tests PRIVATE Qt6::Widgets GTest::gtest_main)
        orbitforge_warnings(orbitforge_welcome_tests)
        orbitforge_copy_runtime_dlls(orbitforge_welcome_tests)
        gtest_discover_tests(orbitforge_welcome_tests DISCOVERY_MODE PRE_TEST
            TEST_PREFIX "desktop." PROPERTIES TIMEOUT 10
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION "QT_PLUGIN_PATH=set:$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>/..")
        add_executable(orbitforge_data_center_tests tests/data_center_test.cpp src/viewer/eop_catalog.cpp)
        target_compile_definitions(orbitforge_data_center_tests PRIVATE
            ORBITFORGE_TEST_PLANETS="${PROJECT_SOURCE_DIR}/data/samples/spice/de440s_planets_2016_leap/manifest.json"
            ORBITFORGE_TEST_TIME="${PROJECT_SOURCE_DIR}/data/samples/time/iers_leap_seconds_2026_07.dat"
            ORBITFORGE_TEST_FINALS="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_finals2000a_2026_09_01_2026_11_01/manifest.json"
            ORBITFORGE_TEST_C04="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json")
        target_link_libraries(orbitforge_data_center_tests PRIVATE OrbitForge::Runtime Qt6::Network GTest::gtest_main)
        orbitforge_warnings(orbitforge_data_center_tests)
        orbitforge_copy_runtime_dlls(orbitforge_data_center_tests)
        gtest_discover_tests(orbitforge_data_center_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "desktop." PROPERTIES TIMEOUT 30)
        add_executable(orbitforge_data_center_ui_tests tests/data_center_dialog_test.cpp
            src/viewer/eop_catalog.cpp src/viewer/data_center_dialog.cpp src/viewer/workbench_style.cpp
            src/viewer/initial_data_setup.cpp)
        qt_add_resources(orbitforge_data_center_ui_tests "data_center_icons" PREFIX "/orbitforge"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
            FILES resources/icons/chevron-down.png resources/artwork/data-preparation.png)
        target_compile_definitions(orbitforge_data_center_ui_tests PRIVATE
            ORBITFORGE_TEST_PLANETS="${PROJECT_SOURCE_DIR}/data/samples/spice/de440s_planets_2016_leap/manifest.json"
            ORBITFORGE_TEST_FINALS="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_finals2000a_2026_09_01_2026_11_01/manifest.json"
            ORBITFORGE_TEST_TIME="${PROJECT_SOURCE_DIR}/data/samples/time/iers_leap_seconds_2026_07.dat")
        target_link_libraries(orbitforge_data_center_ui_tests PRIVATE
            OrbitForge::Runtime Qt6::Widgets Qt6::Network GTest::gtest_main)
        orbitforge_warnings(orbitforge_data_center_ui_tests)
        orbitforge_copy_runtime_dlls(orbitforge_data_center_ui_tests)
        gtest_discover_tests(orbitforge_data_center_ui_tests DISCOVERY_MODE PRE_TEST
            TEST_PREFIX "desktop." PROPERTIES TIMEOUT 20
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION "QT_PLUGIN_PATH=set:$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>/..")
    endif()
    if(BUILD_TESTING AND ORBITFORGE_WITH_ERFA AND ORBITFORGE_WITH_SPICE)
        add_executable(orbitforge_orbit_scene_tests tests/orbit_scene_test.cpp src/viewer/orbit_scene.cpp)
        target_link_libraries(orbitforge_orbit_scene_tests PRIVATE
            OrbitForge::Runtime OrbitForge::Visualization GTest::gtest_main)
        target_compile_definitions(orbitforge_orbit_scene_tests PRIVATE
            ORBITFORGE_TEST_EOP="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_finals2000a_2026_09_01_2026_11_01/manifest.json")
        orbitforge_warnings(orbitforge_orbit_scene_tests)
        orbitforge_copy_runtime_dlls(orbitforge_orbit_scene_tests)
        # Discover after the build: Windows cold starts can exceed 5 s under compiler load.
        gtest_discover_tests(orbitforge_orbit_scene_tests DISCOVERY_MODE PRE_TEST
            DISCOVERY_TIMEOUT 30 TEST_PREFIX "desktop." PROPERTIES TIMEOUT 60)
        add_executable(orbitforge_quantity_tests tests/quantity_editor_test.cpp
            src/viewer/quantity_editor.cpp src/viewer/quantity_editor.hpp
            src/viewer/ground_station_editor.cpp src/viewer/sensor_editor.cpp
            src/viewer/object_selection.cpp src/viewer/object_selection.hpp
            src/viewer/orbit_editor.cpp src/viewer/hpop_settings.cpp src/viewer/data_center_dialog.cpp src/viewer/mission_editor.cpp src/viewer/track_style_editor.cpp src/viewer/epoch_editor.cpp src/viewer/workbench_style.cpp)
        target_sources(orbitforge_quantity_tests PRIVATE src/viewer/eop_catalog.cpp)
        set_target_properties(orbitforge_quantity_tests PROPERTIES AUTOMOC ON)
        qt_add_resources(orbitforge_quantity_tests "quantity_icons" PREFIX "/orbitforge"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources" FILES resources/icons/chevron-down.png resources/icons/chevron-right.png)
        target_compile_definitions(orbitforge_quantity_tests PRIVATE
            ORBITFORGE_TEST_EOP="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_finals2000a_2026_09_01_2026_11_01/manifest.json")
        target_link_libraries(orbitforge_quantity_tests PRIVATE
            OrbitForge::Runtime OrbitForge::Visualization Qt6::Widgets Qt6::Network GTest::gtest_main)
        orbitforge_warnings(orbitforge_quantity_tests)
        orbitforge_copy_runtime_dlls(orbitforge_quantity_tests)
        gtest_discover_tests(orbitforge_quantity_tests DISCOVERY_MODE PRE_TEST
            TEST_PREFIX "desktop." PROPERTIES TIMEOUT 10
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION "QT_PLUGIN_PATH=set:$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>/..")
    endif()
    if(BUILD_TESTING AND ORBITFORGE_WITH_ERFA)
        add_executable(orbitforge_report_ui_tests tests/report_dialog_test.cpp
            src/viewer/report_dialog.cpp src/viewer/report_plot.cpp src/viewer/report_selection.cpp src/viewer/epoch_editor.cpp src/viewer/workbench_style.cpp src/viewer/workspace.cpp
            src/viewer/object_selection.cpp src/viewer/object_selection.hpp)
        set_target_properties(orbitforge_report_ui_tests PROPERTIES AUTOMOC ON)
        qt_add_resources(orbitforge_report_ui_tests "report_icons" PREFIX "/orbitforge"
            BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources" FILES resources/icons/chevron-down.png resources/icons/chevron-right.png)
        target_link_libraries(orbitforge_report_ui_tests PRIVATE OrbitForge::Runtime Qt6::Widgets GTest::gtest_main)
        target_compile_definitions(orbitforge_report_ui_tests PRIVATE
            ORBITFORGE_ACCESS_EOP="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json")
        orbitforge_warnings(orbitforge_report_ui_tests)
        orbitforge_copy_runtime_dlls(orbitforge_report_ui_tests)
        gtest_discover_tests(orbitforge_report_ui_tests DISCOVERY_MODE PRE_TEST
            TEST_PREFIX "desktop." PROPERTIES TIMEOUT 20
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION "QT_PLUGIN_PATH=set:$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>/..")
        add_executable(orbitforge_ground_map_tests tests/ground_map_test.cpp
            src/viewer/ground_map.cpp src/viewer/workbench_profile.cpp)
        target_link_libraries(orbitforge_ground_map_tests PRIVATE
            OrbitForge::Visualization OrbitForge::Application Qt6::Widgets rocky::rocky GTest::gtest_main)
        orbitforge_warnings(orbitforge_ground_map_tests)
        orbitforge_copy_runtime_dlls(orbitforge_ground_map_tests)
        gtest_discover_tests(orbitforge_ground_map_tests DISCOVERY_MODE PRE_TEST
            TEST_PREFIX "desktop." PROPERTIES TIMEOUT 10
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION "QT_PLUGIN_PATH=set:$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>/..")
        add_executable(orbitforge_timeline_tests tests/timeline_test.cpp
            src/viewer/mission_timeline.cpp src/viewer/epoch_editor.cpp
            src/viewer/workbench_style.cpp src/viewer/workspace.cpp tests/workspace_test.cpp)
        target_link_libraries(orbitforge_timeline_tests PRIVATE
            OrbitForge::Runtime Qt6::Widgets GTest::gtest_main)
        orbitforge_warnings(orbitforge_timeline_tests)
        orbitforge_copy_runtime_dlls(orbitforge_timeline_tests)
        gtest_discover_tests(orbitforge_timeline_tests DISCOVERY_MODE PRE_TEST
            TEST_PREFIX "desktop." PROPERTIES TIMEOUT 10
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION "QT_PLUGIN_PATH=set:$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>/..")
    endif()
    # Copy fixed runtime resources alongside the build output, independent of source paths.
    add_custom_command(TARGET orbitforge_desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/data/samples/eop/iers_finals2000a_2026_09_01_2026_11_01"
            "$<TARGET_FILE_DIR:orbitforge_desktop>/resources/eop-current"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ROCKY_SHARE_DIR}/shaders" "$<TARGET_FILE_DIR:orbitforge_desktop>/resources/rocky/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/data/samples/eop/iers_c04_2000_01_01_2000_01_02"
            "$<TARGET_FILE_DIR:orbitforge_desktop>/resources/eop"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/data/samples/imagery/nasa_blue_marble_2002"
            "$<TARGET_FILE_DIR:orbitforge_desktop>/resources/earth"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/data/samples/models"
            "$<TARGET_FILE_DIR:orbitforge_desktop>/resources/models"
        VERBATIM)
    orbitforge_copy_runtime_dlls(orbitforge_desktop)
    if(ORBITFORGE_BUILD_BENCHMARKS)
        find_package(vsgXchange 1.1.13 EXACT CONFIG REQUIRED)
        target_sources(orbitforge_desktop PRIVATE src/viewer/benchmark_scene.cpp src/viewer/benchmark_run.cpp
            src/viewer/benchmark_presentation.cpp src/viewer/benchmark_gpu.cpp
            src/viewer/benchmark_memory.cpp)
        if(WIN32)
            target_link_libraries(orbitforge_desktop PRIVATE Psapi dxgi)
        endif()
        target_link_libraries(orbitforge_desktop PRIVATE orbitforge_graphics_workload vsgXchange::vsgXchange)
        target_compile_definitions(orbitforge_desktop PRIVATE ORBITFORGE_DESKTOP_BENCHMARK)
        add_custom_command(TARGET orbitforge_desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PROJECT_SOURCE_DIR}/data/samples/fonts/roboto_mono_3_001"
                "$<TARGET_FILE_DIR:orbitforge_desktop>/resources/font"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PROJECT_SOURCE_DIR}/benchmarks/scenarios/windows_10k_two_views.json"
                "$<TARGET_FILE_DIR:orbitforge_desktop>/resources/windows_10k_two_views.json"
            VERBATIM)
    endif()
endif()
# The relocatable Windows preview is assembled by tools/packaging/package_windows.py.

