#pragma once
#include "lanevolume.h"
#include "routepolicy.h"
#include "travelrules.h"
#include "lanevehicles.h"
#include "lanetransform.h"
#include <sstream>
#include <iomanip>
#include <map>
#include <set>

namespace laneplan {
constexpr int kMaxEachWay=8, kMaxLanes=16;
enum class Movement { Legacy, Locked, Adaptive, Network, Scripted, Free };
enum class Obstacles { Stop, InLane, Ignore };
enum Field { Forward, Reverse, Width, Height, Gap, Shape, Radius, TopWidth,
    Offset, Elevation, Bank, Taper, Travel, Authority, Static, Moving, People, Recovery,
    ModeRule, BypassRule, MinHeight, MaxHeight, HoverHeight, SupportRange, SettleTime, Fields };
struct Settings {
    int forward=1,reverse=1;
    lanevolume::Profile profile;
    double gap=0,taper=15;
    routepolicy::Travel travel=routepolicy::Travel::Inherit;
    Movement movement=Movement::Locked;
    Obstacles obstacles=Obstacles::Stop,moving=Obstacles::Stop,people=Obstacles::Stop;
    bool recovery=false;
    travelrules::Rules rules;
};
struct Override { int section=-1,lane=-1; std::uint64_t mask=0; Settings value; };
struct Plan {
    bool enabled=false;bool inherited=false;
    lanevehicles::Rules vehicles;
    lanetransform::Plan transforms;
    Settings base;
    std::vector<Override> overrides;
    std::string unsupported; // future/invalid records survive old-editor round trips
    std::vector<std::uint64_t> sectionIds; // route-scoped stable authored section identities
    std::uint64_t nextSectionId=1;
};
inline void EnsureSections(Plan& p,std::size_t count) {
    if(!p.enabled||!p.unsupported.empty())return;
    while(p.sectionIds.size()<count)p.sectionIds.push_back(p.nextSectionId++);
    if(p.sectionIds.size()>count)p.sectionIds.resize(count);
}
inline double Value(const Settings& s,Field f) {
    switch(f){
    case Forward:return s.forward;case Reverse:return s.reverse;case Width:return s.profile.width;case Height:return s.profile.height;
    case Gap:return s.gap;case Shape:return int(s.profile.shape);case Radius:return s.profile.radius;case TopWidth:return s.profile.topWidth;
    case Offset:return s.profile.offset;case Elevation:return s.profile.elevation;case Bank:return s.profile.bank;case Taper:return s.taper;
    case Travel:return int(s.travel);case Authority:return int(s.movement);case Static:return int(s.obstacles);case Moving:return int(s.moving);
    case People:return int(s.people);case Recovery:return s.recovery;
    case ModeRule:return s.rules.mode;case BypassRule:return s.rules.bypass;case MinHeight:return s.rules.minHeight;
    case MaxHeight:return s.rules.maxHeight;case HoverHeight:return s.rules.hoverHeight;case SupportRange:return s.rules.supportRange;case SettleTime:return s.rules.settleSeconds;default:return 0;}
}
inline void FieldCopy(Settings& a,const Settings& b,Field f) {
    switch(f) {
    case Forward:a.forward=b.forward;break;case Reverse:a.reverse=b.reverse;break;
    case Width:a.profile.width=b.profile.width;break;case Height:a.profile.height=b.profile.height;break;
    case Gap:a.gap=b.gap;break;case Shape:a.profile.shape=b.profile.shape;break;
    case Radius:a.profile.radius=b.profile.radius;break;case TopWidth:a.profile.topWidth=b.profile.topWidth;break;
    case Offset:a.profile.offset=b.profile.offset;break;case Elevation:a.profile.elevation=b.profile.elevation;break;
    case Bank:a.profile.bank=b.profile.bank;break;case Taper:a.taper=b.taper;break;case Travel:a.travel=b.travel;break;
    case Authority:a.movement=b.movement;break;case Static:a.obstacles=b.obstacles;break;
    case Moving:a.moving=b.moving;break;case People:a.people=b.people;break;case Recovery:a.recovery=b.recovery;break;
    case ModeRule:a.rules.mode=b.rules.mode;break;case BypassRule:a.rules.bypass=b.rules.bypass;break;
    case MinHeight:a.rules.minHeight=b.rules.minHeight;break;case MaxHeight:a.rules.maxHeight=b.rules.maxHeight;break;
    case HoverHeight:a.rules.hoverHeight=b.rules.hoverHeight;break;case SupportRange:a.rules.supportRange=b.rules.supportRange;break;case SettleTime:a.rules.settleSeconds=b.rules.settleSeconds;break;
    default:break;
    }
}
inline Settings Resolve(const Plan& p,int section=-1,int lane=-1) {
    auto out=p.base;
    // Broad defaults, then section, then lane, then lane-section overrides.
    for(int pass=0;pass<3;++pass) for(const auto& r:p.overrides) {
        if((pass==0&&r.section==section&&section>=0&&r.lane<0)||
           (pass==1&&r.section<0&&r.lane==lane&&lane>=0)||
           (pass==2&&r.section==section&&section>=0&&r.lane==lane&&lane>=0))
            for(int f=0;f<Fields;++f) if(r.mask&(1ull<<f)) FieldCopy(out,r.value,Field(f));
    }
    return out;
}
inline void Set(Plan& p,int section,int lane,Field field,const Settings& value) {
    if(!p.enabled||!p.unsupported.empty()) return;
    if(section<0&&lane<0) { FieldCopy(p.base,value,field);return; }
    for(auto& r:p.overrides) if(r.section==section&&r.lane==lane) { r.mask|=1ull<<field;FieldCopy(r.value,value,field);return; }
    Override r;r.section=section;r.lane=lane;r.mask=1ull<<field;FieldCopy(r.value,value,field);p.overrides.push_back(r);
}
inline bool Valid(const Settings& s) {
    return travelrules::Valid(s.rules)&&s.forward>=0&&s.reverse>=0&&s.forward<=kMaxEachWay&&s.reverse<=kMaxEachWay&&s.forward+s.reverse>0&&
        lanevolume::Valid(s.profile)&&s.profile.width<=100&&s.profile.height<=200&&s.profile.topWidth<=100&&
        std::isfinite(s.gap)&&s.gap>=0&&s.gap<=100&&std::isfinite(s.taper)&&s.taper>=0&&s.taper<=2000&&
        std::fabs(s.profile.offset)<=10000&&std::fabs(s.profile.elevation)<=10000&&std::fabs(s.profile.bank)<=lanevolume::kPi&&
        int(s.travel)>=-1&&int(s.travel)<=2&&int(s.movement)>=0&&int(s.movement)<=5&&
        int(s.obstacles)>=0&&int(s.obstacles)<=2&&int(s.moving)>=0&&int(s.moving)<=2&&int(s.people)>=0&&int(s.people)<=2;
}
// Validate the inherited combinations, not the unused fields in sparse records.
// Group once so validation stays linear in records rather than resolving every
// section by repeatedly scanning the entire plan.
inline const char* Problem(const Plan& p) {
    if(!lanetransform::Valid(p.transforms))return "invalid_lane_transform";
    if(!p.vehicles.unsupported.empty())return "unsupported_lane_vehicle_rules";
    if(!p.unsupported.empty())return "unsupported_lane_plan";
    if(!p.enabled)return nullptr;
    if(!Valid(p.base))return "invalid_lane_defaults";
    {std::set<std::uint64_t> seen;for(auto id:p.sectionIds)if(!id||id>=p.nextSectionId||!seen.insert(id).second)return "invalid_section_identity";}
    std::map<int,std::vector<const Override*>> sections;
    sections[-1];
    for(const auto& r:p.overrides) {
        if(r.section< -1||r.section>100000||r.lane< -1||r.lane>=kMaxLanes||r.mask>=(1ull<<Fields))return "invalid_lane_override";
        sections[r.section].push_back(&r);
    }
    auto apply=[](Settings& s,const Override& r){for(int f=0;f<Fields;++f)if(r.mask&(1ull<<f))FieldCopy(s,r.value,Field(f));};
    const auto& globals=sections.at(-1);
    std::array<Override,kMaxLanes> laneDefaults;
    for(const auto* r:globals)if(r->lane>=0){auto& compiled=laneDefaults[r->lane];apply(compiled.value,*r);compiled.mask|=r->mask;}
    for(const auto& [section,records]:sections) {
        auto bundle=p.base;
        if(section>=0)for(const auto* r:records)if(r->lane<0)apply(bundle,*r);
        if(!Valid(bundle))return "invalid_resolved_lane_bundle";
        for(int lane=0;lane<kMaxLanes;++lane) {
            auto resolved=bundle;
            apply(resolved,laneDefaults[lane]);
            if(section>=0)for(const auto* r:records)if(r->lane==lane)apply(resolved,*r);
            if(!Valid(resolved))return "invalid_resolved_lane_settings";
        }
    }
    return nullptr;
}
inline void WriteSettings(std::ostream& o,const Settings& s,int version=1) {
    const auto& p=s.profile;
    o<<s.forward<<' '<<s.reverse<<' '<<p.width<<' '<<p.height<<' '<<s.gap<<' '<<int(p.shape)<<' '<<p.radius<<' '<<p.topWidth<<' '
     <<p.offset<<' '<<p.elevation<<' '<<p.bank<<' '<<s.taper<<' '<<int(s.travel)<<' '<<int(s.movement)<<' '
     <<int(s.obstacles)<<' '<<int(s.moving)<<' '<<int(s.people)<<' '<<s.recovery;
    if(version>=2)o<<' '<<s.rules.mode<<' '<<s.rules.bypass<<' '<<s.rules.minHeight<<' '<<s.rules.maxHeight<<' '<<s.rules.hoverHeight<<' '<<s.rules.supportRange<<' '<<s.rules.settleSeconds;
}
inline bool ReadSettings(std::istream& in,Settings& s,int version=1) {
    int shape,travel,movement,obstacles,moving,people,recovery;
    auto& p=s.profile;
    if(!(in>>s.forward>>s.reverse>>p.width>>p.height>>s.gap>>shape>>p.radius>>p.topWidth>>p.offset>>p.elevation>>p.bank>>s.taper>>travel>>movement>>obstacles>>moving>>people>>recovery)||recovery<0||recovery>1) return false;
    p.shape=lanevolume::Shape(shape);s.travel=routepolicy::Travel(travel);s.movement=Movement(movement);
    s.obstacles=Obstacles(obstacles);s.moving=Obstacles(moving);s.people=Obstacles(people);s.recovery=recovery!=0;
    if(version>=2&&!(in>>s.rules.mode>>s.rules.bypass>>s.rules.minHeight>>s.rules.maxHeight>>s.rules.hoverHeight>>s.rules.supportRange>>s.rules.settleSeconds))return false;
    return Valid(s);
}
inline bool Extended(const Plan& p) {
    if(!travelrules::Same(p.base.rules,{}))return true;
    for(const auto& r:p.overrides)if((r.mask>>ModeRule)||!travelrules::Same(r.value.rules,{}))return true;
    return false;
}
inline std::string Write(const Plan& p) {
    if(!p.unsupported.empty())return p.unsupported;
    if(!p.enabled)return {};
    const int version=!p.sectionIds.empty()?3:Extended(p)?2:1;
    std::ostringstream out;out<<std::setprecision(17)<<version<<' ';WriteSettings(out,p.base,version);
    out<<' '<<p.overrides.size();for(const auto& r:p.overrides){out<<" | "<<r.section<<' '<<r.lane<<' '<<r.mask<<' ';WriteSettings(out,r.value,version);}
    if(version>=3){out<<" ~ "<<p.nextSectionId<<' '<<p.sectionIds.size();for(auto id:p.sectionIds)out<<' '<<id;}
    return out.str();
}
inline Plan Read(std::string text) {
    Plan p;if(text.empty())return p;
    Plan bad;bad.unsupported=text;
    std::replace(text.begin(),text.end(),'|',' ');std::istringstream in(text);int version;std::size_t count;
    if(!(in>>version)||(version<1||version>3)||!ReadSettings(in,p.base,version)||!(in>>count)||count>65536)return bad;
    for(std::size_t i=0;i<count;++i){Override r;if(!(in>>r.section>>r.lane>>r.mask)||r.section< -1||r.section>100000||r.lane< -1||r.lane>=kMaxLanes||r.mask>=(1ull<<(version==1?int(ModeRule):int(Fields)))||!ReadSettings(in,r.value,version))return bad;p.overrides.push_back(r);}
    if(version>=3){char marker;std::size_t n;if(!(in>>marker>>p.nextSectionId>>n)||marker!='~'||n>100001||!p.nextSectionId)return bad;std::set<std::uint64_t> ids;for(std::size_t i=0;i<n;++i){std::uint64_t id;if(!(in>>id)||!id||id>=p.nextSectionId||!ids.insert(id).second)return bad;p.sectionIds.push_back(id);}}
    std::string extra;if(in>>extra)return bad;p.enabled=true;return p;
}
inline std::vector<int> Lanes(const Settings& s) {
    std::vector<int> out;for(int n=0;n<kMaxEachWay;++n){if(n<s.forward)out.push_back(n*2);if(n<s.reverse)out.push_back(n*2+1);}return out;
}
inline bool Active(const Settings& s,int lane) {return lane>=0&&lane<kMaxLanes&&lane/2<((lane&1)?s.reverse:s.forward);}
inline double Centre(const Settings& s,int lane) {
    const int total=s.forward+s.reverse;
    // Forward lanes are on the right; reverse lanes on the left of source order.
    const int rank=(lane&1)?s.reverse-1-lane/2:s.reverse+lane/2;
    return (rank-(total-1)*.5)*(s.profile.width+s.gap)+s.profile.offset;
}
inline double Centre(const Plan& p,int section,int lane) {
    const auto bundle=Resolve(p,section);std::vector<int> order;
    for(int i=bundle.reverse-1;i>=0;--i)order.push_back(i*2+1);
    for(int i=0;i<bundle.forward;++i)order.push_back(i*2);
    double total=0;for(auto id:order)total+=Resolve(p,section,id).profile.width;
    total+=std::max(0,int(order.size())-1)*bundle.gap;
    double edge=-total*.5;
    for(auto id:order){const auto s=Resolve(p,section,id);const double centre=edge+s.profile.width*.5+s.profile.offset;if(id==lane)return centre;edge+=s.profile.width+bundle.gap;}
    return Centre(Resolve(p,section,lane),lane);
}
inline const char* Name(Movement m) {const char* names[]{"Legacy","Locked","Adaptive","Network","Scripted","Free"};return names[std::clamp(int(m),0,5)];}
inline const char* Name(Obstacles o) {return o==Obstacles::Ignore?"Ignore":o==Obstacles::InLane?"In-lane":"Stop";}
inline const char* Name(lanevolume::Shape s) {const char* names[]{"Rectangle","Rounded rectangle","Ellipse","Trapezoid"};return names[std::clamp(int(s),0,3)];}
inline void Insert(Plan& p,std::size_t at) {
    lanetransform::Insert(p.transforms,int(at));
    if(!p.sectionIds.empty())p.sectionIds.insert(p.sectionIds.begin()+std::min(at,p.sectionIds.size()),p.nextSectionId++);
    std::vector<Override> split;
    for(auto& r:p.overrides) {if(at>0&&r.section==int(at)-1){auto copy=r;copy.section=int(at);split.push_back(copy);}if(r.section>=int(at))++r.section;}
    p.overrides.insert(p.overrides.end(),split.begin(),split.end());
}
inline void Remove(Plan& p,std::size_t at) {lanetransform::Remove(p.transforms,int(at));if(at<p.sectionIds.size())p.sectionIds.erase(p.sectionIds.begin()+at);std::erase_if(p.overrides,[&](const Override& r){return r.section==int(at);});for(auto& r:p.overrides)if(r.section>int(at))--r.section;}
inline void ReverseSections(Plan& p,std::size_t n,bool /*closed*/) {for(auto& r:p.transforms.records)if(r.section>=0&&n>1)r.section=int((2*n-2-r.section)%n);for(auto& r:p.overrides)if(r.section>=0&&n>1)r.section=int((2*n-2-r.section)%n);if(p.sectionIds.size()==n&&n>1){auto ids=p.sectionIds;for(std::size_t i=0;i<n;++i)p.sectionIds[(2*n-2-i)%n]=ids[i];}}
inline Plan Remap(const Plan& p,const std::vector<int>& oldSections) {
    auto out=p;out.transforms=lanetransform::Remap(p.transforms,oldSections);std::erase_if(out.overrides,[](const Override& r){return r.section>=0;});
    if(!p.sectionIds.empty()){out.sectionIds.clear();std::set<std::uint64_t> used;for(auto section:oldSections){const auto id=section>=0&&section<int(p.sectionIds.size())?p.sectionIds[section]:0;out.sectionIds.push_back(id&&used.insert(id).second?id:out.nextSectionId++);}}
    for(std::size_t i=0;i<oldSections.size();++i)for(auto r:p.overrides)if(r.section==oldSections[i]&&r.section>=0){r.section=int(i);out.overrides.push_back(r);}
    return out;
}
}
