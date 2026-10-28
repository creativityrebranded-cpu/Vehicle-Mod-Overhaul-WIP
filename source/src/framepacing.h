#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Spacing of task callbacks is not a GPU/frame-present measurement. It includes
// native simulation, rendering, scheduling and pauses between our callbacks.
namespace framepacing {
struct Window {
    std::array<std::uint64_t,6> buckets{};
    std::uint64_t count=0;
    double previous=0,previousWork=0,sum=0,worst=0,outside=0,began=0;
    bool have=false;
    void Begin(double now) {
        if(!std::isfinite(now))return;
        if(!have||now<previous){*this={};have=true;previous=began=now;return;}
        const double elapsed=now-previous;previous=now;
        ++count;sum+=elapsed;worst=std::max(worst,elapsed);
        outside=std::max(outside,std::max(0.0,elapsed-previousWork));
        const double limits[]{16.7,25,33.4,50,100};std::size_t bucket=0;
        while(bucket<5&&elapsed>limits[bucket])++bucket;
        ++buckets[bucket];
    }
    void Finish(double work){previousWork=std::isfinite(work)?std::max(0.0,work):0;}
    bool Ready()const{return have&&count&&previous-began>=10000;}
    void Next(){const auto at=previous,work=previousWork;*this={};Begin(at);Finish(work);}
};
}
