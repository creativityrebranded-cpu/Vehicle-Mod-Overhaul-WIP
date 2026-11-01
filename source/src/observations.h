#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <cstdarg>
#include <cstdio>

// Diagnostic state only. Never owns game references or changes simulation. Fixed storage, non-waiting lock,
// state-change records plus a five-second heartbeat. Fast oscillations are counted and reported on the next record.
namespace observations {
inline std::uint64_t Key(const char* channel,std::uint32_t vehicle){
    std::uint64_t h=14695981039346656037ull;
    for(;*channel;++channel){h^=static_cast<unsigned char>(*channel);h*=1099511628211ull;}
    return h^(std::uint64_t(vehicle)*0x9E3779B97F4A7C15ull);
}
struct Decision {bool emit=false;unsigned coalesced=0,contended=0;};
class Gate {
    struct Entry {bool used=false;std::uint64_t key=0,state=0,at=0;unsigned coalesced=0;};
    std::array<Entry,4096> entries_{};
    std::mutex mutex_;
    std::atomic<unsigned> contended_{0};
public:
    Decision Sample(std::uint64_t now,std::uint64_t key,std::uint64_t state){
        std::unique_lock lock(mutex_,std::try_to_lock);
        if(!lock.owns_lock()){++contended_;return {};}
        auto* chosen=&entries_[key%entries_.size()];
        for(std::size_t i=0;i<8;++i){auto& e=entries_[(key+i)%entries_.size()];if(!e.used||e.key==key){chosen=&e;break;}}
        auto& e=*chosen;
        if(!e.used||e.key!=key||now<e.at){e={true,key,state,now,0};return {true,0,contended_.exchange(0)};}
        const auto elapsed=now-e.at;
        if(elapsed>=5000||(state!=e.state&&elapsed>=250)){
            const unsigned coalesced=e.coalesced;e.state=state;e.at=now;e.coalesced=0;
            return {true,coalesced,contended_.exchange(0)};
        }
        if(state!=e.state)++e.coalesced;
        return {};
    }
};
inline Gate gate;
inline std::atomic<unsigned> epoch{0};
template<class Sink>
void Record(Sink sink,std::uint64_t now,const char* system,std::uint32_t vehicle,std::uint64_t state,const char* fmt,...){
    if(!sink)return;
    const auto decision=gate.Sample(now,Key(system,vehicle)^(std::uint64_t(epoch.load())*0xD6E8FEB86659FD93ull),state);if(!decision.emit)return;
    char fields[1200]{},line[1500]{};va_list args;va_start(args,fmt);std::vsnprintf(fields,sizeof fields,fmt,args);va_end(args);
    std::snprintf(line,sizeof line,"OBS system=%s vehicle=%08X state=%llX coalesced=%u contention=%u %s",system,vehicle,
        static_cast<unsigned long long>(state),decision.coalesced,decision.contended,fields);
    sink(line);
}
}
