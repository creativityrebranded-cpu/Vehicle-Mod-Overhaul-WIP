#pragma once
#include <array>
#include <cstdint>
#include <algorithm>

// Pure, event-driven timetable. A reservation is a physical bay, not a timer.
// Engine adapters must confirm the corresponding event before advancing.
namespace trafficschedule {
constexpr int kPorts=3,kBays=6,kVehicles=6,kActors=18;
enum class Phase { Wait, Boarding, Departure, Climb, Join, Ring, Approach, Descend, Unload, GoAround, Halted };
enum class Duty { Queue, Ready, Boarding, Driving, Walking, Rest, Unavailable };
inline const char* Name(Phase p) {
    constexpr const char* names[]{"wait for next NPC","boarding","wait for departure","climb","merge","transit / ring","approach","descend","unload","go around","halted"};
    return names[static_cast<unsigned>(p)];
}
struct Actor { Duty duty=Duty::Queue; int bay=-1; std::uint64_t at=0,ticket=0; };
struct Vehicle { Phase phase=Phase::Wait;int bay=-1,port=0,destination=1,actor=-1,pilot=0,waypoint=0;std::uint64_t at=0,notBefore=0;unsigned laps=0; };
struct Bay {int port=0,owner=-1;};
struct Port {bool enabled=true;int corridor=-1;};
struct Schedule {
    std::array<Vehicle,kVehicles> vehicles{};
    std::array<Actor,kActors> actors{};
    std::array<Bay,kBays> bays{};
    std::array<Port,kPorts> ports{};
    std::uint64_t ticket=0;
    void Reset(std::uint64_t now=0) {
        *this={};
        for(int b=0;b<kBays;++b){bays[b]={b/2,b};auto&v=vehicles[b];v.bay=b;v.port=b/2;v.destination=(v.port+1)%kPorts;v.at=now;v.notBefore=now+std::uint64_t(b)*8000;}
        for(int a=0;a<kActors;++a){actors[a].bay=a/3;actors[a].at=now;}
    }
    void Set(int i,Phase phase,std::uint64_t now){vehicles[i].phase=phase;vehicles[i].at=now;}
    int ReadyActor(int bay) const {
        int best=-1;
        for(int a=0;a<kActors;++a)if(actors[a].bay==bay&&actors[a].duty==Duty::Ready&&(best<0||actors[a].ticket<actors[best].ticket))best=a;
        return best;
    }
    void Ready(int actor,std::uint64_t now){auto&a=actors[actor];a.duty=Duty::Ready;a.at=now;a.ticket=++ticket;}
    bool Board(int i,int actor,int pilot,std::uint64_t now) {
        auto&v=vehicles[i];if(v.phase!=Phase::Wait||v.bay<0||actor<0||actor>=kActors||actor!=ReadyActor(v.bay)||!pilot||now<v.notBefore)return false;
        for(int j=0;j<kVehicles;++j)if(j!=i&&vehicles[j].pilot==pilot)return false;
        v.actor=actor;v.pilot=pilot;actors[actor].duty=Duty::Boarding;Set(i,Phase::Boarding,now);return true;
    }
    void BoardFailed(int i,std::uint64_t now) {
        auto&v=vehicles[i];if(v.actor>=0){actors[v.actor].duty=Duty::Rest;actors[v.actor].at=now;}
        v.actor=-1;v.pilot=0;v.notBefore=now+15000;Set(i,Phase::Wait,now);
    }
    bool Seated(int i,std::uint64_t now){auto&v=vehicles[i];if(v.phase!=Phase::Boarding||v.actor<0)return false;actors[v.actor].duty=Duty::Driving;Set(i,Phase::Departure,now);return true;}
    bool Depart(int i,bool clear,std::uint64_t now) {
        auto&v=vehicles[i];auto&p=ports[v.port];
        if(v.phase!=Phase::Departure||!clear||!p.enabled||p.corridor>=0||now-v.at<5000)return false;
        p.corridor=i;Set(i,Phase::Climb,now);return true;
    }
    bool ClearedBay(int i,std::uint64_t now) {
        auto&v=vehicles[i];if(v.phase!=Phase::Climb&&v.phase!=Phase::GoAround)return false;
        if(v.bay>=0&&bays[v.bay].owner==i)bays[v.bay].owner=-1;
        if(ports[v.port].corridor==i)ports[v.port].corridor=-1;
        v.bay=-1;Set(i,Phase::Join,now);return true;
    }
    bool Reserve(int i,int port,const std::array<bool,kBays>& physicallyFree,std::uint64_t now) {
        auto&v=vehicles[i];if(v.phase!=Phase::Ring||port!=v.destination||port<0||port>=kPorts||now-v.at<45000)return false;
        auto&p=ports[port];if(!p.enabled||p.corridor>=0)return false;
        for(int b=0;b<kBays;++b)if(bays[b].port==port&&bays[b].owner<0&&physicallyFree[b]) {
            bays[b].owner=i;p.corridor=i;v.bay=b;v.port=port;Set(i,Phase::Approach,now);return true;
        }
        return false;
    }
    bool Landed(int i,std::uint64_t now) {
        auto&v=vehicles[i];if(v.phase!=Phase::Descend||v.bay<0||bays[v.bay].owner!=i)return false;
        if(ports[v.port].corridor==i)ports[v.port].corridor=-1;
        Set(i,Phase::Unload,now);return true;
    }
    bool Exited(int i,std::uint64_t now) {
        auto&v=vehicles[i];if(v.phase!=Phase::Unload||v.actor<0)return false;
        auto&a=actors[v.actor];a.duty=Duty::Walking;a.bay=v.bay;a.at=now;
        v.actor=-1;v.pilot=0;v.destination=(v.port+1)%kPorts;v.notBefore=now+10000;Set(i,Phase::Wait,now);return true;
    }
    int Occupied(int port) const {int n=0;for(const auto&b:bays)if(b.port==port&&b.owner>=0)++n;return n;}
    bool Valid() const {
        std::array<int,kVehicles> leases{};std::array<bool,kActors> crew{};
        for(int b=0;b<kBays;++b)if(bays[b].owner>=0){const int i=bays[b].owner;if(i>=kVehicles||vehicles[i].bay!=b||++leases[i]>1)return false;}
        for(int i=0;i<kVehicles;++i){const auto&v=vehicles[i];if(v.bay>=0&&(v.bay>=kBays||bays[v.bay].owner!=i))return false;
            if(v.actor>=0){if(v.actor>=kActors||crew[v.actor])return false;crew[v.actor]=true;}
            if(v.pilot)for(int j=0;j<i;++j)if(vehicles[j].pilot==v.pilot)return false;}
        for(int p=0;p<kPorts;++p)if(ports[p].corridor>=0){const int i=ports[p].corridor;if(i>=kVehicles||vehicles[i].port!=p)return false;}
        return true;
    }
};
}
