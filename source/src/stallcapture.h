#pragma once
#include <cstdint>
namespace stallcapture {
using Time=std::uint64_t;
enum class Gate {NoBeat,Healthy,Background,NotGameplay,Quota,Captured,RetryDelay,AttemptsExhausted,Capture};
inline const char* Name(Gate g){switch(g){case Gate::NoBeat:return "no_heartbeat";case Gate::Healthy:return "below_threshold";case Gate::Background:return "background";case Gate::NotGameplay:return "not_gameplay";case Gate::Quota:return "successful_capture_quota";case Gate::Captured:return "already_captured";case Gate::RetryDelay:return "retry_backoff";case Gate::AttemptsExhausted:return "attempts_exhausted";default:return "capture";}}
struct Policy {
    Time capturedBeat=0,attemptBeat=0,lastAttempt=0;unsigned successes=0,attempts=0;
    Gate Check(Time now,Time beat,bool gameplay,bool foreground)const{
        if(!beat||now<beat)return Gate::NoBeat;
        if(now-beat<3000)return Gate::Healthy;
        if(!foreground)return Gate::Background;
        if(!gameplay)return Gate::NotGameplay;
        if(successes>=3)return Gate::Quota;
        if(capturedBeat==beat)return Gate::Captured;
        if(attemptBeat==beat&&attempts>=3)return Gate::AttemptsExhausted;
        if(attemptBeat==beat&&now>=lastAttempt&&now-lastAttempt<1000)return Gate::RetryDelay;
        return Gate::Capture;
    }
    void Result(Time now,Time beat,bool success){if(attemptBeat!=beat){attemptBeat=beat;attempts=0;}lastAttempt=now;++attempts;if(success){capturedBeat=beat;++successes;}}
};
}
