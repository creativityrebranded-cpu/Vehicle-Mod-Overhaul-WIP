#pragma once

#include <cstddef>
#include <cstdint>

// Minimal function detour: overwrites the target's first `prologueLen` bytes (whole instructions, no
// RIP-relative ones) with an absolute jump to `detour`, and builds a trampoline that runs those bytes and
// jumps back. Refuses (returns false, changes nothing) if the target's bytes are not exactly `expected`.
namespace hook
{
	bool Install(std::uintptr_t target, const std::uint8_t* expected, std::size_t prologueLen, void* detour, void** original);
	// Startup-only redirect of one E8 call. Checks the original destination and uses a near relay;
	// the shared callee and all other call sites remain unchanged. Successful relays live until exit.
	bool RedirectCall(std::uintptr_t site, void* expectedTarget, void* detour);
}
