#pragma once
#include "trafficroute.h"
#include <map>
#include <sstream>
#include <iomanip>
namespace routecache {
inline std::string Key(const trafficdata::Route& r){std::ostringstream out;out<<std::setprecision(17)<<r.ring<<' '<<r.closed<<' '<<r.lanes<<' '<<r.width<<' '<<r.corridor<<' '<<int(r.travel)<<' '<<r.shape.cx<<' '<<r.shape.cy<<' '<<r.shape.z<<' '<<r.shape.radius<<' '<<r.shape.aspect<<' '<<r.shape.rotation<<' '<<r.shape.gap<<' '<<r.tilt<<' '<<r.tiltToward<<' '<<laneplan::Write(r.lanePlan)<<' '<<lanevehicles::Write(r.lanePlan.vehicles)<<' '<<roadplan::Write(r.road)<<' '<<splinekit::Write(r.spacePlan)<<' '<<lanetransform::Write(r.lanePlan.transforms);for(const auto& p:r.points)out<<' '<<p.x<<' '<<p.y<<' '<<p.z;out<<" / ";for(double radius:r.rounding)out<<radius<<' ';return out.str();}
class Cache{struct Entry{std::string key;trafficroute::Line line;};std::map<std::string,Entry> entries;public:const trafficroute::Line& Get(const trafficdata::Route& route){const auto key=Key(route);auto found=entries.find(route.name);if(found!=entries.end()&&found->second.key==key)return found->second.line;if(entries.size()>128&&found==entries.end())entries.clear();auto& entry=entries[route.name];entry.key=key;entry.line=trafficroute::Build(route);return entry.line;}void Clear(){entries.clear();}};
}
