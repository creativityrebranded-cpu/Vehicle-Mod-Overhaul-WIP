#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace npcactivity {
constexpr std::uint64_t kMinWindowMs=60000,kMaxGapMs=15000,kStaleMs=kMaxGapMs;
constexpr unsigned kMinSamples=6;
struct Observation {
    std::uint32_t actor=0,base=0,cell=0,package=0;
    std::uint64_t at=0;
    double x=0,y=0,z=0;
    int packageType=-1;
    bool readable=false,free=false,crowd=false;
};
enum class Status { Unknown,Excluded,Observing,Active,Low };
inline const char* Name(Status s) {
    switch(s){case Status::Excluded:return "excluded";case Status::Observing:return "observing";
        case Status::Active:return "active";case Status::Low:return "low-activity-review";default:return "unknown";}
}
struct Entry {
    Observation last{};
    std::uint64_t began=0,quiet=0;
    unsigned samples=0,changes=0;
    double metres=0;
    Status status=Status::Unknown;
    bool Current(std::uint64_t now) const {return last.actor&&now>=last.at&&now-last.at<=kStaleMs;}
    void Add(const Observation& o) {
        const bool valid=o.readable&&o.actor&&o.base&&o.cell&&o.package&&o.packageType>=0&&
            std::isfinite(o.x)&&std::isfinite(o.y)&&std::isfinite(o.z);
        const bool restart=!samples||o.actor!=last.actor||o.base!=last.base||o.cell!=last.cell||
            o.at<last.at||o.at-last.at>kMaxGapMs||!valid||!o.free;
        if(restart){*this={};last=o;began=quiet=o.at;samples=valid&&o.free?1u:0u;
            status=!valid?Status::Unknown:!o.free?Status::Excluded:Status::Observing;return;}
        if(o.at==last.at)return;
        const double distance=std::hypot(std::hypot(o.x-last.x,o.y-last.y),o.z-last.z);
        const double speed=distance*1000.0/double(o.at-last.at);
        const bool changed=o.package!=last.package||o.packageType!=last.packageType;
        metres+=distance;++samples;if(changed)++changes;
        // A jump/rebase invalidates the window; it must not turn into an idle
        // observation or a false long-distance trip.
        if(distance>100){*this={};last=o;began=quiet=o.at;samples=1;status=Status::Unknown;return;}
        if(changed||speed>0.4)quiet=o.at;
        last=o;
        if(!o.crowd){status=Status::Excluded;return;}
        status=o.at-quiet>=kMinWindowMs&&samples>=kMinSamples?Status::Low:
            (changed||speed>0.4?Status::Active:Status::Observing);
    }
};
template<std::size_t N=256> class Census {
    std::array<Entry,N> entries_{};
public:
    Entry* Observe(const Observation& o) {
        if(!o.actor)return nullptr;
        Entry* available=nullptr;
        for(auto& e:entries_) {
            if(e.last.actor==o.actor){e.Add(o);return &e;}
            if(!available&&(!e.last.actor||!e.Current(o.at)))available=&e;
        }
        if(!available)return nullptr;
        *available={};available->Add(o);return available;
    }
    const auto& Entries() const {return entries_;}
    void Reset(){entries_={};}
};
}
