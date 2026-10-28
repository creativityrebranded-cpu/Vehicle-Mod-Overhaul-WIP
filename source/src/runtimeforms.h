#pragma once
#include <cstdint>
#include <cstddef>

// Stable identities at module boundaries. Runtime load-order bits belong only
// to the native adapter, never to a vehicle, city, quest or profile consumer.
namespace runtimeforms {
struct Key {
    const char* plugin=nullptr;
    std::uint32_t local=0;
    constexpr explicit operator bool() const {return plugin&&plugin[0]&&local&&local<=0xFFFFFF;}
};
struct Range {
    const char* plugin=nullptr;
    std::uint32_t first=0,count=0;
    constexpr Key At(std::size_t index) const {
        return index<count&&first&&std::uint64_t(first)+count-1<=0xFFFFFF
            ?Key{plugin,first+std::uint32_t(index)}:Key{};
    }
};
std::uintptr_t Lookup(std::uint32_t id);
std::uintptr_t LookupChecked(std::uint32_t id);
std::uint32_t Id(std::uintptr_t form);
std::uintptr_t LoadedPlugin(const char* name);
std::uint32_t RuntimeId(std::uintptr_t file,std::uint32_t local);
std::uint32_t Resolve(Key key);
}
