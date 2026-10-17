#include "navgen.h"

#include "addresses.h"
#include "game.h"
#include "hook.h"
#include "log.h"

#include <Windows.h>

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace navgen
{
	namespace
	{
		// BGSPlanet NavMeshGen module (vtable kNavMeshGenVtbl).
		constexpr std::uint32_t kSetEnabled = 0x007911F0;   // (module, char on): +0x98 = on; off also frees +0x70/+0x80 lists
		constexpr std::uint32_t kCellEligible = 0x00793330; // (cell, flags) -> bool: per-cell gate
		constexpr std::uint32_t kGenerateCell = 0x00791690; // (module, cell, flags, ...) -> bool: queued sub-cells
		constexpr std::uint32_t kQueueUpdate = 0x007912E0;  // (module, flags): queue UpdateNavMeshTaskNode over the grid
		constexpr std::uint32_t kEnable = 0x00791000;       // slot 7: +0x98 = 1
		constexpr std::uint32_t kSeedStatus = 0x00AD3870;   // (cell) -> int, 0 = gate goes on to the density test
		constexpr std::uint32_t kDensity = 0x00B0D6C0;      // (cell) -> float, OverlayMasks density, -1 = none
		constexpr std::uint32_t kModEnabled = 0x98, kModBusy = 0x99, kModDone = 0x70, kModPending = 0x80;
		constexpr std::uint32_t kCellNavData = 0x120, kNavGenerated = 0x16E;
		constexpr std::uint32_t kUpdateTaskCounter = 0x05E610D4;  // DAT_145e610d4: UpdateNavMeshTaskNode queued or running
		constexpr std::uint32_t kModPendingData = 0x88, kEntrySize = 0x3C;
		// Entry fields in the grid frame (FUN_1406ACB50): exterior X/Y ints, then cell min and max corners (x*100, y*100).
		constexpr std::uint32_t kEntryCellX = 0x08, kEntryCellY = 0x0C;
		constexpr std::uint32_t kEntryMinX = 0x1C, kEntryMinY = 0x20, kEntryMaxX = 0x28, kEntryMaxY = 0x2C;
		constexpr float kEntryCellSize = 100.0f;  // DAT_144f04cb8

		using SetEnabled_t = void (*)(std::uintptr_t, char);
		using Eligible_t = char (*)(std::uintptr_t, std::uint32_t);
		using Generate_t = char (*)(std::uintptr_t, std::uintptr_t, std::uint32_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
		SetEnabled_t g_setEnabled = nullptr;
		Eligible_t g_eligible = nullptr;
		Generate_t g_generate = nullptr;

		std::atomic<unsigned> g_yes{ 0 }, g_no{ 0 }, g_queued{ 0 }, g_none{ 0 }, g_offs{ 0 }, g_reenables{ 0 };
		std::atomic<ULONGLONG> g_offAt{ 0 };

		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Global(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }

		std::uintptr_t Module()
		{
			const auto mgr = Global(addr::kPlanetManagerPtr);
			if (!mgr) {
				return 0;
			}
			const auto count = At<std::uint32_t>(mgr, off::kMgrModuleCount);
			const auto mods = At<std::uintptr_t*>(mgr, off::kMgrModules);
			for (std::uint32_t i = 0; mods && i < count; ++i) {
				if (mods[i] && *reinterpret_cast<std::uintptr_t*>(mods[i]) - game::Base() == addr::kNavMeshGenVtbl) {
					return mods[i];
				}
			}
			return 0;
		}

		// Cell exterior coords and the "navmesh generated" byte; false when unreadable.
		bool CellInfo(std::uintptr_t cell, int& x, int& y, int& generated)
		{
			__try {
				const auto ext = *reinterpret_cast<std::uintptr_t*>(cell + off::kCellExterior);
				x = ext ? *reinterpret_cast<std::int32_t*>(ext) : 0;
				y = ext ? *reinterpret_cast<std::int32_t*>(ext + 4) : 0;
				const auto nav = *reinterpret_cast<std::uintptr_t*>(cell + kCellNavData);
				generated = nav ? *reinterpret_cast<std::uint8_t*>(nav + kNavGenerated) : -1;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		void Stack(char* out, std::size_t size)
		{
			void* frames[16] = {};
			const auto n = RtlCaptureStackBackTrace(2, 16, frames, nullptr);
			std::size_t used = 0;
			out[0] = 0;
			for (USHORT i = 0; i < n && used + 40 < size; ++i) {
				const auto a = reinterpret_cast<std::uintptr_t>(frames[i]);
				HMODULE mod = nullptr;
				char name[64] = "?";
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a), &mod) && mod) {
					char full[MAX_PATH] = {};
					GetModuleFileNameA(mod, full, MAX_PATH);
					const char* b = std::strrchr(full, 92);
					std::snprintf(name, sizeof(name), "%s", b ? b + 1 : full);
				}
				used += std::snprintf(out + used, size - used, "%s%s+0x%llX", i ? " < " : "", name,
					static_cast<unsigned long long>(a - reinterpret_cast<std::uintptr_t>(mod)));
			}
		}

		void SetEnabledDetour(std::uintptr_t module, char on)
		{
			const bool was = module && At<std::uint8_t>(module, kModEnabled) != 0;
			if (!on) {
				g_offs.fetch_add(1);
				g_offAt.store(GetTickCount64());
				char stack[640];
				Stack(stack, sizeof(stack));
				logger::Write("NavGen: generation switched OFF (was %s), queues cleared; caller %s", was ? "on" : "off", stack);
			}
			else if (!was) {
				logger::Write("NavGen: generation switched on");
			}
			g_setEnabled(module, on);
		}

		char EligibleDetour(std::uintptr_t cell, std::uint32_t flags)
		{
			const char ok = g_eligible(cell, flags);
			if (ok) {
				g_yes.fetch_add(1, std::memory_order_relaxed);
				return ok;
			}
			const auto n = g_no.fetch_add(1, std::memory_order_relaxed) + 1;
			if (n <= 60 || n % 200 == 0) {
				int x = 0, y = 0, gen = -1;
				CellInfo(cell, x, y, gen);
				// The gate's own inputs; read-only engine queries (the gate itself calls both on this thread).
				const int status = reinterpret_cast<int (*)(std::uintptr_t)>(game::Base() + kSeedStatus)(cell);
				const float density = reinterpret_cast<float (*)(std::uintptr_t)>(game::Base() + kDensity)(cell);
				logger::Write("NavGen: cell (%d, %d) rejected by the gate (#%u): seed status %d, overlay density %.3f, flags 0x%X, generated %d",
					x, y, n, status, density, flags, gen);
			}
			return ok;
		}

		char GenerateDetour(std::uintptr_t module, std::uintptr_t cell, std::uint32_t flags, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c)
		{
			const char queued = g_generate(module, cell, flags, a, b, c);
			(queued ? g_queued : g_none).fetch_add(1, std::memory_order_relaxed);
			return queued;
		}

		bool PlayerOnSurface(std::uintptr_t& cell)
		{
			const auto player = Global(addr::kPlayerCharacterPtr);
			cell = player ? At<std::uintptr_t>(player, off::kRefParentCell) : 0;
			const auto mgr = Global(addr::kPlanetManagerPtr);
			return cell && At<std::uintptr_t>(cell, off::kCellExterior) && At<std::uint8_t>(cell, off::kCellState) == 10 && mgr &&
				At<std::uint32_t>(mgr, off::kMgrPlanetId);
		}
	}

	void LogState(const char* why)
	{
		const auto m = Module();
		std::uintptr_t cell = 0;
		const bool surface = PlayerOnSurface(cell);
		int x = 0, y = 0, gen = -1;
		if (cell) {
			CellInfo(cell, x, y, gen);
		}
		logger::Write("NavGen [%s]: module %s, enabled %u busy %u, generated list %u pending %u; gate yes %u no %u; cells queued %u none %u; offs %u re-enables %u; player cell (%d, %d) generated %d%s",
			why, m ? "found" : "MISSING", m ? unsigned(At<std::uint8_t>(m, kModEnabled)) : 0u, m ? unsigned(At<std::uint8_t>(m, kModBusy)) : 0u,
			m ? At<std::uint32_t>(m, kModDone) : 0u, m ? At<std::uint32_t>(m, kModPending) : 0u, g_yes.load(), g_no.load(), g_queued.load(), g_none.load(),
			g_offs.load(), g_reenables.load(), x, y, gen, surface ? "" : " (not on a planet surface)");
	}

	unsigned InFlight()
	{
		const auto m = Module();
		if (!m || !At<std::uint8_t>(m, kModEnabled)) {
			return 0;
		}
		const unsigned task = *reinterpret_cast<volatile std::int32_t*>(game::Base() + kUpdateTaskCounter) != 0 ? 1u : 0u;
		return task + At<std::uint32_t>(m, kModDone);
	}

	bool Counts(unsigned& inFlightList, unsigned& pendingList, int& updateTask)
	{
		updateTask = *reinterpret_cast<volatile std::int32_t*>(game::Base() + kUpdateTaskCounter);
		const auto m = Module();
		if (!m) {
			inFlightList = pendingList = 0;
			return false;
		}
		inFlightList = At<std::uint32_t>(m, kModDone);
		pendingList = At<std::uint32_t>(m, kModPending);
		return true;
	}

	unsigned TranslatePending(std::int32_t cx, std::int32_t cy)
	{
		const auto m = Module();
		if (!m) {
			return 0;
		}
		const auto count = At<std::uint32_t>(m, kModPending);
		const auto data = At<std::uintptr_t>(m, kModPendingData);
		if (!count || !data) {
			return 0;
		}
		const float fx = cx * kEntryCellSize, fy = cy * kEntryCellSize;
		for (std::uint32_t i = 0; i < count; ++i) {
			const auto e = data + static_cast<std::uintptr_t>(i) * kEntrySize;
			At<std::int32_t>(e, kEntryCellX) -= cx;
			At<std::int32_t>(e, kEntryCellY) -= cy;
			At<float>(e, kEntryMinX) -= fx;
			At<float>(e, kEntryMinY) -= fy;
			At<float>(e, kEntryMaxX) -= fx;
			At<float>(e, kEntryMaxY) -= fy;
		}
		return count;
	}

	void Service()
	{
		static ULONGLONG lastLog = 0, lastTry = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - lastLog >= 10000) {
			lastLog = now;
			LogState("10 s");
		}
		if (now - lastTry < 1000) {
			return;
		}
		lastTry = now;
		const auto m = Module();
		std::uintptr_t cell = 0;
		if (!m || At<std::uint8_t>(m, kModEnabled) || !PlayerOnSurface(cell)) {
			return;
		}
		// Off while the player is on the surface, 3 s after the engine switched it off (a landing's own sequence
		// switches it back on within that time): the engine's enable, then its grid update, as at landing.
		const auto off = g_offAt.load();
		if (off && now - off < 3000) {
			return;
		}
		reinterpret_cast<void (*)(std::uintptr_t)>(game::Base() + kEnable)(m);
		reinterpret_cast<void (*)(std::uintptr_t, std::uint32_t)>(game::Base() + kQueueUpdate)(m, 0);
		g_reenables.fetch_add(1);
		LogState("re-enabled: off on the surface");
	}

	bool Install()
	{
		static const std::uint8_t kSetPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40 };
		static const std::uint8_t kGatePrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0xF6, 0x41, 0x48, 0x01 };
		static const std::uint8_t kGenPrologue[] = { 0x44, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08 };
		const auto base = game::Base();
		const bool a = hook::Install(base + kSetEnabled, kSetPrologue, sizeof(kSetPrologue), reinterpret_cast<void*>(&SetEnabledDetour), reinterpret_cast<void**>(&g_setEnabled));
		const bool b = hook::Install(base + kCellEligible, kGatePrologue, sizeof(kGatePrologue), reinterpret_cast<void*>(&EligibleDetour), reinterpret_cast<void**>(&g_eligible));
		const bool c = hook::Install(base + kGenerateCell, kGenPrologue, sizeof(kGenPrologue), reinterpret_cast<void*>(&GenerateDetour), reinterpret_cast<void**>(&g_generate));
		logger::Write("NavGen: hooks enable-switch %s, cell gate %s, cell generate %s", a ? "ok" : "FAILED", b ? "ok" : "FAILED", c ? "ok" : "FAILED");
		return a && b && c;
	}
}
