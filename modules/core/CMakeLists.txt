orbitforge_module(orbitforge_core Core
    src/propagation/circular_two_body.cpp src/propagation/ground_station.cpp src/frames/position_rotation.cpp
    src/frames/state_transform.cpp src/propagation/tle.cpp src/propagation/access.cpp
    src/propagation/tabulated_orbit.cpp src/propagation/relativity.cpp src/propagation/earth_gravity.cpp src/propagation/solid_tides.cpp src/propagation/ocean_tides.cpp src/propagation/hpop.cpp src/propagation/mission.cpp src/propagation/third_body.cpp
    src/propagation/space_weather.cpp src/propagation/solar_radiation_pressure.cpp src/propagation/atmospheric_drag.cpp
    src/propagation/atmosphere/jacchia_roberts.cpp src/propagation/atmosphere/nrlmsise00.cpp)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
foreach(notice IN ITEMS NOTICE.txt Apache-2.0.txt Orekit-NOTICE.txt)
    if(ORBITFORGE_INSTALL_SDK)
        install(FILES "src/propagation/atmosphere/${notice}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/OrbitForge/notices"
            RENAME "Atmosphere-${notice}" COMPONENT SDK)
    endif()
    install(FILES "src/propagation/atmosphere/${notice}" DESTINATION "orbitforge/notices"
        RENAME "Atmosphere-${notice}" COMPONENT Python)
endforeach()

if(ORBITFORGE_INSTALL_SDK)
    install(FILES src/propagation/integrators/rklib-LICENSE.txt
        DESTINATION "${CMAKE_INSTALL_DATADIR}/OrbitForge/notices" COMPONENT SDK)
endif()
install(FILES src/propagation/integrators/rklib-LICENSE.txt
    DESTINATION "orbitforge/notices" COMPONENT Python)
