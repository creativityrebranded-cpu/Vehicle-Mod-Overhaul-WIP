#include "realvehicles.h"
#include "runtimeforms.h"
#include "runtimecells.h"
#include "parkedlayout.h"
#include "citytraffic.h"
#include "features.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "npcvehicles.h"
#include "vehicleflight.h"
#include "placement_spots.h"
#include "own_spots.h"  // recording/deleting authored parking spots, not randomization
#include "placementlogic.h"
#include "placementedit.h"
#include "notify.h"
#include "fvopaths.h"

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


		constexpr std::uint32_t kGetOwner = 0x0050AAD0, kSetOwner = 0x0050CED0;
		constexpr std::uint32_t kPlayerVehicleBase = 0x05F04398;
		// The vehicle keyword (DAT_145F04028): the game's vehicle test FUN_140EC72A0 is ref vtable +0x200 and HasKeyword(ref, it).
		constexpr std::uint32_t kVehicleKeyword = 0x05F04028;
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
            bool sourceReady=false,sourceMissingReported=false;
            std::array<std::uint32_t,parkedlayout::kProps.size()> sourceProps{};
			bool haveVehicles = false;
			std::uint8_t vehicleType = 0;  // the vehicle bases' form type (furniture), learned from the player vehicle's base
			std::vector<VehicleForm> vehicles;
			uptr rev8 = 0, deimog = 0, moonJumper = 0;
			std::vector<uptr> pool;      // every vehicle form (vanilla and mods): the ones carrying the vehicle keyword
			ULONGLONG lastFind = 0, last = 0;

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

		// A reference's flags (+0x20) and base form (+0x98), the two fields the cell scans need (2026-10-24, the user: stutters in
		// New Atlantis): reading them directly costs two cache lines where Readable(r, 0xD0) touched 26 places per reference.
		bool RefHead(uptr r, std::uint32_t& flags, uptr& base)
		{
			__try { flags = *reinterpret_cast<const std::uint32_t*>(r + 0x20); base = *reinterpret_cast<const uptr*>(r + 0x98); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

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

		uptr Lookup(std::uint32_t id) {return runtimeforms::Lookup(id);}

		std::uint8_t TypeOf(uptr form) { return form ? At<std::uint8_t>(form, 0x2E) : 0; }

		// Resolve the declared content dependency, then validate its expected form
        // roles. Never infer ownership from coincidental offsets in another plugin.
		void FindMod()
		{
			const ULONGLONG now = GetTickCount64();
			if (g.sourceReady || (g.lastFind && now - g.lastFind < 30000)) return;
			g.lastFind = now;
            for(std::size_t i=0;i<g.sourceProps.size();++i)g.sourceProps[i]=runtimeforms::Resolve(parkedlayout::kProps[i]);
            const uptr a=Lookup(g.sourceProps[0]),b=Lookup(g.sourceProps[1]),c=Lookup(g.sourceProps[2]);
            const auto pack=Lookup(runtimeforms::Resolve(parkedlayout::kPack));
            if(a&&b&&c&&pack&&TypeOf(a)==TypeOf(b)&&TypeOf(a)==TypeOf(c)&&TypeOf(pack)!=TypeOf(a)){
                g.sourceReady=true;const auto base=Ptr(kPlayerVehicleBase);
                logger::Write("Real vehicles: Parked Vehicles (%s) resolved; real vehicle base %08X",parkedlayout::kPack.plugin,base?At<std::uint32_t>(base,0x28):0);
                return;
            }
			if (!g.sourceMissingReported) {
				g.sourceMissingReported=true;
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
			g.vehicleType = type;
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
		bool CellRefs(uptr cell,uptr*& data,std::uint32_t& count) {return runtimecells::References(cell,data,count);}

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
			if (g.vehicleType && TypeOf(base) != g.vehicleType) return false;  // vehicles are furniture: one byte rules out a city's statics
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
			std::wstring path = fvopaths::Dir();
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

        // Native plugin placements retain their original vehicle types.
        uptr LoadedFile(const char* name) { return runtimeforms::LoadedPlugin(name); }
        std::uint32_t RuntimeId(uptr file, std::uint32_t local) { return runtimeforms::RuntimeId(file,local); }
        struct Own {
            bool ready=false, retire=false;
            std::map<std::uint64_t,std::uint32_t> spawned;
        } g_own;

		std::wstring OwnSpawnedPath()
		{
			PWSTR docs = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return {};
			std::wstring w = fvopaths::Dir();
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
            if (g_own.ready || !g.haveVehicles) return;
            g_own.ready=true;
            FILE* f=nullptr;
            if (_wfopen_s(&f,OwnSpawnedPath().c_str(),L"r")==0 && f) {
                char line[256];
                while (std::fgets(line,sizeof(line),f)) {
                    unsigned long long key=0; unsigned id=0;
                    if (std::sscanf(line,"%llx %x",&key,&id)==2 && id) g_own.spawned[key]=id;
                }
                std::fclose(f);
            }
            g_own.retire=!g_own.spawned.empty();
            logger::Write("Parked vehicles: plugin-native types; runtime randomizer removed; %zu legacy spawned vehicles to retire",g_own.spawned.size());
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

		// Records every real vehicle in the cell; never creates one (the user, 2026-10-17 20:2x: no swapping at run time;
		// the props become real vehicles in Vehicles.esm itself). Duplicates an older build created at a parked spot are
		// still cleared once (one kept).
		void ScanCell(uptr cell, int& budget)
		{
			(void)budget;
			uptr* refs = nullptr;
			std::uint32_t count = 0;
			if (!CellRefs(cell, refs, count)) return;
			std::vector<uptr> parked;
			for (std::uint32_t i = 0; i < count && i < 8192; ++i) {
				const uptr r = refs[i];
				std::uint32_t flags = 0;
				uptr base = 0;
				if (!r || !RefHead(r, flags, base) || !base) continue;
				if (IsVehicleBase(base)) {
					if (!(At<std::uint32_t>(r, 0x20) & 0x800)) {
						g.loadedVehicles.push_back(At<game::Vec3>(r, off::kRefPosition));
						Register(r, cell);
						if (g.sourceReady && !Protected(r) && std::strcmp(StateOf(r), "parked") == 0) parked.push_back(r);
					}
					continue;
				}
				const std::uint32_t id = At<std::uint32_t>(base, 0x28);
                if(!g.sourceReady||std::find(g.sourceProps.begin(),g.sourceProps.end(),id)==g.sourceProps.end())continue;
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

    namespace {
        bool g_registryActive=false;
        framework::ScanCursor g_registryCursor;
        int g_registryBudget=0;
        struct RegistryScan {
            uptr cell=0; bool failed=false;
            unsigned Count() const {return runtimecells::GridSize();}
            framework::Cell Describe(unsigned index) {
                cell=runtimecells::Attached(index);return {cell,cell?1u:0u};
            }
            void Visit(unsigned) {
                __try {if(g_registryBudget>0)ScanCell(cell,g_registryBudget);}
                __except(EXCEPTION_EXECUTE_HANDLER){failed=true;}
            }
        };
    }
	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
        if(!g_registryActive){
            if(now-g.last<2000)return;
            g.last=now;
            RecordingPlacements();FindMod();DiscoverVehicles();PrepareOwn();
            if(g.dirty&&now-g.lastSave>=30000){g.lastSave=now;g.dirty=false;SaveRegistry();}
            g.loadedVehicles.clear();g_registryCursor={};g_registryActive=true;g_registryBudget=kSwapsPerTick;
        }
		const uptr tes = Ptr(addr::kTESPtr);
        if(!tes){g_registryActive=false;g_registryCursor={};return;}
        RegistryScan source;
        const bool complete=framework::ScanStep(g_registryCursor,source.Count(),source,[]{return false;},1);
        if(source.failed){g_registryActive=false;g_registryCursor={};logger::Write("Real vehicles: exception while scanning cells");return;}
        if(!complete)return;
        g_registryActive=false;
		OwnRetire();
	}

    bool CanManagePlacement(std::uint32_t id){
        if(!id||!g.haveVehicles)return false;
        const auto ref=Lookup(id);
        if(!ref||!Readable(ref,0xD0)||!IsVehicleBase(At<uptr>(ref,0x98)))return false;
        if(Protected(ref))return false;
        if(At<std::uint32_t>(ref,0x20)&0x800)return true;
        return std::strcmp(StateOf(ref),"parked")==0&&!npcvehicles::VehicleDriverId(id);
    }

	bool RecordingPlacements()
	{
		static ULONGLONG checked=0;static bool enabled=false,told=false;
		const auto now=GetTickCount64();if(checked&&now-checked<1000)return enabled;checked=now;
		wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
		std::wstring flag=exe;const auto slash=flag.find_last_of(L"\\/");if(slash==std::wstring::npos)return false;
		flag.resize(slash+1);flag+=L"Data\\SFSE\\Plugins\\";
		const bool flagged=GetFileAttributesW((flag+L"FlyingVehicleOverhaul.placement-recording").c_str())!=INVALID_FILE_ATTRIBUTES||
			GetFileAttributesW((flag+L"SeamlessVehicle.placement-recording").c_str())!=INVALID_FILE_ATTRIBUTES;  // the pre-0.73.0 name still counts
		// The spot recorder was the user's build tool (removed 2026-10-28): neither the flag file nor the setting turns it on.
		(void)flagged;
		const bool active=false;
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
		std::wstring path=fvopaths::Dir();CoTaskMemFree(docs);
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

	// ---- Placement mode (2026-10-24, the user): delete a spot, set an area's summon point (placementedit.h) ----
	namespace
	{
		std::wstring PlacementFile(const wchar_t* name)
		{
			PWSTR docs=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&docs)))return {};
			std::wstring path=fvopaths::Dir();CoTaskMemFree(docs);
			SHCreateDirectoryExW(nullptr,path.c_str(),nullptr);return path+L"\\"+name;
		}
		// The player's exterior worldspace (0 indoors) and its editor id (stable between sessions, unlike a planet's FF id).
		uptr PlayerWorld(std::string& name)
		{
			const auto player=Ptr(addr::kPlayerCharacterPtr),cell=player?At<uptr>(player,off::kRefParentCell):0;
			const auto world=cell?At<uptr>(cell,off::kCellWorldspace):0;
			name=world?EditorId(world):std::string();
			return world;
		}
		std::vector<std::string> ReadLines(const std::wstring& path)
		{
			std::vector<std::string> out;FILE* f=nullptr;
			if(path.empty()||_wfopen_s(&f,path.c_str(),L"r")!=0||!f)return out;
			char line[2048];while(std::fgets(line,sizeof(line),f)){std::string s=line;while(!s.empty()&&(s.back()=='\n'||s.back()=='\r'))s.pop_back();out.push_back(s);}
			std::fclose(f);return out;
		}
		bool WriteLines(const std::wstring& path,const std::vector<std::string>& lines)
		{
			FILE* f=nullptr;if(path.empty()||_wfopen_s(&f,path.c_str(),L"w")!=0||!f)return false;
			bool ok=true;for(const auto& l:lines)ok=std::fprintf(f,"%s\n",l.c_str())>=0&&ok;
			return std::fclose(f)==0&&ok;
		}
		std::vector<std::string> Fields(const std::string& line)
		{
			std::vector<std::string> out;std::size_t at=0;
			for(;;){const auto tab=line.find('\t',at);out.push_back(line.substr(at,tab==std::string::npos?std::string::npos:tab-at));if(tab==std::string::npos)break;at=tab+1;}
			return out;
		}
		std::vector<placementedit::SummonPoint> LoadSummonPoints()
		{
			std::vector<placementedit::SummonPoint> out;
			for(const auto& l:ReadLines(PlacementFile(L"summon-points.tsv"))){placementedit::SummonPoint p;if(placementedit::Parse(l,p))out.push_back(p);}
			return out;
		}
	}

	bool HasPlacementNear(double x,double y,double z)
	{
        if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return false;
        std::string worldName;const uptr world=PlayerWorld(worldName);if(!world)return false;
        // Main-thread presentation query. Refresh at most once a second; never read authoring files from GFx.
        static uptr cachedWorld=0;static ULONGLONG at=0;static std::vector<placementedit::Point> points;
        const auto now=GetTickCount64();
        if(world!=cachedWorld||!at||now-at>=1000){
            cachedWorld=world;at=now;points.clear();
            const auto lines=ReadLines(PlacementFile(L"placement-markers.tsv"));
            for(std::size_t i=1;i<lines.size();++i){const auto f=Fields(lines[i]);if(f.size()>=10&&f[4]==worldName)
                points.push_back({std::atof(f[6].c_str()),std::atof(f[7].c_str()),std::atof(f[8].c_str())});}
            const auto removed=ReadLines(PlacementFile(L"placement-removed.tsv"));
            uptr files[std::size(ownspots::kFiles)]={};
            for(std::size_t i=0;i<std::size(files);++i)files[i]=LoadedFile(ownspots::kFiles[i]);
            for(const auto& spot:ownspots::kSpots){
                const auto file=spot.file<std::size(files)?files[spot.file]:0;
                const auto cell=file?Lookup(RuntimeId(file,spot.cellLocal)):0;
                if(!cell||At<uptr>(cell,off::kCellWorldspace)!=world)continue;
                char key[24];std::snprintf(key,sizeof(key),"%016llX",static_cast<unsigned long long>(spot.key));
                if(std::any_of(removed.begin(),removed.end(),[&](const std::string& line){return line.rfind(key,0)==0;}))continue;
                points.push_back({spot.x,spot.y,spot.z});
            }
        }
        return placementedit::Pick(points,{x,y,z})>=0;
    }
	bool DeletePlacement(double x,double y,double z)
	{
		std::string worldName;const uptr world=PlayerWorld(worldName);
		if(!world){notify::Show("Placement mode: an exterior worldspace is required");return false;}
		// Candidates: the recorded markers in this world, and the baked spots whose cell lies in it.
		const auto markersPath=PlacementFile(L"placement-markers.tsv");
		auto lines=ReadLines(markersPath);
		std::vector<placementedit::Point> points;std::vector<int> source;  // >= 0: a marker line, < 0: -(baked index + 1)
		for(std::size_t i=1;i<lines.size();++i){
			const auto f=Fields(lines[i]);if(f.size()<10||f[4]!=worldName)continue;
			points.push_back({std::atof(f[6].c_str()),std::atof(f[7].c_str()),std::atof(f[8].c_str())});source.push_back(int(i));
		}
		uptr files[std::size(ownspots::kFiles)]={};
		for(std::size_t f=0;f<std::size(ownspots::kFiles);++f)files[f]=LoadedFile(ownspots::kFiles[f]);
		for(std::size_t i=0;i<std::size(ownspots::kSpots);++i){
			const auto& s=ownspots::kSpots[i];
			const uptr file=s.file<std::size(files)?files[s.file]:0;
			const uptr cell=file?Lookup(RuntimeId(file,s.cellLocal)):0;
			if(!cell||At<uptr>(cell,off::kCellWorldspace)!=world)continue;
			points.push_back({s.x,s.y,s.z});source.push_back(-int(i)-1);
		}
		const int pick=placementedit::Pick(points,{x,y,z});
		if(pick<0){notify::Show("No saved spot there (aim within %.0f m of one)",placementedit::kPickRadius);return false;}
		const int src=source[std::size_t(pick)];
		const auto& p=points[std::size_t(pick)];
		if(src>=0){
			lines.erase(lines.begin()+src);
			if(!WriteLines(markersPath,lines)){notify::Show("Placement could not be deleted");return false;}
			logger::Write("Placement mode: deleted recorded spot at (%.2f, %.2f, %.2f) in %s",p.x,p.y,p.z,worldName.c_str());
			notify::Show("Parking spot deleted");return true;
		}
		// A baked spot: listed for the next bake (scripts/bake_own_spots.py skips it).
		const auto& s=ownspots::kSpots[std::size_t(-src-1)];
		const auto removedPath=PlacementFile(L"placement-removed.tsv");
		auto removed=ReadLines(removedPath);
		char key[24];std::snprintf(key,sizeof(key),"%016llX",static_cast<unsigned long long>(s.key));
		for(const auto& l:removed)if(l.rfind(key,0)==0){notify::Show("That spot is already removed (after the next rebuild)");return true;}
		if(removed.empty())removed.push_back("key\tworld_editor_id\tx\ty\tz");
		char line[256];std::snprintf(line,sizeof(line),"%s\t%s\t%.3f\t%.3f\t%.3f",key,worldName.c_str(),double(s.x),double(s.y),double(s.z));
		removed.push_back(line);
		if(!WriteLines(removedPath,removed)){notify::Show("Placement could not be deleted");return false;}
		logger::Write("Placement mode: baked spot %s at (%.2f, %.2f, %.2f) listed in placement-removed.tsv",key,p.x,p.y,p.z);
		notify::Show("Parking spot removed (after the next rebuild)");return true;
	}

	bool SetSummonPoint(double x,double y,double z,double yaw)
	{
		std::string worldName;
		if(!PlayerWorld(worldName)||worldName.empty()){notify::Show("Placement mode: an exterior worldspace is required");return false;}
		if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(yaw)){notify::Show("Summon point not set: aim at a visible surface");return false;}
		auto points=LoadSummonPoints();
		const int replaced=placementedit::SetSummon(points,{worldName,x,y,z,yaw});
		std::vector<std::string> lines;for(const auto& p:points)lines.push_back(placementedit::Line(p));
		if(!WriteLines(PlacementFile(L"summon-points.tsv"),lines)){notify::Show("Summon point could not be saved");return false;}
		logger::Write("Placement mode: summon point (%.2f, %.2f, %.2f) heading %.3f in %s%s",x,y,z,yaw,worldName.c_str(),replaced?" (replaced this area's old one)":"");
		notify::Show(replaced?"Summon point moved here":"Summon point set");return true;
	}

	bool SummonPointNear(double px,double py,double& x,double& y,double& z,double& yaw)
	{
		std::string worldName;
		if(!PlayerWorld(worldName)||worldName.empty())return false;
		const auto points=LoadSummonPoints();
		const int i=placementedit::SummonFor(points,worldName,px,py);
		if(i<0)return false;
		const auto& p=points[std::size_t(i)];x=p.x;y=p.y;z=p.z;yaw=p.yaw;return true;
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





	namespace {
        struct LoadedScan {
            uptr* refs=nullptr;
            void (*fn)(uptr,void*)=nullptr;void* ctx=nullptr;
            unsigned Count() const {return g.haveVehicles?runtimecells::GridSize():0;}
            framework::Cell Describe(unsigned index) {
                refs=nullptr;const auto cell=runtimecells::Attached(index);std::uint32_t count=0;
                return cell&&runtimecells::References(cell,refs,count)
                    ?framework::Cell{cell,std::min(count,8192u)}:framework::Cell{};
            }
            void Visit(unsigned index) const {
                __try {
                    const uptr ref=refs?refs[index]:0;uptr base=0;std::uint32_t flags=0;
                    if(ref&&RefHead(ref,flags,base)&&!(flags&0x800)&&IsVehicleBase(base))fn(ref,ctx);
                } __except(EXCEPTION_EXECUTE_HANDLER) {}
            }
        };
    }
    const std::vector<std::uintptr_t>& VehicleTypes(){return g.pool;}
    bool ForEachLoadedStep(void (*fn)(std::uintptr_t ref, void* ctx), void* ctx, framework::ScanCursor& cursor)
    {
        LoadedScan source{nullptr,fn,ctx};
        LARGE_INTEGER began{},frequency{};QueryPerformanceCounter(&began);QueryPerformanceFrequency(&frequency);
        return framework::ScanStep(cursor,source.Count(),source,[&] {
            LARGE_INTEGER at{};QueryPerformanceCounter(&at);
            return double(at.QuadPart-began.QuadPart)>double(frequency.QuadPart)*0.0005;
        });
    }

	void Reset()
	{
        g_registryActive=false;g_registryCursor={};g.loadedVehicles.clear();
        g.sourceReady=false;g.sourceMissingReported=false;g.sourceProps={};g.lastFind=0;
		g.done.clear();
		g.cellOwner.clear();
		g.registry.clear();  // form ids belong to one save
		g.ownVehicles.clear();
	}
}
