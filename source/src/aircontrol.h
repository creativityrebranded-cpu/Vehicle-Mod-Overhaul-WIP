#pragma once
#include "airtrafficmath.h"
#include <atomic>
#include <cstdint>

// Atomic payload prevents the C++ data race present in plain-data seqlocks.
// One producer; readers make at most two attempts and skip on contention.
namespace aircontrol {
struct Target {
    std::uintptr_t instance=0,ref=0;
    std::uint32_t body=0,form=0;
    std::uint64_t at=0,epoch=0;
    airtrafficmath::Vec position,reference,velocity;
    double yaw=0;
};
class Mailbox {
    std::atomic<std::uint64_t> sequence{0},at{0},epoch{0};
    std::atomic<std::uintptr_t> instance{0},ref{0};
    std::atomic<std::uint32_t> body{0},form{0};
    std::atomic<double> values[10]{};
public:
    void Publish(const Target&t){
        sequence.fetch_add(1,std::memory_order_seq_cst);
        instance=t.instance;ref=t.ref;body=t.body;form=t.form;at=t.at;epoch=t.epoch;
        const double v[]{t.position.x,t.position.y,t.position.z,t.reference.x,t.reference.y,t.reference.z,t.velocity.x,t.velocity.y,t.velocity.z,t.yaw};
        for(int i=0;i<10;++i)values[i].store(v[i],std::memory_order_seq_cst);
        sequence.fetch_add(1,std::memory_order_seq_cst);
    }
    bool Read(std::uintptr_t expected,std::uint32_t id,std::uint64_t now,Target&out)const{
        if(!expected||instance.load()!=expected||!id||body.load()!=id)return false;
        for(int attempt=0;attempt<2;++attempt){
            const auto before=sequence.load();if(before&1)continue;
            Target t;t.instance=instance;t.ref=ref;t.body=body;t.form=form;t.at=at;t.epoch=epoch;
            double v[10];for(int i=0;i<10;++i)v[i]=values[i].load();
            if(sequence.load()!=before)continue;
            if(t.instance!=expected||t.body!=id||!t.ref||!t.form||now<t.at||now-t.at>500)return false;
            t.position={v[0],v[1],v[2]};t.reference={v[3],v[4],v[5]};t.velocity={v[6],v[7],v[8]};t.yaw=v[9];out=t;return true;
        }
        return false;
    }
};
static_assert(std::atomic<double>::is_always_lock_free&&std::atomic<std::uint64_t>::is_always_lock_free);
}
