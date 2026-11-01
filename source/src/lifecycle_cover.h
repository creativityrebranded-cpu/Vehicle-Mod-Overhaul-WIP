#pragma once
// Behind the loading screen (2026-10-31, the 15:32 run placed 0 of 79): while Main+0x44C is set the main loop
// takes the reduced step FUN_1418891E0, which has no service-queue drain, so commands sent to the native queue
// wait until the screen closes. Traffic's commands are kept here instead and run at the end of that step, on the
// main thread, under the service's own lock, as the native paused drain (FUN_141885CB0) also runs them on the
// main thread under that lock. Only traffic's own commands run early; the engine's queued work is not touched.
// What is left when the screen closes goes to the native queue, ahead of anything newer.
#include "lifecycle_requests.h"
#include <Windows.h>
#include <deque>
#include <mutex>

namespace lifecycle {
// The service lock at 145F43DC8, layout read from FUN_1422C8D10 (acquire) and the release inline in
// FUN_1418BD310 (1418BD3A4..1418BD3F6): word 0 the owner's thread id, word 1 the state, whose low 16 bits
// count threads inside or waiting and bits 16..30 the hold level (0x10000 first hold, 0x20000 per re-entry).
struct ServiceLock {
    // Never waits. The engine's acquire adds 1 to the low half, then sets 0x10000 by compare-and-swap when no
    // one holds it; one swap from s to (s+1)|0x10000 leaves the same state. Busy: false, nothing changed.
    static bool TryAcquire(volatile LONG* word,DWORD self) {
        if(static_cast<DWORD>(word[0])==self){InterlockedExchangeAdd(&word[1],0x20000);return true;}
        const LONG s=word[1];
        if(s&0x7FFF0000)return false;
        if(InterlockedCompareExchange(&word[1],(s+1)|0x10000,s)!=s)return false;
        word[0]=static_cast<LONG>(self);return true;
    }
    // Exactly the engine's release: last hold clears the owner, drops the hold and our count, wakes one waiter.
    static void Release(volatile LONG* word) {
        if((word[1]&0x7FFF0000)==0x10000) {
            word[0]=0;
            const LONG old=InterlockedExchangeAdd(&word[1],static_cast<LONG>(0xFFFEFFFF));
            if((old&0xFFFF)>1)WakeByAddressSingle(const_cast<LONG*>(&word[1]));
        } else InterlockedExchangeAdd(&word[1],static_cast<LONG>(0xFFFE0000));
    }
};

// FIFO of tickets kept while the screen is up. Runs happen outside the mutex (a command may post again).
class CoverList {
    std::mutex mutex_;
    std::deque<Ticket> list_;
    std::atomic<std::size_t> size_{0};
public:
    void Add(const Ticket& ticket) {std::lock_guard lock(mutex_);list_.push_back(ticket);size_=list_.size();}
    std::size_t Size() const {return size_.load();}
    // Runs in order until `budgetMs` has passed (at least one command when any is waiting). Returns how many ran;
    // `longest` gets the slowest single command in ms.
    template<class Clock>
    std::size_t Run(double budgetMs,Clock clock,double& longest) {
        const double start=clock();std::size_t ran=0;
        for(;;) {
            Ticket next;
            {std::lock_guard lock(mutex_);if(list_.empty())break;next=std::move(list_.front());list_.pop_front();size_=list_.size();}
            const double before=clock();next->Run();const double took=clock()-before;
            if(took>longest)longest=took;
            ++ran;next.reset();
            if(clock()-start>=budgetMs)break;
        }
        return ran;
    }
    std::deque<Ticket> TakeAll() {std::deque<Ticket> out;{std::lock_guard lock(mutex_);out.swap(list_);size_=0;}return out;}
};
}
