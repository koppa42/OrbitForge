add_executable(orbitforge_core_tests universal_kepler_test.cpp vop_interpolation_test.cpp circular_two_body_test.cpp tabulated_orbit_test.cpp ground_station_test.cpp hpop_test.cpp sundman_test.cpp mission_test.cpp srp_differential_test.cpp drag_differential_test.cpp force_differential_test.cpp gravity_differential_test.cpp gravity_test.cpp solid_tides_test.cpp ocean_tides_test.cpp relativity_test.cpp third_body_test.cpp solar_radiation_pressure_test.cpp atmospheric_drag_test.cpp)
target_compile_definitions(orbitforge_core_tests PRIVATE
    ORBITFORGE_SRP_UNION_DIFFERENTIAL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_union_differential.txt"
    ORBITFORGE_SRP_DIFFERENTIAL_HIGH_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_differential_high.txt"
    ORBITFORGE_SRP_DIFFERENTIAL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_differential.txt"
    ORBITFORGE_DRAG_DIFFERENTIAL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/drag_differential.txt"
    ORBITFORGE_DRAG_DENSITY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/drag_density_differential.txt"
    ORBITFORGE_DRAG_DENSITY_HIGH_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/drag_density_high.txt"
    ORBITFORGE_FORCE_DIFFERENTIAL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/force_differential.txt"
    ORBITFORGE_GRAVITY_DIFFERENTIAL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/gravity_differential.txt"
    ORBITFORGE_GRAVITY_DIFFERENTIAL_POLE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/gravity_differential_poles.txt"
    ORBITFORGE_UNIVERSAL_KEPLER_STM_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/universal_kepler_stm.txt"
    ORBITFORGE_VOP_INTERPOLATION_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/vop_interpolation.txt"
    ORBITFORGE_UNIVERSAL_KEPLER_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/universal_kepler.txt"
    ORBITFORGE_SRP_BOUNDARY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_boundaries.txt"
    ORBITFORGE_SUNDMAN_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/sundman.txt"
    ORBITFORGE_INTERPOLATION_REFERENCE=\"${PROJECT_SOURCE_DIR}/tests/reference/propagators/orbit_interpolation.txt\"
    ORBITFORGE_LONG_STEP_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/long_steps.txt"
    ORBITFORGE_BS_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/bulirsch_stoer.txt"
    ORBITFORGE_RKV89_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/rkv89.txt"
    ORBITFORGE_GRAVITY_FULL_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/gravity_full.txt"
    ORBITFORGE_RELATIVITY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/relativity.txt"
    ORBITFORGE_LOVE_TIDE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/solid_tides_love.txt"
    ORBITFORGE_TIME_TIDE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/solid_tides_time_dependent.txt"
    ORBITFORGE_SOLID_TIDE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/solid_tides.txt"
    ORBITFORGE_GRAVITY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/gravity.txt"
    ORBITFORGE_THIRD_BODY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/third_body.txt"
    ORBITFORGE_SRP_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp.txt"
    ORBITFORGE_SRP_MULTI_TRAJECTORIES="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_multi_trajectories.txt"
    ORBITFORGE_SRP_MULTI_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_multi_geometry.txt"
    ORBITFORGE_SRP_GEOMETRY_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/srp_geometry.txt"
    ORBITFORGE_NRLMSISE_AP_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/nrlmsise00_ap.txt"
    ORBITFORGE_NRLMSISE_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/nrlmsise00.txt"
    ORBITFORGE_JR_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/jacchia_roberts_precise.txt"
    ORBITFORGE_MASS_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/mass.txt"
    ORBITFORGE_DRAG_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/drag.txt"
    ORBITFORGE_HPOP_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators/hpop.txt")
target_link_libraries(orbitforge_core_tests PRIVATE OrbitForge::Core GTest::gtest_main)
orbitforge_warnings(orbitforge_core_tests)
orbitforge_copy_runtime_dlls(orbitforge_core_tests)
gtest_discover_tests(orbitforge_core_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "core.")

target_include_directories(orbitforge_core_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
