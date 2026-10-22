#include "realvehicles.h"
#include "features.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "npcvehicles.h"
#include "vehicleflight.h"
#include "placement_spots.h"
#include "own_spots.h"
#include "placementlogic.h"
#include "notify.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdio>

// Engine pieces (1.16.244, from the decompile):
//  - Reference: OBJ_REFR at +0x80 (rotation +0x80, position +0x8C, base object +0x98), parent cell +0xB0, extra data
//    +0xC8, form flags +0x20 (0x800 disabled), form id +0x28, form type +0x2E.
//  - CreateReference FUN_1405A0520(dataHandler, handle out, NEW_REFR_DATA*) (the PlaceAtMe core; NEW_REFR_DATA as the
//    ship beacon in flight.cpp used it); handle -> ref through the handle table functor.
//  - Owner: FUN_14050AAD0(extra data) -> owner form (ObjectReference.GetFactionOwner's core, FUN_142070F70; a faction is
//    form type 0x0D); FUN_14050CED0(extra data, ref, owner, char) sets it (SetFactionOwner's core, FUN_14207F540).
//  - The player's vehicle base form: VehicleDefaultPlayerVehicle_DO (DAT_145F04350) form at +0x48 = DAT_145F04398,
//    the fallback FUN_14219ED60 spawns at the player's ship.
// Vehicles.esm (parsed): the props are statics Vehicle_Ref8_Static 0x80B, Vehicle_Deimog_Static 0x80C,
// Vehicle_MoonJumper_Static 0x8DD, with Vehicle_Ref8_Wheels_Static 0xACB and Vehicle_Ref8_Turret_Static 0xAF1 placed
// next to them inside pack-ins.
namespace realvehicles
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		constexpr std::uint32_t kLookupForm = 0x005DE3B0;
		constexpr std::uint32_t kGetOwner = 0x0050AAD0, kSetOwner = 0x0050CED0;
		constexpr std::uint32_t kPlayerVehicleBase = 0x05F04398;
		// The vehicle keyword (DAT_145F04028): the game's vehicle test FUN_140EC72A0 is ref vtable +0x200 and HasKeyword(ref, it).
		constexpr std::uint32_t kVehicleKeyword = 0x05F04028;
		constexpr std::uint32_t kStaticRev8 = 0x80B, kStaticDeimog = 0x80C, kStaticMoonJumper = 0x8DD, kStaticWheels = 0xACB, kStaticTurret = 0xAF1,
			kLvlpStatic = 0x800;
		constexpr std::uint8_t kFormTypeFaction = 0x0D;
		constexpr int kSwapsPerTick = 4;
		constexpr float kCompanionRadius = 4.0f;  // m: wheels / turret of the same prop
		// One vehicle per spot, ever (2026-10-17: the props come from a leveled pack-in, so every load makes them again
		// with new FF ids -- FF013B72, then FF013BB2 at the same (657, 635) -- and each load added another vehicle of
		// a different random type). A spot that already has a vehicle within kSpotRadius gets nothing new; the saved
		// vehicle just stays. The type is picked from the spot's position, so it is always the same one.
		constexpr float kSpotRadius = 6.0f;

		// Every vehicle form loaded in the game (the data handler's form list of the player vehicle's type), named by
		// editor id or, failing that, by the model path found in the form; each prop maps to its own vehicle.
		struct VehicleForm
		{
			uptr form = 0;
			std::uint32_t id = 0;
			std::string name;  // lower case
		};

		// The vehicle registry (the user, 2026-10-17: "we don't generate, we just store vehicles at location ... we store
		// what happens with it"). Nothing is created: every real vehicle the game has in a loaded cell is recorded once,
		// with where it is and who uses it, and each change is kept (log + vehicles.tsv).
		struct Record
		{
			std::uint32_t id = 0, base = 0, owner = 0, worldspace = 0;
			float x = 0, y = 0, z = 0;      // last stored place
			const char* state = "parked";   // parked / player / npc / own
			ULONGLONG firstSeen = 0, lastSeen = 0;
			unsigned moves = 0;
		};

		struct State
		{
			int modIndex = -1;           // Vehicles.esm's load index; -2: not loaded
			bool haveVehicles = false;
			std::vector<VehicleForm> vehicles;
			uptr rev8 = 0, deimog = 0, moonJumper = 0;
			std::vector<uptr> pool;      // every vehicle form (vanilla and mods): the ones carrying the vehicle keyword
			ULONGLONG lastFind = 0, last = 0;
			int refsOffset = -1;         // cell reference array (0x80 in CommonLibSF's 1.14 layout; checked at run time)
			std::unordered_set<std::uint32_t> done;   // prop refs handled (by form id)
			std::unordered_map<uptr, uptr> cellOwner;  // cell -> owner form (0: none)
			unsigned swaps = 0;
			std::map<std::uint32_t, Record> registry;
			bool dirty = false;
			ULONGLONG lastSave = 0;
			std::vector<game::Vec3> loadedVehicles;  // every enabled vehicle in the attached cells, this scan
			std::unordered_set<std::uint32_t> ownVehicles;  // vehicles the mod's own placements spawned (never cleared as duplicates)
		} g;
		constexpr float kMoveStore = 20.0f;  // m: a move this long is stored as a change of place

		bool Readable(uptr p, std::size_t n)
		{
			__try {
				volatile std::uint8_t x = 0;
				for (std::size_t i = 0; i < n; i += 8) x ^= *reinterpret_cast<volatile std::uint8_t*>(p + i);
				(void)x;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		uptr Lookup(std::uint32_t id)
		{
			__try {
				return EngineFn<uptr (*)(std::uint32_t)>(kLookupForm)(id);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		std::uint8_t TypeOf(uptr form) { return form ? At<std::uint8_t>(form, 0x2E) : 0; }

		// Vehicles.esm's index: the one whose 0x80B, 0x80C and 0x8DD are forms of one type (statics) and 0x800 another
		// (a leveled pack-in).
		void FindMod()
		{
			const ULONGLONG now = GetTickCount64();
			if (g.modIndex >= 0 || (g.lastFind && now - g.lastFind < 30000)) return;
			g.lastFind = now;
			for (std::uint32_t i = 1; i < 0xFE; ++i) {
				const std::uint32_t hi = i << 24;
				const uptr a = Lookup(hi | kStaticRev8), b = Lookup(hi | kStaticDeimog), c = Lookup(hi | kStaticMoonJumper), l = Lookup(hi | kLvlpStatic);
				if (a && b && c && l && TypeOf(a) == TypeOf(b) && TypeOf(a) == TypeOf(c) && TypeOf(l) != TypeOf(a)) {
					g.modIndex = int(i);
					const uptr base = Ptr(kPlayerVehicleBase);
					logger::Write("Real vehicles: Parked Vehicles (Vehicles.esm by SarthesArai) found at load index %02X; real vehicle base %08X", i,
						base ? At<std::uint32_t>(base, 0x28) : 0);
					return;
				}
			}
			if (g.modIndex == -1) {
				g.modIndex = -2;
				logger::Write("Real vehicles: Vehicles.esm (Parked Vehicles) not loaded; nothing to do");
			}
		}

		std::string Lower(const char* t)
		{
			std::string r;
			for (; t && *t && r.size() < 200; ++t) r += char(std::tolower(static_cast<unsigned char>(*t)));
			return r;
		}

		// A readable C string at p (printable, up to 200 chars) copied into buf; false when not text.
		bool CopyText(uptr p, char* buf)
		{
			buf[0] = 0;
			if (!p || !Readable(p, 8)) return false;
			__try {
				for (int i = 0; i < 200; ++i) {
					const char c = *reinterpret_cast<const char*>(p + i);
					buf[i] = c;
					if (!c) return true;
					if (c < 0x20 || c > 0x7E) { buf[0] = 0; return false; }
				}
				buf[200] = 0;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				buf[0] = 0;
				return false;
			}
		}

		std::string TextAt(uptr p)
		{
			char buf[201];
			return CopyText(p, buf) ? std::string(buf) : std::string();
		}

		uptr EditorIdPtr(uptr form)
		{
			__try {
				auto** vt = *reinterpret_cast<void***>(form);
				return reinterpret_cast<uptr>(reinterpret_cast<const char* (*)(uptr)>(vt[0x4D])(form));
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		std::string EditorId(uptr form) { return TextAt(EditorIdPtr(form)); }

		// The base form lists `kw` among its keywords: a pointer inside the form (up to +0x300) to an array whose first 16
		// entries include the keyword form (the keyword array of its keyword component).
		bool FormHasKeyword(uptr form, uptr kw)
		{
			for (std::uint32_t o = 0x30; o < 0x300; o += 8) {
				if (!Readable(form + o, 8)) return false;
				const uptr arr = At<uptr>(form, o);
				if (arr < 0x10000 || (arr & 7) || !Readable(arr, 16 * 8)) continue;
				for (int i = 0; i < 16; ++i) {
					if (reinterpret_cast<const uptr*>(arr)[i] == kw) return true;
				}
			}
			return false;
		}

		// A model path inside the form: a pointer (direct, or a string-pool entry with its text at +0x18) to text with a
		// backslash and ".nif" / ".mesh".
		std::string ModelHint(uptr form)
		{
			for (std::uint32_t o = 0x30; o < 0x400; o += 8) {
				if (!Readable(form + o, 8)) break;
				const uptr q = At<uptr>(form, o);
				if (q < 0x10000) continue;
				for (const uptr cand : { q, q + 0x18 }) {
					const std::string t = TextAt(cand);
					if (t.size() > 6 && t.find('\\') != std::string::npos && (t.find(".nif") != std::string::npos || t.find(".mesh") != std::string::npos)) return Lower(t.c_str());
				}
			}
			return {};
		}

		// The data handler's per-type form lists: {lock 8, uint32 size, uint32 capacity, form** data} each 0x18 bytes,
		// starting at +0x70 in CommonLibSF's 1.14 layout; the start is checked by finding the player vehicle's base.
		void DiscoverVehicles()
		{
			if (g.haveVehicles) return;
			g.haveVehicles = true;
			const uptr dh = Ptr(addr::kDataHandlerPtr);
			const uptr base = Ptr(kPlayerVehicleBase);
			g.rev8 = base;
			if (!dh || !base) {
				logger::Write("Real vehicles: no data handler or player vehicle base; REV-8 everywhere");
				return;
			}
			const std::uint8_t type = TypeOf(base);
			for (std::uint32_t start = 0x50; start <= 0xA0 && g.vehicles.empty(); start += 8) {
				const uptr fa = dh + start + std::uint32_t(type) * 0x18;
				if (!Readable(fa, 0x18)) continue;
				const std::uint32_t size = At<std::uint32_t>(fa, 8), cap = At<std::uint32_t>(fa, 12);
				uptr* data = At<uptr*>(fa, 16);
				if (!data || !size || size > cap || size > 4096 || !Readable(reinterpret_cast<uptr>(data), size * 8)) continue;
				bool has = false;
				for (std::uint32_t i = 0; i < size; ++i) has = has || data[i] == base;
				if (!has) continue;
				for (std::uint32_t i = 0; i < size; ++i) {
					const uptr f = data[i];
					if (!f || !Readable(f, 0x40) || TypeOf(f) != type) continue;
					VehicleForm v{ f, At<std::uint32_t>(f, 0x28), Lower(EditorId(f).c_str()) };
					if (v.name.empty()) v.name = ModelHint(f);
					g.vehicles.push_back(v);
				}
				logger::Write("Real vehicles: %zu vehicle forms in the game (form type %02X, list at data handler+0x%X):", g.vehicles.size(), type, start);
			}
			for (const auto& v : g.vehicles) {
				// (the whole furniture list -- chairs, beds, markers -- is no longer logged; the pool lines below are the vehicles)
				if (!g.deimog && v.name.find("deimog") != std::string::npos) g.deimog = v.form;
				if (!g.moonJumper && (v.name.find("moonjump") != std::string::npos || v.name.find("moon_jump") != std::string::npos ||
										  v.name.find("moon jump") != std::string::npos)) g.moonJumper = v.form;
			}
			logger::Write("Real vehicles: props map to REV-8 %08X, Deimog %08X%s, Moon Jumper %08X%s", At<std::uint32_t>(base, 0x28),
				g.deimog ? At<std::uint32_t>(g.deimog, 0x28) : 0, g.deimog ? "" : " (not found: REV-8)", g.moonJumper ? At<std::uint32_t>(g.moonJumper, 0x28) : 0,
				g.moonJumper ? "" : " (not found: REV-8)");
			const uptr kw = Ptr(kVehicleKeyword);
			if (!kw || TypeOf(kw) != 4) {
				logger::Write("Real vehicles: vehicle keyword not found (%p); parked spots keep their own vehicle type", reinterpret_cast<void*>(kw));
				return;
			}
			for (const auto& v : g.vehicles) {
				if (FormHasKeyword(v.form, kw)) g.pool.push_back(v.form);
			}
			if (std::find(g.pool.begin(), g.pool.end(), base) == g.pool.end()) {
				logger::Write("Real vehicles: the REV-8 base does not show the vehicle keyword %08X (%s); the vehicle pool is not used",
					At<std::uint32_t>(kw, 0x28), EditorId(kw).c_str());
				g.pool.clear();
				return;
			}
			logger::Write("Real vehicles: %zu vehicle forms carry the vehicle keyword %08X (%s); REV-8 spots get any of them:", g.pool.size(),
				At<std::uint32_t>(kw, 0x28), EditorId(kw).c_str());
			for (const uptr f : g.pool) {
				for (const auto& v : g.vehicles) {
					if (v.form == f) logger::Write("Real vehicles:   pool %08X %s", v.id, v.name.c_str());
				}
			}
		}

		// The cell's reference array: {uint32 size, uint32 capacity, ref** data}; its offset is checked on a cell with refs.
		bool CellRefs(uptr cell, uptr*& data, std::uint32_t& count)
		{
			const int offs[2] = { 0x80, 0x88 };
			for (const int o : offs) {
				if (g.refsOffset >= 0 && g.refsOffset != o) continue;
				if (!Readable(cell + o, 16)) continue;
				const std::uint32_t size = At<std::uint32_t>(cell, o), cap = At<std::uint32_t>(cell, o + 4);
				uptr* arr = At<uptr*>(cell, o + 8);
				if (!arr || size > cap || cap > 200000 || !size) continue;
				if (!Readable(reinterpret_cast<uptr>(arr), 8)) continue;
				const uptr first = arr[0];
				if (!first || !Readable(first, 0xC0) || At<uptr>(first, off::kRefParentCell) != cell) continue;
				if (g.refsOffset < 0) {
					g.refsOffset = o;
					logger::Write("Real vehicles: cell reference array at cell+0x%X", o);
				}
				data = arr;
				count = size;
				return true;
			}
			return false;
		}

		uptr OwnerOf(uptr extra)
		{
			if (!extra) return 0;
			__try {
				return EngineFn<uptr (*)(uptr)>(kGetOwner)(extra);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		// The location's owner: the cell's own owner, else the faction that owns most things in the cell.
		uptr LocationOwner(uptr cell, uptr* refs, std::uint32_t count)
		{
			if (const auto it = g.cellOwner.find(cell); it != g.cellOwner.end()) return it->second;
			uptr owner = OwnerOf(At<uptr>(cell, 0x50));
			if (owner && TypeOf(owner) != kFormTypeFaction) owner = 0;
			if (!owner) {
				std::map<uptr, int> votes;
				for (std::uint32_t i = 0; i < count && i < 4096; ++i) {
					const uptr r = refs[i];
					if (!r) continue;
					const uptr o = OwnerOf(At<uptr>(r, 0xC8));
					if (o && TypeOf(o) == kFormTypeFaction) ++votes[o];
				}
				int best = 0;
				for (const auto& [o, n] : votes) {
					if (n > best) { best = n; owner = o; }
				}
			}
			g.cellOwner[cell] = owner;
			return owner;
		}

		uptr RefFromHandle(std::uint32_t handle)
		{
			uptr ref = 0;
			uptr* out = &ref;
			struct
			{
				uptr vtbl;
				uptr** capture;
			} lookup{ game::Base() + addr::kHandleToRefFunctorVtbl, &out };
			EngineFn<void (*)(uptr, std::uint32_t, void*)>(addr::kHandleLookup)(Ptr(addr::kRefHandleTablePtr), handle, &lookup);
			return ref;
		}

		// A real vehicle base: one carrying the vehicle keyword (the pool). Vehicles are furniture (form type 0x2F), so
		// the whole form list also holds every chair, bed and marker (2026-10-17 20:13: 1054 forms, 827 "vehicles"
		// registered; in New Atlantis a bench near a parked spot counted as that spot's vehicle, so no vehicle came).
		bool IsVehicleBase(uptr base)
		{
			if (!base) return false;
			if (!g.pool.empty()) return std::find(g.pool.begin(), g.pool.end(), base) != g.pool.end();
			return base == g.rev8 || base == g.deimog || base == g.moonJumper;
		}

		void Disable(uptr ref);

		// A vehicle that must stay: the player's own, the one the player is in, one an NPC job uses.
		bool Protected(uptr r)
		{
			const std::uint32_t id = At<std::uint32_t>(r, 0x28);
			if (id == vehicleflight::OwnRoverId() || npcvehicles::UsesVehicle(id) || g.ownVehicles.count(id)) return true;
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			__try {
				if (player && EngineFn<uptr (*)(uptr)>(addr::kPlayerVehicle)(player) == r) return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return true;
			}
			return false;
		}

		// The spot's vehicle: every enabled vehicle within kSpotRadius; one is kept (a protected one, else the oldest =
		// lowest id) and the other created duplicates (FF ids from earlier loads) are disabled. Plugin-placed
		// vehicles (non-FF ids) are never touched.
		uptr VehicleAtSpot(const game::Vec3& p, uptr* refs, std::uint32_t count)
		{
			std::vector<uptr> here;
			for (std::uint32_t i = 0; i < count && i < 8192; ++i) {
				const uptr r = refs[i];
				if (!r || !Readable(r, 0xD0)) continue;
				if (At<std::uint32_t>(r, 0x20) & 0x800) continue;  // disabled
				if (!IsVehicleBase(At<uptr>(r, 0x98))) continue;
				const auto q = At<game::Vec3>(r, off::kRefPosition);
				if (std::fabs(q.x - p.x) < kSpotRadius && std::fabs(q.y - p.y) < kSpotRadius && std::fabs(q.z - p.z) < kSpotRadius) here.push_back(r);
			}
			if (here.empty()) return 0;
			uptr keep = 0;
			for (const uptr r : here) {
				if (Protected(r)) { keep = r; break; }
			}
			if (!keep) {
				keep = here[0];
				for (const uptr r : here) {
					if (At<std::uint32_t>(r, 0x28) < At<std::uint32_t>(keep, 0x28)) keep = r;
				}
			}
			int removed = 0;
			for (const uptr r : here) {
				const std::uint32_t id = At<std::uint32_t>(r, 0x28);
				if (r == keep || (id >> 24) != 0xFF || Protected(r)) continue;
				Disable(r);
				++removed;
				logger::Write("Real vehicles: duplicate vehicle %08X at spot (%.0f, %.0f, %.0f) removed (kept %08X)", id, p.x, p.y, p.z, At<std::uint32_t>(keep, 0x28));
			}
			(void)removed;
			return keep;
		}

		// A real vehicle at a place and rotation in the cell (no owner: parked vehicles are free to take, as Parked Vehicles'
		// are); returns its form id, 0 when nothing was placed.
		std::uint32_t Spawn(uptr cell, const game::Vec3& p, const game::Vec3& r, uptr base)
		{
			const uptr ws = At<uptr>(cell, off::kCellWorldspace);
			if (!base) return 0;
			alignas(16) std::uint8_t d[0xC0] = {};
			const auto dp = reinterpret_cast<uptr>(d);
			At<uptr>(dp, 0x00) = game::Base() + addr::kNewRefrDataVtbl;
			At<float>(dp, 0x10) = p.x;
			At<float>(dp, 0x14) = p.y;
			At<float>(dp, 0x18) = p.z + 0.3f;
			At<float>(dp, 0x1C) = r.x;
			At<float>(dp, 0x20) = r.y;
			At<float>(dp, 0x24) = r.z;
			At<float>(dp, 0x28) = 1.0f;
			At<uptr>(dp, 0x30) = base;
			At<uptr>(dp, 0x38) = cell;
			At<uptr>(dp, 0x40) = ws;
			At<std::uint8_t>(dp, 0x8C) = 1;
			At<std::uint8_t>(dp, 0x8D) = 1;
			At<std::uint32_t>(dp, 0x98) = 1;
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			uptr ref = 0;
			__try {
				EngineFn<void* (*)(uptr, std::uint64_t*, void*)>(addr::kCreateReference)(Ptr(addr::kDataHandlerPtr), &handle, d);
				ref = RefFromHandle(static_cast<std::uint32_t>(handle));
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				logger::Write("Real vehicles: placing raised an exception");
				return 0;
			}
			if (!ref) return 0;
			const std::uint32_t id = At<std::uint32_t>(ref, 0x28);
			EngineFn<void (*)(uptr)>(addr::kRefRelease)(ref);
			return id;
		}

		void Disable(uptr ref)
		{
			__try {
				EngineFn<void (*)(uptr, char)>(addr::kDisableReference)(ref, 0);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}

		const char* StateOf(uptr r)
		{
			const std::uint32_t id = At<std::uint32_t>(r, 0x28);
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			uptr driven = 0;
			__try {
				driven = player ? EngineFn<uptr (*)(uptr)>(addr::kPlayerVehicle)(player) : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
			if (driven == r) return "player";
			if (const char* prof = npcvehicles::ProfileOf(id)) return std::strcmp(prof, "pursuit") == 0 ? "npc pursuit" : "npc escort";
			if (npcvehicles::UsesVehicle(id)) return "npc";
			if (id == vehicleflight::OwnRoverId()) return "own";
			return "parked";
		}

		void Register(uptr r, uptr cell)
		{
			const std::uint32_t id = At<std::uint32_t>(r, 0x28);
			const auto p = At<game::Vec3>(r, off::kRefPosition);
			const uptr ws = At<uptr>(cell, off::kCellWorldspace);
			const ULONGLONG now = GetTickCount64();
			const char* st = StateOf(r);
			auto it = g.registry.find(id);
			if (it == g.registry.end()) {
				Record rec;
				rec.id = id;
				rec.base = At<std::uint32_t>(At<uptr>(r, 0x98), 0x28);
				const uptr owner = OwnerOf(At<uptr>(r, 0xC8));
				rec.owner = owner ? At<std::uint32_t>(owner, 0x28) : 0;
				rec.worldspace = ws ? At<std::uint32_t>(ws, 0x28) : 0;
				rec.x = p.x; rec.y = p.y; rec.z = p.z;
				rec.state = st;
				rec.firstSeen = rec.lastSeen = now;
				g.registry[id] = rec;
				g.dirty = true;
				logger::Write("Vehicle registry: %08X (base %08X, owner %08X) stored at (%.0f, %.0f, %.0f), %s", id, rec.base, rec.owner, p.x, p.y, p.z, st);
				return;
			}
			Record& rec = it->second;
			rec.lastSeen = now;
			if (std::strcmp(rec.state, st) != 0) {
				logger::Write("Vehicle registry: %08X %s -> %s at (%.0f, %.0f, %.0f)", id, rec.state, st, p.x, p.y, p.z);
				rec.state = st;
				g.dirty = true;
			}
			const float moved = std::hypot(p.x - rec.x, p.y - rec.y);
			if (moved >= kMoveStore && std::strcmp(st, "player") != 0 && std::strncmp(st, "npc", 3) != 0) {
				// stored when it comes to rest somewhere else (not while someone drives it)
				logger::Write("Vehicle registry: %08X moved %.0f m: (%.0f, %.0f) -> (%.0f, %.0f, %.0f)", id, moved, rec.x, rec.y, p.x, p.y, p.z);
				rec.x = p.x; rec.y = p.y; rec.z = p.z;
				rec.worldspace = ws ? At<std::uint32_t>(ws, 0x28) : rec.worldspace;
				++rec.moves;
				g.dirty = true;
			}
		}

		void SaveRegistry()
		{
			PWSTR docs = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return;
			std::wstring path = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessVehicle";
			CoTaskMemFree(docs);
			CreateDirectoryW(path.c_str(), nullptr);
			path += L"\\vehicles.tsv";
			FILE* f = nullptr;
			if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) return;
			std::fprintf(f, "id\tbase\towner\tworldspace\tx\ty\tz\tstate\tmoves\n");
			for (const auto& [id, r] : g.registry) {
				std::fprintf(f, "%08X\t%08X\t%08X\t%08X\t%.1f\t%.1f\t%.1f\t%s\t%u\n", r.id, r.base, r.owner, r.worldspace, r.x, r.y, r.z, r.state, r.moves);
			}
			std::fclose(f);
		}

		// Co-located parked vehicles (2026-10-17 20:58: REV-8 FF013B99 from Vehicles - Real.esm and V-47 FF019056 from the
		// earlier run-time swap, both at (657, 635, 17)): the swap's vehicles live in the save and the props that used to
		// trigger the duplicate check are gone. Two parked, unprotected vehicles within kDupRadius are one spot: the one
		// with the spot's own type (REV-8 / Deimog / Moon Jumper, what the patch places) is kept, else the lowest id; the
		// other created ones (FF ids) are disabled. kDupRadius is tight so neighbouring real spots are never touched.
		constexpr float kDupRadius = 2.5f;
		void ClearStacked(const std::vector<uptr>& parked)
		{
			auto spotType = [](uptr r) {
				const uptr b = At<uptr>(r, 0x98);
				return b && (b == g.rev8 || b == g.deimog || b == g.moonJumper);
			};
			std::vector<bool> gone(parked.size(), false);
			for (std::size_t i = 0; i < parked.size(); ++i) {
				if (gone[i]) continue;
				const auto p = At<game::Vec3>(parked[i], off::kRefPosition);
				std::vector<std::size_t> here{ i };
				for (std::size_t j = i + 1; j < parked.size(); ++j) {
					if (gone[j]) continue;
					const auto q = At<game::Vec3>(parked[j], off::kRefPosition);
					if (std::fabs(q.x - p.x) < kDupRadius && std::fabs(q.y - p.y) < kDupRadius && std::fabs(q.z - p.z) < kDupRadius) here.push_back(j);
				}
				if (here.size() < 2) continue;
				std::size_t keep = here[0];
				for (const std::size_t k : here) {
					const bool better = spotType(parked[k]) && !spotType(parked[keep]);
					const bool same = spotType(parked[k]) == spotType(parked[keep]);
					if (better || (same && At<std::uint32_t>(parked[k], 0x28) < At<std::uint32_t>(parked[keep], 0x28))) keep = k;
				}
				for (const std::size_t k : here) {
					const uptr r = parked[k];
					const std::uint32_t id = At<std::uint32_t>(r, 0x28);
					if (k == keep || (id >> 24) != 0xFF || Protected(r)) continue;
					Disable(r);
					gone[k] = true;
					g.registry.erase(id);
					g.dirty = true;
					logger::Write("Real vehicles: stacked vehicle %08X (base %08X) at (%.0f, %.0f, %.0f) removed; kept %08X (base %08X)", id,
						At<std::uint32_t>(At<uptr>(r, 0x98), 0x28), p.x, p.y, p.z, At<std::uint32_t>(parked[keep], 0x28),
						At<std::uint32_t>(At<uptr>(parked[keep], 0x98), 0x28));
				}
			}
		}

		// ---- Placement patch (2026-10-21, the user): the kinds of vehicle that carfield.esm and Vehicles.esm park are
		// stand-ins. In each location every stand-in resolves to one vehicle from the whole load order, picked from a seeded
		// random distribution per location: the same every load, so nothing is spawned and no vehicle is added. Optional: a
		// mod that is not loaded is left alone; with neither loaded nothing here runs. Carfield's own vehicle types are known
		// wherever they stand; Vehicles.esm parks vanilla REV-8s through pack-ins, so its stand-in is the pack-in kind, known
		// from the spots baked from the plugin (scripts/bake_placement_spots.py). The vehicle keeps its reference (owner, place, save): only its base
		// changes, through the game's own SetObjectReference (ref vtable 0xF7: TESObjectREFR 140B4D430, Actor 1418D8950,
		// the override the game uses on the live player) while the vehicle is disabled, then it is enabled again.
		constexpr std::uint32_t kLookupModByName = 0x00598430;  // (data handler, name) -> TESFile*, name at +0x38; loaded: +0x218 != 0xFF
		constexpr std::uint32_t kFileLocalToRuntime = 0x005B78C0;  // (TESFile*, uint32* id): a file-local id -> its run-time id
		constexpr std::uint32_t kRefSetBaseSlot = 0xF7;
		constexpr std::uint32_t kSetBaseRefr = 0x00B4D430, kSetBaseActor = 0x018D8950;
		constexpr float kPatchSpotRadius = 4.0f;    // m: the mods' spots can be 3.4 m apart; the nearest within this wins
		constexpr float kPatchPlayerClear = 10.0f;  // m: not while the player stands right beside it (the user: closer than 30 m)
		constexpr std::uint64_t kDefaultSeed = 0x5EA3B1E5C0FFEE21ull;  // the mod's stored seed ([placement] seed= overrides)

		bool Signature(std::uint32_t rva, std::initializer_list<std::uint8_t> bytes)
		{
			std::size_t i = 0;
			for (const auto b : bytes) if (*reinterpret_cast<const std::uint8_t*>(game::Base() + rva + i++) != b) return false;
			return true;
		}

		using placementlogic::Mix;
		using placementlogic::Fnv;

		struct PatchSpot { std::uint32_t world = 0, worldLocal = 0; std::uint8_t worldFile = 0; float x = 0, y = 0, z = 0; std::uint64_t key = 0, location = 0, standin = 0; };
		// A baked cell (placementspots::kCells) by its run-time id: its worldspace in the file and its grid there.
		struct CellInfo { std::uint8_t worldFile = 0; std::uint32_t worldLocal = 0; int gx = 0, gy = 0; };
		constexpr float kCellSize = 100.0f;  // m: an exterior cell (New Atlantis' recorded spots fall in their cells' grids at exactly this)
		struct SwapJob { std::uint32_t ref = 0; uptr base = 0; int stage = 0; ULONGLONG since = 0; std::uint64_t spot = 0; };
		struct Patch
		{
			bool ready = false, active = false;
			std::uint64_t seed = kDefaultSeed;
			bool enabled = true;
			std::vector<PatchSpot> spots;                 // active spots (loaded mods only), run-time worldspace ids
			std::vector<std::pair<uptr, std::uint64_t>> pool;  // vehicle base, stable key (model + file-local id)
			std::vector<std::pair<uptr, std::uint64_t>> standins;  // a mod's own vehicle base, its stand-in key
			std::vector<std::pair<uptr, const char*>> files;   // loaded files a worldspace's location key is named by
			std::unordered_map<std::uint32_t, std::uint64_t> locations;  // run-time worldspace id -> location key
			std::unordered_map<std::uint32_t, CellInfo> cells;            // run-time cell id -> its baked worldspace and grid
			std::unordered_map<std::uint32_t, std::pair<int, int>> shifts;  // run-time cell id -> how many cells its worldspace was moved
			std::unordered_set<std::uint32_t> handled;    // refs decided this session
			std::vector<SwapJob> jobs;
			unsigned swaps = 0, kept = 0;
		} g_patch;

		std::wstring IniPath()
		{
			PWSTR docs = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return {};
			std::wstring w = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessVehicle\\seamless.ini";
			CoTaskMemFree(docs);
			return w;
		}

		uptr LoadedFile(const char* name)
		{
			__try {
				const uptr file = EngineFn<uptr (*)(uptr, const char*)>(kLookupModByName)(Ptr(addr::kDataHandlerPtr), name);
				return file && At<std::uint8_t>(file, 0x218) != 0xFF ? file : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		std::uint32_t RuntimeId(uptr file, std::uint32_t local)
		{
			std::uint32_t id = local;
			__try {
				EngineFn<void (*)(uptr, std::uint32_t*)>(kFileLocalToRuntime)(file, &id);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
			return id;
		}

		// Once the vehicle pool is known: which placement mods are loaded, their spots in run-time ids, the seed.
		void PreparePatch()
		{
			if (g_patch.ready || !g.haveVehicles) return;
			g_patch.ready = true;
			const std::wstring ini = IniPath();
			g_patch.enabled = features::On(features::Id::Placement);  // [placement] enabled, also the settings menu
			wchar_t seed[40] = L"";
			GetPrivateProfileStringW(L"placement", L"seed", L"", seed, 40, ini.c_str());
			if (seed[0]) g_patch.seed = std::wcstoull(seed, nullptr, 0);
			if (!Signature(kLookupModByName, { 0x48, 0x89, 0x6C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xFF, 0x48, 0x8B, 0xEA, 0x48 }) ||
				!Signature(kFileLocalToRuntime, { 0x44, 0x8B, 0x02, 0x4C, 0x8B, 0xD2, 0x45, 0x85, 0xC0, 0x0F, 0x84, 0xD0, 0x00, 0x00, 0x00, 0x8B })) {
				logger::Write("Placement patch: off (plugin lookup functions do not match this game version)");
				return;
			}
			bool loaded[std::size(placementspots::kMods)] = {};
			unsigned count = 0;
			for (std::size_t m = 0; m < std::size(placementspots::kMods); ++m) {
				loaded[m] = LoadedFile(placementspots::kMods[m]) != 0;
				logger::Write("Placement patch: %s %s", placementspots::kMods[m], loaded[m] ? "loaded: its parked vehicles get the patch" : "not loaded (nothing to patch)");
			}
			uptr files[std::size(placementspots::kFiles)] = {};
			for (std::size_t f = 0; f < std::size(placementspots::kFiles); ++f) files[f] = LoadedFile(placementspots::kFiles[f]);
			for (const auto& s : placementspots::kSpots) {
				if (!loaded[s.mod] || !files[s.worldFile]) continue;
				PatchSpot p;
				p.world = RuntimeId(files[s.worldFile], s.worldLocal);
				if (!p.world) continue;
				p.worldFile = s.worldFile; p.worldLocal = s.worldLocal;
				p.x = s.x; p.y = s.y; p.z = s.z; p.key = s.key; p.standin = s.standin;
				p.location = Fnv(placementspots::kFiles[s.worldFile]) ^ Mix(s.worldLocal);  // a location: the worldspace, load-order independent
				g_patch.spots.push_back(p);
				++count;
			}
			for (std::size_t f = 0; f < std::size(placementspots::kFiles); ++f) if (files[f]) g_patch.files.push_back({ files[f], placementspots::kFiles[f] });
			for (const auto& c : placementspots::kCells) {
				if (!files[c.file]) continue;
				if (const std::uint32_t id = RuntimeId(files[c.file], c.local)) g_patch.cells[id] = { c.worldFile, c.worldLocal, c.gx, c.gy };
			}
			// The mods' own vehicle types, resolved through the file the plugin writes them under, else through the plugin itself.
			for (const auto& b : placementspots::kStandinBases) {
				if (!loaded[b.mod]) continue;
				uptr form = 0;
				for (const uptr file : { files[b.file], LoadedFile(placementspots::kMods[b.mod]) }) {
					const uptr f = file ? Lookup(RuntimeId(file, b.local)) : 0;
					if (f && std::find(g.pool.begin(), g.pool.end(), f) != g.pool.end()) { form = f; break; }
				}
				if (form) g_patch.standins.push_back({ form, b.standin });
				logger::Write("Placement patch: %s vehicle type %06X (%s) %s", placementspots::kMods[b.mod], b.local, placementspots::kFiles[b.file],
					form ? "found: a stand-in wherever it is parked" : "not found among the vehicles");
				if (form) logger::Write("Placement patch:   = %08X %s", At<std::uint32_t>(form, 0x28), ModelHint(form).c_str());
			}
			// The pool's stable keys: the model path and the file-local part of the id (light FE: 12 bits, medium FD: 16, else 24).
			// A stand-in is never a target (the 11:05 run: Carfield's REVs became Carfield's Deimog copies, then were rolled again).
			for (const uptr f : g.pool) {
				if (std::any_of(g_patch.standins.begin(), g_patch.standins.end(), [&](const auto& e) { return e.first == f; })) continue;
				const std::uint32_t id = At<std::uint32_t>(f, 0x28);
				const std::uint32_t local = (id >> 24) == 0xFE ? id & 0xFFF : (id >> 24) == 0xFD ? id & 0xFFFF : id & 0xFFFFFF;
				g_patch.pool.push_back({ f, Fnv(ModelHint(f)) ^ Mix(local) });
			}
			g_patch.active = g_patch.enabled && (count > 0 || !g_patch.standins.empty()) && g_patch.pool.size() > 1;
			logger::Write("Placement patch: %s; %u spots (%zu cells) and %zu stand-in types active, %zu vehicle types, seed %016llX%s", g_patch.active ? "ON" : "off", count,
				g_patch.cells.size(), g_patch.standins.size(), g_patch.pool.size(),
				static_cast<unsigned long long>(g_patch.seed), g_patch.enabled ? "" : " ([placement] enabled=0)");
		}

		// The vehicle a stand-in becomes in a location (placementlogic::Pick: a seeded mix per location; one vehicle per
		// stand-in and location).
		uptr PatchPick(std::uint64_t location, std::uint64_t standin)
		{
			std::vector<std::uint64_t> keys;
			for (const auto& e : g_patch.pool) keys.push_back(e.second);
			const int i = placementlogic::Pick(g_patch.seed, location, standin, keys.data(), int(keys.size()));
			return i >= 0 ? g_patch.pool[i].first : 0;
		}

		// A worldspace's location key, the same the baked spots use: its file's name and its file-local id (load-order
		// independent); a worldspace from a file the table does not name falls back to its run-time id.
		std::uint64_t LocationKey(std::uint32_t world)
		{
			if (const auto it = g_patch.locations.find(world); it != g_patch.locations.end()) return it->second;
			std::uint64_t key = Mix(world) ^ 0x6C6F636174696F6Eull;
			for (const auto& [file, name] : g_patch.files) {
				bool found = false;
				for (const std::uint32_t mask : { 0xFFFFFFu, 0xFFFFu, 0xFFFu }) {
					if (RuntimeId(file, world & mask) == world) { key = Fnv(name) ^ Mix(world & mask); found = true; break; }
				}
				if (found) break;
			}
			g_patch.locations[world] = key;
			return key;
		}

		// How many cells a loaded cell's worldspace was moved onto its planet: the grid most of its references stand in, minus
		// the cell's grid in the file. (0, 0) for a worldspace used as it is. False when the cell is not a baked one or has no refs.
		bool CellShift(uptr cell, const CellInfo& info, int& sx, int& sy)
		{
			const std::uint32_t id = At<std::uint32_t>(cell, 0x28);
			if (const auto it = g_patch.shifts.find(id); it != g_patch.shifts.end()) { sx = it->second.first; sy = it->second.second; return true; }
			uptr* refs = nullptr;
			std::uint32_t count = 0;
			if (!CellRefs(cell, refs, count)) return false;
			std::map<std::pair<int, int>, int> votes;
			for (std::uint32_t i = 0; i < count && i < 512; ++i) {
				const uptr r = refs[i];
				if (!r || !Readable(r, 0xD0)) continue;
				const auto q = At<game::Vec3>(r, off::kRefPosition);
				if (!std::isfinite(q.x) || !std::isfinite(q.y) || std::fabs(q.x) > 1e6f || std::fabs(q.y) > 1e6f) continue;
				++votes[{ int(std::floor(q.x / kCellSize)), int(std::floor(q.y / kCellSize)) }];
			}
			int best = 0, total = 0;
			std::pair<int, int> grid{};
			for (const auto& [g2, n] : votes) { total += n; if (n > best) { best = n; grid = g2; } }
			if (best < 3 || best * 2 < total) return false;  // too few refs, or no clear majority: asked again on the next scan
			sx = grid.first - info.gx;
			sy = grid.second - info.gy;
			g_patch.shifts[id] = { sx, sy };
			logger::Write("Placement patch: cell %08X (%s %06X grid %d, %d) stands at grid %d, %d: its spots move %d, %d cells (%d of %d refs)", id,
				placementspots::kFiles[info.worldFile], info.worldLocal, info.gx, info.gy, grid.first, grid.second, sx, sy, best, total);
			return true;
		}

		// The spot a vehicle stands on. In a baked cell the spots of its file worldspace are compared moved by the cell's shift,
		// in x/y only (the overlay also moves z: New Atlantis -238.97 m, not known from the grid); elsewhere by worldspace id, in 3D.
		const PatchSpot* SpotFor(uptr cell, std::uint32_t world, const game::Vec3& p, bool& pending)
		{
			pending = false;
			const PatchSpot* best = nullptr;
			double bestD = kPatchSpotRadius;
			if (const auto it = g_patch.cells.find(At<std::uint32_t>(cell, 0x28)); it != g_patch.cells.end()) {
				int sx = 0, sy = 0;
				if (!CellShift(cell, it->second, sx, sy)) { pending = true; return nullptr; }
				const double qx = p.x - sx * kCellSize, qy = p.y - sy * kCellSize;
				for (const auto& s : g_patch.spots) {
					if (s.worldFile != it->second.worldFile || s.worldLocal != it->second.worldLocal) continue;
					const double d = std::hypot(qx - s.x, qy - s.y);
					if (d < bestD) { bestD = d; best = &s; }
				}
				return best;
			}
			for (const auto& s : g_patch.spots) {
				if (s.world != world) continue;
				const double d = std::sqrt(std::pow(p.x - s.x, 2) + std::pow(p.y - s.y, 2) + std::pow(p.z - s.z, 2));
				if (d < bestD) { bestD = d; best = &s; }
			}
			return best;
		}

		void PatchConsider(uptr r, uptr cell)
		{
			if (!g_patch.active || g_patch.jobs.size() >= 2) return;
			const std::uint32_t id = At<std::uint32_t>(r, 0x28);
			if (g_patch.handled.count(id) || Protected(r) || std::strcmp(StateOf(r), "parked") != 0) return;
			const uptr ws = At<uptr>(cell, off::kCellWorldspace);
			if (!ws) { g_patch.handled.insert(id); return; }
			const auto p = At<game::Vec3>(r, off::kRefPosition);
			const uptr base = At<uptr>(r, 0x98);
			std::uint64_t location = 0, standin = 0, spotKey = 0;
			// Its spot decides first, whatever its base is now (an earlier swap is kept, never rolled again); off a spot only a
			// mod's own vehicle type is a stand-in.
			bool pending = false;
			if (const PatchSpot* spot = SpotFor(cell, At<std::uint32_t>(ws, 0x28), p, pending)) { location = spot->location; standin = spot->standin; spotKey = spot->key; }
			else if (pending) return;  // the cell's shift is not known yet: decided on a later scan
			else for (const auto& [form, key] : g_patch.standins) if (form == base) { location = LocationKey(At<std::uint32_t>(ws, 0x28)); standin = key; }
			if (!standin) { g_patch.handled.insert(id); return; }
			const uptr want = PatchPick(location, standin);
			if (!want || want == At<uptr>(r, 0x98)) {
				g_patch.handled.insert(id);
				++g_patch.kept;
				return;
			}
			if (const uptr player = Ptr(addr::kPlayerCharacterPtr)) {
				const auto pp = At<game::Vec3>(player, off::kRefPosition);
				if (std::hypot(pp.x - p.x, pp.y - p.y) < kPatchPlayerClear) return;  // later, when the player is not beside it
			}
			// Only the game's own base-change functions (the reference's or the actor's override) are ever called.
			const auto slot = reinterpret_cast<uptr>((*reinterpret_cast<void***>(r))[kRefSetBaseSlot]) - game::Base();
			if (slot != kSetBaseRefr && slot != kSetBaseActor) {
				g_patch.handled.insert(id);
				logger::Write("Placement patch: vehicle %08X skipped (its base-change slot is %08llX, not a known one)", id, static_cast<unsigned long long>(slot));
				return;
			}
			g_patch.handled.insert(id);
			g_patch.jobs.push_back({ id, want, 0, GetTickCount64(), spotKey ? spotKey : standin });
		}

		// One step of a swap. 0: disable it; 1: once its 3D is gone (or after 3 s), the new base and enable. Returns the old
		// base form id after a swap, 0 while waiting, ~0u on an exception (the vehicle is enabled again, unchanged).
		std::uint32_t PatchStep(uptr r, SwapJob& j, ULONGLONG now)
		{
			__try {
				if (j.stage == 0) {
					Disable(r);
					j.stage = 1;
					j.since = now;
					return 0;
				}
				if (At<uptr>(r, off::kRefLoadedData) != 0 && now - j.since <= 3000) return 0;
				const uptr old = At<uptr>(r, 0x98);
				reinterpret_cast<void (*)(uptr, uptr, bool)>((*reinterpret_cast<void***>(r))[kRefSetBaseSlot])(r, j.base, true);
				EngineFn<void (*)(uptr)>(addr::kEnableReference)(r);
				j.stage = 9;
				return old ? At<std::uint32_t>(old, 0x28) : 0xFFFFFFFEu;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				__try { EngineFn<void (*)(uptr)>(addr::kEnableReference)(r); } __except (EXCEPTION_EXECUTE_HANDLER) {}
				j.stage = 9;
				return ~0u;
			}
		}

		// Each frame: the running swaps (at most two at a time).
		void PatchJobs()
		{
			const ULONGLONG now = GetTickCount64();
			static ULONGLONG lastEnable = 0;
			bool enabledThisFrame = false;
			for (auto& j : g_patch.jobs) {
				const uptr r = Lookup(j.ref);
				if (!r || !Readable(r, 0xD0)) { j.stage = 9; continue; }
				const int stage = j.stage;
				// The base change and enable load the new vehicle's model: one per frame, at least 500 ms apart (2026-10-21 11:06-11:07:
				// four swaps after a load fell in the minute with a 144 ms frame).
				if (stage == 1 && (enabledThisFrame || now - lastEnable < 500)) continue;
				LARGE_INTEGER f, t0, t1;
				QueryPerformanceFrequency(&f);
				QueryPerformanceCounter(&t0);
				const std::uint32_t old = PatchStep(r, j, now);
				QueryPerformanceCounter(&t1);
				const double ms = 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart);
				if (stage == 1 && j.stage == 9) { enabledThisFrame = true; lastEnable = now; }
				if (old == ~0u) logger::Write("Placement patch: vehicle %08X raised an exception in stage %d; enabled again, left as it was", j.ref, stage);
				else if (old) {
					++g_patch.swaps;
					const auto p = At<game::Vec3>(r, off::kRefPosition);
					logger::Write("Placement patch: #%u vehicle %08X at (%.0f, %.0f, %.0f): base %08X -> %08X (%s), spot / stand-in %016llX; %.1f ms", g_patch.swaps, j.ref, p.x, p.y, p.z,
						old, At<std::uint32_t>(j.base, 0x28), ModelHint(j.base).c_str(), static_cast<unsigned long long>(j.spot), ms);
				}
			}
			std::erase_if(g_patch.jobs, [](const SwapJob& j) { return j.stage == 9; });
		}

		// ---- The mod's own placements (2026-10-21, the user recorded them with scanner T; scripts/bake_own_spots.py -> own_spots.h).
		// Each spot gets one vehicle when its cell is attached; a row gets one type (placementlogic::Pick with the row as the
		// spot, from the same pool and seed as the patch). The vehicle a spot spawned is remembered in placement-spawned.tsv
		// (spot key -> form id): while it exists anywhere (the player drove it off, an NPC took it) the spot stays empty, so
		// nothing piles up; a spot whose vehicle is gone, or that is free in another save, gets a new one. A vehicle already
		// standing on the spot counts as its vehicle. Same switch as the patch ([placement] enabled).
		constexpr float kOwnOccupied = 3.0f;      // m: a vehicle this close to a spot is standing on it
		constexpr float kOwnPlayerClear = 20.0f;  // m: not popped in right beside the player
		constexpr ULONGLONG kOwnSpawnGap = 500;   // ms between spawns (each loads a model; the patch's spacing)
		constexpr const char* kPlacementsPlugin = "FlyingVehicleOverhaul - Placements.esm";

		struct OwnSpot { std::uint32_t cell = 0; game::Vec3 p{}; float heading = 0; std::uint64_t key = 0, row = 0, location = 0; bool done = false, queued = false; };
		struct Own
		{
			bool ready = false, active = false;
			bool retire = false;  // the placements plugin is loaded: the vehicles spawned into saves before it are retired
			std::vector<OwnSpot> spots;
			std::map<std::uint64_t, std::uint32_t> spawned;  // spot key -> the vehicle it spawned
			std::vector<std::size_t> queue;
			ULONGLONG lastSpawn = 0;
			unsigned count = 0;
		} g_own;

		std::wstring OwnSpawnedPath()
		{
			PWSTR docs = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return {};
			std::wstring w = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessVehicle";
			CoTaskMemFree(docs);
			SHCreateDirectoryExW(nullptr, w.c_str(), nullptr);
			return w + L"\\placement-spawned.tsv";
		}

		void SaveOwnSpawned()
		{
			FILE* f = nullptr;
			if (_wfopen_s(&f, OwnSpawnedPath().c_str(), L"w") != 0 || !f) return;
			std::fprintf(f, "spot\tvehicle\n");
			for (const auto& [key, id] : g_own.spawned) std::fprintf(f, "%016llX\t%08X\n", static_cast<unsigned long long>(key), id);
			std::fclose(f);
		}

		void PrepareOwn()
		{
			if (g_own.ready || !g_patch.ready) return;
			g_own.ready = true;
			uptr files[std::size(ownspots::kFiles)] = {};
			for (std::size_t f = 0; f < std::size(ownspots::kFiles); ++f) files[f] = LoadedFile(ownspots::kFiles[f]);
			for (const auto& s : ownspots::kSpots) {
				if (!files[s.file]) continue;
				OwnSpot o;
				o.cell = RuntimeId(files[s.file], s.cellLocal);
				if (!o.cell) continue;
				o.p = { s.x, s.y, s.z };
				o.heading = s.heading; o.key = s.key; o.row = s.row; o.location = s.location;
				g_own.spots.push_back(o);
			}
			FILE* f = nullptr;
			if (_wfopen_s(&f, OwnSpawnedPath().c_str(), L"r") == 0 && f) {
				char line[256];
				while (std::fgets(line, sizeof(line), f)) {
					unsigned long long key = 0;
					unsigned id = 0;
					if (std::sscanf(line, "%llx %x", &key, &id) == 2 && id) g_own.spawned[key] = id;
				}
				std::fclose(f);
			}
			// The placements now ship as plugin references (FlyingVehicleOverhaul - Placements.esm, scripts/make_placements_esm.py; Carfield's
			// way). With it loaded nothing is spawned at run time any more (runtime-created vehicles live in the save, and on the
			// 2026-10-21 runs every vehicle there loaded frozen), and the ones spawned before are retired.
			if (LoadedFile(kPlacementsPlugin)) {
				g_own.active = false;
				g_own.retire = !g_own.spawned.empty();
				logger::Write("Own placements: %s loaded: placed by the plugin, nothing spawned at run time; %zu earlier spawned vehicles to retire",
					kPlacementsPlugin, g_own.spawned.size());
				return;
			}
			// Runtime spawning stays off (2026-10-21: every runtime-created vehicle loaded frozen and never got its physics vehicle; the
			// placements plugin crashed the cell streaming in its first run). Earlier spawned vehicles are retired.
			g_own.active = false;
			g_own.retire = !g_own.spawned.empty();
			logger::Write("Own placements: runtime spawning off (plugin not loaded); %zu earlier spawned vehicles to retire", g_own.spawned.size());
		}

		// Every 2 s while some remain: a vehicle the runtime placements spawned is disabled once it is loaded, unless someone uses it.
		void OwnRetire()
		{
			if (!g_own.retire) return;
			bool changed = false;
			for (auto it = g_own.spawned.begin(); it != g_own.spawned.end();) {
				const uptr r = Lookup(it->second);
				if (!r || !Readable(r, 0xD0)) { ++it; continue; }  // not loaded (yet)
				if (!IsVehicleBase(At<uptr>(r, 0x98))) {        // that id is something else in this save
					it = g_own.spawned.erase(it);
					changed = true;
					continue;
				}
				if (Protected(r) || std::strcmp(StateOf(r), "parked") != 0) { ++it; continue; }  // in use: later
				if (!(At<std::uint32_t>(r, 0x20) & 0x800)) Disable(r);
				logger::Write("Own placements: retired runtime-spawned vehicle %08X (spot %016llX); the plugin places that spot now", it->second,
					static_cast<unsigned long long>(it->first));
				g.ownVehicles.erase(it->second);
				it = g_own.spawned.erase(it);
				changed = true;
			}
			if (changed) SaveOwnSpawned();
			if (g_own.spawned.empty()) g_own.retire = false;
		}

		bool IsLiveVehicle(std::uint32_t id)
		{
			const uptr r = id ? Lookup(id) : 0;
			return r && Readable(r, 0xD0) && !(At<std::uint32_t>(r, 0x20) & 0x800) && IsVehicleBase(At<uptr>(r, 0x98));
		}

		// Every 2 s after the cell scan: spots of attached cells that need a vehicle are queued.
		void OwnConsider()
		{
			if (!g_own.active) return;
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			if (!player) return;
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			for (std::size_t i = 0; i < g_own.spots.size(); ++i) {
				OwnSpot& s = g_own.spots[i];
				if (s.done || s.queued) continue;
				const uptr cell = Lookup(s.cell);
				if (!cell || !Readable(cell, 0x60) || At<std::uint8_t>(cell, off::kCellState) != 10) continue;
				if (const auto it = g_own.spawned.find(s.key); it != g_own.spawned.end() && IsLiveVehicle(it->second)) {
					g.ownVehicles.insert(it->second);
					s.done = true;
					continue;
				}
				if (std::any_of(g.loadedVehicles.begin(), g.loadedVehicles.end(), [&](const game::Vec3& v) {
						return std::fabs(v.x - s.p.x) < kOwnOccupied && std::fabs(v.y - s.p.y) < kOwnOccupied && std::fabs(v.z - s.p.z) < kOwnOccupied; })) {
					s.done = true;
					continue;
				}
				if (std::hypot(pp.x - s.p.x, pp.y - s.p.y) < kOwnPlayerClear) continue;
				s.queued = true;
				g_own.queue.push_back(i);
			}
		}

		// Each frame while spots are queued: one spawn, kOwnSpawnGap apart.
		void OwnSpawn()
		{
			const ULONGLONG now = GetTickCount64();
			if (now - g_own.lastSpawn < kOwnSpawnGap) return;
			const std::size_t i = g_own.queue.front();
			g_own.queue.erase(g_own.queue.begin());
			OwnSpot& s = g_own.spots[i];
			s.queued = false;
			const uptr cell = Lookup(s.cell);
			if (!cell || !Readable(cell, 0x60) || At<std::uint8_t>(cell, off::kCellState) != 10) return;  // unloaded meanwhile: asked again later
			g_own.lastSpawn = now;
			s.done = true;
			const uptr base = PatchPick(s.location, s.row);
			const std::uint32_t id = Spawn(cell, game::Vec3{ s.p.x, s.p.y, s.p.z }, game::Vec3{ 0.0f, 0.0f, s.heading }, base);
			if (!id) {
				logger::Write("Own placements: spot %016llX in cell %08X: nothing placed", static_cast<unsigned long long>(s.key), s.cell);
				return;
			}
			g.ownVehicles.insert(id);
			g_own.spawned[s.key] = id;
			SaveOwnSpawned();
			++g_own.count;
			logger::Write("Own placements: #%u vehicle %08X (%08X %s) at (%.1f, %.1f, %.1f) heading %.3f, spot %016llX, row %016llX, cell %08X", g_own.count, id,
				At<std::uint32_t>(base, 0x28), ModelHint(base).c_str(), s.p.x, s.p.y, s.p.z, s.heading, static_cast<unsigned long long>(s.key),
				static_cast<unsigned long long>(s.row), s.cell);
		}

		// Records every real vehicle in the cell; never creates one (the user, 2026-10-17 20:2x: no swapping at run time;
		// the props become real vehicles in Vehicles.esm itself). Duplicates an older build created at a parked spot are
		// still cleared once (one kept).
		void ScanCell(uptr cell, int& budget)
		{
			(void)budget;
			uptr* refs = nullptr;
			std::uint32_t count = 0;
			if (!CellRefs(cell, refs, count)) return;
			const std::uint32_t hi = g.modIndex >= 0 ? std::uint32_t(g.modIndex) << 24 : 0xFFFFFFFFu;
			std::vector<uptr> parked;
			for (std::uint32_t i = 0; i < count && i < 8192; ++i) {
				const uptr r = refs[i];
				if (!r || !Readable(r, 0xD0)) continue;
				const uptr base = At<uptr>(r, 0x98);
				if (!base) continue;
				if (IsVehicleBase(base)) {
					if (!(At<std::uint32_t>(r, 0x20) & 0x800)) {
						g.loadedVehicles.push_back(At<game::Vec3>(r, off::kRefPosition));
						Register(r, cell);
						PatchConsider(r, cell);
						if (g.modIndex >= 0 && !Protected(r) && std::strcmp(StateOf(r), "parked") == 0) parked.push_back(r);
					}
					continue;
				}
				const std::uint32_t id = At<std::uint32_t>(base, 0x28);
				if ((id & 0xFF000000) != hi) continue;
				const std::uint32_t lo = id & 0xFFFFFF;
				if (lo != kStaticRev8 && lo != kStaticDeimog && lo != kStaticMoonJumper) continue;
				const std::uint32_t pid = At<std::uint32_t>(r, 0x28);
				if (g.done.count(pid) || (At<std::uint32_t>(r, 0x20) & 0x800)) continue;
				g.done.insert(pid);
				VehicleAtSpot(At<game::Vec3>(r, off::kRefPosition), refs, count);  // clears old duplicates only
			}
			// Vehicles standing inside each other (the 11:05 run at Waggoner Farm: an old swap's V-47 in the pack-in's REV-8 at
			// (656, 635)) push each other about; the extra created one goes (ClearStacked keeps the spot's own vehicle).
			if (parked.size() > 1) ClearStacked(parked);
		}
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		if (!g_patch.jobs.empty()) PatchJobs();  // every frame while a swap runs
		if (!g_own.queue.empty()) OwnSpawn();
		if (now - g.last < 2000) return;
		g.last = now;
		RecordingPlacements();
		FindMod();
		DiscoverVehicles();
		PreparePatch();
		PrepareOwn();
		if (g.dirty && now - g.lastSave >= 30000) {
			g.lastSave = now;
			g.dirty = false;
			SaveRegistry();
		}
		const uptr tes = Ptr(addr::kTESPtr);
		if (!tes) return;
		int budget = kSwapsPerTick;
		g.loadedVehicles.clear();
		__try {
			const auto entries = At<uptr*>(tes, off::kTESGridCells);
			const auto n = std::min(At<std::uint32_t>(tes, off::kTESGridCount), 64u);
			for (std::uint32_t i = 0; entries && i < n && budget > 0; ++i) {
				const uptr e = entries[i];
				if (!e) continue;
				const auto state = At<std::uint32_t>(e, off::kGridEntryState);
				if ((((state & 0x70000000u) + 0xD0000000u) & 0xEFFFFFFFu) != 0) continue;
				const uptr cell = At<uptr>(e, off::kGridEntryCell);
				if (!cell || At<std::uint8_t>(cell, off::kCellState) != 10) continue;  // attached (fully loaded)
				ScanCell(cell, budget);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			logger::Write("Real vehicles: exception while scanning cells");
			return;
		}
		OwnConsider();
		OwnRetire();
	}

	bool RecordingPlacements()
	{
		static ULONGLONG checked=0;static bool enabled=false,told=false;
		const auto now=GetTickCount64();if(checked&&now-checked<1000)return enabled;checked=now;
		wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
		std::wstring flag=exe;const auto slash=flag.find_last_of(L"\\/");if(slash==std::wstring::npos)return false;
		flag.resize(slash+1);flag+=L"Data\\SFSE\\Plugins\\SeamlessVehicle.placement-recording";
		const bool active=GetFileAttributesW(flag.c_str())!=INVALID_FILE_ATTRIBUTES;
		if(!told||enabled!=active){enabled=active;told=true;logger::Write("Placement recorder: %s; scanner taps %s",enabled?"ON":"off",enabled?"save native vehicle parking positions":"use normal controls");}
		return enabled;
	}

	bool RecordPlacement(double x,double y,double z,double yaw,bool estimated,const char* label)
	{
		if(!RecordingPlacements())return false;
		if(estimated||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(yaw)||std::fabs(x)>1e7||std::fabs(y)>1e7||std::fabs(z)>1e7){
			notify::Show("Placement not saved: aim at a visible surface");return false;
		}
		const auto player=Ptr(addr::kPlayerCharacterPtr),cell=player?At<uptr>(player,off::kRefParentCell):0;
		const auto world=cell?At<uptr>(cell,off::kCellWorldspace):0;
		if(!world){notify::Show("Placement not saved: an exterior worldspace is required");return false;}
		const auto worldId=At<std::uint32_t>(world,off::kFormId);
		const std::uint32_t mask=(worldId>>24)==0xFE?0xFFF:(worldId>>24)==0xFD?0xFFFF:0xFFFFFF;
		const auto local=worldId&mask;std::string worldFile="UNRESOLVED";
		for(const auto name:placementspots::kFiles)if(const auto file=LoadedFile(name);file&&RuntimeId(file,local)==worldId){worldFile=name;break;}
		const auto worldName=EditorId(world);
		const auto key=placementlogic::Fnv(worldFile+"|"+worldName+"|"+std::to_string(local)+"|"+
			std::to_string(std::llround(x*2))+"|"+std::to_string(std::llround(y*2))+"|"+std::to_string(std::llround(z*2)));
		PWSTR docs=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&docs)))return false;
		std::wstring path=std::wstring(docs)+L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessVehicle";CoTaskMemFree(docs);
		SHCreateDirectoryExW(nullptr,path.c_str(),nullptr);path+=L"\\placement-markers.tsv";
		FILE* read=nullptr;bool existing=false;
		if(_wfopen_s(&read,path.c_str(),L"r")==0&&read){
			char line[2048];while(std::fgets(line,sizeof(line),read)){unsigned long long saved=0;if(std::sscanf(line,"%llx",&saved)==1&&saved==key){std::fclose(read);notify::Show("That placement is already recorded");return true;}}
			existing=std::ftell(read)>0;std::fclose(read);
		}
		std::string name=label?label:"";for(auto&c:name)if(c=='\t'||c=='\r'||c=='\n')c=' ';
		FILE* output=nullptr;if(_wfopen_s(&output,path.c_str(),L"a")!=0||!output){notify::Show("Placement could not be saved");return false;}
		if(!existing)std::fprintf(output,"key\tworld_file\tworld_local\tworld_runtime\tworld_editor_id\tplayer_cell_runtime\tx\ty\tz\tyaw\tlabel\n");
		const int written=std::fprintf(output,"%016llX\t%s\t%06X\t%08X\t%s\t%08X\t%.6f\t%.6f\t%.6f\t%.7f\t%s\n",
			static_cast<unsigned long long>(key),worldFile.c_str(),local,worldId,worldName.c_str(),At<std::uint32_t>(cell,off::kFormId),x,y,z,yaw,name.c_str());
		const int closed=std::fclose(output);if(written<0||closed){notify::Show("Placement write failed");return false;}
		logger::Write("Native vehicle placement: key %016llX world %s:%06X (%s), player cell %08X, position (%.6f, %.6f, %.6f), heading %.7f [%s]",
			static_cast<unsigned long long>(key),worldFile.c_str(),local,worldName.c_str(),At<std::uint32_t>(cell,off::kFormId),x,y,z,yaw,name.c_str());
		notify::Show("Vehicle placement saved");return true;
	}

	std::uintptr_t PlaceOwner(std::uintptr_t ref)
	{
		if (!ref) return 0;
		const uptr cell = At<uptr>(ref, off::kRefParentCell);
		uptr* refs = nullptr;
		std::uint32_t count = 0;
		__try {
			if (!cell || !CellRefs(cell, refs, count)) return 0;
			return LocationOwner(cell, refs, count);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	void ForEachLoaded(void (*fn)(std::uintptr_t ref, void* ctx), void* ctx)
	{
		const uptr tes = Ptr(addr::kTESPtr);
		if (!tes || !g.haveVehicles) return;
		__try {
			const auto entries = At<uptr*>(tes, off::kTESGridCells);
			const auto n = std::min(At<std::uint32_t>(tes, off::kTESGridCount), 64u);
			for (std::uint32_t i = 0; entries && i < n; ++i) {
				const uptr e = entries[i];
				if (!e) continue;
				const auto state = At<std::uint32_t>(e, off::kGridEntryState);
				if ((((state & 0x70000000u) + 0xD0000000u) & 0xEFFFFFFFu) != 0) continue;
				const uptr cell = At<uptr>(e, off::kGridEntryCell);
				if (!cell || At<std::uint8_t>(cell, off::kCellState) != 10) continue;
				uptr* refs = nullptr;
				std::uint32_t count = 0;
				if (!CellRefs(cell, refs, count)) continue;
				for (std::uint32_t k = 0; k < count && k < 8192; ++k) {
					const uptr r = refs[k];
					if (!r || !Readable(r, 0xD0) || (At<std::uint32_t>(r, 0x20) & 0x800)) continue;
					if (IsVehicleBase(At<uptr>(r, 0x98))) fn(r, ctx);
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	void Reset()
	{
		g.done.clear();
		g.cellOwner.clear();
		g.registry.clear();  // form ids belong to one save
		g_patch.handled.clear();
		g_patch.locations.clear();
		g_patch.shifts.clear();  // a planet's overlay belongs to the save's world
		g_patch.jobs.clear();  // a swap in flight belongs to the save being left (a disabled vehicle there stays disabled)
		g.ownVehicles.clear();
		g_own.queue.clear();
		for (auto& s : g_own.spots) s.done = s.queued = false;  // each save decides its spots again
	}
}
