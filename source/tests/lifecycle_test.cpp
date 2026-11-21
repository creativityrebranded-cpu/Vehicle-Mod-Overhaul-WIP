#include "lifecycle_transport.h"
#include "lifecycle_cover.h"
#include "cellfit.h"
#include "traffic/admission.h"
#include <cstdio>
#include <thread>
#include <vector>
#include <array>

namespace {
alignas(8) std::array<unsigned char,2048> page{},queue{};
unsigned nextOffset=0;
std::uint64_t fakeNow=1000;std::uint64_t FakeClock(){return fakeNow;}
void* FakeAllocate(std::uintptr_t q,lifecycle::Reservation* slot,int size,unsigned alignment) {
    if(q!=reinterpret_cast<std::uintptr_t>(queue.data())||alignment!=8||size!=sizeof(lifecycle::Command))return nullptr;
    *slot={reinterpret_cast<std::uintptr_t>(page.data()),nextOffset,size+8};
    const auto offset=nextOffset;nextOffset+=unsigned(size+8);
    return page.data()+offset+0x48;
}
}
int main() {
    using namespace lifecycle;
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* message){++checks;if(!ok){++failures;std::printf("FAIL %s\n",message);}};
    Requests requests(4);std::vector<Ticket> commands;int executed=0,acquired=0,released=0;
    auto submit=[&](const Ticket& ticket){commands.push_back(ticket);return true;};
    auto factory=[&]()->Work {
        ++acquired;auto ref=std::shared_ptr<int>(new int(42),[&](int* p){
            // A captured native release may re-enter this table.
            (void)requests.Pending();++released;delete p;
        });
        return [&,ref]()->std::uint64_t {++executed;check(requests.Pending(77),"engine callback can reenter controller without mutex deadlock");return *ref;};
    };
    check(!requests.Poll("birth",77,factory,submit),"new request is pending");
    for(int n=0;n<100;++n)check(!requests.Poll("birth",77,factory,submit),"retries await same ticket");
    check(commands.size()==1&&acquired==1&&executed==0&&released==0,"one owned birth, no engine work in polling thread");
    std::thread worker([&]{commands[0]->Run();commands[0]->Run();});worker.join();
    check(executed==1&&released==1,"execute and native release exactly once");
    check(requests.Pending(77),"completed unconsumed birth remains discoverable");
    check(requests.Poll("birth",77,factory,submit)==42,"completion crosses worker boundary");
    check(!requests.Pending(),"consuming completion removes pending state");commands.clear();
    {
        Requests births(1,FakeClock);std::vector<Ticket> queued;int prepared=0;bool tracked=false;
        auto queue=[&](const Ticket& t){queued.push_back(t);return true;};
        auto work=[&]()->Work{++prepared;return []()->std::uint64_t{return 55;};};
        check(!births.Poll("create:first",1,work,queue,false,false,&tracked)&&!tracked&&prepared==0,
              "collect never starts an absent birth");
        check(!births.Poll("create:first",1,work,queue,false,true,&tracked)&&tracked&&prepared==1,
              "a submitted birth reports its exact ticket");
        check(!births.Poll("create:other",1,work,queue,false,true,&tracked)&&!tracked&&prepared==1,
              "a full table cannot report that it tracked a new birth");
        check(!births.Poll("create:first",1,work,queue,false,false,&tracked)&&tracked&&queued.size()==1,
              "collect can find a pending exact ticket");
        queued[0]->Run();
        check(births.Poll("create:first",1,work,queue,false,false,&tracked)==55&&tracked&&prepared==1,
              "collect returns the birth without calling the factory");
        check(!births.Poll("create:first",1,work,queue,false,false,&tracked)&&!tracked&&queued.size()==1,
              "an already collected birth cannot be recreated by polling");
    }
    requests.Poll("cancel",77,factory,submit);requests.Reset();
    commands[0]->Run();commands.clear();
    check(executed==1&&released==2,"save reset cancels old command but releases its capture");
    requests.Poll("cancel",77,factory,submit);commands.back()->Run();
    check(requests.Poll("cancel",77,factory,submit)==42,"same id in new save receives fresh request");commands.clear();
    requests.Poll("fail",2,[]()->Work{return []{return 1;};},[](const Ticket&){return false;});
    check(requests.Pending(2),"failed submission has an acknowledgement");
    check(requests.Poll("fail",2,factory,submit)==0&&!requests.Pending(),"submission failure consumed without duplicate birth");
    for(int n=0;n<4;++n)requests.Poll(std::to_string(n),3,[]()->Work{return []{return 1;};},submit);
    {const int before=acquired;const unsigned refused=requests.Refused();
    check(!requests.Poll("overflow",4,factory,submit)&&commands.size()==4&&acquired==before,"a full table makes a request wait, before acquiring engine refs");
    check(requests.Refused()==refused+1,"and counts it");
    check(requests.Pending(4,"overflow")&&requests.Pending(99),"a full table reads as pending to every caller, never as a failed birth");}
    requests.Reset();for(auto& c:commands)c->Run();commands.clear();
    for(int n=0;n<100;++n){requests.Poll("pose",9,[]()->Work{return []{return 1;};},submit,true);commands.back()->Run();commands.clear();}
    check(requests.Poll("ordinary",9,[]()->Work{return []{return 1;};},submit).has_value()==false,"fire-and-observe completions do not exhaust queue");
    requests.Reset();for(auto& c:commands)c->Run();commands.clear();
    // The game's load routine: what is queued never runs, and no caller reads the empty table as a failure.
    {
        int ran=0,built=0;const unsigned refusedBefore=requests.Refused();
        auto counted=[&]()->Work{++built;return [&]()->std::uint64_t{++ran;return 1;};};
        requests.Poll("old world",5,counted,submit);check(built==1&&commands.size()==1,"queued before the load");
        requests.Close();commands[0]->Run();commands.clear();
        check(ran==0,"work queued before a load never runs against the world being read");
        check(!requests.Poll("during",5,counted,submit)&&built==1&&commands.empty(),"nothing is built or queued during a load");
        bool accepted=true;
        check(!requests.Poll("posted",5,counted,submit,true,true,&accepted)&&!accepted&&built==1&&commands.empty(),
              "a closed table cannot acknowledge a post");
        check(requests.Pending(5)&&requests.Pending(5,"anything"),"during a load an unanswered request reads as pending, not failed");
        check(requests.Refused()==refusedBefore,"a closed table is not a full one");
        requests.Open();
        check(!requests.Pending(5),"the table opens empty");
        check(!requests.Poll("during",5,counted,submit)&&built==2&&commands.size()==1,"the next ask after the load is a fresh request");
        commands[0]->Run();commands.clear();
        check(ran==1&&requests.Poll("during",5,counted,submit)==1,"and it completes in the new world");
    }
    // A full table is counted, so a run can show it; and a post dropped by it can be told from one taken.
    {
        Requests small(1);std::vector<Ticket> queued;auto take=[&](const Ticket& t){queued.push_back(t);return true;};
        auto one=[]()->Work{return []()->std::uint64_t{return 1;};};
        check(!small.Poll("a",1,one,take,true),"post taken");
        check(small.Poll("b",2,one,take,true).has_value()&&small.Refused()==1,"post dropped at the limit says so and is counted");
        check(!small.Poll("a",1,one,take,true)&&small.Refused()==1,"a repeat of a queued post is not a refusal");
        for(auto& t:queued)t->Run();
        check(!small.Poll("b",2,one,take,true),"finished posts free the table");
        Requests posts(4);
        bool accepted=true;
        auto refuse=[](const Ticket&){return false;};
        posts.Poll("submit_failure",2,one,refuse,true,true,&accepted);
        check(!accepted,"a rejected native submission leaves the post eligible for retry");
        posts.Poll("empty_work",3,[] {return Work{};},take,true,true,&accepted);
        check(!accepted,"unavailable work is not an accepted post");
        posts.Poll("submit_failure",2,one,take,true,true,&accepted);
        check(accepted,"a previously refused post can submit successfully");
        for(auto& t:queued)t->Run();
    }
    // A result its caller stopped asking for (an abandoned move key) is evicted once the table is full and it is 10 s old,
    // so a full table cannot stay full; younger results and unfinished requests are never evicted.
    {
        Requests table(2,FakeClock);std::vector<Ticket> queued;auto take=[&](const Ticket& t){queued.push_back(t);return true;};
        auto one=[]()->Work{return []()->std::uint64_t{return 1;};};
        fakeNow=1000;table.Poll("move:a",1,one,take);table.Poll("move:b",2,one,take);queued[0]->Run();
        check(!table.Poll("create:c",3,one,take)&&queued.size()==2,"full, nothing old enough: wait");
        fakeNow=1000+Requests::kAbandonMs-1;
        check(!table.Poll("create:c",3,one,take)&&queued.size()==2,"9.999 s: still kept for its caller");
        fakeNow=1000+Requests::kAbandonMs;
        check(!table.Poll("create:c",3,one,take)&&queued.size()==3,"10 s: the abandoned result makes room");
        check(table.Outstanding("move")==1&&table.Outstanding("create")==1&&table.Outstanding("enable")==0,"outstanding counts unfinished requests by kind");
        check(table.Pending(1,"move"),"full again (2 of 2): every caller still reads pending");
        queued[2]->Run();check(table.Outstanding("create")==0,"a finished one is not outstanding");
        check(table.Poll("create:c",3,one,take)==1,"the waiting birth completes");
        check(table.Pending(2,"move")&&!table.Pending(1,"move"),"with room again: the unfinished one stays, the abandoned one is gone");
        for(auto& t:queued)t->Run();
        fakeNow=1000+Requests::kAbandonMs*3;
        Requests roomy(8,FakeClock);roomy.Poll("move:x",4,one,take);queued.back()->Run();fakeNow+=Requests::kAbandonMs*5;
        check(roomy.Pending(4),"below the limit an uncollected result is kept however old");
    }
    // The service lock at 145F43DC8 (owner, state), as FUN_1422C8D10 and the release in FUN_1418BD310 use it.
    {
        alignas(8) volatile LONG word[2]={0,0};const DWORD self=GetCurrentThreadId();
        check(ServiceLock::TryAcquire(word,self)&&DWORD(word[0])==self&&word[1]==0x10001,"free: taken, owner set, one hold and our count");
        check(ServiceLock::TryAcquire(word,self)&&word[1]==0x30001,"re-entry adds 0x20000 as the engine's does");
        ServiceLock::Release(word);check(word[1]==0x10001&&DWORD(word[0])==self,"inner release drops 0x20000 and keeps the owner");
        ServiceLock::Release(word);check(word[0]==0&&word[1]==0,"last release clears owner, hold and count");
        bool other=true;std::thread([&]{other=ServiceLock::TryAcquire(word,GetCurrentThreadId());if(other)ServiceLock::Release(word);}).join();
        check(other&&word[0]==0&&word[1]==0,"another thread can take it after");
        word[0]=LONG(self+1);word[1]=0x10002; // held by another thread with one waiter
        check(!ServiceLock::TryAcquire(word,self)&&word[1]==0x10002&&DWORD(word[0])==self+1,"held elsewhere: refused, nothing changed, no wait");
        word[0]=0;word[1]=0x3; // free, three threads between their count and their swap
        check(ServiceLock::TryAcquire(word,self)&&word[1]==0x10004,"free with waiters counted: taken with ours added");
        ServiceLock::Release(word);check(word[1]==0x3&&word[0]==0,"release leaves the waiters' counts (and wakes one)");
    }
    // The private list behind the screen: order kept, a step stops at its budget, the rest is handed off in order.
    {
        CoverList list;std::vector<int> order;auto epoch=std::make_shared<Epoch>();
        auto make=[&](int n,double cost,double& t){auto r=std::make_shared<Request>();r->epoch=epoch;r->generation=1;
            r->work=[&order,n,cost,&t]()->std::uint64_t{order.push_back(n);t+=cost;return std::uint64_t(n);};r->state=Request::Queued;return r;};
        double t=0;auto clock=[&]{return t;};
        std::vector<Ticket> all;for(int n=0;n<6;++n){all.push_back(make(n,1.0,t));list.Add(all.back());}
        double longest=0;
        check(list.Run(3.0,clock,longest)==3&&order==std::vector<int>({0,1,2})&&longest==1.0,"a 3 ms step runs commands in order until its budget");
        check(list.Size()==3,"the rest wait for the next step");
        auto slow=make(9,10.0,t);CoverList one;one.Add(slow);longest=0;
        check(one.Run(3.0,clock,longest)==1&&slow->state==Request::Done&&longest==10.0,"one slow command still runs, and is reported");
        auto rest=list.TakeAll();
        check(rest.size()==3&&rest[0]==all[3]&&rest[2]==all[5]&&list.Size()==0,"hand-off keeps the order");
        for(auto& r:rest)r->Run();
        check(order==std::vector<int>({0,1,2,9,3,4,5})&&all[5]->result==5,"handed-off commands complete with their results");
        CoverList empty;longest=0;check(empty.Run(3.0,clock,longest)==0,"an empty list runs nothing");
    }
    // Exterior grid as FUN_140595DF0 computes it: floor(metres * 0.01f), 100 m cells (16:07 hang, cellfit.h).
    {
        using cellfit::Grid;using cellfit::Inside;
        check(Grid(0)==0&&Grid(99.9f)==0&&Grid(100.0f)==1&&Grid(-0.1f)==-1&&Grid(-100.0f)==-1&&Grid(-100.5f)==-2,"grid floors, negative side included");
        check(Grid(-830.1f)==-9&&Grid(1526.0f)==15,"a 16:06 birth spot lies in cell (-9, 15)");
        check(Inside(-9,15,-830.1f,1526.0f)&&!Inside(-9,15,-899.0f-2.0f,1526.0f)&&!Inside(-9,15,-830.1f,1600.0f),"inside its cell, and out of it across either edge");
    }
    // Exercise the same native arena publication and callback layout used by
    // the DLL, including initial head and subsequent linked command paths.
    auto q=reinterpret_cast<std::uintptr_t>(queue.data());
    *reinterpret_cast<unsigned*>(queue.data()+0x164)=1024;
    *reinterpret_cast<unsigned*>(page.data()+0x10)=1024;
    auto a=std::make_shared<Request>();a->epoch=std::make_shared<Epoch>();a->generation=1;
    int runs=0;a->work=[&]()->std::uint64_t{++runs;return 5;};a->state=Request::Queued;
    check(Enqueue(q,FakeAllocate,a),"native arena submission");
    check(*reinterpret_cast<unsigned*>(page.data()+0x14)==0,"first command publishes head");
    auto b=std::make_shared<Request>();b->epoch=a->epoch;b->generation=1;b->work=[&]()->std::uint64_t{++runs;return 6;};b->state=Request::Queued;
    check(Enqueue(q,FakeAllocate,b),"second native command");
    check(*reinterpret_cast<unsigned*>(page.data()+0x44)==sizeof(Command)+8,"previous command links to next offset");
    check(*reinterpret_cast<unsigned*>(page.data()+0xC)==2*(sizeof(Command)+8),"publication commits reserved bytes after initialization");
    auto ca=reinterpret_cast<Command*>(page.data()+0x48),cb=reinterpret_cast<Command*>(page.data()+0x48+sizeof(Command)+8);
    std::weak_ptr<Request> weak=a;a.reset();
    ca->callback(ca);check(weak.expired()&&runs==1,"native callback destroys capture without freeing arena storage");
    cb->callback(cb);check(b->result==6&&runs==2,"native callbacks execute owned commands");
    check(!Enqueue(0,FakeAllocate,b),"missing service blocks mutations");
    check(trafficadmission::ValidOrigin(100,100)&&trafficadmission::ValidOrigin(5000,100),"authored remote origins have no arbitrary distance cap");
    check(trafficadmission::Capacity(12,0,0,80)&&trafficadmission::Capacity(79,0,0,80)&&!trafficadmission::Capacity(80,0,0,80)&&!trafficadmission::Capacity(79,1,0,80),"traffic-rule target controls fleet capacity, including pending births");
    check(trafficadmission::Capacity(0,0,24,80)&&!trafficadmission::Capacity(0,0,160,80)&&!trafficadmission::Capacity(0,0,0,0),"retirement backpressure scales with requested fleet; zero target creates nothing");
    std::printf("lifecycle: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
