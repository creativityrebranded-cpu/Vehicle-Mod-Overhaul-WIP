#pragma once
#include <cstddef>
#include <cstdint>
namespace logbuffer {
struct Policy {
    bool batching=false;
    std::size_t pending=0;
    std::uint64_t last=0;
    void Begin(std::uint64_t now) { if(!batching){batching=true;last=now;} }
    bool Due(std::uint64_t now) const {
        return pending && (!batching || pending>=32768 || now-last>=250);
    }
    void Added(std::size_t bytes) { pending+=bytes; }
    void Flushed(std::uint64_t now) { pending=0;last=now; }
};
}
