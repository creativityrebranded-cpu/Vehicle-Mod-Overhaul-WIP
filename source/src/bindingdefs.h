#pragma once
#include "bindlogic.h"
#include <cstddef>

// One semantic command per row. Context and gesture are part of each device slot.
namespace bindings {
enum class Action {
    Scanner, Summon, Cover, Stance, HoverToggle, LiftUp, LiftDown, Gear,
    Boost, Accelerate, Brake, TurnLeft, TurnRight, VerticalBoost, RideAlong,
    RetryTrip, SkipStop, OwnProfile, ShowHints,
    RecallEscorts, CrewDrive, AutoToggle, ClearRoute, GuardPoint,
    FocusTarget, ClearFocus, ThrottleLock, PassengerExit, TakeWheel,
    DriverExit, BoardPassenger, PullDriver, FlightAssist,
    SavePlacement, DeletePlacement, SetSummonPoint,
    HudEdit, HudPrevious, HudNext, HudLeft, HudRight, HudUp, HudDown,
    HudSpacingLess, HudSpacingMore, HudGrow, HudShrink, HudReset, HudFast,
    ShipPreview, NpcProfile, Count
};
enum class Layer { Gameplay, Target, Placement, Editor };
struct Def {
    const wchar_t* key;
    const char* spec;
    const char* what;
    const char* gameContext;
    const char* gameEvent;
    int defaultVk;
    Layer layer = Layer::Gameplay;
};
inline constexpr Def kDefs[] = {
    {L"scanner", "default-key:tap@scanner, Pad:Y:tap@scanner", "Select destination / append route stop", nullptr,nullptr,'T',Layer::Target},
    {L"summon", "default-key:tap@foot|scanner, Pad:B:tap@scanner", "Summon your vehicle",nullptr,nullptr,'N'},
    {L"cover", "default-key:tap@foot|scanner, Pad:LS:tap@scanner", "Follow / cover on or off",nullptr,nullptr,'U'},
    {L"stance", "default-key@foot|vehicle|scanner, Pad:DRight@vehicle, Pad:View:tap@scanner", "Cycle Defend / Attack / Hold fire",nullptr,nullptr,'K'},
    {L"hover_toggle", "default-key@vehicle, Pad:LS@vehicle", "Toggle hover / land",nullptr,nullptr,'V'},
    {L"lift_up", "default-key@flying, Pad:Y@flying", "Climb",nullptr,nullptr,0x20},
    {L"lift_down", "default-key@flying, Pad:X@flying, Pad:A:hold@flying", "Descend",nullptr,nullptr,'C'},
    {L"gear", "default-key:tap@vehicle, Pad:A:tap@vehicle", "Change gear",nullptr,nullptr,0xA4},
    {L"boost", "default-key@vehicle, Pad:RB@vehicle", "Boost while held", "Vehicle","VehicleBoost",0xA0},
    {L"accelerate", "default-key@vehicle|passenger, Pad:LStickUp@vehicle|passenger", "Accelerate", "Vehicle","VehicleAccel",'W'},
    {L"brake", "default-key@vehicle|passenger, Pad:LStickDown@vehicle|passenger", "Brake", "Vehicle","VehicleBrake",'S'},
    {L"turn_left", "default-key@vehicle, Pad:LStickLeft@vehicle", "Turn / roll left", "Vehicle","VehicleTurnLeft",'A'},
    {L"turn_right", "default-key@vehicle, Pad:LStickRight@vehicle", "Turn / roll right", "Vehicle","VehicleTurnRight",'D'},
    {L"vertical_boost", "default@ground", "Ground vertical boost", "Vehicle","VehicleVertBoost",0x20},
    {L"ride_along", "default-key:tap@foot|scanner, Pad:DRight:hold@scanner", "Board a nearby NPC vehicle as passenger",nullptr,nullptr,'G'},
    {L"retry_trip", "default-key@foot|vehicle|scanner, Pad:DLeft:tap@scanner", "Retry held trip",nullptr,nullptr,0xBA},
    {L"skip_stop", "default-key@foot|vehicle|scanner, Pad:DRight:tap@scanner", "Skip route stop",nullptr,nullptr,0xDB},
    {L"own_profile", "default-key@foot|vehicle|scanner, Pad:DLeft:hold@vehicle|scanner", "Cycle own vehicle navigation profile",nullptr,nullptr,0xDD},
    {L"show_hints", "default-key@foot|vehicle|scanner, Pad:RS:tap@vehicle, Pad:View:hold@scanner", "Show / hide all hints",nullptr,nullptr,'J'},
    {L"recall_escorts", "default-key:hold@foot|vehicle|scanner, Pad:B:hold@scanner, Pad:DUp@vehicle", "Recall escorts (and your vehicle on foot)",nullptr,nullptr,'N'},
    {L"crew_drive", "default-key:hold@foot|scanner, Pad:LS:hold@scanner", "Ask a follower to drive your vehicle",nullptr,nullptr,'U'},
    {L"auto_toggle", "default-key:tap@vehicle, Pad:DLeft:tap@vehicle", "Autopilot on / off",nullptr,nullptr,'T'},
    {L"clear_route", "default-key:double@scanner, Pad:Y:double@scanner", "Clear route / hold position",nullptr,nullptr,'T',Layer::Target},
    {L"guard_point", "default-key:hold@scanner, Pad:Y:hold@scanner", "Guard the aimed location / fly there and hold; on a person or vehicle: focus fire on it",nullptr,nullptr,'T',Layer::Target},
    {L"focus_target", "default-key@scanner, Pad:LS:double@scanner", "Focus fire on the aimed hostile person or vehicle",nullptr,nullptr,'H',Layer::Target},
    {L"clear_focus", "default-key@foot|vehicle|scanner, Pad:DLeft:double@scanner", "Clear combat focus",nullptr,nullptr,'O'},
    {L"throttle_lock", "default-key:double@flying, Pad:RB:double@flying", "Toggle throttle lock",nullptr,nullptr,0xA0},
    {L"passenger_exit", "default-key:tap@passenger, Pad:A:tap@passenger", "Exit passenger seat",nullptr,nullptr,'G'},
    {L"take_wheel", "default-key:hold@passenger, Pad:A:hold@passenger", "Take the wheel from passenger seat",nullptr,nullptr,'G'},
    {L"driver_exit", "default@vehicle", "Exit driver seat (land first while hovering)","Vehicle","VehicleExit",'E'},
    {L"board_passenger", "default:tap@foot", "Ride in the aimed occupied vehicle", "MainGameplay","Activate",'E'},
    {L"pull_driver", "default:hold@foot", "Pull out the aimed driver / take their seat", "MainGameplay","Activate",'E'},
    {L"flight_assist", "default-key@flying, Pad:DDown@flying", "Flight assist on / off",nullptr,nullptr,'B'},
    {L"save_placement", "default-key:tap@scanner, Pad:Y:tap@scanner", "Record parking spot",nullptr,nullptr,'T',Layer::Placement},
    {L"delete_placement", "default-key:hold@scanner, Pad:Y:hold@scanner", "Delete aimed parking spot",nullptr,nullptr,'T',Layer::Placement},
    {L"set_summon_point", "default-key:double@scanner, Pad:Y:double@scanner", "Set area summon point",nullptr,nullptr,'T',Layer::Placement},
    {L"hud_edit", "default-key, Pad:View:double@scanner, Pad:B@editor", "HUD editor on / off (save)",nullptr,nullptr,0x76},
    {L"hud_previous", "default-key@editor, Pad:LB@editor", "HUD: previous element",nullptr,nullptr,0x21,Layer::Editor},
    {L"hud_next", "default-key@editor, Pad:RB@editor", "HUD: next element",nullptr,nullptr,0x22,Layer::Editor},
    {L"hud_left", "default-key@editor, Pad:LStickLeft@editor", "HUD: move left",nullptr,nullptr,0x25,Layer::Editor},
    {L"hud_right", "default-key@editor, Pad:LStickRight@editor", "HUD: move right",nullptr,nullptr,0x27,Layer::Editor},
    {L"hud_up", "default-key@editor, Pad:LStickUp@editor", "HUD: move up",nullptr,nullptr,0x26,Layer::Editor},
    {L"hud_down", "default-key@editor, Pad:LStickDown@editor", "HUD: move down",nullptr,nullptr,0x28,Layer::Editor},
    {L"hud_spacing_less", "default-key@editor, Pad:DLeft@editor", "HUD: decrease spacing",nullptr,nullptr,0x24,Layer::Editor},
    {L"hud_spacing_more", "default-key@editor, Pad:DRight@editor", "HUD: increase spacing",nullptr,nullptr,0x23,Layer::Editor},
    {L"hud_grow", "default-key@editor, NumpadPlus@editor, Pad:RT@editor", "HUD: increase size",nullptr,nullptr,0xBB,Layer::Editor},
    {L"hud_shrink", "default-key@editor, NumpadMinus@editor, Pad:LT@editor", "HUD: decrease size",nullptr,nullptr,0xBD,Layer::Editor},
    {L"hud_reset", "default-key@editor, Pad:X@editor", "HUD: reset selected element",nullptr,nullptr,0x2E,Layer::Editor},
    {L"hud_fast", "default-key@editor, Pad:LS@editor", "HUD: larger movement step",nullptr,nullptr,0x11,Layer::Editor},
    {L"ship_preview", "default-key@vehicle, Pad:RS:hold@vehicle", "Toggle ship hull preview",nullptr,nullptr,0x77},
    {L"npc_profile", "default-key@foot|scanner, Pad:DRight:double@scanner", "Cycle escort NPC navigation profile",nullptr,nullptr,0x78},
};
static_assert(std::size(kDefs)==std::size_t(Action::Count));
inline bool LayersOverlap(Layer a,Layer b) {
    return !((a==Layer::Target&&b==Layer::Placement)||(a==Layer::Placement&&b==Layer::Target));
}
}
