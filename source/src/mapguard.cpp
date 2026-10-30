#include "mapguard.h"

#include "addresses.h"
#include "game.h"
#include "fvopaths.h"
#include "hook.h"
#include "log.h"
#include "runtimeforms.h"
#include "vehicleworld.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace mapguard
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T> T At(uptr p, std::uint32_t o) { return *reinterpret_cast<const T*>(p + o); }

		// FunctorNode<BSProcGen::Resources, lambda, vector<SurfaceMapModelLoader::RefModel>>: vftable 0x144C54E38, slot 2 =
		// FUN_14142FAE0(task) -> FUN_141426FC0(task + 0xB0, task + 0xB8); the vector (begin, end, capacity) at +0xB8.
		constexpr std::uint32_t kTaskVtbl = 0x04C54E38, kRunSlot = 2, kRunRva = 0x0142FAE0;
		constexpr std::uint32_t kVector = 0xB8, kRefModelSize = 0x70;
		constexpr std::size_t kMaxModels = 60000;  // under the 16-bit child count of the node it builds
		using Run_t = char (*)(uptr);
		Run_t g_original = nullptr;
		std::atomic<std::size_t> g_lastCount{ 0 }, g_worst{ 0 };
		std::atomic<bool> g_reportNow{ false };

		char RunDetour(uptr task)
		{
			auto* begin = reinterpret_cast<uptr*>(task + kVector);
			auto* end = begin + 1;
			const uptr b = *begin, e = *end;
			const std::size_t n = e > b ? (e - b) / kRefModelSize : 0;
			g_lastCount = n;
			if (n > g_worst.load()) g_worst = n;
			if (n > 20000) g_reportNow = true;
			if (n <= kMaxModels) return g_original(task);
			logger::Write("Map guard: the local map's model build has %zu references (the game's node holds 65535): building the first %zu only",
				n, kMaxModels);
			*end = b + kMaxModels * kRefModelSize;  // the rest stay owned by the vector and are destroyed with it
			const char r = g_original(task);
			*end = e;
			return r;
		}

		uptr Player() { __try { return *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

		// The worldspace's persistent cell (FUN_140BB54A0 without creating one): +0x1D8, else its parent world's.
		uptr PersistentCell(uptr ws)
		{
			for (int k = 0; ws && k < 4; ++k) {
				if (const uptr c = At<uptr>(ws, 0x1D8)) return c;
				if (!At<uptr>(ws, 0x1F0) || !((At<std::uint8_t>(ws, 0xF2) >> 6) & 1)) return 0;
				ws = At<uptr>(ws, 0x1F0);
			}
			return 0;
		}

		struct Count { std::uint32_t base = 0; unsigned n = 0; };

		// Plain data only (a __try function): run-time references counted by base in a small open table.
		constexpr unsigned kTable = 256;
		struct Scan { unsigned total = 0, created = 0, fullLod = 0, read = 0, flagDisabled = 0, flagDeleted = 0; bool ok = false, cell = false; Count byBase[kTable]; };
		void ScanCell(uptr player, Scan& s)
		{
			__try {
				const uptr cell = At<uptr>(player, off::kRefParentCell);
				const uptr ws = cell ? At<uptr>(cell, off::kCellWorldspace) : 0;
				const uptr pc = PersistentCell(ws);
				if (!pc) { s.ok = true; return; }
				s.cell = true;
				s.total = At<std::uint32_t>(pc, 0x80);
				const uptr arr = At<uptr>(pc, 0x88);
				const unsigned n = std::min(s.total, 400000u);
				for (unsigned i = 0; arr && i < n; ++i) {
					const uptr r = At<uptr>(arr, i * 8);
					if (!r) continue;
					++s.read;
					const std::uint32_t id = At<std::uint32_t>(r, off::kFormId);
					if (At<std::uint32_t>(r, 0x20) & 0x10000) ++s.fullLod;
					if ((id >> 24) != 0xFF) continue;
					++s.created;
					{
						const std::uint32_t f = At<std::uint32_t>(r, 0x20);
						if (f & 0x800) ++s.flagDisabled;
						if (f & 0x20) ++s.flagDeleted;
					}
					const uptr base = At<uptr>(r, 0x98);
					if (!base) continue;
					const std::uint32_t b = At<std::uint32_t>(base, off::kFormId);
					for (unsigned k = 0, h = (b * 2654435761u) % kTable; k < kTable; ++k, h = (h + 1) % kTable) {
						if (s.byBase[h].n && s.byBase[h].base != b) continue;
						s.byBase[h].base = b; ++s.byBase[h].n; break;
					}
				}
				s.ok = true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { s.ok = false; }
		}

		void Report(const char* why)
		{
			const uptr player = Player();
			if (!player) return;
			static Scan s;
			s = Scan{};
			ScanCell(player, s);
			if (!s.ok) { logger::Write("Map guard: reading the persistent cell faulted after %u references (%s)", s.read, why); return; }
			if (!s.cell) return;
			const unsigned total = s.total, created = s.created, fullLod = s.fullLod;
			std::vector<Count> top;
			for (const auto& c : s.byBase) if (c.n) top.push_back(c);
			std::sort(top.begin(), top.end(), [](const Count& a, const Count& b) { return a.n > b.n; });
			char list[256] = {};
			int used = 0;
			for (std::size_t k = 0; k < top.size() && k < 6 && used < 200; ++k)
				used += std::snprintf(list + used, sizeof(list) - used, "%s%08X x%u", k ? ", " : "", top[k].base, top[k].n);
			logger::Write("Map guard (%s): persistent cell %u references, %u made at run time (FF ids; %u disabled, %u marked deleted), %u Full LOD; most made at run time by base: %s; "
				"last map build %zu references, longest %zu", why, total, created, s.flagDisabled, s.flagDeleted, fullLod, top.empty() ? "none" : list, g_lastCount.load(), g_worst.load());
		}
	}

	namespace
	{
		// The REV-8's wheel dust (2026-10-29 13:31 run: the persistent cell held 29 049 references, 28 967 made at run time, 22 464
		// of them SFBGS004_RoverWheelDust_Concrete): each puff is a movable static the vehicle places beside itself, in its own cell.
		// FVO's traffic vehicles are persistent (their cell is the world's persistent cell, never unloaded), so their puffs were
		// never cleared: they piled up in the save, slowed the game around New Atlantis and overflowed the local map's model build.
		// Removed here like the console's disable + markfordelete: only run-time (FF) references whose base is one of these six,
		// never within kKeepNear of the player (a puff still playing), at most kDeletesPerTick a frame.
		constexpr std::uint32_t kDustLocal[] = { 0x2DF, 0x317, 0x332, 0x333, 0x334, 0x339 };  // dirt, water, stone, snow, ice, concrete
		constexpr unsigned kScanPerTick = 4000, kDeletesPerTick = 100;
		constexpr double kKeepNear = 30.0;
		constexpr ULONGLONG kPassEveryMs = 20000;
		constexpr unsigned kSetDeleteSlot = 0x33;  // TESForm::SetDelete(bool), as loadedarea.cpp uses it
		std::uint32_t g_dust[6] = {};
		bool g_dustResolved = false;
		unsigned g_cursor = 0;
		bool g_passing = false;
		ULONGLONG g_passAt = 0;
		unsigned g_passDeleted = 0, g_totalDeleted = 0;

		bool IsDust(std::uint32_t base)
		{
			for (const auto d : g_dust) if (d && d == base) return true;
			return false;
		}
		void ResolveDust()
		{
			if (g_dustResolved) return;
			g_dustResolved = true;
			int found = 0;
			for (int k = 0; k < 6; ++k) if ((g_dust[k] = runtimeforms::Resolve(runtimeforms::Key{ "SFBGS004.esm", kDustLocal[k] }))) ++found;
			logger::Write("Map guard: REV-8 wheel dust bases %s (%d of 6); spawn filtering active; manual deletion disabled (native lifetime ownership)", found ? "found" : "NOT found", found);
		}

		// No dust into the persistent cell (2026-10-29 16:27 run, the user: "major stutters"): with 91 persistent traffic vehicles the
		// persistent cell grew 916 -> 102 823 references in 12 minutes (84 864 concrete wheel-dust puffs), ~10 000 a minute, and the
		// engine stalled 5-7 s between frames walking it; marking puffs deleted never takes them out of the cell's list. A puff is now
		// not made at all when it would go into a persistent cell: the game's create-reference (FUN_1405A0520, the PlaceAtMe core;
		// NEW_REFR_DATA base +0x30, cell +0x38, worldspace +0x40) returns the invalid handle for it, as for a failed placement (its normal
		// path returns the handle pointer it was given). Vehicles in ordinary cells, yours included, keep their dust.
		constexpr std::uint8_t kCreatePrologue[15] = { 0x48,0x8B,0xC4, 0x4C,0x89,0x40,0x18, 0x48,0x89,0x50,0x10, 0x48,0x89,0x48,0x08 };
		using Create_t = void* (*)(uptr, void*, uptr);
		Create_t g_createOriginal = nullptr;
		std::atomic<unsigned> g_dustBlocked{ 0 }, g_dustOther{ 0 }, g_dustNoCell{ 0 }, g_dustPersistLoad{ 0 }, g_dustPersistPlay{ 0 }, g_dustTraced{ 0 };
		// Play or a save load, for NoteDustPersist: the last Tick not loading (a load runs on another thread; Tick may stop meanwhile).
		std::atomic<bool> g_loadingNow{ true };
		std::atomic<ULONGLONG> g_playTickAt{ 0 };
		// 0 not dust, 1 dust into a persistent cell, 2 dust into another cell, 3 dust with no cell given. Any thread; plain data only.
		int DustKind(uptr data)
		{
			__try {
				const uptr base = At<uptr>(data, 0x30), cell = At<uptr>(data, 0x38);
				if (!base || !IsDust(At<std::uint32_t>(base, off::kFormId))) return 0;
				if (!cell) return 3;
				uptr ws = At<uptr>(data, 0x40);
				if (!ws) ws = At<uptr>(cell, off::kCellWorldspace);
				return ws && cell == PersistentCell(ws) ? 1 : 2;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}
		// 2026-10-29 17:03 run: the hook stopped ~3 000 puffs in 5 min while the persistent cell still grew by ~24 000 (dust): most
		// puffs get there another way. The first few of each kind are traced (the caller, the creation data's flag bytes) so a run
		// names that way; loadedarea's persistence hook refuses dust being made persistent.
		void TraceDust(int kind, uptr data, void* caller)
		{
			if (g_dustTraced.fetch_add(1, std::memory_order_relaxed) >= 12) return;
			std::uint8_t flags[0x20] = {};
			__try { for (int k = 0; k < 0x20; ++k) flags[k] = At<std::uint8_t>(data, 0x80 + k); } __except (EXCEPTION_EXECUTE_HANDLER) {}
			char hex[0x20 * 3 + 1] = {};
			for (int k = 0; k < 0x20; ++k) std::snprintf(hex + k * 3, 4, "%02X ", flags[k]);
			const uptr c = reinterpret_cast<uptr>(caller);
			logger::Write("Map guard: dust creation %s, caller %llX, creation data +0x80: %s", kind == 1 ? "into the persistent cell (not made)" : kind == 2 ? "into an ordinary cell" : "with no cell",
				static_cast<unsigned long long>(c >= game::Base() ? c - game::Base() + 0x140000000ull : c), hex);
		}
		// The dust pass (2026-10-29, the user: "have them not create any [dust] except for those within a region around me and then do
		// the deleting, acting as a pass"): dust is a look; nobody sees it far away. A puff is made only within g_dustNear (settings.ini
		// [world] dustNear, metres, default 120) of the player, in the player's worldspace; elsewhere it is not made at all. Near the
		// player, a puff headed for the persistent cell (a persistent traffic vehicle's) is made in the player's own exterior cell
		// instead, so it unloads with that area like any other dust. The cleanup pass still removes old puffs from the persistent cell.
		std::atomic<float> g_dustNear{ 120.0f };
		std::atomic<unsigned> g_dustFar{ 0 }, g_dustMoved{ 0 };
		// True when the puff's position is near the player in the player's worldspace; `playerCell` is the player's cell when it is an
		// ordinary exterior cell of that worldspace (else 0). Any thread; plain data only.
		bool NearPlayer(uptr data, uptr& playerCell)
		{
			playerCell = 0;
			__try {
				const uptr player = *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr);
				const uptr pcell = player ? At<uptr>(player, off::kRefParentCell) : 0;
				const uptr pws = pcell ? At<uptr>(pcell, off::kCellWorldspace) : 0;
				const uptr cell = At<uptr>(data, 0x38);
				uptr ws = At<uptr>(data, 0x40);
				if (!ws && cell) ws = At<uptr>(cell, off::kCellWorldspace);
				if (!pws || !ws || pws != ws) return false;  // the player is indoors or in another world: nobody sees it
				const auto pp = At<game::Vec3>(player, off::kRefPosition);
				const double dx = double(At<float>(data, 0x10)) - pp.x, dy = double(At<float>(data, 0x14)) - pp.y;
				const double r = g_dustNear.load(std::memory_order_relaxed);
				if (dx * dx + dy * dy > r * r) return false;
				if (pcell != PersistentCell(pws) && !(At<std::uint8_t>(pcell, off::kCellFlags) & 1)) playerCell = pcell;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		bool SetCell(uptr data, uptr cell)
		{
			__try { *reinterpret_cast<uptr*>(data + 0x38) = cell; return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		void* NotMade(void* handleOut)
		{
			if (handleOut) *reinterpret_cast<std::uint32_t*>(handleOut) = *reinterpret_cast<const std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			return handleOut;
		}
		void* CreateDetour(uptr dataHandler, void* handleOut, uptr data)
		{
			const int kind = data ? DustKind(data) : 0;
			if (kind) {
				TraceDust(kind, data, _ReturnAddress());
				uptr playerCell = 0;
				if (!NearPlayer(data, playerCell)) { g_dustFar.fetch_add(1, std::memory_order_relaxed); return NotMade(handleOut); }
				// Headed for the persistent cell: not made, near you too (moving it into your cell is OFF since the 2026-10-29 17:38 crash,
				// one of the two dust changes new in that run; kMoveDustToYourCell brings it back).
				constexpr bool kMoveDustToYourCell = false;
				if (kind == 1) {
					if (kMoveDustToYourCell && playerCell && SetCell(data, playerCell)) g_dustMoved.fetch_add(1, std::memory_order_relaxed);
					else { g_dustBlocked.fetch_add(1, std::memory_order_relaxed); return NotMade(handleOut); }
				}
				else if (kind == 2) g_dustOther.fetch_add(1, std::memory_order_relaxed);
				else g_dustNoCell.fetch_add(1, std::memory_order_relaxed);
			}
			return g_createOriginal(dataHandler, handleOut, data);
		}

		// One chunk of the pass. Plain data only (a __try function). Returns false when the pass is over.
		float g_vehicles[world::kMaxVehicles][2];
		unsigned g_vehicleCount = 0;
		bool CleanChunk(uptr player, unsigned& deleted)
		{
			__try {
				const uptr cell = At<uptr>(player, off::kRefParentCell);
				const uptr ws = cell ? At<uptr>(cell, off::kCellWorldspace) : 0;
				const uptr pc = PersistentCell(ws);
				if (!pc) return false;
				const unsigned n = At<std::uint32_t>(pc, 0x80);
				const uptr arr = At<uptr>(pc, 0x88);
				if (!arr || g_cursor >= n) return false;
				const auto pp = At<game::Vec3>(player, off::kRefPosition);
				const unsigned stop = std::min(n, g_cursor + kScanPerTick);
				for (; g_cursor < stop && deleted < kDeletesPerTick; ++g_cursor) {
					const uptr r = At<uptr>(arr, g_cursor * 8);
					if (!r) continue;
					const std::uint32_t id = At<std::uint32_t>(r, off::kFormId);
					if ((id >> 24) != 0xFF) continue;
					const std::uint32_t flags = At<std::uint32_t>(r, 0x20);
					if (flags & 0x20) continue;  // already marked deleted
					const uptr base = At<uptr>(r, 0x98);
					if (!base || !IsDust(At<std::uint32_t>(base, off::kFormId))) continue;
					// 2026-10-29 15:00 run: 48 removed while ~29 000 stayed and the count grew: the game disables a puff when its effect
					// ends but never removes it, and this skipped disabled ones. A disabled puff is only marked deleted (no new disable).
					const bool disabled = (flags & 0x800) != 0;
					const auto p = At<game::Vec3>(r, off::kRefPosition);
					const double dx = double(p.x) - pp.x, dy = double(p.y) - pp.y;
					if (dx * dx + dy * dy < kKeepNear * kKeepNear) continue;
					bool busy = false;  // a vehicle's own fresh puffs stay
					for (unsigned k = 0; k < g_vehicleCount && !busy; ++k) {
						const double vx = double(p.x) - g_vehicles[k][0], vy = double(p.y) - g_vehicles[k][1];
						busy = vx * vx + vy * vy < 15.0 * 15.0;
					}
					if (busy) continue;
					if (!disabled) reinterpret_cast<void (*)(uptr, char)>(game::Base() + addr::kDisableReference)(r, 0);
					reinterpret_cast<void (*)(uptr, bool)>((*reinterpret_cast<void***>(r))[kSetDeleteSlot])(r, true);
					++deleted;
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		void CleanDust(ULONGLONG now)
		{
			if (!g_passAt) { g_passAt = now; return; }  // the first pass 20 s in (the world settles first)
			if (!g_dust[5] && !g_dust[0]) return;
			const uptr player = Player();
			if (!player) return;
			if (!g_passing) {
				if (g_passAt && now - g_passAt < kPassEveryMs) return;
				g_passing = true; g_cursor = 0; g_passDeleted = 0; g_passAt = now;
			}
			{
				static thread_local world::Snapshot snap;
				g_vehicleCount = 0;
				if (world::Vehicles(snap))
					for (std::uint32_t k = 0; k < snap.count && g_vehicleCount < world::kMaxVehicles; ++k) {
						g_vehicles[g_vehicleCount][0] = float(snap.v[k].pos[0]); g_vehicles[g_vehicleCount][1] = float(snap.v[k].pos[1]); ++g_vehicleCount;
					}
			}
			unsigned deleted = 0;
			const bool more = CleanChunk(player, deleted);
			g_passDeleted += deleted; g_totalDeleted += deleted;
			if (!more) {
				g_passing = false;
				if (g_passDeleted) logger::Write("Map guard: removed %u REV-8 wheel dust puff(s) from the persistent cell (%u this session; %u not made there)", g_passDeleted, g_totalDeleted, g_dustBlocked.load());
			}
		}
	}

	bool IsDustBase(std::uintptr_t base)
	{
		if (!base) return false;
		__try { return IsDust(At<std::uint32_t>(base, off::kFormId)); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	bool NoteDustPersist()
	{
		const bool play = !g_loadingNow.load(std::memory_order_relaxed) && GetTickCount64() - g_playTickAt.load(std::memory_order_relaxed) < 1000;
		if (!play) { g_dustPersistLoad.fetch_add(1, std::memory_order_relaxed); return false; }
		return g_dustPersistPlay.fetch_add(1, std::memory_order_relaxed) < 6;
	}

	bool Install()
	{
		auto** p = reinterpret_cast<void**>(game::Base() + kTaskVtbl) + kRunSlot;
		if (reinterpret_cast<uptr>(*p) != game::Base() + kRunRva) {
			logger::Write("Map guard: the local map's model build task is not the expected function; guard off");
			return false;
		}
		DWORD old = 0;
		if (!VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &old)) return false;
		g_original = reinterpret_cast<Run_t>(*p);
		*p = reinterpret_cast<void*>(&RunDetour);
		VirtualProtect(p, sizeof(void*), old, &old);
		logger::Write("Map guard: installed (local map model builds capped at %zu references)", kMaxModels);
		void* original = nullptr;
		if (hook::Install(game::Base() + addr::kCreateReference, kCreatePrologue, sizeof(kCreatePrologue), reinterpret_cast<void*>(&CreateDetour), &original)) {
			g_createOriginal = reinterpret_cast<Create_t>(original);
			logger::Write("Map guard: wheel dust is not made in the persistent cell (create-reference hook at 1405A0520)");
		}
		else logger::Write("Map guard: create-reference hook NOT installed (bytes differ); wheel dust still piles up in the persistent cell");
		return true;
	}

	void Tick(bool loading)
	{
		static ULONGLONG at = 0;
		const ULONGLONG now = GetTickCount64();
		g_loadingNow.store(loading, std::memory_order_relaxed);
		if (loading) { g_passing = false; g_passAt = now; return; }  // never while a save loads
		g_playTickAt.store(now, std::memory_order_relaxed);
		ResolveDust();  // at once: the create-reference hook needs the dust bases from the first frame on
		{
			static fvopaths::Watch watch;  // settings.ini [world] dustNear: the dust pass radius (metres)
			if (watch.Changed(fvopaths::Settings())) {
				wchar_t text[32]{};
				GetPrivateProfileStringW(L"world", L"dustNear", L"120", text, 32, fvopaths::Settings().c_str());
				const double v = _wtof(text);
				g_dustNear.store(float(std::isfinite(v) && v >= 0 && v <= 2000 ? v : 120.0), std::memory_order_relaxed);
			}
		}
		// Two crashes followed manual disable/SetDelete of saved dust: an
		// allocator fault and then a stale Havok owner. Preserve native lifetime
		// ownership until a queued teardown adapter is verified. Spawn filtering
		// remains active, so new distant dust does not refill the persistent cell.
		const bool asked = g_reportNow.exchange(false);
		if (!asked && at && now - at < 60000) return;
		if (!at) { at = now; return; }  // the first report a minute in (after the load settles)
		at = now;
		Report(asked ? "long map build" : "every 60 s");
		logger::Write("Map guard: wheel dust this session (pass %.0f m): %u not made far from you, %u near you moved from the persistent cell to yours, "
			"%u not made (persistent, no cell of yours to use), %u made in ordinary cells, %u with no cell given; made persistent: %u by the save load, %u during play",
			double(g_dustNear.load()), g_dustFar.load(), g_dustMoved.load(), g_dustBlocked.load(), g_dustOther.load(), g_dustNoCell.load(), g_dustPersistLoad.load(), g_dustPersistPlay.load());
	}
}
