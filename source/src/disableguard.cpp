#include "disableguard.h"

#include "addresses.h"
#include "game.h"
#include "hook.h"
#include "log.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>

namespace disableguard
{
	namespace
	{
		using uptr = std::uintptr_t;
		constexpr std::uint32_t kRvaActorDisable = 0x01958630;  // FUN_141958630: the actor's vtable +0x7E8 (queued disable)
		// mov [rsp+10h], rbx / mov [rsp+18h], rsi / push rdi / sub rsp, 20h (whole instructions, no RIP-relative ones)
		constexpr std::uint8_t kPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20 };
		using Disable_t = void (*)(uptr);
		Disable_t g_original = nullptr;
		std::atomic<unsigned> g_skipped{ 0 };

		bool Unready(uptr actor, std::uint32_t& id, std::uint32_t& base)
		{
			__try {
				id = *reinterpret_cast<const std::uint32_t*>(actor + off::kFormId);
				const uptr b = *reinterpret_cast<const uptr*>(actor + 0x98);
				base = b ? *reinterpret_cast<const std::uint32_t*>(b + off::kFormId) : 0;
				return *reinterpret_cast<const uptr*>(actor + 0x358) == 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return true; }
		}
		void Detour(uptr actor)
		{
			std::uint32_t id = 0, base = 0;
			if (actor && Unready(actor, id, base)) {
				const unsigned n = g_skipped.fetch_add(1) + 1;
				if (n <= 50)
					logger::Write("Disable guard: actor %08X (base %08X) was queued for disable before it was set up (+0x358 empty); not disabled "
						"(the game would crash here; %u so far, thread %lu)", id, base, n, GetCurrentThreadId());
				return;
			}
			g_original(actor);
		}
	}

	bool Install()
	{
		void* orig = nullptr;
		const bool ok = hook::Install(game::Base() + kRvaActorDisable, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&Detour), &orig);
		if (ok) g_original = reinterpret_cast<Disable_t>(orig);
		logger::Write("Disable guard: %s", ok ? "on (actors not yet set up are not disabled)" : "NOT installed (the actor disable step differs)");
		return ok;
	}
}
