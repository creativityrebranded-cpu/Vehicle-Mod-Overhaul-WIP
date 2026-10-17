#include "realvehicles.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "npcvehicles.h"
#include "vehicleflight.h"

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
				logger::Write("Real vehicles:   %08X %s%s", v.id, v.name.empty() ? "(no name or model found)" : v.name.c_str(), v.form == base ? "  <- player default" : "");
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

		bool IsVehicleBase(uptr base)
		{
			if (!base) return false;
			for (const auto& v : g.vehicles) {
				if (v.form == base) return true;
			}
			return base == g.rev8;
		}

		void Disable(uptr ref);

		// A vehicle that must stay: the player's own, the one the player is in, one an NPC job uses.
		bool Protected(uptr r)
		{
			const std::uint32_t id = At<std::uint32_t>(r, 0x28);
			if (id == vehicleflight::OwnRoverId() || npcvehicles::UsesVehicle(id)) return true;
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

		// A real vehicle at the prop's place and heading, owned by the location.
		bool Spawn(uptr prop, uptr cell, uptr owner, uptr base)
		{
			const uptr ws = At<uptr>(cell, off::kCellWorldspace);
			if (!base) return false;
			const auto p = At<game::Vec3>(prop, off::kRefPosition);
			const auto r = At<game::Vec3>(prop, off::kRefRotation);
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
				if (ref && owner) EngineFn<void (*)(uptr, uptr, uptr, char)>(kSetOwner)(At<uptr>(ref, 0xC8), ref, owner, 0);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				logger::Write("Real vehicles: placing raised an exception");
				return false;
			}
			if (!ref) return false;
			++g.swaps;
			logger::Write("Real vehicles: #%u real vehicle %08X (base %08X) for prop %08X at (%.0f, %.0f, %.0f), owner %08X", g.swaps, At<std::uint32_t>(ref, 0x28),
				At<std::uint32_t>(base, 0x28), At<std::uint32_t>(prop, 0x28), p.x, p.y, p.z, owner ? At<std::uint32_t>(owner, 0x28) : 0);
			EngineFn<void (*)(uptr)>(addr::kRefRelease)(ref);
			return true;
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

		// Records every real vehicle in the cell; never creates one. Duplicates an older build created at a parked
		// spot are still cleared once (one kept).
		void ScanCell(uptr cell, int& budget)
		{
			(void)budget;
			uptr* refs = nullptr;
			std::uint32_t count = 0;
			if (!CellRefs(cell, refs, count)) return;
			const std::uint32_t hi = g.modIndex >= 0 ? std::uint32_t(g.modIndex) << 24 : 0xFFFFFFFFu;
			for (std::uint32_t i = 0; i < count && i < 8192; ++i) {
				const uptr r = refs[i];
				if (!r || !Readable(r, 0xD0)) continue;
				const uptr base = At<uptr>(r, 0x98);
				if (!base) continue;
				if (IsVehicleBase(base)) {
					if (!(At<std::uint32_t>(r, 0x20) & 0x800)) Register(r, cell);
					continue;
				}
				const std::uint32_t id = At<std::uint32_t>(base, 0x28);
				if ((id & 0xFF000000) != hi) continue;
				const std::uint32_t lo = id & 0xFFFFFF;
				if (lo != kStaticRev8 && lo != kStaticDeimog && lo != kStaticMoonJumper) continue;
				const std::uint32_t pid = At<std::uint32_t>(r, 0x28);
				if (g.done.count(pid)) continue;
				g.done.insert(pid);
				VehicleAtSpot(At<game::Vec3>(r, off::kRefPosition), refs, count);  // clears old duplicates only
			}
		}
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		if (now - g.last < 2000) return;
		g.last = now;
		FindMod();
		DiscoverVehicles();
		if (g.dirty && now - g.lastSave >= 30000) {
			g.lastSave = now;
			g.dirty = false;
			SaveRegistry();
		}
		const uptr tes = Ptr(addr::kTESPtr);
		if (!tes) return;
		int budget = kSwapsPerTick;
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
		}
	}

	void Reset()
	{
		g.done.clear();
		g.cellOwner.clear();
		g.registry.clear();  // form ids belong to one save
	}
}
