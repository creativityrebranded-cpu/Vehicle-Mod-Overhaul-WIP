#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

// Child offsets live in panel coordinates. A panel move/resize therefore keeps
// the arrangement; nudging a child changes only that child, by one displayed unit.
namespace hudchildren {
inline constexpr int CardCount=8, RouteCount=16, ActionCount=3;
inline constexpr int CardBase=200, RouteBase=220, ActionBase=240;
struct Placement { double x=0,y=0,scale=1; };
struct Layout {
    std::array<std::array<Placement,CardCount>,2> card{};
    std::array<Placement,RouteCount> route{};
    std::array<Placement,ActionCount> actions{};
    // The route panel shown while the scanner is held keeps its own line offsets (2026-10-27, the user: sizing one
    // changed the other). Copied from the plain ones when a saved layout has none.
    std::array<Placement,RouteCount> routeScan{};
    std::array<Placement,ActionCount> actionsScan{};
    bool haveScan=false;
};
inline bool In(int s,int base,int count){return s>=base&&s<base+count;}
inline Placement* Find(Layout& l,bool vehicle,int s,bool scanner=false){
    if(In(s,CardBase,CardCount))return &l.card[vehicle?1:0][s-CardBase];
    if(In(s,RouteBase,RouteCount))return scanner?&l.routeScan[s-RouteBase]:&l.route[s-RouteBase];
    if(In(s,ActionBase,ActionCount))return scanner?&l.actionsScan[s-ActionBase]:&l.actions[s-ActionBase];
    return nullptr;
}
inline bool IsSelection(int s){return In(s,CardBase,CardCount)||In(s,RouteBase,RouteCount)||In(s,ActionBase,ActionCount);}
inline std::string Name(int s){
    if(In(s,CardBase,CardCount)){
        const char* names[]={"Status: heading","Status: health line 1","Status: health line 2","Status: trip","Status: escort 1","Status: escort 2","Status: escort 3","Status: escort 4"};
        return names[s-CardBase];
    }
    if(In(s,RouteBase,RouteCount)){
        const int i=s-RouteBase;
        if(i<11)return "Route: text line "+std::to_string(i+1);
        return i==11?"Route: escorts heading":"Route: escort "+std::to_string(i-11);
    }
    if(In(s,ActionBase,ActionCount))return "Route: action "+std::to_string(s-ActionBase+1);
    return {};
}
inline void Move(Placement& p,double dx,double dy,double parentScale){
    if(!std::isfinite(parentScale)||parentScale<=0)return;
    p.x+=dx/parentScale;p.y+=dy/parentScale;
}
inline Placement Apply(const Placement& p,double x,double y,double parentScale){
    return {x+parentScale*p.x,y+parentScale*p.y,parentScale*p.scale};
}
inline std::string Save(const Layout& l){
    std::string out;char line[128];
    const auto write=[&](const char* group,int i,const Placement& p){
        std::snprintf(line,sizeof(line),"child_%s_%d = %.6f %.6f %.6f\n",group,i,p.x,p.y,p.scale);out+=line;
    };
    for(int c=0;c<2;++c)for(int i=0;i<CardCount;++i)write(c?"car":"foot",i,l.card[c][i]);
    for(int i=0;i<RouteCount;++i)write("route",i,l.route[i]);
    for(int i=0;i<ActionCount;++i)write("action",i,l.actions[i]);
    for(int i=0;i<RouteCount;++i)write("sroute",i,l.routeScan[i]);
    for(int i=0;i<ActionCount;++i)write("saction",i,l.actionsScan[i]);
    return out;
}
inline bool Load(const std::string& line,Layout& l){
    char group[16]{};int i=-1;double x=0,y=0,s=1;
    if(std::sscanf(line.c_str(),"child_%15[^_]_%d = %lf %lf %lf",group,&i,&x,&y,&s)!=5 ||
        !std::isfinite(x)||!std::isfinite(y)||!std::isfinite(s))return false;
    Placement* p=nullptr;const std::string g=group;
    if((g=="car"||g=="foot")&&i>=0&&i<CardCount)p=&l.card[g=="car"?1:0][i];
    if(g=="route"&&i>=0&&i<RouteCount)p=&l.route[i];
    if(g=="action"&&i>=0&&i<ActionCount)p=&l.actions[i];
    if(g=="sroute"&&i>=0&&i<RouteCount){p=&l.routeScan[i];l.haveScan=true;}
    if(g=="saction"&&i>=0&&i<ActionCount){p=&l.actionsScan[i];l.haveScan=true;}
    if(!p)return false;
    *p={x,y,std::clamp(s,.5,2.5)};return true;
}
}
