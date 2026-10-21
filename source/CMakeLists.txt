cmake_minimum_required(VERSION 3.24)
project(SeamlessVehicle VERSION 0.63.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT MSVC)
    message(FATAL_ERROR "SeamlessVehicle builds with MSVC (Visual Studio 2022 Build Tools).")
endif()

# Only sfse/PluginAPI.h is used (MIT, bundled in third_party/sfse with its notice).
set(SFSE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sfse" CACHE PATH "SFSE headers")
function(sp_options target)
    target_include_directories(${target} PRIVATE src "${SFSE_ROOT}")
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHa)
endfunction()

add_library(sp_common STATIC
    src/game.cpp src/hook.cpp src/log.cpp src/notify.cpp src/telemetry.cpp
    src/anchor.cpp src/trace.cpp src/controlmap.cpp)
sp_options(sp_common)

# The rover flight DLL has its own main loop, ground probes and scene update
# when SeamlessPlanets is not installed. With the core present it uses its API.

add_library(SeamlessVehicle SHARED
    src/main_vehicle.cpp src/api_client.cpp src/vehicle_probe.cpp src/hoverui.cpp src/realvehicles.cpp src/crash.cpp
    src/npcvehicles.cpp src/vehicleaim.cpp src/passenger.cpp src/pilotquest.cpp src/vehicleworld.cpp src/routeplan.cpp src/navmap.cpp src/navfield.cpp src/environment.cpp src/traffic.cpp src/context.cpp src/viewdist.cpp src/vcontract.cpp src/profiles.cpp src/modscene.cpp src/scantarget.cpp src/vehiclehits.cpp src/features.cpp src/navseed.cpp src/mappoints.cpp
    src/vehicleflight.cpp src/shipmodel.cpp src/shipvehicle.cpp
    src/flightmodel.cpp src/flighttune.cpp src/camtune.cpp)
sp_options(SeamlessVehicle)
target_compile_definitions(SeamlessVehicle PRIVATE SP_FEATURE_VEHICLE)
target_link_libraries(SeamlessVehicle PRIVATE sp_common Synchronization)

add_executable(navfield_test tests/navfield_test.cpp src/navfield.cpp)
sp_options(navfield_test)

add_executable(scannercontext_test tests/scannercontext_test.cpp)
sp_options(scannercontext_test)

add_executable(triplogic_test tests/triplogic_test.cpp)
sp_options(triplogic_test)

add_executable(environment_test tests/environment_test.cpp src/environment.cpp src/navfield.cpp)
sp_options(environment_test)
add_executable(recovery_test tests/recovery_test.cpp)
sp_options(recovery_test)
add_executable(routeplan_test tests/routeplan_test.cpp src/routeplan.cpp)
sp_options(routeplan_test)

add_executable(vehiclelogic_test tests/vehiclelogic_test.cpp)
sp_options(vehiclelogic_test)
add_executable(profileclass_test tests/profileclass_test.cpp src/vcontract.cpp)
sp_options(profileclass_test)
add_executable(manoeuvre_test tests/manoeuvre_test.cpp)
sp_options(manoeuvre_test)
add_executable(combat_steer_test tests/combat_steer_test.cpp)
sp_options(combat_steer_test)
add_executable(combat_bfm_test tests/combat_bfm_test.cpp)
sp_options(combat_bfm_test)
add_executable(combat_tactics_test tests/combat_tactics_test.cpp)
sp_options(combat_tactics_test)
add_executable(combat_brain_test tests/combat_brain_test.cpp)
sp_options(combat_brain_test)
add_executable(combat_ai_test tests/combat_ai_test.cpp)
sp_options(combat_ai_test)

add_executable(weapondamage_test tests/weapondamage_test.cpp)
sp_options(weapondamage_test)

add_executable(combat_relations_test tests/combat_relations_test.cpp)
sp_options(combat_relations_test)

add_executable(native_integration_test tests/native_integration_test.cpp)
sp_options(native_integration_test)

add_executable(placementlogic_test tests/placementlogic_test.cpp)
sp_options(placementlogic_test)

add_executable(travelmode_test tests/travelmode_test.cpp)
sp_options(travelmode_test)
