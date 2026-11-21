#pragma once
// PF-2 (2026-11-12): the freeze recorder keeps evidence. The 13:11 freeze dump was overwritten 48 minutes later by a background-window
// false alarm, and its heap state was lost; the 11-10 17:20 hang dumps went the same way. Pure (no engine calls); tests: tests/dumpkeep_test.cpp.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace dumpkeep {
inline constexpr int kKeepHang = 3, kKeepStall = 6;

// "hang-20261112-131148-fg.dmp": sorts by time; fg/bg = the game window was in front or not when it was written.
inline std::wstring Name(const wchar_t* kind, int year, int month, int day, int hour, int minute, int second, bool foreground) {
    wchar_t text[64];
    swprintf_s(text, L"%s-%04d%02d%02d-%02d%02d%02d-%s.dmp", kind, year, month, day, hour, minute, second, foreground ? L"fg" : L"bg");
    return text;
}
// Of the kept files of one kind (names as Name gives), the ones to delete so that `keep` remain: the oldest first.
inline std::vector<std::wstring> ToDelete(std::vector<std::wstring> names, const std::wstring& kind, int keep) {
    const std::wstring prefix = kind + L"-";
    names.erase(std::remove_if(names.begin(), names.end(), [&](const std::wstring& n) {
        return n.size() < prefix.size() + 20 || n.compare(0, prefix.size(), prefix) != 0 || n.compare(n.size() - 4, 4, L".dmp") != 0; }), names.end());
    std::sort(names.begin(), names.end());  // the date and time after the prefix sort by age
    if (int(names.size()) <= keep) return {};
    names.resize(names.size() - std::size_t(std::max(0, keep)));
    return names;
}

// The game allocator's endless retry (1.16.244): FUN_1423f7440 loops { FUN_1422c8200 = MemoryManager.cpp:0x667 out-of-memory handler;
// FUN_1423f6fc0 -> FUN_1423f6da0 } while nothing comes back. A thread found executing in these is waiting for heap that never frees.
struct Range { std::uint32_t rva, size; const char* name; };
inline constexpr Range kAllocatorSpin[] = {
    { 0x023F6DA0, 0xF3, "FUN_1423f6da0 pool alloc" },
    { 0x023F6FC0, 0x2D6, "FUN_1423f6fc0 size classes" },
    { 0x023F7440, 0xCC, "FUN_1423f7440 retry loop" },
    { 0x022C8200, 0x210, "FUN_1422c8200 out-of-memory handler" },
};
// The name of the allocator function an address (as an offset from the game's base) is in, or nullptr.
inline const char* AllocatorSpinAt(std::uint64_t rva) {
    for (const auto& r : kAllocatorSpin) if (rva >= r.rva && rva < std::uint64_t(r.rva) + r.size) return r.name;
    return nullptr;
}
// A return address from that loop on the stack also counts: the thread may be inside the handler's own callees when suspended.
inline constexpr std::uint32_t kRetryReturn = 0x023F74D4;  // FUN_1423f7440's call into FUN_1423f6fc0 returns here
}
