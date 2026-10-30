cmake_minimum_required(VERSION 3.24)
project(FlyingVehicleOverhaul VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(native_route_session_test tests/native_route_session_test.cpp)
target_include_directories(native_route_session_test PRIVATE src)
add_executable(native_movement_output_test tests/native_movement_output_test.cpp)
target_include_directories(native_movement_output_test PRIVATE src)
add_executable(native_movement_controller_test tests/native_movement_controller_test.cpp src/native_movement_controller.cpp)
target_include_directories(native_movement_controller_test PRIVATE src)
add_executable(autodrive_test tests/autodrive_test.cpp)
target_include_directories(autodrive_test PRIVATE src)
add_executable(boardinggate_test tests/boardinggate_test.cpp src/hook.cpp)
target_include_directories(boardinggate_test PRIVATE src)
target_compile_definitions(boardinggate_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_executable(hudspread_test tests/hudspread_test.cpp)
target_include_directories(hudspread_test PRIVATE src)
add_executable(trafficpick_test tests/trafficpick_test.cpp)
target_include_directories(trafficpick_test PRIVATE src)
add_executable(ringtrip_test tests/ringtrip_test.cpp)
target_include_directories(ringtrip_test PRIVATE src)
add_executable(trafficdata_test tests/trafficdata_test.cpp)
target_include_directories(trafficdata_test PRIVATE src)
add_executable(trafficroute_test tests/trafficroute_test.cpp)
target_include_directories(trafficroute_test PRIVATE src)
add_executable(trafficarrival_test tests/trafficarrival_test.cpp)
target_include_directories(trafficarrival_test PRIVATE src)
add_executable(routeeditor_test tests/routeeditor_test.cpp)
target_include_directories(routeeditor_test PRIVATE src)
add_executable(routeschedule_test tests/routeschedule_test.cpp)
target_include_directories(routeschedule_test PRIVATE src)
add_executable(surfaceprojection_test tests/surfaceprojection_test.cpp)
target_include_directories(surfaceprojection_test PRIVATE src)
add_executable(routemap_test tests/routemap_test.cpp)
target_include_directories(routemap_test PRIVATE src)
add_executable(routefiles_test tests/routefiles_test.cpp)
target_include_directories(routefiles_test PRIVATE src)
add_executable(trafficlinks_test tests/trafficlinks_test.cpp)
target_include_directories(trafficlinks_test PRIVATE src)
add_executable(strokefont_test tests/strokefont_test.cpp)
target_include_directories(strokefont_test PRIVATE src)
add_executable(routeshare_test tests/routeshare_test.cpp)
target_include_directories(routeshare_test PRIVATE src)
add_executable(cellbake_test tests/cellbake_test.cpp)
target_include_directories(cellbake_test PRIVATE src)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT MSVC)
    message(FATAL_ERROR "SeamlessPlanets builds with MSVC (Visual Studio 2022 Build Tools).")
endif()

set(SFSE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../ref/sfse" CACHE PATH "SFSE source")
function(sp_options target)
    target_include_directories(${target} PRIVATE src "${SFSE_ROOT}")
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHa)
endfunction()

add_library(sp_common STATIC
    src/game.cpp src/hook.cpp src/log.cpp src/notify.cpp src/telemetry.cpp
    src/anchor.cpp src/trace.cpp src/controlmap.cpp src/runtimeforms.cpp src/runtimecells.cpp)
sp_options(sp_common)

# One owner for every traversal hook and every derived world state.
add_library(SeamlessPlanets SHARED
    src/main.cpp src/api_core.cpp src/landing.cpp src/diagnostics.cpp
    src/recentre.cpp src/watchpoint.cpp src/terrainguard.cpp src/hexgrid.cpp
    src/sphere.cpp src/handoff.cpp src/playarea.cpp src/navgen.cpp
    src/hangwatch.cpp src/rebase.cpp src/parking.cpp src/gametick.cpp
    src/autosave.cpp src/crash.cpp src/prefetch.cpp src/content.cpp
    src/globe.cpp src/flight.cpp src/shipflight.cpp src/shipmodel.cpp
    src/camtune.cpp src/flightmodel.cpp src/flighttune.cpp
    src/distant.cpp src/modscene.cpp src/stream.cpp)
sp_options(SeamlessPlanets)
target_link_libraries(SeamlessPlanets PRIVATE sp_common Synchronization)

# The rover flight DLL has its own main loop, ground probes and scene update
# when SeamlessPlanets is not installed. With the core present it uses its API.

add_library(FlyingVehicleOverhaul SHARED
    src/native_movement_controller.cpp src/native_movement_engine.cpp src/native_movement_probe.cpp
    src/main_vehicle.cpp src/resources.cpp src/api_client.cpp src/vehicle_probe.cpp src/hoverui.cpp src/realvehicles.cpp src/ringroad.cpp src/routeprobes.cpp src/api_traffic.cpp src/crash.cpp
    src/npcvehicles.cpp src/vehicleaim.cpp src/passenger.cpp src/pilotquest.cpp src/vehicleworld.cpp src/routeplan.cpp src/navmap.cpp src/navfield.cpp src/groundnav.cpp src/environment.cpp src/traffic.cpp src/context.cpp src/viewdist.cpp src/vehiclekeep.cpp src/loadedarea.cpp src/vcontract.cpp src/profiles.cpp src/modscene.cpp src/scantarget.cpp src/bindings.cpp src/inputgate.cpp src/controlspage.cpp src/vehiclediag.cpp src/secretunpause.cpp src/vehiclehits.cpp src/features.cpp src/navseed.cpp src/mappoints.cpp src/routeedit.cpp src/disableguard.cpp src/mapguard.cpp src/cellbake.cpp
    src/vehicleflight.cpp src/vehiclecamera.cpp src/shipmodel.cpp src/shipvehicle.cpp
    src/flightmodel.cpp src/flighttune.cpp src/camtune.cpp)
sp_options(FlyingVehicleOverhaul)
target_compile_definitions(FlyingVehicleOverhaul PRIVATE SP_FEATURE_VEHICLE)
target_link_libraries(FlyingVehicleOverhaul PRIVATE sp_common Synchronization Dxgi)

# FVO Traffic addon (2026-10-29, was FVO City Placements): all traffic decisions; talks to core through src/traffic/traffic_api.h
# (API v2) and fvo_addon_api.h. FVO Placements is only FVO_CityPlacements.esm now. See notes/TRAFFIC_ADDON_PROPOSAL_20261029.md.
add_library(FVO_Traffic SHARED src/traffic/traffic_main.cpp src/traffic/generator.cpp src/traffic/everyday.cpp)
sp_options(FVO_Traffic)
target_link_libraries(FVO_Traffic PRIVATE Shell32 Ole32)

add_executable(trafficcycle_test tests/trafficcycle_test.cpp src/traffic/generator.cpp src/traffic/everyday.cpp)
sp_options(trafficcycle_test)
target_link_libraries(trafficcycle_test PRIVATE Shell32 Ole32)
add_executable(trafficpair_test tests/trafficpair_test.cpp)
sp_options(trafficpair_test)
add_executable(traffic_generator_test tests/traffic_generator_test.cpp)
sp_options(traffic_generator_test)
target_link_libraries(traffic_generator_test PRIVATE Shell32 Ole32)

# Gravspace was deleted 2026-10-22 (source in disabled_gravspace_20261022/Source). Gravlanes was deleted 2026-10-24 at the user's request.

# Interactive Cover: on-foot run-to-cover mod (notes/INTERACTIVE_COVER_PLAN.md). Standalone; test stage for now.
add_library(InteractiveCover SHARED src/main_cover.cpp src/cover_test.cpp src/crash.cpp)
sp_options(InteractiveCover)
target_link_libraries(InteractiveCover PRIVATE sp_common Synchronization)

add_executable(flightscript_test tests/flightscript_test.cpp)
add_executable(scriptbuild_test tests/scriptbuild_test.cpp)
sp_options(scriptbuild_test)
sp_options(flightscript_test)

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

add_executable(physicssetup_test tests/physicssetup_test.cpp)
sp_options(physicssetup_test)

add_executable(physicsstep_test tests/physicsstep_test.cpp)
sp_options(physicsstep_test)

add_executable(vtolphysics_test tests/vtolphysics_test.cpp)
sp_options(vtolphysics_test)

add_executable(vtolai_test tests/vtolai_test.cpp)
sp_options(vtolai_test)

add_executable(vtolplan_test tests/vtolplan_test.cpp)
sp_options(vtolplan_test)

add_executable(ringlogic_test tests/ringlogic_test.cpp)
sp_options(ringlogic_test)
add_executable(ringschedule_test tests/ringschedule_test.cpp)
sp_options(ringschedule_test)
add_executable(listenerboot_test tests/listenerboot_test.cpp)
sp_options(listenerboot_test)
add_executable(framework_test tests/framework_test.cpp)
sp_options(framework_test)
add_executable(performance_test tests/performance_test.cpp src/log.cpp)
sp_options(performance_test)
target_compile_definitions(performance_test PRIVATE SP_LOG_TEST)
add_executable(runtimeperf_test tests/runtimeperf_test.cpp src/controlmap.cpp src/navseed.cpp src/runtimeforms.cpp src/runtimecells.cpp src/vehicleworld.cpp src/pilotquest.cpp)
sp_options(runtimeperf_test)
target_compile_definitions(runtimeperf_test PRIVATE SP_RUNTIME_PERF_TEST)
target_link_libraries(runtimeperf_test PRIVATE Shell32 Ole32)
add_executable(connections_test tests/connections_test.cpp)
sp_options(connections_test)

add_executable(npcactivity_test tests/npcactivity_test.cpp)
sp_options(npcactivity_test)

add_executable(trafficschedule_test tests/trafficschedule_test.cpp)
sp_options(trafficschedule_test)

add_executable(cameraroll_test tests/cameraroll_test.cpp)
sp_options(cameraroll_test)

add_executable(hook_call_test tests/hook_call_test.cpp src/hook.cpp)
sp_options(hook_call_test)

add_executable(vehiclecamera_test tests/vehiclecamera_test.cpp src/vehiclecamera.cpp)
sp_options(vehiclecamera_test)

add_executable(placementlogic_test tests/placementlogic_test.cpp)
sp_options(placementlogic_test)
add_executable(bindlogic_test tests/bindlogic_test.cpp)
sp_options(bindlogic_test)
add_executable(commandinput_test tests/commandinput_test.cpp)
sp_options(commandinput_test)

add_executable(travelmode_test tests/travelmode_test.cpp)
sp_options(travelmode_test)

add_executable(groundplan_test tests/groundplan_test.cpp)
sp_options(groundplan_test)

add_executable(freeroll_test tests/freeroll_test.cpp)
sp_options(freeroll_test)
add_executable(vtolassist_test tests/vtolassist_test.cpp)
sp_options(vtolassist_test)
add_executable(placementedit_test tests/placementedit_test.cpp)
sp_options(placementedit_test)
add_executable(hudprompts_test tests/hudprompts_test.cpp)
sp_options(hudprompts_test)
add_executable(inlinehints_test tests/inlinehints_test.cpp)
sp_options(inlinehints_test)
add_executable(hudchildren_test tests/hudchildren_test.cpp)
sp_options(hudchildren_test)
add_executable(featureinstaller_test tests/featureinstaller_test.cpp)
sp_options(featureinstaller_test)
add_executable(placementenable_test tests/placementenable_test.cpp)
sp_options(placementenable_test)
