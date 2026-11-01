#include "birthholds.h"
#include <atomic>
#include <cstdio>
#include <thread>
int main() {
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* why){++checks;if(!ok){++failures;std::printf("FAIL %s\n",why);}};
    {
        // Only real changes reach the service: no physics yet and already frozen post nothing.
        birthholds::Snapshot held;int body=0,other=0;
        check(!birthholds::NeedsApply(held,nullptr),"no physics yet: nothing to post");
        check(birthholds::NeedsApply(held,&body),"physics appeared: post");
        held.object=birthholds::Object(&body,[](void*){});
        check(birthholds::NeedsApply(held,&body),"known physics not frozen yet: post");
        held.applied=true;
        check(!birthholds::NeedsApply(held,&body),"already frozen on it: nothing to post");
        check(birthholds::NeedsApply(held,&other),"physics replaced: post");
        check(birthholds::NeedsApply(held,nullptr),"physics gone: post once to let the old object go");
        held.releasing=true;
        check(!birthholds::NeedsApply(held,&other),"releasing: never frozen again");
    }
    birthholds::Table table;
    std::atomic<int> destroyed{0};
    auto object=[&] {return birthholds::Object(new int(42),[&](void* p){
        // Native release is allowed to call back into this very table. A lock
        // held during destruction would deadlock this real callback.
        table.Add(999); table.Remove(999); ++destroyed; delete static_cast<int*>(p);
    });};
    table.Add(1);auto token=*table.Get(1);
    auto first=object();void* firstAddress=first.get();
    check(table.Publish(token,first),"publish acquired object");
    first.reset();
    check(destroyed==0 && table.Holds(firstAddress),"table retains object after caller releases it");
    table.Applied(token,table.Get(1)->object);
    check(table.Get(1)->applied,"successful hold acknowledged");
    table.Dirty(1);
    table.Applied(token,table.Get(1)->object);
    check(!table.Get(1)->applied,"worker build invalidates older applied acknowledgement");
    auto current=*table.Get(1);
    check(table.BeginRelease(current),"release starts");
    check(!table.Holds(firstAddress),"nested engine dynamic request is not vetoed during release");
    table.FinishRelease(current,false);
    check(table.Holds(firstAddress) && !table.Get(1)->applied,"failed native release keeps pending hold for retry");
    auto second=object();
    check(table.Publish(current,second),"replace physics object");
    check(destroyed==0,"in-flight snapshot retains replaced physics");
    current.object.reset();
    check(destroyed==1,"last old reference releases outside mutex with reentrant deleter");
    current=*table.Get(1);
    check(table.BeginRelease(current),"begin second release");
    table.FinishRelease(current,true);
    check(!table.Get(1) && !table.Holds(second.get()),"successful release removes identity");
    table.Add(1);
    check(table.Get(1)->generation!=current.generation,"reused id receives a new generation");
    check(!table.Publish(current,second),"stale worker cannot republish into replacement id");
    table.FinishRelease(current,true);
    check(table.Get(1).has_value(),"stale completion cannot erase replacement");
    current.object.reset();second.reset();
    check(destroyed==2,"all references released exactly once");
    {
        auto third=object();auto fresh=*table.Get(1);
        check(table.Publish(fresh,third),"fresh generation accepted");
        third.reset();table.Clear();
        check(destroyed==3,"clear releases outside mutex");
    }
    std::atomic<bool> stop=false;
    std::atomic<unsigned> observations=0;
    auto held=object();
    std::thread worker([&]{while(!stop.load()){table.Holds(held.get());table.Dirty(7);++observations;}});
    for(int i=0;i<10000;++i){
        table.Add(7);auto expected=*table.Get(7);
        table.Publish(expected,held);table.Applied(expected,held);
        if(i%2){table.BeginRelease(expected);table.FinishRelease(expected,true);}
        else table.Remove(7);
    }
    stop=true;worker.join();held.reset();
    check(destroyed==4 && !table.Get(7),"concurrent native observations and frame replacements preserve lifetime");
    std::printf("birthholds: %d checks, %d failures; %u concurrent observations\n",checks,failures,observations.load());
    return failures?1:0;
}
