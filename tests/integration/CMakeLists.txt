add_executable(orbitforge_integration_tests runtime_test.cpp tabulated_orbit_test.cpp hpop_ephemeris_test.cpp scenario_test.cpp project_test.cpp report_test.cpp
    propagators_test.cpp access_test.cpp project_document_test.cpp analysis_test.cpp catalog_analysis_test.cpp
    project_analysis_persistence_test.cpp analysis_access_test.cpp project_facade_test.cpp hpop_test.cpp mission_test.cpp gravity_test.cpp third_body_test.cpp planetary_centres_test.cpp solar_radiation_pressure_test.cpp atmospheric_drag_test.cpp space_weather_test.cpp mean_weather_test.cpp monthly_weather_test.cpp fxm_weather_test.cpp ocean_tides_test.cpp secular_gravity_test.cpp)
target_link_libraries(orbitforge_integration_tests PRIVATE OrbitForge::Runtime GTest::gtest_main)
target_compile_definitions(orbitforge_integration_tests PRIVATE
    ORBITFORGE_WEATHER_MONTHLY_DIR="${PROJECT_SOURCE_DIR}/tests/reference/propagators"
    ORBITFORGE_WEATHER_FXM_DIR="${PROJECT_SOURCE_DIR}/tests/reference/propagators"
    ORBITFORGE_WEATHER_MEAN_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/space_weather_mean.txt"
    ORBITFORGE_WEATHER_SPLINE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/space_weather_spline.txt"
    ORBITFORGE_SECULAR_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators"
    ORBITFORGE_OCEAN_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators"
    ORBITFORGE_EGM2008_FULL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/gravity_egm2008_full.txt"
    ORBITFORGE_REPORT_EOP="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"
    ORBITFORGE_DRAG_EOP="${PROJECT_SOURCE_DIR}/data/samples/eop/iers_c04_2016_12_28_2017_01_04/manifest.json"
    ORBITFORGE_EXPECT_ERFA=$<BOOL:${ORBITFORGE_WITH_ERFA}>
    ORBITFORGE_EXPECT_SPICE=$<BOOL:${ORBITFORGE_WITH_SPICE}>
    ORBITFORGE_PROJECT_CURRENT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/project-current.orbitforge"
    ORBITFORGE_INTEGRATION_SPICE="${PROJECT_SOURCE_DIR}/data/samples/spice/de440s_2016_leap/manifest.json"
    ORBITFORGE_INTEGRATION_PLANETS="${PROJECT_SOURCE_DIR}/data/samples/spice/de440s_planets_2016_leap/manifest.json"
    ORBITFORGE_INTEGRATION_WORKSPACE="${CMAKE_CURRENT_BINARY_DIR}/spice-workspace")
if(ORBITFORGE_BUILD_VISUALIZATION)
    target_link_libraries(orbitforge_integration_tests PRIVATE OrbitForge::Visualization)
    target_compile_definitions(orbitforge_integration_tests PRIVATE ORBITFORGE_HAS_VISUALIZATION)
endif()
orbitforge_warnings(orbitforge_integration_tests)
orbitforge_copy_runtime_dlls(orbitforge_integration_tests)
gtest_discover_tests(orbitforge_integration_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "integration.")

target_include_directories(orbitforge_integration_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
