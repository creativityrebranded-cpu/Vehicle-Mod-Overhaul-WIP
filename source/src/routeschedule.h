#pragma once
#include "fvolimits.h"
// Pure route-schedule policy. Times are minutes of the verified planet-local
// game day, not host time or UT. The engine clock adapter must supply valid=false
// when local time is unknown; no live clock binding is implied by this module.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace routeschedule {
constexpr int kMinutesPerDay=24*60;
struct Window { int start=0, end=0, vehicles=1; }; // [start,end), may cross midnight
// Time rules by busy words (2026-10-30, the user: split the day into zones, each called Closed, Quiet, Steady, Busy or Rush, with
// editable percentages; the count eases between zones). A zone runs from its start to the next zone's. Expand turns them into
// ordinary windows, so everything that reads a schedule stays as it is: a lower count takes no new vehicles and those already
// flying finish their lap, land and stay parked.
constexpr int kWords=5;
inline const char* WordName(int word) { static const char* const names[kWords]={"Closed","Quiet","Steady","Busy","Rush"}; return names[word>=0&&word<kWords?word:0]; }
struct Zone { int start=0, word=2; };
struct Schedule {
    bool allDay=true; int allDayVehicles=1; std::vector<Window> windows;
    std::vector<Zone> zones;              // empty: plain windows or all day
    int peak=0;                           // vehicles at 100%
    int levels[kWords]={0,25,50,75,100};  // each word's share of peak
    int blend=30;                         // minutes to ease from one zone to the next
};
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
    // Two windows that share a minute: one holds the other's first minute (midnight included). Evaluate calls this every time.
    for(std::size_t i=0;i<s.windows.size();++i) for(std::size_t j=i+1;j<s.windows.size();++j)
        if(Contains(s.windows[i],s.windows[j].start) || Contains(s.windows[j],s.windows[i].start)) return Issue::Overlap;
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
inline int ZoneVehicles(int peak,double percent) { return percent<=0?0:std::max(peak>0?1:0,int(std::lround(peak*percent/100.0))); }
inline void SortZones(Schedule& s) { std::stable_sort(s.zones.begin(),s.zones.end(),[](const Zone& a,const Zone& b){return a.start<b.start;}); }
// The zones as windows. Closed has no window (so a branch fed by junctions counts as shut then, as outside its hours).
inline void Expand(Schedule& s) {
    s.windows.clear();
    if(s.zones.empty()) return;
    SortZones(s);
    s.allDay=false;
    const int n=int(s.zones.size());
    auto level=[&](int i){ const int w=s.zones[std::size_t(i)].word; return double(s.levels[w>=0&&w<kWords?w:0]); };
    auto add=[&](int from,int to,int vehicles,bool open){
        from%=kMinutesPerDay; to%=kMinutesPerDay;
        if(from==to||!open) return;
        if(!s.windows.empty()&&s.windows.back().end==from&&s.windows.back().vehicles==vehicles&&s.windows.back().start!=to) s.windows.back().end=to;
        else s.windows.push_back({from,to,vehicles});
    };
    for(int i=0;i<n;++i) {
        const int start=s.zones[std::size_t(i)].start, next=n>1?s.zones[std::size_t((i+1)%n)].start:start;
        int length=(next-start+kMinutesPerDay)%kMinutesPerDay;
        if(!length) length=kMinutesPerDay;
        const double to=level(i), from=n>1?level((i+n-1)%n):to;
        const int ramp=from==to?0:std::min(std::max(0,s.blend),length-1);
        int at=start;
        for(int step=1;step<=3&&ramp>=3;++step) {  // three even steps up or down
            const double pct=from+(to-from)*step/4.0;
            add(at,start+ramp*step/3,ZoneVehicles(s.peak,pct),pct>0);
            at=start+ramp*step/3;
        }
        const int end=start+length;
        if(end-at==kMinutesPerDay) { add(at,at+720,ZoneVehicles(s.peak,to),to>0); add(at+720,end,ZoneVehicles(s.peak,to),to>0); }  // a window can't start and end at one minute
        else add(at,end,ZoneVehicles(s.peak,to),to>0);
    }
    if(s.windows.size()>1&&s.windows.back().end==s.windows.front().start&&s.windows.back().vehicles==s.windows.front().vehicles&&s.windows.back().start!=s.windows.front().end) {
        s.windows.front().start=s.windows.back().start; s.windows.pop_back();
    }
}
// INI: day = 0 quiet, 360 busy (start minute, word); levels = five percentages. False: not understood, nothing changed.
inline bool ParseDay(const std::string& text,Schedule& out) {
    std::vector<Zone> zones;
    std::istringstream input(text); std::string part;
    while(std::getline(input,part,',')) {
        std::istringstream fields(part); Zone z; std::string word,extra;
        if(!(fields>>z.start>>word)||(fields>>extra)||z.start<0||z.start>=kMinutesPerDay) return false;
        for(auto& c:word) if(c>='A'&&c<='Z') c=char(c-'A'+'a');
        z.word=-1;
        for(int k=0;k<kWords;++k) { std::string name=WordName(k); name[0]=char(name[0]-'A'+'a'); if(name==word) z.word=k; }
        if(z.word<0) return false;
        zones.push_back(z);
    }
    if(zones.empty()) return false;
    for(std::size_t i=0;i<zones.size();++i) for(std::size_t j=i+1;j<zones.size();++j) if(zones[i].start==zones[j].start) return false;
    out.zones=std::move(zones);
    return true;
}
inline std::string FormatDay(const Schedule& s) {
    std::string t;
    for(const auto& z:s.zones) {
        std::string name=WordName(z.word); name[0]=char(name[0]-'A'+'a');
        t+=(t.empty()?"":", ")+std::to_string(z.start)+" "+name;
    }
    return t;
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
