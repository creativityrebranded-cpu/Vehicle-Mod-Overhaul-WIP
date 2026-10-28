#include "native_route_session.h"
#undef NDEBUG
#include <cassert>
#include <limits>
using namespace native_route_session;
int main() {
    Session npc, player;
    auto first=npc.Begin(10,{1,2,3});
    auto second=npc.Begin(10,{4,5,6});
    auto other=player.Begin(20,{2,3,4});
    assert(!npc.Publish(10,first.generation,Status::Following));
    assert(!npc.Publish(20,second.generation,Status::Following));
    assert(npc.Publish(10,second.generation,Status::Following));
    assert(player.GetStatus()==Status::Waiting);
    assert(player.Publish(20,other.generation,Status::Blocked));
    npc.Cancel();
    assert(!npc.Publish(10,second.generation,Status::Following));
    assert(npc.GetStatus()==Status::Idle);
    auto invalid=npc.Begin(10,{std::numeric_limits<double>::quiet_NaN(),0,0});
    assert(npc.GetStatus()==Status::Unavailable);
    assert(!npc.Publish(10,invalid.generation,Status::Following));
    auto terrain=npc.Begin(10,{1,2,std::numeric_limits<double>::quiet_NaN()});
    assert(npc.GetStatus()==Status::Waiting);
    assert(npc.Publish(10,terrain.generation,Status::Following));
}
