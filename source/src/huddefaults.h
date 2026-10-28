#pragma once
#include "hudchildren.h"
#include <sstream>

// Captured from the user's saved HUD layout, 2026-10-25 15:29.
// Positions/sizes/hint visibility only; no ride counter or gameplay settings.
namespace huddefaults {
inline constexpr char Layout[] = R"FVOHUD(
cluster = 371.0 -247.0 62.0 0
column = 726.0 -254.0 54.5
gearbox = 431.4 63.2
nudge_W = 7.0 -63.0
nudge_A = -24.0 2.0
nudge_S = 0.0 0.0
nudge_D = 24.0 1.0
nudge_geardown = -767.0 237.0
nudge_gearup = 0.0 0.0
nudge_hover = -770.0 92.0
scanner_key = 1739.0 961.0
scanner_follow = 1665.0 961.0
scanner_status = 1701.0 908.0
route = 38.0 111.0
attitude = 2.0 415.0
attitude_text = 2.0 337.5
show_all = 1
size_W = 1.25
size_A = 1.30
size_S = 1.25
size_D = 1.25
size_geardown = 1.55
size_gearup = 1.50
size_hover = 1.50
size_gear = 1.30
size_foot_summon = 1.15
size_foot_follow = 1.15
size_foot_getin = 1.15
size_foot_status = 1.15
size_foot_gang = 1.15
size_foot_pass = 1.15
size_foot_eject = 1.15
size_foot_vehicles = 1.15
size_autoline = 1.80
size_ganginveh = 1.90
size_vehiclesinveh = 1.80
size_route = 1.30
size_attitude = 0.90
size_attitude_text = 1.40
foot = -292.4 -237.4
extras = -990.0 -143.0
foot_summon = -9.0 25.0
foot_follow = 165.0 24.0
foot_getin = -96.0 32.0
foot_status = 64.0 -100.0
foot_gang = 65.0 -185.0
foot_pass = 51.0 -88.0
foot_eject = -243.0 -135.0
foot_vehicles = 66.0 -238.0
nudge_autoline = 994.0 -166.0
nudge_ganginveh = 989.0 -484.0
nudge_vehiclesinveh = 992.0 -412.0
prompt_kb_scanner = 46 138 1 1
prompt_kb_summon = 46 168 1 1
prompt_kb_cover = 46 198 1 1
prompt_kb_stance = 46 228 1 1
prompt_kb_hover_toggle = 46 258 1 1
prompt_kb_lift_up = 46 288 1 1
prompt_kb_lift_down = 46 318 1 1
prompt_kb_gear = 46 348 1 1
prompt_kb_boost = 46 378 1 1
prompt_kb_accelerate = 46 408 1 1
prompt_kb_brake = 46 438 1 1
prompt_kb_turn_left = 46 468 1 1
prompt_kb_turn_right = 46 498 1 1
prompt_kb_vertical_boost = 46 528 1 1
prompt_kb_ride_along = 46 558 1 1
prompt_kb_retry_trip = 46 588 1 1
prompt_kb_skip_stop = 46 618 1 1
prompt_kb_own_profile = 46 648 1 1
prompt_kb_show_hints = 46 678 1 1
prompt_kb_recall_escorts = 46 708 1 1
prompt_kb_crew_drive = 46 738 1 1
prompt_kb_auto_toggle = 46 768 1 1
prompt_kb_clear_route = 746 138 1 1
prompt_kb_guard_point = 746 168 1 1
prompt_kb_focus_target = 746 228 1 1
prompt_kb_clear_focus = 746 258 1 1
prompt_kb_throttle_lock = 746 288 1 1
prompt_kb_passenger_exit = 746 318 1 1
prompt_kb_take_wheel = 746 348 1 1
prompt_kb_driver_exit = 746 378 1 1
prompt_kb_board_passenger = 746 408 1 1
prompt_kb_pull_driver = 746 438 1 1
prompt_kb_flight_assist = 746 468 1 1
prompt_kb_save_placement = 746 498 1 1
prompt_kb_delete_placement = 746 528 1 1
prompt_kb_set_summon_point = 746 558 1 1
prompt_kb_hud_edit = 746 588 1 1
prompt_kb_ship_preview = 746 618 1 1
prompt_kb_npc_profile = 746 648 1 1
prompt_kb_scanner_orders = 746 678 1 1
prompt_kb_native_fire = 746 708 1 1
prompt_kb_native_aim = 746 738 1 1
prompt_kb_native_look = 746 768 1 1
prompt_pad_scanner = 0 0 1 0
prompt_pad_summon = 0 0 1 0
prompt_pad_cover = 0 0 1 0
prompt_pad_stance = 0 0 1 0
prompt_pad_hover_toggle = 0 0 1 0
prompt_pad_lift_up = 0 0 1 0
prompt_pad_lift_down = 0 0 1 0
prompt_pad_gear = 0 0 1 0
prompt_pad_boost = 0 0 1 0
prompt_pad_accelerate = 0 0 1 0
prompt_pad_brake = 0 0 1 0
prompt_pad_turn_left = 0 0 1 0
prompt_pad_turn_right = 0 0 1 0
prompt_pad_vertical_boost = 0 0 1 0
prompt_pad_ride_along = 0 0 1 0
prompt_pad_retry_trip = 0 0 1 0
prompt_pad_skip_stop = 0 0 1 0
prompt_pad_own_profile = 0 0 1 0
prompt_pad_show_hints = 0 0 1 0
prompt_pad_recall_escorts = 0 0 1 0
prompt_pad_crew_drive = 0 0 1 0
prompt_pad_auto_toggle = 0 0 1 0
prompt_pad_clear_route = 0 0 1 0
prompt_pad_guard_point = 0 0 1 0
prompt_pad_focus_target = 0 0 1 0
prompt_pad_clear_focus = 0 0 1 0
prompt_pad_throttle_lock = 0 0 1 0
prompt_pad_passenger_exit = 0 0 1 0
prompt_pad_take_wheel = 0 0 1 0
prompt_pad_driver_exit = 0 0 1 0
prompt_pad_board_passenger = 0 0 1 0
prompt_pad_pull_driver = 0 0 1 0
prompt_pad_flight_assist = 0 0 1 0
prompt_pad_save_placement = 0 0 1 0
prompt_pad_delete_placement = 0 0 1 0
prompt_pad_set_summon_point = 0 0 1 0
prompt_pad_hud_edit = 0 0 1 0
prompt_pad_ship_preview = 0 0 1 0
prompt_pad_npc_profile = 0 0 1 0
prompt_pad_scanner_orders = 0 0 1 0
prompt_pad_native_fire = 0 0 1 0
prompt_pad_native_aim = 0 0 1 0
prompt_pad_native_look = 0 0 1 0
child_foot_0 = 0.000000 0.000000 1.000000
child_foot_1 = 0.000000 0.000000 1.000000
child_foot_2 = 0.000000 0.000000 1.000000
child_foot_3 = 0.000000 0.000000 1.000000
child_foot_4 = 0.000000 0.000000 1.000000
child_foot_5 = 0.000000 0.000000 1.000000
child_foot_6 = 0.000000 0.000000 1.000000
child_foot_7 = 0.000000 0.000000 1.000000
child_car_0 = 0.000000 0.000000 1.000000
child_car_1 = 0.000000 0.000000 1.000000
child_car_2 = -400.000000 407.407407 1.000000
child_car_3 = 0.000000 0.000000 1.000000
child_car_4 = 0.000000 0.000000 1.000000
child_car_5 = 0.000000 0.000000 1.000000
child_car_6 = 0.000000 0.000000 1.000000
child_car_7 = 0.000000 0.000000 1.000000
child_route_0 = 0.000000 0.000000 1.000000
child_route_1 = 0.000000 0.000000 1.000000
child_route_2 = 0.000000 0.000000 1.000000
child_route_3 = 0.000000 0.000000 1.000000
child_route_4 = 0.000000 0.000000 1.000000
child_route_5 = 0.000000 0.000000 1.000000
child_route_6 = 0.000000 0.000000 1.000000
child_route_7 = 0.000000 0.000000 1.000000
child_route_8 = 0.000000 0.000000 1.000000
child_route_9 = 0.000000 0.000000 1.000000
child_route_10 = 0.000000 0.000000 1.000000
child_route_11 = 0.000000 0.000000 1.000000
child_route_12 = 0.000000 0.000000 1.000000
child_route_13 = 0.000000 0.000000 1.000000
child_route_14 = 0.000000 0.000000 1.000000
child_route_15 = 0.000000 0.000000 1.000000
child_action_0 = 0.000000 0.000000 1.000000
child_action_1 = 0.000000 0.000000 1.000000
child_action_2 = 0.000000 0.000000 1.000000
)FVOHUD";
inline const hudchildren::Layout& Children(){
    static const auto value=[] {
        hudchildren::Layout result;
        std::istringstream input(Layout);std::string line;
        while(std::getline(input,line))hudchildren::Load(line,result);
        return result;
    }();
    return value;
}
inline hudchildren::Placement Child(bool vehicle,int selection){
    auto defaults=Children();
    if(const auto* p=hudchildren::Find(defaults,vehicle,selection))return *p;
    return {};
}
}
