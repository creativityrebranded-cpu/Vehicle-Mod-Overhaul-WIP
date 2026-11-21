#pragma once
#include "traffic_api.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <source_location>
#include <tuple>
#include <array>
#include <algorithm>
#include <mutex>
#include <cstdio>
#include <chrono>

// A forwarding facade, not another native adapter. No scheduling, retries,
// caching, reference ownership, validation queries, or callback registration.
// Existing caller guards and the core's native checks remain authoritative.
namespace engineport {
enum class Event { Unavailable, Refused, Pending, Suppressed };
struct Caller {const char* module;std::source_location location;Caller(const char* name,std::source_location where=std::source_location::current()):module(name),location(where){}};
struct Trace {
    Event event;
    const char* operation;
    const char* caller;
    const char* reason;
    unsigned vehicle;
    std::uint64_t request;const char* file="";unsigned line=0;
};
struct Record {Event event=Event::Unavailable;unsigned vehicle=0,line=0;std::uint64_t request=0,at=0;char operation[64]{},caller[64]{},reason[192]{},file[512]{};};
#ifndef FVO_RELEASE
inline std::mutex recordLock;inline std::array<Record,256> records;inline std::uint64_t recordCount=0;
inline void Remember(const Trace& trace){Record record;record.event=trace.event;record.vehicle=trace.vehicle;record.line=trace.line;record.request=trace.request;record.at=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());std::snprintf(record.operation,sizeof record.operation,"%s",trace.operation);std::snprintf(record.caller,sizeof record.caller,"%s",trace.caller);std::snprintf(record.reason,sizeof record.reason,"%s",trace.reason);std::snprintf(record.file,sizeof record.file,"%s",trace.file);std::lock_guard lock(recordLock);records[recordCount++%records.size()]=record;}
inline bool ReadRecord(int index,Record& out){std::lock_guard lock(recordLock);if(index<0||std::uint64_t(index)>=std::min<std::uint64_t>(recordCount,records.size()))return false;out=records[(recordCount-1-std::uint64_t(index))%records.size()];return true;}
#else
inline void Remember(const Trace&){}inline bool ReadRecord(int,Record&){return false;}
#endif
using Observer=void (*)(const Trace&);
inline std::atomic<Observer> observer{nullptr};
inline std::atomic<std::uint64_t> sequence{0};
inline const char* Name(Event e) {return e==Event::Suppressed?"suppressed":e==Event::Pending?"pending":e==Event::Refused?"refused":"unavailable";}
inline void Report(Event event,const char* operation,Caller caller,unsigned vehicle,const char* reason,std::uint64_t request=0) {
    const Trace trace{event,operation,caller.module,reason,vehicle,request?request:sequence.fetch_add(1,std::memory_order_relaxed)+1,caller.location.file_name(),caller.location.line()};Remember(trace);if(auto sink=observer.load(std::memory_order_relaxed))sink(trace);
}
inline bool Mutation(const char* operation){for(const char* name:{"startTrip","startScripted","startSupply","startPatrol","startArmed","releaseBirth","sendPair","deleteRef","bringDriver","canStartTrip","mayUse"})if(std::strcmp(name,operation)==0)return true;return false;}
template<class... Args> unsigned Subject(unsigned explicitId,const char* operation,Args&&... args){if(explicitId)return explicitId;if constexpr(sizeof...(Args)>0){auto tuple=std::forward_as_tuple(args...);if constexpr(sizeof...(Args)>1){using Second=std::remove_cvref_t<decltype(std::get<1>(tuple))>;if constexpr(std::is_integral_v<Second>){if(std::strcmp(operation,"startTrip")==0||std::strcmp(operation,"startScripted")==0||std::strcmp(operation,"startArmed")==0||std::strcmp(operation,"mayUse")==0||std::strcmp(operation,"bringDriver")==0)return unsigned(std::get<1>(tuple));}}using First=std::remove_cvref_t<decltype(std::get<0>(tuple))>;if constexpr(std::is_integral_v<First>)return unsigned(std::get<0>(tuple));}return 0;}
// Member identifies the published function pointer at compile time. Forward
// exactly once, synchronously on the calling thread, with unchanged arguments.
// A missing export is an interface refusal, never an invented engine result.
// Callers must retain their established capability checks for optional methods.
template<class Function,class... Args>
auto FunctionForward(Function function,const char* operation,Caller caller,unsigned vehicle,Args&&... args)
    -> std::invoke_result_t<Function,Args...> {
    using Result=std::invoke_result_t<Function,Args...>;
    vehicle=Subject(vehicle,operation,args...);
    if(!function){Report(Event::Unavailable,operation,caller,vehicle,"missing_capability");if constexpr(!std::is_void_v<Result>)return Result{};else return;}
    return function(std::forward<Args>(args)...);
}
template<auto Member,class... Args>
auto Forward(const FvoTrafficApi* api,const char* operation,Caller caller,unsigned vehicle,Args&&... args)
    -> std::invoke_result_t<decltype(std::declval<FvoTrafficApi>().*Member),Args...> {
    using Result=std::invoke_result_t<decltype(std::declval<FvoTrafficApi>().*Member),Args...>;
    vehicle=Subject(vehicle,operation,args...);
    if(!api||!(api->*Member)) {
        Report(Event::Unavailable,operation,caller,vehicle,"missing_capability");
        if constexpr(!std::is_void_v<Result>)return Result{};else return;
    }
    if constexpr(std::is_void_v<Result>)return (api->*Member)(std::forward<Args>(args)...);
    else {
        const auto result=(api->*Member)(std::forward<Args>(args)...);
        if(std::strcmp(operation,"mutationPending")==0&&result!=0)Report(Event::Pending,operation,caller,vehicle,"existing_operation_pending");
        else if(Mutation(operation)&&result==0)Report(Event::Refused,operation,caller,vehicle,"not_accepted_or_pending; adapter_result_0");
        return result;
    }
}
}
