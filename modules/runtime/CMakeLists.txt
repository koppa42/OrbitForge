orbitforge_module(orbitforge_runtime Runtime src/demo.cpp src/time_frames.cpp src/ephemeris.cpp
    src/two_body.cpp src/j2.cpp src/j4.cpp src/sgp4.cpp src/project.cpp src/data_catalog.cpp src/report.cpp src/access.cpp src/analysis.cpp
    src/tabulated_orbit.cpp src/project_facade.cpp src/hpop.cpp src/mission.cpp src/gravity.cpp src/space_weather.cpp src/ocean_tides.cpp)
target_link_libraries(orbitforge_runtime PUBLIC OrbitForge::Application)
target_link_libraries(orbitforge_runtime PRIVATE OrbitForge::Sgp4)
target_include_directories(orbitforge_runtime SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/modules/application/third_party")
if(ORBITFORGE_WITH_DATA_CATALOG)
    find_package(unofficial-sqlite3 CONFIG REQUIRED)
    find_package(nlohmann_json 3.12.0 EXACT CONFIG REQUIRED)
    find_path(PICOSHA2_INCLUDE_DIR NAMES picosha2.h REQUIRED)
    target_link_libraries(orbitforge_runtime PRIVATE unofficial::sqlite3::sqlite3
        "$<BUILD_INTERFACE:nlohmann_json::nlohmann_json>")
    target_include_directories(orbitforge_runtime SYSTEM PRIVATE "${PICOSHA2_INCLUDE_DIR}")
    target_compile_definitions(orbitforge_runtime PRIVATE ORBITFORGE_HAS_DATA_CATALOG)
    find_file(SQLITE_LICENSE_FILE NAMES copyright
        PATHS "${PICOSHA2_INCLUDE_DIR}/../share/sqlite3" NO_DEFAULT_PATH REQUIRED)
    install(FILES "${SQLITE_LICENSE_FILE}" DESTINATION "${CMAKE_INSTALL_DATADIR}/OrbitForge/notices"
        RENAME SQLite.txt COMPONENT SDK)
    install(FILES "${SQLITE_LICENSE_FILE}" DESTINATION "orbitforge/notices"
        RENAME SQLite.txt COMPONENT Python)
endif()
if(ORBITFORGE_WITH_ERFA)
    target_link_libraries(orbitforge_runtime PRIVATE OrbitForge::Erfa OrbitForge::Iers)
    target_compile_definitions(orbitforge_runtime PRIVATE ORBITFORGE_HAS_ERFA)
endif()
if(ORBITFORGE_WITH_SPICE)
    target_link_libraries(orbitforge_runtime PRIVATE OrbitForge::Spice)
    target_compile_definitions(orbitforge_runtime PRIVATE ORBITFORGE_HAS_SPICE)
endif()

