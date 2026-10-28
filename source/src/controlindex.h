#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
namespace controlindex {
struct Less {
    using is_transparent=void;
    template<class A,class B> bool operator()(const A& a,const B& b) const {
        return a.first<b.first || (a.first==b.first && a.second<b.second);
    }
};
class Index {
    std::map<std::pair<std::string,std::string>,std::uint32_t,Less> entries;
public:
    void Clear() { entries.clear(); }
    void Add(const std::string& context,const std::string& event,std::uint32_t code) {
        if(code && code!=0xFF)entries.try_emplace(std::make_pair(context,event),code);
    }
    std::uint32_t Find(std::string_view context,std::string_view event,std::uint32_t fallback=0) const {
        const auto at=entries.find(std::make_pair(context,event));
        return at==entries.end()?fallback:at->second;
    }
};
}
