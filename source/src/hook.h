#pragma once

#include <cstddef>
#include <cstdint>

// Minimal function detour: overwrites the target's first `prologueLen` bytes (whole instructions, no
// RIP-relative ones) with an absolute jump to `detour`, and builds a trampoline that runs those bytes and
// jumps back. Refuses (returns false, changes nothing) if the target's bytes are not exactly `expected`.
namespace hook
{
	bool Install(std::uintptr_t target, const std::uint8_t* expected, std::size_t prologueLen, void* detour, void** original);
}
