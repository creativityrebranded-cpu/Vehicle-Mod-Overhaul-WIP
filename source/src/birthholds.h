#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

// Own the collision object while it is used as a hold identity. Never call the
// engine, including an object's deleter, under the table mutex. Worker hooks
// only read this table or mark a build dirty; scene changes run on the frame thread.
namespace birthholds {
using Object = std::shared_ptr<void>;
struct Snapshot {
    std::uint32_t id = 0;
    std::uint64_t generation = 0, dirty = 0;
    Object object;
    bool applied = false, releasing = false;
};
// Whether a hold has service work to do, given the physics object seen now. A vehicle still without physics, or already
// frozen on this object, has none (2026-10-31 18:59 run: all 77 held vehicles posted every update and filled the request table).
inline bool NeedsApply(const Snapshot& held,const void* physics){
    return !held.releasing&&(physics!=held.object.get()||(physics&&!held.applied));
}
class Table {
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, Snapshot> entries_;
    std::uint64_t generation_ = 0;
public:
    void Add(std::uint32_t id) {
        if (!id) return;
        std::lock_guard lock(mutex_);
        if (!entries_.count(id)) entries_.emplace(id, Snapshot{id, ++generation_});
    }
    std::optional<Snapshot> Get(std::uint32_t id) const {
        std::lock_guard lock(mutex_);
        const auto it=entries_.find(id);
        if(it==entries_.end()) return {};
        return it->second;
    }
    std::vector<Snapshot> All() const {
        std::vector<Snapshot> result;
        std::lock_guard lock(mutex_);
        result.reserve(entries_.size());
        for(const auto& [id,entry]:entries_) result.push_back(entry);
        return result;
    }
    bool Holds(const void* object) const {
        if(!object) return false;
        std::lock_guard lock(mutex_);
        for(const auto& [id,entry]:entries_)
            if(!entry.releasing && entry.object.get()==object) return true;
        return false;
    }
    void Dirty(std::uint32_t id) {
        std::lock_guard lock(mutex_);
        if(auto it=entries_.find(id);it!=entries_.end()) {++it->second.dirty;it->second.applied=false;}
    }
    // Publish before entering the setter so a nested dynamic request sees the hold.
    bool Publish(const Snapshot& expected,Object object) {
        Object retired;
        {
            std::lock_guard lock(mutex_);
            const auto it=entries_.find(expected.id);
            if(it==entries_.end() || it->second.generation!=expected.generation || it->second.releasing) return false;
            retired.swap(it->second.object);
            it->second.object=std::move(object);
            it->second.applied=false;
        }
        return true;
    }
    void Applied(const Snapshot& expected,const Object& object) {
        std::lock_guard lock(mutex_);
        const auto it=entries_.find(expected.id);
        if(it!=entries_.end() && it->second.generation==expected.generation &&
           it->second.dirty==expected.dirty && it->second.object==object && !it->second.releasing)
            it->second.applied=true;
    }
    bool BeginRelease(const Snapshot& expected) {
        std::lock_guard lock(mutex_);
        const auto it=entries_.find(expected.id);
        if(it==entries_.end() || it->second.generation!=expected.generation || it->second.releasing) return false;
        it->second.releasing=true;
        return true;
    }
    void FinishRelease(const Snapshot& expected,bool success) {
        Object retired;
        {
            std::lock_guard lock(mutex_);
            const auto it=entries_.find(expected.id);
            if(it==entries_.end() || it->second.generation!=expected.generation) return;
            if(success) { retired.swap(it->second.object);entries_.erase(it); }
            else { it->second.releasing=false;it->second.applied=false;++it->second.dirty; }
        }
    }
    void Remove(std::uint32_t id) {
        Object retired;
        {
            std::lock_guard lock(mutex_);
            const auto it=entries_.find(id);
            if(it==entries_.end()) return;
            retired.swap(it->second.object);entries_.erase(it);
        }
    }
    void Clear() {
        std::unordered_map<std::uint32_t,Snapshot> retired;
        {std::lock_guard lock(mutex_);retired.swap(entries_);}
    }
};
}
