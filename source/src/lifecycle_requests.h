#pragma once
// Bounded, non-blocking requests. The engine service owns execution; callers
// consume completion on a later update. No engine work or captured deleter runs
// while the request-table mutex is held.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace lifecycle {
using Work = std::function<std::uint64_t()>;
inline std::uint64_t SteadyMs(){return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
struct Epoch { std::atomic<std::uint64_t> value{1}; };
struct Request {
    enum State { Preparing, Queued, Running, Done };
    std::atomic<State> state{Preparing};
    std::atomic<bool> canceled{false};
    std::shared_ptr<Epoch> epoch;
    std::uint64_t generation=0, result=0;
    std::uint64_t seenDone=0; // under the table's mutex: when the table first saw it Done; uncollected, evicted kAbandonMs later
    unsigned id=0;
    bool forget=false;
    Work work;
    void Run() {
        State expected=Queued;
        if(!state.compare_exchange_strong(expected,Running))return;
        auto owned=std::move(work);
        if(!canceled.load(std::memory_order_acquire)&&epoch->value.load(std::memory_order_acquire)==generation && owned)
            result=owned();
        owned={}; // drop engine references BEFORE publishing completion
        state.store(Done,std::memory_order_release);
    }
};
using Ticket=std::shared_ptr<Request>;
class Requests {
    std::mutex mutex_;
    std::map<std::string,Ticket> jobs_;
    std::shared_ptr<Epoch> epoch_=std::make_shared<Epoch>();
    const std::size_t limit_;
    bool closed_=false;
    std::atomic<unsigned> refused_{0};
    std::uint64_t (*clock_)();
    // Under mutex_. Finished posts go; so do finished results their caller stopped asking for (an abandoned
    // move key, a pair reset mid-request). Without that a full table could stay full and everything would wait.
    void Purge() {
        const auto now=clock_();
        for(auto it=jobs_.begin();it!=jobs_.end();) {
            const auto& t=it->second;
            const bool done=t->state.load(std::memory_order_acquire)==Request::Done;
            if(done&&!t->seenDone)t->seenDone=now;
            if(done&&(t->forget||(jobs_.size()>=limit_&&now-t->seenDone>=kAbandonMs)))it=jobs_.erase(it);else ++it;
        }
    }
public:
    static constexpr std::uint64_t kAbandonMs=10000;
    explicit Requests(std::size_t limit=64,std::uint64_t (*clock)()=SteadyMs):limit_(limit),clock_(clock){}
    template<class Factory,class Submit>
    std::optional<std::uint64_t> Poll(const std::string& key,unsigned id,Factory factory,Submit submit,bool forget=false,bool allowNew=true,bool* tracked=nullptr) {
        Ticket ticket;
        bool fresh=false,done=false;
        if(tracked)*tracked=false;
        {
            std::lock_guard lock(mutex_);
            if(closed_)return std::nullopt; // not a failure: the caller asks again once the table opens
            if(allowNew)Purge();
            auto it=jobs_.find(key);
            if(it!=jobs_.end()) {
                ticket=it->second;
                done=ticket->state.load(std::memory_order_acquire)==Request::Done;
                if(done)jobs_.erase(it); // ticket keeps captures alive past unlock
            } else {
                if(!allowNew)return std::nullopt;
                // Full: a post is dropped and says so (its owner keeps the wanted state and asks again); a request
                // waits, and Pending answers true, so no caller reads a full table as a failed birth (15:32 run).
                if(jobs_.size()>=limit_){refused_.fetch_add(1);if(forget)return std::uint64_t(0);return std::nullopt;}
                ticket=std::make_shared<Request>();
                ticket->epoch=epoch_;ticket->generation=epoch_->value.load();ticket->id=id;ticket->forget=forget;
                jobs_.emplace(key,ticket);fresh=true;
            }
            if(tracked)*tracked=true;
        }
        if(done)return ticket->generation==epoch_->value.load()?ticket->result:0;
        if(!fresh)return std::nullopt;
        ticket->work=factory(); // owning-handle lookup may re-enter the table
        ticket->state.store(Request::Queued,std::memory_order_release);
        if(!ticket->work || ticket->generation!=epoch_->value.load() || !submit(ticket)) {
            ticket->work={};ticket->state.store(Request::Done,std::memory_order_release);
            // Leave a completed failure for the next poll; never create twice
            // because the submitter happened to finish synchronously.
        }
        return std::nullopt;
    }
    bool Pending(unsigned id=0,const std::string& prefix={}) {
        std::lock_guard lock(mutex_);
        if(closed_)return true; // nothing may read "no request and no result" as a failure while closed
        Purge();
        if(jobs_.size()>=limit_)return true; // a full table is a wait, not a refusal
        for(const auto& [key,ticket]:jobs_)if((!id||ticket->id==id)&&
            (prefix.empty()||key.starts_with(prefix+":"))&&(!ticket->forget||ticket->state.load()!=Request::Done))return true;
        return false;
    }
    void Cancel(unsigned id) {
        std::map<std::string,Ticket> discarded;
        {
            std::lock_guard lock(mutex_);
            for(auto it=jobs_.begin();it!=jobs_.end();) {
                if(it->second->id==id){it->second->canceled.store(true);discarded.emplace(it->first,it->second);it=jobs_.erase(it);}else ++it;
            }
        }
    }
    void Forget(const std::string& key) {
        Ticket discarded;
        {std::lock_guard lock(mutex_);auto it=jobs_.find(key);if(it!=jobs_.end()){
            discarded=it->second;discarded->canceled.store(true);jobs_.erase(it);
        }}
    }
    void Reset() {
        std::map<std::string,Ticket> discarded;
        {
            std::lock_guard lock(mutex_);
            epoch_->value.fetch_add(1,std::memory_order_acq_rel);
            discarded.swap(jobs_);
        } // captures released here, never under mutex
    }
    // Reset, and nothing new until Open: for as long as the engine replaces the world.
    void Close() {
        {std::lock_guard lock(mutex_);closed_=true;}
        Reset();
    }
    void Open() {std::lock_guard lock(mutex_);closed_=false;}
    unsigned Refused() const {return refused_.load();}
    // Wanted-state owners pace themselves against this (visibility, persistence): requests with this prefix not yet finished.
    std::size_t Outstanding(const std::string& prefix) {
        std::lock_guard lock(mutex_);std::size_t n=0;
        for(const auto& [key,ticket]:jobs_)if((prefix.empty()||key.starts_with(prefix+":"))&&ticket->state.load()!=Request::Done)++n;
        return n;
    }
};
}
