#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Sampling and gameplay consumption have different cadences. Keep one event
// per action until the next gameplay batch; repeated samples cannot erase it.
// Scope/eligibility changes discard stale input instead of replaying it later.
namespace inputevents {
template<std::size_t N> class Buffer {
    std::array<bool,N> pending_{},active_{};
    std::uint8_t context_=0;
    bool haveContext_=false;
public:
    void Sample(const std::array<bool,N>& events,const std::array<bool,N>& enabled,std::uint8_t context,bool allowed) {
        if(!allowed||(haveContext_&&context!=context_)){pending_={};active_={};}
        context_=context;haveContext_=allowed;
        for(std::size_t i=0;i<N;++i) {
            if(!allowed||!enabled[i])pending_[i]=active_[i]=false;
            else pending_[i]=pending_[i]||events[i];
        }
    }
    void Publish(){active_=pending_;pending_={};}
    bool Active(std::size_t index)const{return index<N&&active_[index];}
    void End(){active_={};}
    void Reset(){*this=Buffer{};}
};
}
