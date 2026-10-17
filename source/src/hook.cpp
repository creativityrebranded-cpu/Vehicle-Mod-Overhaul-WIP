#include "hook.h"

#include "log.h"

#include <Windows.h>

#include <cstring>

namespace hook
{
	namespace
	{
		constexpr std::size_t kJumpSize = 14;  // jmp qword ptr [rip+0]; dq address

		void WriteJump(std::uint8_t* at, std::uint64_t to)
		{
			at[0] = 0xFF;
			at[1] = 0x25;
			std::memset(at + 2, 0, 4);
			std::memcpy(at + 6, &to, 8);
		}
	}

	bool Install(std::uintptr_t target, const std::uint8_t* expected, std::size_t prologueLen, void* detour, void** original)
	{
		if (prologueLen < kJumpSize || std::memcmp(reinterpret_cast<const void*>(target), expected, prologueLen) != 0) {
			logger::Write("Hook: target %p does not match the expected bytes; not installed", reinterpret_cast<void*>(target));
			return false;
		}
		auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!trampoline) {
			logger::Write("Hook: could not allocate a trampoline");
			return false;
		}
		std::memcpy(trampoline, reinterpret_cast<const void*>(target), prologueLen);
		WriteJump(trampoline + prologueLen, target + prologueLen);

		DWORD oldProtect = 0;
		if (!VirtualProtect(reinterpret_cast<void*>(target), prologueLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			VirtualFree(trampoline, 0, MEM_RELEASE);
			logger::Write("Hook: could not unprotect %p", reinterpret_cast<void*>(target));
			return false;
		}
		auto* code = reinterpret_cast<std::uint8_t*>(target);
		WriteJump(code, reinterpret_cast<std::uint64_t>(detour));
		std::memset(code + kJumpSize, 0x90, prologueLen - kJumpSize);  // nop the rest of the moved instructions
		VirtualProtect(reinterpret_cast<void*>(target), prologueLen, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(target), prologueLen);

		*original = trampoline;
		logger::Write("Hook: installed at %p", reinterpret_cast<void*>(target));
		return true;
	}
}
