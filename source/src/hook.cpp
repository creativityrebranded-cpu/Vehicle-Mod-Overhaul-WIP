#include "hook.h"

#include "log.h"

#include <Windows.h>

#include <cstring>
#include <algorithm>
#include <limits>

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

		bool ReadCall(std::uintptr_t site, void* expected, std::uint8_t (&bytes)[5])
		{
			__try {
				std::memcpy(bytes, reinterpret_cast<const void*>(site), 5);
				std::int32_t displacement = 0;
				std::memcpy(&displacement, bytes + 1, 4);
				return bytes[0] == 0xE8 && site + 5 + displacement == reinterpret_cast<std::uintptr_t>(expected);
			} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		void* NearRelay(std::uintptr_t site)
		{
			SYSTEM_INFO si{}; GetSystemInfo(&si);
			const auto gran = std::uintptr_t(si.dwAllocationGranularity);
			const auto reach = std::uintptr_t(INT32_MAX);
			const auto low = std::max(site + 5 > reach ? site + 5 - reach : 0, reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress));
			const auto high = std::min(site + 5 + reach, reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress));
			for (auto at = (low + gran - 1) & ~(gran - 1); at < high;) {
				MEMORY_BASIC_INFORMATION mbi{};
				if (!VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi))) break;
				const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
				if (mbi.State == MEM_FREE && high - at >= si.dwPageSize && end - at >= si.dwPageSize) {
					if (auto* p = VirtualAlloc(reinterpret_cast<void*>(at), si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return p;
					at += gran; // a racing allocation must not make this scan stall
				} else {
					const auto next = (end + gran - 1) & ~(gran - 1);
					if (next <= at) break;
					at = next;
				}
			}
			return nullptr;
		}
	}

	bool RedirectCall(std::uintptr_t site, void* expectedTarget, void* detour)
	{
		std::uint8_t expected[5]{};
		if (!site || !expectedTarget || !detour || !ReadCall(site, expectedTarget, expected)) {
			logger::Write("Hook: call %p has an unexpected destination; unchanged", reinterpret_cast<void*>(site));
			return false;
		}
		auto* relay = static_cast<std::uint8_t*>(NearRelay(site));
		if (!relay) return false;
		WriteJump(relay, reinterpret_cast<std::uint64_t>(detour));
		DWORD old = 0;
		std::uint8_t current[5]{};
		if (!VirtualProtect(relay, kJumpSize, PAGE_EXECUTE_READ, &old) ||
			!ReadCall(site, expectedTarget, current) || std::memcmp(expected, current, 5) != 0 ||
			!VirtualProtect(reinterpret_cast<void*>(site), 5, PAGE_EXECUTE_READWRITE, &old)) {
			VirtualFree(relay, 0, MEM_RELEASE); return false;
		}
		FlushInstructionCache(GetCurrentProcess(), relay, kJumpSize);
		const auto distance = reinterpret_cast<std::intptr_t>(relay) - static_cast<std::intptr_t>(site + 5);
		const auto displacement = static_cast<std::int32_t>(distance); // relay allocation is bounded to rel32 reach
		std::uint8_t patch[5] = {0xE8}; std::memcpy(patch + 1, &displacement, 4);
		std::memcpy(reinterpret_cast<void*>(site), patch, 5);
		DWORD ignored = 0;
		VirtualProtect(reinterpret_cast<void*>(site), 5, old, &ignored);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 5);
		logger::Write("Hook: redirected camera call %p", reinterpret_cast<void*>(site));
		return true;
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
