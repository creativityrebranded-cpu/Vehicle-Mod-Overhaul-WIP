#include "lifecycle.h"
#include "lifecycle_transport.h"
#include "lifecycle_cover.h"
#include "hook.h"
#include "addresses.h"
#include "game.h"
#include "log.h"
#include "crash.h"
#include <cstring>
#include <string>

namespace lifecycle {
namespace {
Requests requests;
constexpr unsigned kService=0x05E71728,kOwner=0x05F43DC8,kThreading=0x058D585E,kAllocate=0x022CDF60;
// Main (DAT_145E71710) +0x44C: "Is loading screen open", cleared by EndLoadingScreen FUN_14057F430. While set, the
// main loop FUN_14189D520 takes the reduced step FUN_1418891E0 instead of FUN_141888410 (work log 15:45 trace).
constexpr unsigned kMain=0x05E71710,kMainLoadingOpen=0x44C,kReducedStep=0x018891E0;
constexpr double kCoverBudgetMs=3.0; // per reduced step; frames behind the screen already reach 600 ms (log lines 261, 532)
CoverList cover;
thread_local bool inCover=false;
thread_local const char* tCurrentKey=nullptr;  // the request being submitted, for crash::Action
std::atomic<unsigned long> coverThread{0};
struct CoverStats { std::atomic<unsigned> ran{0},steps{0},busy{0},handed{0};std::atomic<long long> longestUs{0}; } stats;
bool ScreenUp() {
    __try {
        const auto main=*reinterpret_cast<const std::uintptr_t*>(game::Base()+kMain);
        return main&&*reinterpret_cast<const volatile unsigned char*>(main+kMainLoadingOpen)!=0;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool EnqueueNative(const Ticket& ticket) {
    const auto service=*reinterpret_cast<std::uintptr_t*>(game::Base()+kService);
    return service&&Enqueue(*reinterpret_cast<std::uintptr_t*>(service),reinterpret_cast<Allocate>(game::Base()+kAllocate),ticket);
}
void Fail(const Ticket& ticket) {ticket->work={};ticket->state.store(Request::Done,std::memory_order_release);}
// Screen down: what the cover did not reach goes to the native queue in its order, before anything newer.
void HandOff() {
    if((!cover.Size()&&!stats.steps.load()&&!stats.busy.load())||ScreenUp())return;
    auto rest=cover.TakeAll();
    for(auto& ticket:rest)if(!EnqueueNative(ticket))Fail(ticket);
    stats.handed+=unsigned(rest.size());
    const unsigned ran=stats.ran.exchange(0),steps=stats.steps.exchange(0),busy=stats.busy.exchange(0),handed=stats.handed.exchange(0);
    logger::Write("Traffic lifecycle: behind the loading screen %u command(s) ran on the main thread in %u step(s) (longest %.2f ms, %.1f ms a step); %u left for the native queue; %u step(s) skipped with the service lock held elsewhere",
        ran,steps,double(stats.longestUs.exchange(0))/1000.0,kCoverBudgetMs,handed,busy);
}
bool Compatible() {
    static const bool valid=[] {
        const unsigned char expected[]={0x48,0x89,0x5C,0x24,0x20,0x48,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x54};
        const bool ok=std::memcmp(reinterpret_cast<void*>(game::Base()+kAllocate),expected,sizeof(expected))==0;
        logger::Write("Traffic lifecycle: native service arena %s (owned asynchronous commands; 64 request limit)",ok?"verified":"UNSUPPORTED; mutations blocked");
        return ok;
    }();
    return valid;
}
bool Submit(const Ticket& ticket) {
    if(!Compatible())return false;
    const auto service=*reinterpret_cast<std::uintptr_t*>(game::Base()+kService);
    if(!service)return false;
    auto work=std::move(ticket->work);const unsigned id=ticket->id;
    char what[28]{};if(tCurrentKey){strncpy_s(what,tCurrentKey,_TRUNCATE);if(char* colon=std::strchr(what,':'))*colon=0;}
    ticket->work=[work=std::move(work),id,name=std::string(what)]()->std::uint64_t {
        crash::Action(name.c_str(),id);
        const auto owner=*reinterpret_cast<volatile unsigned*>(game::Base()+kOwner);
        if(!InService()){logger::Write("Traffic lifecycle: refused command %08X outside service (thread %u, owner %u)",id,GetCurrentThreadId(),owner);return 0;}
        static std::atomic<unsigned> reported{0};
        if(reported.fetch_add(1)<8)logger::Write("Traffic lifecycle: execute %08X in service (thread %u, owner %u%s)",id,GetCurrentThreadId(),owner,inCover?", behind the loading screen":"");
        return work();
    };
    // Behind the loading screen the native queue does not drain; keep it for the reduced step (lifecycle_cover.h).
    if(ScreenUp()&&coverThread.load()){cover.Add(ticket);return true;}
    HandOff();
    // Always enqueue, even from the service: completion never calls back into
    // the traffic controller, and ordering stays identical for all callers.
    return Enqueue(*reinterpret_cast<std::uintptr_t*>(service),reinterpret_cast<Allocate>(game::Base()+kAllocate),ticket);
}
std::uintptr_t Acquire(std::uintptr_t ref,unsigned expectedId) {
    __try {
        const unsigned handle=*reinterpret_cast<unsigned*>(ref+0x24);
        if(!handle)return 0;
        std::uintptr_t result=0;auto* output=&result;
        struct { std::uintptr_t vtable;std::uintptr_t** capture; } lookup{game::Base()+addr::kHandleToRefFunctorVtbl,&output};
        reinterpret_cast<void(*)(std::uintptr_t,unsigned,void*)>(game::Base()+addr::kHandleLookup)(*reinterpret_cast<std::uintptr_t*>(game::Base()+addr::kRefHandleTablePtr),handle,&lookup);
        if(result!=ref||(expectedId&&*reinterpret_cast<unsigned*>(result+off::kFormId)!=expectedId)) {
            if(result)reinterpret_cast<void(*)(std::uintptr_t)>(game::Base()+addr::kRefRelease)(result);
            return 0;
        }
        return result;
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
}
namespace {
using ReducedStep=std::uint64_t(*)(std::uintptr_t,std::uintptr_t,std::uintptr_t,std::uintptr_t);
ReducedStep g_reducedStep=nullptr;
void RunCover() {
    const unsigned long self=GetCurrentThreadId();
    if(self!=coverThread.load()||!cover.Size())return; // every caller is the main loop's thread (work log); never another
    auto* lock=reinterpret_cast<volatile LONG*>(game::Base()+kOwner);
    if(!ServiceLock::TryAcquire(lock,self)){++stats.busy;return;} // held elsewhere: next step, never a wait
    static LARGE_INTEGER frequency{};if(!frequency.QuadPart)QueryPerformanceFrequency(&frequency);
    auto clock=[]{LARGE_INTEGER t;QueryPerformanceCounter(&t);return double(t.QuadPart)*1000.0/double(frequency.QuadPart);};
    double longest=0;std::size_t ran=0;inCover=true;
    // A fault inside a command must not leave the service lock held: that would stop every service drain.
    __try{ran=cover.Run(kCoverBudgetMs,clock,longest);}
    __except(EXCEPTION_EXECUTE_HANDLER){static std::atomic<bool> told{false};if(!told.exchange(true))logger::Write("Traffic lifecycle: a command faulted behind the loading screen; lock released, the rest go on");}
    inCover=false;ServiceLock::Release(lock);
    stats.ran+=unsigned(ran);++stats.steps;
    const long long us=static_cast<long long>(longest*1000.0);
    for(long long seen=stats.longestUs.load();us>seen&&!stats.longestUs.compare_exchange_weak(seen,us);){}
}
std::uint64_t ReducedStepDetour(std::uintptr_t a,std::uintptr_t b,std::uintptr_t c,std::uintptr_t d) {
    const auto result=g_reducedStep(a,b,c,d);
    // The first call names the main loop's thread. Every caller found (14189D520 x3, 141888410, 141840F50,
    // 1418A2520) runs on it; a call from any other thread is logged once and never runs commands.
    unsigned long expected=0;const unsigned long self=GetCurrentThreadId();
    if(coverThread.compare_exchange_strong(expected,self))logger::Write("Traffic lifecycle: loading-screen step runs on thread %lu",self);
    else if(expected!=self){static std::atomic<bool> told{false};if(!told.exchange(true))logger::Write("Traffic lifecycle: loading-screen step also called on thread %lu (main %lu); no commands run there",self,expected);}
    RunCover();
    return result;
}
}
void InstallCover() {
    static const std::uint8_t kPrologue[15]={0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x48,0x89,0x4C,0x24,0x08};
    void* original=nullptr;
    if(hook::Install(game::Base()+kReducedStep,kPrologue,sizeof(kPrologue),reinterpret_cast<void*>(&ReducedStepDetour),&original)) {
        g_reducedStep=reinterpret_cast<ReducedStep>(original);
        logger::Write("Traffic lifecycle: loading-screen step hooked (1418891E0); traffic commands run after it on the main thread");
    } else logger::Write("Traffic lifecycle: loading-screen step NOT hooked (bytes differ); traffic commands wait for the screen to close");
}
bool InCover() {return inCover;}
bool InService() {
    return !*reinterpret_cast<const unsigned char*>(game::Base()+kThreading) ||
        *reinterpret_cast<volatile unsigned*>(game::Base()+kOwner)==GetCurrentThreadId();
}
Reference Keep(std::uintptr_t ref,unsigned expectedId) {
    const auto owned=ref?Acquire(ref,expectedId):0;
    if(!owned)return {};
    return Reference(reinterpret_cast<void*>(owned),[](void* value) {
        reinterpret_cast<void(*)(void*)>(game::Base()+addr::kRefRelease)(value);
    });
}
namespace {
void ReportRefused(const std::string& key) {
    static unsigned seen=0;const unsigned refused=requests.Refused();
    if(refused==seen)return;
    seen=refused;
    if(refused<=8||refused%256==0)logger::Write("Traffic lifecycle: request table full; refused %s (%u refused since start)",key.c_str(),refused);
}
}
std::optional<std::uint64_t> Poll(const std::string& key,unsigned id,const std::function<Work()>& factory) {
    HandOff();
    tCurrentKey=key.c_str();
    const auto result=requests.Poll(key,id,factory,Submit);
    tCurrentKey=nullptr;
    logger::ObserveReference("lifecycle.poll",id,result?(1ull+(*result!=0)):0,"key=%s complete=%d result=%llu outstanding=%zu in_service=%d",key.c_str(),bool(result),static_cast<unsigned long long>(result?*result:0),requests.Outstanding(""),InService());
    ReportRefused(key);return result;
}
std::size_t Outstanding(const char* operation) {return requests.Outstanding(operation);}
bool Pending(unsigned id,const char* operation) {HandOff();return requests.Pending(id,operation?operation:"");}
bool Post(const std::string& key,unsigned id,const std::function<Work()>& factory) {
    HandOff();
    // Finished posts are purged before the lookup, so a value here only means the table was full.
    tCurrentKey=key.c_str();
    const bool taken=!requests.Poll(key,id,factory,Submit,true);
    tCurrentKey=nullptr;
    logger::ObserveReference("lifecycle.post",id,unsigned(taken),"key=%s accepted_or_pending=%d outstanding=%zu in_service=%d",key.c_str(),taken,requests.Outstanding(""),InService());
    ReportRefused(key);return taken;
}
void Reset() {++observations::epoch;logger::Write("OBS system=runtime event=reset");requests.Reset();cover.TakeAll();} // old captures released here
void Close() {requests.Close();cover.TakeAll();}
void Open() {requests.Open();}
void Cancel(unsigned id) {if(id)requests.Cancel(id);}
void Forget(const std::string& key) {requests.Forget(key);}
}
