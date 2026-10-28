#pragma once
#include <algorithm>
#include <cstdint>

namespace framework {
struct Cell { std::uintptr_t identity=0; unsigned count=0; };
struct ScanCursor { unsigned cell=0, ref=0; std::uintptr_t identity=0; };
// Reacquire a cell's current array on every call. Never retain engine pointers
// to reference arrays across frames. The last complete snapshot stays available.
template<class Source, class Expired>
bool ScanStep(ScanCursor& cursor, unsigned cells, Source& source, Expired expired,unsigned maxVisits=256) {
    unsigned visited=0;
    while(cursor.cell<cells) {
        const auto cell=source.Describe(cursor.cell);
        if(cursor.identity!=cell.identity) { cursor.ref=0; cursor.identity=cell.identity; }
        if(!cell.identity || cursor.ref>=cell.count) {
            ++cursor.cell; cursor.ref=0; cursor.identity=0; continue;
        }
        while(cursor.ref<cell.count) {
            if(visited>=maxVisits || (visited && visited%16==0 && expired())) return false;
            source.Visit(cursor.ref++); ++visited;
        }
        ++cursor.cell; cursor.ref=0; cursor.identity=0;
    }
    cursor={}; return true;
}
struct Retry {
    std::uint64_t next=0;
    unsigned failures=0;
    bool Ready(std::uint64_t now) const { return !next || now>=next; }
    void Failed(std::uint64_t now) {
        failures=std::min(failures+1,5u);
        next=now+std::min(std::uint64_t(2000)<<(failures-1),std::uint64_t(30000));
    }
    void Clear() { *this={}; }
};
}
