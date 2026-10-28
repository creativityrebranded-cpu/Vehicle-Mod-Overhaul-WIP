#include "../src/placementenable.h"
#include "../src/features.h"
#include <cstdio>
#include <string>

struct Engine {
    bool disabled = true, enableWorks = true, queuedDisable = true;
    std::string calls;
    void CancelRequests() { calls += 'C'; queuedDisable = false; }
    bool Disabled() const { return disabled; }
    void Enable() { calls += 'E'; if (enableWorks && !queuedDisable) disabled = false; }
};
int main()
{
    int failures=0;
    auto check=[&](bool good,const char* name){std::printf("%s %s\n",good?"PASS":"FAIL",name);if(!good)++failures;};
    check(!features::Available(features::Id::Placement),"retired randomizer cannot be enabled by old profiles or menu events");
    check(features::Available(features::Id::Traffic)&&features::Available(features::Id::NpcVehicles),"traffic and NPC vehicle use remain available");
    Engine e;
    e.CancelRequests();
    check(e.Disabled(),"regression: cancelling state requests leaves the reference disabled");
    e.calls.clear();
    check(placementenable::Restore(e)&&e.calls=="CE","restore performs actual native enable after cancellation");
    e.calls.clear();e.queuedDisable=true;
    check(placementenable::Restore(e)&&e.calls=="C"&&!e.queuedDisable,"already enabled references cancel pending disable without loading again");
    e.disabled=true;e.enableWorks=false;e.calls.clear();
    check(!placementenable::Restore(e),"failed native enable is not reported as restored");
    using placementenable::BeforeSwap;using placementenable::Wait;
    check(BeforeSwap(false,false,100)==Wait::Pending,"unloaded reference still waits for queued disable to finish");
    check(BeforeSwap(true,true,100)==Wait::Pending,"disabled reference waits for old model unload");
    check(BeforeSwap(true,false,100)==Wait::Ready,"only disabled and unloaded reference may change base");
    check(BeforeSwap(true,true,3000)==Wait::Timeout,"unload timeout aborts instead of changing a live model");
    check(BeforeSwap(false,false,3000)==Wait::Timeout,"disable timeout aborts instead of racing the engine queue");
    return failures?1:0;
}
