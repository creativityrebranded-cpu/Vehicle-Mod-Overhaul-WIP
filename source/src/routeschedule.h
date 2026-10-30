#pragma once
#include "fvolimits.h"
// Pure route-schedule policy. Times are minutes of the verified planet-local
// game day, not host time or UT. The engine clock adapter must supply valid=false
// when local time is unknown; no live clock binding is implied by this module.
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace routeschedule {
constexpr int kMinutesPerDay=24*60;
struct Window { int start=0, end=0, vehicles=1; }; // [start,end), may cross midnight
struct Schedule { bool allDay=true; int allDayVehicles=1; std::vector<Window> windows; };
enum class Issue { None, NoWindows, InvalidTime, EmptyWindow, InvalidDensity, Overlap };
inline bool Contains(const Window& w,int minute) {
    return w.start<w.end ? minute>=w.start && minute<w.end : minute>=w.start || minute<w.end;
}
inline Issue Validate(const Schedule& s) {
    // 64 vehicles at most (2026-10-29: was 16 here while windows allowed 64, so a 64-vehicle all-day route failed every check).
    if(s.allDay) return s.allDayVehicles<0 || s.allDayVehicles>fvolimits::kVehicles ? Issue::InvalidDensity : Issue::None;
    if(s.windows.empty()) return Issue::NoWindows;
    for(const auto& w:s.windows) {
        if(w.start<0 || w.start>=kMinutesPerDay || w.end<0 || w.end>=kMinutesPerDay) return Issue::InvalidTime;
        if(w.start==w.end) return Issue::EmptyWindow;
        if(w.vehicles<0 || w.vehicles>fvolimits::kVehicles) return Issue::InvalidDensity;
    }
    // Bounds are small and exact in editor units (one minute), including midnight.
    for(int minute=0;minute<kMinutesPerDay;++minute) {
        int active=0;
        for(const auto& w:s.windows) if(Contains(w,minute) && ++active>1) return Issue::Overlap;
    }
    return Issue::None;
}
// INI format: all-day density uses density=N; scheduled windows are a comma-
// separated list of start minute, end minute, max vehicles. E.g. "1080 360 2,
// 360 1080 4". Strict parsing prevents malformed schedules becoming all-day.
inline bool ParseWindows(const std::string& text,Schedule& out) {
    Schedule next; next.allDay=false;
    std::istringstream input(text); std::string part;
    while(std::getline(input,part,',')) {
        std::istringstream fields(part); Window w; std::string extra;
        if(!(fields>>w.start>>w.end>>w.vehicles) || (fields>>extra)) return false;
        next.windows.push_back(w);
    }
    if(text.empty() || text.back()==',' || Validate(next)!=Issue::None) return false;
    out=std::move(next); return true;
}
struct Decision { bool known=false, active=false; int vehicles=0; };
// What a failed Validate means, for the map editor's messages.
inline const char* IssueText(Issue i) {
    switch(i) {
    case Issue::NoWindows: return "the schedule has no time windows";
    case Issue::InvalidTime: return "a window's time is outside the day";
    case Issue::EmptyWindow: return "a window starts and ends at the same time";
    case Issue::InvalidDensity: return "a vehicle count is outside 0 to the vehicle limit (fvolimits.h)";
    case Issue::Overlap: return "two time windows overlap";
    default: return "";
    }
}
inline Decision Evaluate(const Schedule& s,bool localTimeKnown,int localMinute) {
    if(Validate(s)!=Issue::None) return {};
    if(s.allDay) return {true,s.allDayVehicles>0,s.allDayVehicles};
    if(!localTimeKnown || localMinute<0 || localMinute>=kMinutesPerDay) return {};
    for(const auto& w:s.windows) if(Contains(w,localMinute)) return {true,w.vehicles>0,w.vehicles};
    return {true,false,0};
}
}
