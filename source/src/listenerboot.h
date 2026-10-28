#pragma once
#include <array>
#include <cstdint>
#include <cstring>

// 1.14.70: the native local-static initializer publishes this already constructed
// .data object, then subscribes it to events. Persistent vehicle records can call
// its pending-list callbacks BEFORE that initializer. Publish only the pointer;
// leave native subscriptions, TLS guard, keyword lookup and destruction alone.
namespace listenerboot {
inline constexpr std::uint32_t kPointer = 0x061EEAD8, kObject = 0x05979AF0, kAssignment = 0x013169DB;
inline constexpr std::array<std::uint8_t, 7> kPublishBytes{0x48,0x89,0x1D,0xF6,0x80,0xED,0x04};
inline constexpr std::array<std::uint32_t, 4> kVtables{0x04C47020,0x04C46FF0,0x04C47008,0x04C47080};
enum class Result { Invalid, Published, Existing };
// read(address, out, bytes); publish(address, desired) returns previous value,
// atomically replacing NULL only. Never clear the pending list or its lock.
template<class Read, class Publish>
Result Prepare(std::uintptr_t base, Read read, Publish publish) {
    std::array<std::uint8_t, 7> bytes{};
    if (!base || !read(base+kAssignment, bytes.data(), bytes.size()) || bytes!=kPublishBytes) return Result::Invalid;
    std::array<std::uintptr_t, 4> tables{};
    if (!read(base+kObject,tables.data(),sizeof(tables))) return Result::Invalid;
    for (unsigned i=0;i<tables.size();++i) if (tables[i]!=base+kVtables[i]) return Result::Invalid;
    return publish(base+kPointer,base+kObject) ? Result::Existing : Result::Published;
}
}
