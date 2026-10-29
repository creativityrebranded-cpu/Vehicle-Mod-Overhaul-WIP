#include "loadedarea.h"

#include "features.h"
#include "addresses.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "npcvehicles.h"
#include "vehiclediag.h"
#include "vehicleflight.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace loadedarea
{
	namespace
	{
		using uptr = std::uintptr_t;
		using Update_t = void (*)(uptr bound, uptr cell, uptr arg);
		using MoveBack_t = std::uint64_t (*)(uptr bound, uptr world, std::uint32_t body, uptr list);
		using FindRef_t = uptr (*)(uptr object3D, bool searchParents);
		using FilterPairs_t = std::uint32_t (*)(uptr filter, uptr world, std::uint32_t* pairs, std::uint32_t count);
		using SetDelete_t = void (*)(uptr form, bool deleted);
		using Lookup_t = uptr (*)(std::uint32_t formId);
		using Persist_t = std::uint64_t (*)(uptr ref, char persistent, char arg);

		constexpr uptr kUpdate = 0x00D50650;      // LoadedAreaBound update
		constexpr uptr kUpdateCall = 0x00578544;  // its only call (FUN_140578350)
		constexpr uptr kMoveBack = 0x00D500B0;    // move a body back inside the box
		constexpr uptr kMoveBackCalls[] = { 0x00D5114C, 0x00D51841, 0x00D52F7F };  // touches, broad-phase set, after rebuild
		constexpr uptr kFindRef = 0x00B3CF40;     // reference owning a 3D object
		constexpr uptr kFilterSlot = 0x04D64960;  // bhkCollisionFilter vtable 0x144D64948, slot 3
		constexpr uptr kFilterPairs = 0x02399100; // its body-pair filter
		constexpr uptr kLookup = 0x005DE3B0;      // form by id
		constexpr unsigned kSetDeleteSlot = 0x33; // TESForm::SetDelete(bool)
		constexpr uptr kPersist = 0x00B36C40;     // (ref, persistent, ...): sets/clears form flag 0x400, moves the ref to/from the persistent cell
		constexpr std::uint8_t kPersistPrologue[19] = { 0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x01, 0x00, 0x00 };
		constexpr std::uint32_t kPersistentFlag = 0x400;
		constexpr uptr kSetRefPosition = 0x00B4E410;  // TESObjectREFR position setter (ref, float pos[3])

		// LoadedAreaBound layout
		constexpr uptr kWalls = 0x10;        // 6 wall objects; 0/1 floor and ceiling, 2..5 the sides
		constexpr uptr kTouchCount = 0x68;   // int
		constexpr uptr kTouches = 0x70;      // pairs { moving body id, wall body id }
		constexpr uptr kWorld = 0xC0;        // hknpWorld*
		// hknpWorld bodies: array +0x38, count +0x40, 0xB0 each: position +0x30, id +0x50, user object +0x98
		constexpr uptr kBodies = 0x38, kBodyCount = 0x40, kBodySize = 0xB0, kBodyPos = 0x30, kBodyId = 0x50, kBodyUser = 0x98;
		constexpr uptr kRefFormId = 0x28;
		constexpr std::uint32_t kNoBody = 0xFFFFFFFF;

		constexpr int kMaxIds = 16, kMaxBodies = 256;  // a driver brings ~18 skeleton bodies
		std::atomic<std::uint32_t> g_ids[kMaxIds];
		std::atomic<int> g_idCount{ 0 };
		std::atomic<std::uint32_t> g_bodies[kMaxBodies];
		std::atomic<int> g_bodyCount{ 0 };
		std::atomic<std::uint32_t> g_walls[6];
		std::atomic<uptr> g_world{ 0 }, g_bound{ 0 };
		std::atomic<unsigned> g_persistBlocked{ 0 }, g_persistRestored{ 0 }, g_synced{ 0 };
		std::atomic<unsigned> g_touches{ 0 }, g_moveBacks{ 0 }, g_pairs{ 0 }, g_rebuilds{ 0 };
		std::atomic<std::uint32_t> g_lastId{ 0 };

		Update_t g_update = nullptr;
		MoveBack_t g_moveBack = nullptr;
		FindRef_t g_findRef = nullptr;
		FilterPairs_t g_filterPairs = nullptr;
		SetDelete_t g_setDelete = nullptr;
		Lookup_t g_lookup = nullptr;
		Persist_t g_persist = nullptr;

		template <class T> T At(uptr p, uptr off) { return *reinterpret_cast<T*>(p + off); }

		bool Listed(std::uint32_t id)
		{
			const int n = g_idCount.load(std::memory_order_acquire);
			for (int i = 0; i < n; ++i) if (g_ids[i].load(std::memory_order_relaxed) == id) return true;
			return false;
		}
		bool InBodySet(std::uint32_t body)
		{
			const int n = g_bodyCount.load(std::memory_order_acquire);
			for (int i = 0; i < n; ++i) if (g_bodies[i].load(std::memory_order_relaxed) == body) return true;
			return false;
		}
		bool IsSide(std::uint32_t body)
		{
			if (body == kNoBody) return false;
			for (int i = 2; i < 6; ++i) if (g_walls[i].load(std::memory_order_relaxed) == body) return true;
			return false;
		}

		uptr BodyAt(uptr world, std::uint32_t body)
		{
			const std::uint32_t index = body & 0xFFFFFF;
			if (!world || index == 0xFFFFFF || index >= At<std::uint32_t>(world, kBodyCount)) return 0;
			const uptr b = At<uptr>(world, kBodies) + uptr(index) * kBodySize;
			return At<std::uint32_t>(b, kBodyId) == body ? b : 0;
		}

		// The form id of the reference owning a body, 0 when none. Main thread (the game's own LoadedAreaBound code uses
		// the same walk there). Caller guards faults.
		std::uint32_t OwnerOf(uptr world, std::uint32_t body)
		{
			const uptr b = BodyAt(world, body);
			const uptr user = b ? At<uptr>(b, kBodyUser) : 0;
			if (!user) return 0;
			const uptr collision = reinterpret_cast<uptr (*)(uptr)>(At<uptr>(At<uptr>(user, 0), 0x138))(user);
			if (!collision) return 0;
			const uptr object3D = At<uptr>(collision, 0x10);
			if (!object3D) return 0;
			const uptr ref = g_findRef(object3D, true);
			return ref ? At<std::uint32_t>(ref, kRefFormId) : 0;
		}

		bool Passes(uptr world, std::uint32_t body)
		{
			if (!g_idCount.load(std::memory_order_acquire)) return false;
			if (InBodySet(body)) return true;
			const std::uint32_t owner = OwnerOf(world, body);
			if (!owner || !Listed(owner)) return false;
			g_lastId.store(owner, std::memory_order_relaxed);
			return true;
		}

		std::uint32_t WallBody(uptr wall)
		{
			const uptr system = wall ? At<uptr>(wall, 0x20) : 0;
			if (!system) return kNoBody;
			const std::uint32_t index = At<std::uint32_t>(wall, 0x28);
			const uptr data = At<uptr>(system, 0x20);
			if (!data || index >= At<std::uint32_t>(data, 0x38)) return kNoBody;
			return At<std::uint32_t>(At<uptr>(data, 0x30), uptr(index) * 4);
		}

		void PublishWalls(uptr bound)
		{
			bool changed = false;
			for (int i = 0; i < 6; ++i) {
				std::uint32_t id = kNoBody;
				__try { id = WallBody(At<uptr>(bound, kWalls + uptr(i) * 8)); } __except (EXCEPTION_EXECUTE_HANDLER) { id = kNoBody; }
				if (g_walls[i].exchange(id, std::memory_order_relaxed) != id) changed = true;
			}
			if (changed) g_rebuilds.fetch_add(1, std::memory_order_relaxed);
			__try { g_world.store(At<uptr>(bound, kWorld), std::memory_order_relaxed); } __except (EXCEPTION_EXECUTE_HANDLER) {}
			g_bound.store(bound, std::memory_order_relaxed);
		}

		void FilterTouches(uptr bound)
		{
			const int count = At<int>(bound, kTouchCount);
			auto* pairs = At<std::uint32_t*>(bound, kTouches);
			const uptr world = At<uptr>(bound, kWorld);
			if (count <= 0 || !pairs || !world) return;
			int kept = 0;
			for (int i = 0; i < count; ++i) {
				const std::uint32_t mover = pairs[i * 2], wall = pairs[i * 2 + 1];
				if (IsSide(wall) && Passes(world, mover)) { g_touches.fetch_add(1, std::memory_order_relaxed); continue; }
				pairs[kept * 2] = mover; pairs[kept * 2 + 1] = wall; ++kept;
			}
			if (kept != count) *reinterpret_cast<int*>(bound + kTouchCount) = kept;
		}

		void Update(uptr bound, uptr cell, uptr arg)
		{
			const bool on = bound && features::On(features::Id::WallPass);
			if (on) {
				PublishWalls(bound);  // the walls the touches were recorded against
				__try { FilterTouches(bound); } __except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			g_update(bound, cell, arg);
			if (on) PublishWalls(bound);  // a rebuild inside the update made new walls: the pair filter needs them before the next step
		}

		std::uint64_t MoveBack(uptr bound, uptr world, std::uint32_t body, uptr list)
		{
			if (features::On(features::Id::WallPass)) {
				bool pass = false;
				__try { pass = Passes(world, body); } __except (EXCEPTION_EXECUTE_HANDLER) { pass = false; }
				if (pass) { g_moveBacks.fetch_add(1, std::memory_order_relaxed); return 0; }
			}
			return g_moveBack(bound, world, body, list);
		}

		// Physics threads: id comparisons only.
		std::uint32_t FilterPairs(uptr filter, uptr world, std::uint32_t* pairs, std::uint32_t count)
		{
			const std::uint32_t n = g_filterPairs(filter, world, pairs, count);
			if (!n || !pairs || !g_bodyCount.load(std::memory_order_acquire) || !features::On(features::Id::WallPass)) return n;
			std::uint32_t kept = 0;
			for (std::uint32_t i = 0; i < n; ++i) {
				const std::uint32_t a = pairs[i * 2], b = pairs[i * 2 + 1];
				if ((IsSide(a) && InBodySet(b)) || (IsSide(b) && InBodySet(a))) { g_pairs.fetch_add(1, std::memory_order_relaxed); continue; }
				pairs[kept * 2] = a; pairs[kept * 2 + 1] = b; ++kept;
			}
			return kept;
		}

		// --- body set (main thread) ---
		void RefreshBodies()
		{
			const uptr world = g_world.load(std::memory_order_relaxed);
			std::uint32_t found[kMaxBodies];
			int n = 0;
			auto add = [&](std::uint32_t body) {
				if (body == kNoBody || !body) return;
				for (int i = 0; i < n; ++i) if (found[i] == body) return;
				if (n < kMaxBodies) found[n++] = body;
			};
			const int ids = g_idCount.load(std::memory_order_acquire);
			float centres[kMaxIds][3]; int nc = 0;
			for (int i = 0; i < ids; ++i) {
				const std::uint32_t id = g_ids[i].load(std::memory_order_relaxed);
				const uptr component = vehiclediag::ComponentOf(id);
				if (!component) continue;  // a driver, or a vehicle without a component
				__try {
					const uptr instance = At<uptr>(component, 0x10);
					const std::uint32_t chassis = instance ? At<std::uint32_t>(instance, 0x20) : kNoBody;
					const uptr b = BodyAt(world, chassis);
					if (!b) continue;
					add(chassis);
					const float* p = reinterpret_cast<const float*>(b + kBodyPos);
					if (nc < kMaxIds && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
						centres[nc][0] = p[0]; centres[nc][1] = p[1]; centres[nc][2] = p[2]; ++nc;
					}
				} __except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			if (world && nc) {
				__try {
					const std::uint32_t count = At<std::uint32_t>(world, kBodyCount);
					const uptr array = At<uptr>(world, kBodies);
					for (std::uint32_t i = 0; i < count && n < kMaxBodies; ++i) {
						const uptr b = array + uptr(i) * kBodySize;
						const std::uint32_t id = At<std::uint32_t>(b, kBodyId);
						if ((id & 0xFFFFFF) != i || !At<uptr>(b, kBodyUser)) continue;
						const float* p = reinterpret_cast<const float*>(b + kBodyPos);
						bool close = false;
						for (int c = 0; c < nc && !close; ++c) {
							const float dx = p[0] - centres[c][0], dy = p[1] - centres[c][1], dz = p[2] - centres[c][2];
							close = dx * dx + dy * dy + dz * dz < 144.0f;
						}
						if (!close) continue;
						std::uint32_t owner = 0;
						__try { owner = OwnerOf(world, id); } __except (EXCEPTION_EXECUTE_HANDLER) { owner = 0; }
						if (owner && Listed(owner)) add(id);
					}
				} __except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			for (int i = 0; i < n; ++i) g_bodies[i].store(found[i], std::memory_order_relaxed);
			g_bodyCount.store(n, std::memory_order_release);
		}

		// --- driver-loss trace ---
		struct Watched { float pos[3]{}; std::uint32_t flags = 0; std::uint8_t cellState = 0; ULONGLONG seen = 0; bool alive = false; };
		std::unordered_map<std::uint32_t, Watched> g_watch;   // main thread
		std::atomic<std::uint32_t> g_watchIds[kMaxIds];       // read by the SetDelete hook
		std::atomic<int> g_watchCount{ 0 };
		bool g_deleteHooked = false;

		void SetDelete(uptr form, bool deleted)
		{
			if (deleted) {
				__try {
					const std::uint32_t id = At<std::uint32_t>(form, kRefFormId);
					const int n = g_watchCount.load(std::memory_order_acquire);
					for (int i = 0; i < n; ++i) if (g_watchIds[i].load(std::memory_order_relaxed) == id) {
						void* frames[8] = {};
						const USHORT got = RtlCaptureStackBackTrace(1, 8, frames, nullptr);
						char text[200] = {}; int at = 0;
						for (USHORT f = 0; f < got && at < 180; ++f) {
							const uptr a = reinterpret_cast<uptr>(frames[f]);
							at += std::snprintf(text + at, sizeof(text) - at, " %llX", static_cast<unsigned long long>(a >= game::Base() ? a - game::Base() + 0x140000000ull : a));
						}
						logger::Write("Loaded-area watch: driver %08X SetDelete(true) on thread %lu; callers%s", id, GetCurrentThreadId(), text);
						break;
					}
				} __except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			g_setDelete(form, deleted);
		}

		void HookSetDelete(uptr actor)
		{
			if (g_deleteHooked || !actor) return;
			g_deleteHooked = true;  // one try
			__try {
				auto** slot = reinterpret_cast<void**>(At<uptr>(actor, 0)) + kSetDeleteSlot;
				DWORD old = 0;
				if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
				g_setDelete = reinterpret_cast<SetDelete_t>(*slot);
				*slot = reinterpret_cast<void*>(&SetDelete);
				VirtualProtect(slot, sizeof(void*), old, &old);
				logger::Write("Loaded-area watch: SetDelete traced on the NPC vtable (slot 0x33 was %llX)",
					static_cast<unsigned long long>(reinterpret_cast<uptr>(g_setDelete) - game::Base() + 0x140000000ull));
			} __except (EXCEPTION_EXECUTE_HANDLER) { logger::Write("Loaded-area watch: SetDelete trace failed"); }
		}

		void WatchDrivers(const std::uint32_t* drivers, int count, ULONGLONG now)
		{
			for (int i = 0; i < count; ++i) if (drivers[i]) g_watch[drivers[i]].seen = now;
			int published = 0;
			for (auto it = g_watch.begin(); it != g_watch.end();) {
				const std::uint32_t id = it->first;
				Watched& w = it->second;
				uptr actor = 0;
				__try { actor = g_lookup(id); if (actor && At<std::uint32_t>(actor, kRefFormId) != id) actor = 0; } __except (EXCEPTION_EXECUTE_HANDLER) { actor = 0; }
				if (actor) {
					__try {
						const float* p = reinterpret_cast<const float*>(actor + off::kRefPosition);
						w.pos[0] = p[0]; w.pos[1] = p[1]; w.pos[2] = p[2];
						w.flags = At<std::uint32_t>(actor, 0x20);  // TESForm flags
						const uptr cell = At<uptr>(actor, off::kRefParentCell);
						w.cellState = cell ? At<std::uint8_t>(cell, off::kCellState) : 0xFF;
					} __except (EXCEPTION_EXECUTE_HANDLER) {}
					w.alive = true;
					HookSetDelete(actor);
				} else if (w.alive) {
					w.alive = false;
					logger::Write("Loaded-area watch: driver %08X no longer resolves (last at %.0f, %.0f, %.0f; form flags %08X; parent cell state %u; side walls %08X %08X %08X %08X; %u box rebuild(s) so far)",
						id, w.pos[0], w.pos[1], w.pos[2], w.flags, w.cellState, g_walls[2].load(), g_walls[3].load(), g_walls[4].load(), g_walls[5].load(), g_rebuilds.load());
				}
				if (now - w.seen > 60000 && !w.alive) { it = g_watch.erase(it); continue; }
				if (published < kMaxIds) g_watchIds[published++].store(id, std::memory_order_relaxed);
				++it;
			}
			g_watchCount.store(published, std::memory_order_release);
		}

		bool Watched(std::uint32_t id)
		{
			const int n = g_watchCount.load(std::memory_order_acquire);
			for (int i = 0; i < n; ++i) if (g_watchIds[i].load(std::memory_order_relaxed) == id) return true;
			return false;
		}

		void Callers(char* text, std::size_t size)
		{
			void* frames[8] = {};
			const USHORT got = RtlCaptureStackBackTrace(2, 8, frames, nullptr);
			int at = 0;
			for (USHORT f = 0; f < got && at + 20 < int(size); ++f) {
				const uptr a = reinterpret_cast<uptr>(frames[f]);
				at += std::snprintf(text + at, size - at, " %llX", static_cast<unsigned long long>(a >= game::Base() ? a - game::Base() + 0x140000000ull : a));
			}
		}

		// The game un-persisting a flown vehicle or its driver (20:40 run: the drivers' flag 0x400 was gone, and they were
		// unloaded with the unloaded cell they flew into). Blocked for FVO's refs; the caller is logged so a run names it.
		std::uint64_t Persist(uptr ref, char persistent, char arg)
		{
			if (!persistent && ref && features::On(features::Id::WallPass)) {
				std::uint32_t id = 0;
				__try { id = At<std::uint32_t>(ref, kRefFormId); } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
				if (id && (Listed(id) || Watched(id))) {
					if (g_persistBlocked.fetch_add(1, std::memory_order_relaxed) < 40) {
						char text[200] = {};
						Callers(text, sizeof(text));
						logger::Write("Loaded-area watch: kept %08X persistent: the game tried to clear it (thread %lu); callers%s", id, GetCurrentThreadId(), text);
					}
					return 0;
				}
			}
			return g_persist(ref, persistent, arg);
		}

		// Readings for flown vehicles every 5 s: where the physics body is against the reference, how fast the body really
		// moves, and the box.
		struct Track { float pos[3]{}; ULONGLONG at = 0; };
		std::unordered_map<std::uint32_t, Track> g_track;

		void LogBody(std::uint32_t id, uptr world, const float* box, ULONGLONG now)
		{
			const uptr component = vehiclediag::ComponentOf(id);
			if (!component) return;
			__try {
				const uptr instance = At<uptr>(component, 0x10);
				const std::uint32_t chassis = instance ? At<std::uint32_t>(instance, 0x20) : kNoBody;
				const uptr b = BodyAt(world, chassis);
				uptr ref = g_lookup(id);
				if (ref && At<std::uint32_t>(ref, kRefFormId) != id) ref = 0;
				const float* rp = ref ? reinterpret_cast<const float*>(ref + off::kRefPosition) : nullptr;
				const uptr cell = ref ? At<uptr>(ref, off::kRefParentCell) : 0;
				const std::uint32_t refFlags = ref ? At<std::uint32_t>(ref, 0x20) : 0u;
				const int cellState = cell ? int(At<std::uint8_t>(cell, off::kCellState)) : -1;
				if (!b) {
					logger::Write("Loaded-area body: %08X chassis %08X not in the physics world (ref at %.0f, %.0f, %.0f; ref flags %08X; parent cell state %d)", id, chassis,
						rp ? rp[0] : 0.f, rp ? rp[1] : 0.f, rp ? rp[2] : 0.f, refFlags, cellState);
					return;
				}
				const float* p = reinterpret_cast<const float*>(b + kBodyPos);
				float node[3] = { 0, 0, 0 };
				if (ref) if (const uptr loaded = At<uptr>(ref, off::kRefLoadedData)) if (const uptr root = At<uptr>(loaded, off::kLoadedData3D)) {
					const float* w = reinterpret_cast<const float*>(root + off::kNodeWorldT); node[0] = w[0]; node[1] = w[1]; node[2] = w[2]; }
				Track& t = g_track[id];
				double speed = -1;
				if (t.at && now > t.at) {
					const double dx = p[0] - t.pos[0], dy = p[1] - t.pos[1], dz = p[2] - t.pos[2];
					speed = std::sqrt(dx * dx + dy * dy + dz * dz) / ((now - t.at) / 1000.0);
				}
				t.pos[0] = p[0]; t.pos[1] = p[1]; t.pos[2] = p[2]; t.at = now;
				const bool inside = p[0] >= box[0] && p[0] <= box[3] && p[1] >= box[1] && p[1] <= box[4];
				logger::Write("Loaded-area body: %08X chassis %08X at (%.1f, %.1f, %.1f) moving %.1f m/s, body flags %02X, %s the box (%.0f..%.0f, %.0f..%.0f, z %.0f..%.0f); ref at (%.1f, %.1f, %.1f) flags %08X, parent cell state %d; model at (%.1f, %.1f, %.1f)",
					id, chassis, p[0], p[1], p[2], speed, unsigned(At<std::uint8_t>(b, 0x54)), inside ? "inside" : "OUTSIDE", box[0], box[3], box[1], box[4], box[2], box[5],
					rp ? rp[0] : 0.f, rp ? rp[1] : 0.f, rp ? rp[2] : 0.f, refFlags, cellState, node[0], node[1], node[2]);
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		void LogBodies(ULONGLONG now)
		{
			const uptr world = g_world.load(std::memory_order_relaxed), bound = g_bound.load(std::memory_order_relaxed);
			float box[6] = {};
			__try { if (bound) for (int i = 0; i < 3; ++i) { box[i] = At<float>(bound, 0x40 + i * 4); box[3 + i] = At<float>(bound, 0x50 + i * 4); } }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			const int ids = g_idCount.load(std::memory_order_acquire);
			for (int i = 0; i < ids; ++i) LogBody(g_ids[i].load(std::memory_order_relaxed), world, box, now);
		}

		// A flown vehicle or driver found without the persistent flag gets it back (the same call the pool makes at creation).
		std::uint32_t FlagsOf(std::uint32_t id, uptr& ref)
		{
			ref = 0;
			__try {
				ref = g_lookup(id);
				if (ref && At<std::uint32_t>(ref, kRefFormId) != id) ref = 0;
				return ref ? At<std::uint32_t>(ref, 0x20) : 0;
			} __except (EXCEPTION_EXECUTE_HANDLER) { ref = 0; return 0; }
		}
		bool SetPersistent(uptr ref)
		{
			__try { g_persist(ref, 1, 0); return (At<std::uint32_t>(ref, 0x20) & kPersistentFlag) != 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		std::unordered_map<std::uint32_t, ULONGLONG> g_lastFix;
		void KeepPersistent(ULONGLONG now)
		{
			const int ids = g_idCount.load(std::memory_order_acquire);
			for (int i = 0; i < ids; ++i) {
				const std::uint32_t id = g_ids[i].load(std::memory_order_relaxed);
				uptr ref = 0;
				const std::uint32_t flags = FlagsOf(id, ref);
				if (!ref || (flags & kPersistentFlag)) continue;
				ULONGLONG& at = g_lastFix[id];
				if (now - at < 2000) continue;
				at = now;
				const bool ok = SetPersistent(ref);
				g_persistRestored.fetch_add(1, std::memory_order_relaxed);
				logger::Write("Loaded-area watch: %08X had lost its persistent flag (flags %08X); %s", id, flags, ok ? "set again" : "could NOT set it again");
			}
		}

		// Past the loaded cells the game stops copying a vehicle's physics body to its reference and model (21:06 run: bodies
		// flew on at 10-15 m/s while the references stayed on y = 1400.x, the last spot inside; FVO's pilot then read "stuck").
		// For flown vehicles whose reference trails the body by over 1 m, FVO writes the body position to the reference (the
		// game's own setter) and shifts the model's root node by the same amount. Inside the loaded area the game keeps them
		// within 0.2 m, so this does nothing there. Rotation is left to the game.
		std::unordered_map<std::uint32_t, ULONGLONG> g_syncLogged;
		void SyncOne(std::uint32_t id, uptr world, ULONGLONG now)
		{
			const uptr component = vehiclediag::ComponentOf(id);
			if (!component) return;
			__try {
				const uptr instance = At<uptr>(component, 0x10);
				const uptr b = BodyAt(world, instance ? At<std::uint32_t>(instance, 0x20) : kNoBody);
				if (!b) return;
				uptr ref = g_lookup(id);
				if (!ref || At<std::uint32_t>(ref, kRefFormId) != id) return;
				const float* p = reinterpret_cast<const float*>(b + kBodyPos);
				float* rp = reinterpret_cast<float*>(ref + off::kRefPosition);
				if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return;
				const float dx = p[0] - rp[0], dy = p[1] - rp[1], dz = p[2] - rp[2];
				if (dx * dx + dy * dy + dz * dz < 1.0f) return;
				float target[3] = { p[0], p[1], p[2] };
				reinterpret_cast<void (*)(uptr, float*)>(game::Base() + kSetRefPosition)(ref, target);
				if (const uptr loaded = At<uptr>(ref, off::kRefLoadedData)) if (const uptr root = At<uptr>(loaded, off::kLoadedData3D)) {
					float* w = reinterpret_cast<float*>(root + off::kNodeWorldT);
					float* l = reinterpret_cast<float*>(root + off::kNodeLocalT);
					const float mx = p[0] - w[0], my = p[1] - w[1], mz = p[2] - w[2];
					l[0] += mx; l[1] += my; l[2] += mz;
					w[0] = p[0]; w[1] = p[1]; w[2] = p[2];
				}
				g_synced.fetch_add(1, std::memory_order_relaxed);
				ULONGLONG& at = g_syncLogged[id];
				if (now - at > 10000) {
					at = now;
					logger::Write("Loaded-area sync: %08X reference was %.1f m behind its physics body; moved to (%.1f, %.1f, %.1f)", id, std::sqrt(dx * dx + dy * dy + dz * dz), p[0], p[1], p[2]);
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		void SyncHeld(ULONGLONG now)
		{
			const uptr world = g_world.load(std::memory_order_relaxed);
			if (!world) return;
			const int ids = g_idCount.load(std::memory_order_acquire);
			for (int i = 0; i < ids; ++i) SyncOne(g_ids[i].load(std::memory_order_relaxed), world, now);
		}
	}

	bool IsWall(std::uint32_t bodyId)
	{
		if (bodyId == kNoBody) return false;
		for (int i = 0; i < 6; ++i) if (g_walls[i].load(std::memory_order_relaxed) == bodyId) return true;
		return false;
	}

	bool Install()
	{
		for (auto& w : g_walls) w.store(kNoBody);
		g_update = reinterpret_cast<Update_t>(game::Base() + kUpdate);
		g_moveBack = reinterpret_cast<MoveBack_t>(game::Base() + kMoveBack);
		g_findRef = reinterpret_cast<FindRef_t>(game::Base() + kFindRef);
		g_lookup = reinterpret_cast<Lookup_t>(game::Base() + kLookup);
		const bool update = hook::RedirectCall(game::Base() + kUpdateCall, reinterpret_cast<void*>(g_update), reinterpret_cast<void*>(&Update));
		int backs = 0;
		for (const uptr site : kMoveBackCalls)
			if (hook::RedirectCall(game::Base() + site, reinterpret_cast<void*>(g_moveBack), reinterpret_cast<void*>(&MoveBack))) ++backs;
		bool filter = false;
		__try {
			auto** slot = reinterpret_cast<void**>(game::Base() + kFilterSlot);
			if (reinterpret_cast<uptr>(*slot) == game::Base() + kFilterPairs) {
				DWORD old = 0;
				if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
					g_filterPairs = reinterpret_cast<FilterPairs_t>(*slot);
					*slot = reinterpret_cast<void*>(&FilterPairs);
					VirtualProtect(slot, sizeof(void*), old, &old);
					filter = true;
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) { filter = false; }
		void* persistOriginal = nullptr;
		const bool persist = hook::Install(game::Base() + kPersist, kPersistPrologue, sizeof(kPersistPrologue), reinterpret_cast<void*>(&Persist), &persistOriginal);
		g_persist = persist ? reinterpret_cast<Persist_t>(persistOriginal) : reinterpret_cast<Persist_t>(game::Base() + kPersist);
		logger::Write("Loaded-area watch: persistence guard %s", persist ? "on" : "NOT installed (bytes differ)");
		const bool all = update && backs == 3 && filter;
		logger::Write("Loaded-area wall: FVO pass %s (collision filter %s, touch filter %s, move-back %d/3; [physics] fvoWallPass, now %s)",
			all ? "installed" : "PARTLY installed", filter ? "on" : "MISSING (vehicles still hit the walls)", update ? "on" : "MISSING", backs,
			features::On(features::Id::WallPass) ? "on" : "off");
		return all;
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		std::uint32_t vehicles[kMaxIds], drivers[kMaxIds];
		const int jobs = npcvehicles::FlownVehicles(vehicles, drivers, kMaxIds / 2 - 1);
		int n = 0;
		for (int i = 0; i < jobs; ++i) {
			g_ids[n++].store(vehicles[i], std::memory_order_relaxed);
			if (drivers[i]) g_ids[n++].store(drivers[i], std::memory_order_relaxed);
		}
		// The player's own vehicle while FVO's autopilot flies it (summons, follow, Auto); never the player.
		if (const std::uint32_t own = vehicleflight::OwnRoverId(); own && vehicleflight::PilotActive(0) && n < kMaxIds)
			g_ids[n++].store(own, std::memory_order_relaxed);
		g_idCount.store(n, std::memory_order_release);

		static ULONGLONG bodiesAt = 0;
		if (now - bodiesAt >= 250) { bodiesAt = now; RefreshBodies(); }
		WatchDrivers(drivers, jobs, now);
		if (features::On(features::Id::WallPass)) KeepPersistent(now);
		if (features::On(features::Id::WallPass)) SyncHeld(now);
		static ULONGLONG bodyLogAt = 0;
		if (now - bodyLogAt >= 5000) { bodyLogAt = now; LogBodies(now); }

		static ULONGLONG logAt = 0;
		static unsigned lt = 0, lb = 0, lp = 0, lr = 0, lk = 0;
		const unsigned t = g_touches.load(), b = g_moveBacks.load(), p = g_pairs.load(), r = g_rebuilds.load(), k = g_persistBlocked.load() + g_persistRestored.load() + g_synced.load() / 50;
		if ((t != lt || b != lb || p != lp || r != lr || k != lk) && now - logAt >= 5000) {
			logAt = now;
			logger::Write("Loaded-area wall: FVO pass so far: %u wall collision pair(s) dropped, %u side-wall touch(es), %u move-back(s) skipped; %u box rebuild(s); %u un-persist(s) blocked, %u persistent flag(s) restored; %u position sync(s) past the loaded cells; %d id(s), %d bod(ies) listed",
				p, t, b, r, g_persistBlocked.load(), g_persistRestored.load(), g_synced.load(), n, g_bodyCount.load());
			lt = t; lb = b; lp = p; lr = r; lk = k;
		}
	}
}
