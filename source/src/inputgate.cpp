#include "inputgate.h"

#include "bindlogic.h"
#include "game.h"
#include "hook.h"
#include "log.h"

#include <Windows.h>

#include <atomic>

namespace inputgate
{
	namespace
	{
		// Starfield.exe 1.16.244 (7e9adb14...): the queue add's call of the resolver (bytes E8 -> 0x1422F1840).
		constexpr std::uint32_t kResolveCall = 0x022DAC60;
		constexpr std::uint32_t kResolve = 0x022F1840;    // (ControlMap*, InputEvent*): fills the event's user event
		// InputEvent: +0x08 u8 device (2 gamepad), +0x10 u8 type (0 button), +0x30 u32 code (XInput mask, 9 / 10 the
		// triggers), +0x34 u8 block reason (0 = delivered; 4 = the enable manager's, the one the game itself uses).
		constexpr std::uint8_t kGamepad = 2, kButton = 0, kBlockedReason = 4;

		using Resolve_t = void (*)(std::uintptr_t, std::uintptr_t);
		Resolve_t g_resolve = nullptr;
		std::atomic<std::uint32_t> g_blocked{ 0 };
		std::atomic<unsigned> g_count{ 0 };

		bool IsBlocked(std::uint32_t code, std::uint32_t bits)
		{
			if (code == 9) return (bits & bindlogic::kLT) != 0;
			if (code == 10) return (bits & bindlogic::kRT) != 0;
			return code && code <= 0xFFFFu && (code & (code - 1)) == 0 && (bits & code) != 0;
		}

		void Gate(std::uintptr_t event)
		{
			const std::uint32_t bits = g_blocked.load(std::memory_order_relaxed);
			if (!bits) return;
			__try {
				auto* reason = reinterpret_cast<std::uint8_t*>(event + 0x34);
				if (*reinterpret_cast<std::uint8_t*>(event + 0x10) != kButton || *reinterpret_cast<std::uint8_t*>(event + 0x08) != kGamepad || *reason) return;
				if (!IsBlocked(*reinterpret_cast<std::uint32_t*>(event + 0x30), bits)) return;
				*reason = kBlockedReason;
				g_count.fetch_add(1, std::memory_order_relaxed);
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		void Resolve(std::uintptr_t controlMap, std::uintptr_t event)
		{
			g_resolve(controlMap, event);
			if (event) Gate(event);
		}
	}

	bool Install()
	{
		g_resolve = reinterpret_cast<Resolve_t>(game::Base() + kResolve);
		const bool ok = hook::RedirectCall(game::Base() + kResolveCall, reinterpret_cast<void*>(g_resolve), reinterpret_cast<void*>(&Resolve));
		logger::Write("Input gate: %s", ok ? "on (the game's action is switched off on the mod's controller buttons)"
			: "NOT installed (the resolver call differs); the game still acts on the mod's controller buttons");
		return ok;
	}

	void SetBlocked(std::uint32_t padBits) { g_blocked.store(padBits, std::memory_order_relaxed); }
	unsigned TakeBlockedCount() { return g_count.exchange(0, std::memory_order_relaxed); }
}
