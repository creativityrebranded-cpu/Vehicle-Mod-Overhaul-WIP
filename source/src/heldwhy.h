#pragma once
// PD-1c (2026-11-12, notes/logic-fix-20261112/DESIGN.md): the addon's lane `held` carries its reasons when bit 30 is set
// (traffic/coordinator.h Holds::Hold, same bit values as the traffic.lane_hold trace). An older addon sends 1: held, reason not given.
// An older core reads any non-zero value as held, as before. Pure; tests: tests/coordinator_test.cpp.
namespace heldwhy {
inline constexpr int kHeldReasons = 0x40000000;
inline const char* Name(int held) {
    if (!held) return "";
    if (!(held & kHeldReasons)) return "route hold (reason not given)";
    const unsigned b = unsigned(held) & 0xFFFFu;
    return b & 512 ? "route hold: junction right of way" : b & 256 ? "route hold: stop line" : b & 1024 ? "route hold: following the car ahead" :
           b & 128 ? "route hold: pickup wait" : b & 4 ? "route hold: connection blocked" : b & 2 ? "route hold: script" :
           b & 16 ? "route hold: zone closed" : b & 32 ? "route hold: zone mode" : b & 64 ? "route hold: lane plan inactive" :
           b & 8 ? "route hold: excluded model" : b & 1 ? "route hold: holding" : "route hold";
}
}
