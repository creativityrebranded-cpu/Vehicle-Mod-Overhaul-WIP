#pragma once
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <vector>
// Additive authoring data, separate from the established lane_plan versions.
namespace lanevehicles {
struct Rule {int lane=0;std::string group;std::vector<unsigned> exclude;};
struct Rules {std::vector<Rule> lanes;std::string unsupported;};
inline const Rule* Find(const Rules& rules,int lane){for(const auto& r:rules.lanes)if(r.lane==lane)return &r;return nullptr;}
inline Rule& Edit(Rules& rules,int lane){for(auto& r:rules.lanes)if(r.lane==lane)return r;rules.lanes.push_back({lane,{},{}});return rules.lanes.back();}
inline std::string Write(const Rules& rules){if(!rules.unsupported.empty())return rules.unsupported;if(rules.lanes.empty())return {};std::ostringstream out;out<<1<<' '<<rules.lanes.size();for(const auto& r:rules.lanes){out<<" | "<<r.lane<<' '<<std::quoted(r.group)<<' '<<r.exclude.size();for(auto id:r.exclude)out<<' '<<id;}return out.str();}
inline Rules Read(const std::string& text){Rules rules;if(text.empty())return rules;Rules bad;bad.unsupported=text;auto input=text;std::replace(input.begin(),input.end(),'|',' ');std::istringstream in(input);int version;std::size_t count;if(!(in>>version>>count)||version!=1||count>16)return bad;std::set<int> lanes;for(std::size_t n=0;n<count;++n){Rule r;std::size_t ids;if(!(in>>r.lane>>std::quoted(r.group)>>ids)||r.lane<0||r.lane>=16||!lanes.insert(r.lane).second||ids>4096||r.group.size()>128||r.group.find_first_of("\r\n|;")!=std::string::npos)return bad;for(std::size_t i=0;i<ids;++i){unsigned id;if(!(in>>id)||!id)return bad;r.exclude.push_back(id);}rules.lanes.push_back(std::move(r));}std::string extra;if(in>>extra)return bad;return rules;}
}
