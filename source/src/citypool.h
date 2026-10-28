#pragma once
#include "citybuild.h"
#include <array>
#include <sstream>

namespace citypool {
using Assignments=std::map<std::uint64_t,unsigned>;
inline Assignments Read(const std::vector<std::string>& lines,bool& valid){
    Assignments out;std::set<unsigned> used;valid=true;
    for(const auto& line:lines){if(line.empty()||line[0]==';')continue;
        std::istringstream in(line);std::string key,extra;unsigned slot=0;std::uint64_t id=0;
        if(!(in>>key>>slot)||in>>extra||!citybuild::Key(key,id)||slot>=citybuild::kPool||out.count(id)||!used.insert(slot).second){valid=false;continue;}out[id]=slot;
    }return out;
}
inline std::string Text(const Assignments& assignments){std::string text="; key pool-slot; stable assignments, do not edit\n";char b[64];for(const auto& [key,slot]:assignments){std::snprintf(b,sizeof(b),"%016llX %u\n",(unsigned long long)key,slot);text+=b;}return text;}
inline Assignments Plan(Assignments current,const std::vector<citybuild::Spot>& pending,const std::array<bool,citybuild::kPool>& busy){
    std::set<std::uint64_t> want;for(const auto& spot:pending)want.insert(spot.key);
    for(auto it=current.begin();it!=current.end();)if(!want.count(it->first)&&!busy[it->second])it=current.erase(it);else ++it;
    std::set<unsigned> used;for(const auto& [key,slot]:current)used.insert(slot);
    for(const auto key:want){if(current.count(key))continue;for(unsigned slot=0;slot<citybuild::kPool;++slot)if(!used.count(slot)&&!busy[slot]){current[key]=slot;used.insert(slot);break;}}
    return current;
}
}
