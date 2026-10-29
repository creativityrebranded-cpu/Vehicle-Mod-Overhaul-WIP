#include "trafficpool.h"
#include "addresses.h"
#include "features.h"
#include "fvo_addon_api.h"
#include "fvopaths.h"
#include "game.h"
#include "log.h"
#include "npcvehicles.h"
#include "runtimeforms.h"
#include "vehicleworld.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// The spawn and seat steps are the ones confirmed in game on 2026-10-25 (notes/road-traffic-20261025/RESULT.md, run 02:05):
// the game's create-reference for the vehicle and the driver (the driver's creation data runs SeatNewborn inside the
// creation), then, once both have 3D, the driver must really sit (sit state > 0), else the instant seat (SnapNow) every
// second, six tries at most.
namespace trafficpool
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T> T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }
		template <class F> F EngineFn(std::uint32_t rva) { return reinterpret_cast<F>(game::Base() + rva); }
		constexpr std::uint32_t kFormFlags = 0x20, kDisabledFlag = 0x800, kDeletedFlag = 0x20, kBaseForm = 0x98;
		constexpr std::uint32_t kDriverBase = 0x001591EA;  // Starfield.esm LvlCitizen_UC (what the road traffic spawned)
		constexpr std::uint32_t kRev8 = 0xFE001033;        // SFBGS004.esm Rev8, as the addon matches it
		constexpr int kMaxSpots = 6;
		constexpr ULONGLONG kCreateGapMs = 500, kLoadWaitMs = 120000, kSeatEveryMs = 1000, kTripRetryMs = 2000, kWatchMs = 1000;
		constexpr int kRestarts = 5;          // a pair whose trip ended at its spot is started again this often
		constexpr ULONGLONG kIdleMs = 10000;  // ... after standing this long with no trip
		constexpr double kNearSpot = 20.0;    // m: "near its spot" (a driver out elsewhere ended a trip: ordinary traffic now)
		constexpr int kSeatTries = 6;
		constexpr double kSpawnNear = 350.0;  // m: spawn only while the spot is this close (its cell is loaded)
		constexpr double kSpawnFar = 40.0;    // m: and not right beside the player
		constexpr double kSpotClear = 7.0;    // m: no other vehicle this close to the spot

		std::mutex g_lock;
		std::string g_wantWorld;
		std::vector<FvoPoolSpot> g_wantSpots;
		FvoPoolTrip g_trip = nullptr;

		enum class St { Unknown, Empty, Created, Ready, Started, Failed };
		struct Pair {
			St st = St::Unknown;
			std::uint32_t vehicle = 0, driver = 0;
			ULONGLONG at = 0, seatAt = 0, tripAt = 0, watchAt = 0, idleSince = 0;
			int tries = 0, restarts = 0;
		};
		std::string g_world;
		std::vector<FvoPoolSpot> g_spots;
		std::vector<Pair> g_pairs;
		ULONGLONG g_createdAt = 0;
		bool g_off = false;  // the driver base made no actor: stop for the session

		uptr Ref(std::uint32_t id)
		{
			if (!id) return 0;
			__try { const uptr r = runtimeforms::LookupChecked(id); return r && At<std::uint32_t>(r, off::kFormId) == id ? r : 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}
		std::uint32_t BaseId(uptr ref)
		{
			__try { const uptr b = At<uptr>(ref, kBaseForm); return b ? At<std::uint32_t>(b, off::kFormId) : 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}
		bool Usable(uptr ref, bool& has3D)
		{
			has3D = false;
			__try {
				if (At<std::uint32_t>(ref, kFormFlags) & (kDisabledFlag | kDeletedFlag)) return false;
				const uptr ld = At<uptr>(ref, off::kRefLoadedData);
				has3D = ld && At<uptr>(ld, off::kLoadedData3D);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		void Disable(std::uint32_t id)
		{
			const uptr r = Ref(id);
			if (!r) return;
			__try { EngineFn<void (*)(uptr, char)>(addr::kDisableReference)(r, 0); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		uptr RefFromHandle(std::uint32_t handle)
		{
			uptr ref = 0; uptr* out = &ref;
			struct { uptr vtbl; uptr** capture; } lookup{ game::Base() + addr::kHandleToRefFunctorVtbl, &out };
			__try { EngineFn<void (*)(uptr, std::uint32_t, void*)>(addr::kHandleLookup)(Ptr(addr::kRefHandleTablePtr), handle, &lookup); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
			return ref;
		}

		// One-step seated creation: the create-reference calls the creation data's vftable[0](data, newRef) after the new
		// reference has its base and place, before it joins a cell (stock: an empty ret). Ours seats the new driver.
		struct Birth { uptr vehicle = 0; int result = -99; } g_birth;
		void SeatInside(uptr ref)
		{
			__try { g_birth.result = npcvehicles::SeatNewborn(ref, g_birth.vehicle); }
			__except (EXCEPTION_EXECUTE_HANDLER) { g_birth.result = -1; }
		}
		void OnCreated(void*, void* ref) { if (g_birth.vehicle && ref) SeatInside(reinterpret_cast<uptr>(ref)); }
		void* const kSeatedCreation[1] = { reinterpret_cast<void*>(&OnCreated) };

		std::uint32_t Create(uptr base, uptr cell, uptr ws, double x, double y, double z, double heading, const void* vtbl = nullptr)
		{
			const uptr data = Ptr(addr::kDataHandlerPtr);
			if (!base || !cell || !ws || !data) return 0;
			alignas(16) std::uint8_t d[0xC0] = {};
			const auto dp = reinterpret_cast<uptr>(d);
			At<uptr>(dp, 0x00) = vtbl ? reinterpret_cast<uptr>(vtbl) : game::Base() + addr::kNewRefrDataVtbl;
			At<float>(dp, 0x10) = float(x); At<float>(dp, 0x14) = float(y); At<float>(dp, 0x18) = float(z);
			At<float>(dp, 0x24) = float(heading); At<float>(dp, 0x28) = 1.0f;
			At<uptr>(dp, 0x30) = base; At<uptr>(dp, 0x38) = cell; At<uptr>(dp, 0x40) = ws;
			At<std::uint8_t>(dp, 0x8C) = 1; At<std::uint8_t>(dp, 0x8D) = 1; At<std::uint32_t>(dp, 0x98) = 1;
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			__try { EngineFn<void* (*)(uptr, std::uint64_t*, void*)>(addr::kCreateReference)(data, &handle, d); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
			const uptr ref = RefFromHandle(static_cast<std::uint32_t>(handle));
			if (!ref) return 0;
			std::uint32_t id = 0;
			__try { id = At<std::uint32_t>(ref, off::kFormId); EngineFn<void (*)(uptr)>(addr::kRefRelease)(ref); }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			return id;
		}
		bool PlayerWorld(uptr& cell, uptr& ws, game::Vec3& pos, char (&name)[201])
		{
			name[0] = 0;
			__try {
				const uptr player = Ptr(addr::kPlayerCharacterPtr);
				cell = player ? At<uptr>(player, off::kRefParentCell) : 0;
				ws = cell ? At<uptr>(cell, off::kCellWorldspace) : 0;
				if (!ws) return false;
				pos = At<game::Vec3>(player, off::kRefPosition);
				auto** vt = *reinterpret_cast<void***>(ws);
				const char* n = reinterpret_cast<const char* (*)(uptr)>(vt[0x4D])(ws);
				int i = 0;
				for (; n && n[i] && i < 200; ++i) name[i] = n[i];
				name[i] = 0;
				return i > 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		// trafficpool.ini [<world>] spotN = vehicle driver: the pairs made for this world in earlier visits (any save; checked).
		const std::wstring& Ini() { static const std::wstring p = fvopaths::File(L"trafficpool.ini"); return p; }
		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
		void LoadIds(int i, Pair& p)
		{
			wchar_t text[64]{};
			GetPrivateProfileStringW(Wide(g_world).c_str(), (L"spot" + std::to_wstring(i)).c_str(), L"", text, 64, Ini().c_str());
			unsigned v = 0, d = 0;
			if (std::swscanf(text, L"%x %x", &v, &d) == 2) { p.vehicle = v; p.driver = d; }
		}
		void SaveIds(int i, const Pair& p)
		{
			wchar_t text[64];
			std::swprintf(text, 64, L"%08X %08X", p.vehicle, p.driver);
			WritePrivateProfileStringW(Wide(g_world).c_str(), (L"spot" + std::to_wstring(i)).c_str(), text, Ini().c_str());
		}

		// Traffic NPCs (2026-10-28 18:27 run: two pool drivers were "lost" while their vehicles flew 300-450 m out; keep in sight held
		// the vehicles, but the game's full-LOD call does nothing for actors (FUN_140B36640 returns for 'K'), so the drivers were
		// unloaded and gone). Pool vehicles and their drivers are a fixed pool, made once and reused: both persistent (FUN_140B36C40
		// ref, 1: flag 0x400, moved into the world's persistent cell, as named NPCs are), so distance never unloads them. The drivers
		// stay ordinary mortal citizens (the user: "not be essential and unable to be killed"); the essential bit (actor +0x378 bit 18)
		// an earlier build set on them is cleared.
		constexpr std::uint32_t kPersistentFlag = 0x400, kEssentialBit = 1u << 18;
		bool MakeTrafficRef(std::uint32_t id, bool actor)
		{
			const uptr r = Ref(id);
			if (!r) return false;
			__try {
				if (!(At<std::uint32_t>(r, kFormFlags) & kPersistentFlag))
					EngineFn<void (*)(uptr, char, char)>(addr::kMarkerPost1)(r, 1, 0);
				if (actor) At<std::uint32_t>(r, 0x378) &= ~kEssentialBit;
				return (At<std::uint32_t>(r, kFormFlags) & kPersistentFlag) != 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		void MakeTrafficPair(int i, const Pair& p)
		{
			const bool v = MakeTrafficRef(p.vehicle, false), d = MakeTrafficRef(p.driver, true);
			logger::Write("Traffic pool: spot %d: vehicle %08X %s, driver %08X %s", i, p.vehicle, v ? "persistent" : "NOT persistent",
				p.driver, d ? "persistent" : "NOT persistent");
		}

		void Fail(int i, Pair& p, const char* why)
		{
			Disable(p.driver); Disable(p.vehicle);
			logger::Write("Traffic pool: spot %d (%s): vehicle %08X / driver %08X disabled: %s", i, g_world.c_str(), p.vehicle, p.driver, why);
			p.st = St::Failed;
		}

		// The REV-8 base: the placements' own vehicle near the spot (its runtime id depends on the load order), else 0xFE001033.
		uptr VehicleBase(const FvoPoolSpot& s, const world::Snapshot& snap)
		{
			const uptr rev8 = Ref(kRev8);
			std::uint32_t best = 0; double bestD = 200.0;
			for (std::uint32_t k = 0; k < snap.count; ++k) {
				const auto& v = snap.v[k];
				if (v.who != world::VehicleInfo::Who::Parked || !v.baseId) continue;
				const double d = std::hypot(v.pos[0] - s.x, v.pos[1] - s.y);
				if (d < bestD && (v.baseId == kRev8 || (v.baseId >> 24) == 0xFE)) { bestD = d; best = v.baseId; }
			}
			return best ? runtimeforms::Lookup(best) : rev8;
		}

		void Step(int i, Pair& p, const FvoPoolSpot& s, uptr cell, uptr ws, const game::Vec3& pp, const world::Snapshot& snap, ULONGLONG now)
		{
			switch (p.st) {
			case St::Unknown: {
				LoadIds(i, p);
				const uptr v = Ref(p.vehicle), d = Ref(p.driver);
				bool v3 = false, d3 = false;
				if (v && d && Usable(v, v3) && Usable(d, d3) && npcvehicles::LooksLikeActor(d) && !npcvehicles::IsDead(d)) {
					// Made in an earlier visit and still there: reuse it, never make another.
					const bool seated = npcvehicles::VehicleOf(d) == v;
					p.st = seated ? St::Created : St::Started;
					p.at = now;
					MakeTrafficPair(i, p);
					logger::Write("Traffic pool: spot %d (%s): reusing vehicle %08X / driver %08X (%s)", i, g_world.c_str(), p.vehicle, p.driver,
						seated ? "driver aboard: starting it" : "driver out: it is ordinary traffic now");
				}
				else { p.vehicle = p.driver = 0; p.st = St::Empty; }
				return;
			}
			case St::Empty: {
				if (now - g_createdAt < kCreateGapMs) return;
				const double away = std::hypot(s.x - pp.x, s.y - pp.y);
				if (away > kSpawnNear || away < kSpawnFar) return;
				for (std::uint32_t k = 0; k < snap.count; ++k)
					if (std::hypot(snap.v[k].pos[0] - s.x, snap.v[k].pos[1] - s.y) < kSpotClear && std::fabs(snap.v[k].pos[2] - s.z) < 6.0) return;  // occupied: wait
				const uptr base = VehicleBase(s, snap), driverBase = runtimeforms::Lookup(kDriverBase);
				if (!base || !driverBase) { logger::Write("Traffic pool: spot %d: no REV-8 base (%p) or driver base (%p)", i, (void*)base, (void*)driverBase); p.st = St::Failed; return; }
				g_createdAt = now;
				p.vehicle = Create(base, cell, ws, s.x, s.y, s.z + 0.3, s.heading);
				const uptr v = Ref(p.vehicle);
				if (!v) { logger::Write("Traffic pool: spot %d: the vehicle was not created", i); p.st = St::Failed; return; }
				g_birth = { v, -99 };
				p.driver = Create(driverBase, cell, ws, s.x + 2.5 * std::cos(s.heading), s.y - 2.5 * std::sin(s.heading), s.z + 0.3, s.heading, kSeatedCreation);
				const int born = g_birth.result;
				g_birth = {};
				if (!p.driver || !npcvehicles::LooksLikeActor(Ref(p.driver))) {
					Fail(i, p, "the driver base made no actor; the pool stops for this session");
					g_off = true; return;
				}
				SaveIds(i, p);
				MakeTrafficPair(i, p);
				p.st = St::Created; p.at = now; p.tries = 0; p.seatAt = 0;
				logger::Write("Traffic pool: spot %d (%s): created vehicle %08X (base %08X) and driver %08X at (%.0f, %.0f, %.0f), seat at creation %d",
					i, g_world.c_str(), p.vehicle, BaseId(v), p.driver, s.x, s.y, s.z, born);
				return;
			}
			case St::Created: {
				const uptr v = Ref(p.vehicle), d = Ref(p.driver);
				bool v3 = false, d3 = false;
				if (!v || !d || !Usable(v, v3) || !Usable(d, d3) || npcvehicles::IsDead(d)) { Fail(i, p, "vehicle or driver gone"); return; }
				if (!v3 || !d3) { if (now - p.at > kLoadWaitMs) Fail(i, p, "no 3D after 120 s"); return; }
				const bool sitting = npcvehicles::VehicleOf(d) == v && npcvehicles::SitState(d) > 0;
				if (sitting) {
					logger::Write("Traffic pool: spot %d: driver %08X sits in vehicle %08X (sit state %d)", i, p.driver, p.vehicle, npcvehicles::SitState(d));
					p.st = St::Ready; p.tripAt = 0; return;
				}
				if (p.seatAt && now - p.seatAt < kSeatEveryMs) return;
				if (p.tries >= kSeatTries) { Fail(i, p, "the driver would not sit after 6 instant seats"); return; }
				p.seatAt = now; ++p.tries;
				const int r = npcvehicles::SnapNow(d, v);
				logger::Write("Traffic pool: spot %d: instant seat -> %d, sit state %d (try %d)", i, r, npcvehicles::SitState(d), p.tries);
				return;
			}
			case St::Ready: {
				if (p.tripAt && now - p.tripAt < kTripRetryMs) return;
				p.tripAt = now;
				const uptr v = Ref(p.vehicle), d = Ref(p.driver);
				if (!v || !d || npcvehicles::VehicleOf(d) != v) { p.st = St::Created; p.at = now; p.tries = 0; return; }  // fell out: seat again
				if (!g_trip) return;
				const auto vp = At<game::Vec3>(v, off::kRefPosition);
				FvoTripClaim claim{};
				if (!g_trip(i, p.vehicle, vp.x, vp.y, vp.z, &claim) || !claim.step) return;  // the addon waits (a slot, a route)
				const bool ok = npcvehicles::StartScriptedSeated(d, v, claim);
				logger::Write("Traffic pool: spot %d: vehicle %08X with driver %08X starts a %s -> %s", i, p.vehicle, p.driver, claim.name ? claim.name : "trip", ok ? "driving" : "refused");
				if (ok) { p.st = St::Started; p.watchAt = now; }
				return;
			}
			case St::Started: {
				// Watch: the trip ended at its spot (2026-10-28 17:49 run: the game's AI asked every pool driver to dismount at once while
				// the player was indoors; the exit was refused, the drivers stayed seated, the trips were over). After kIdleMs with no trip:
				// a driver still aboard gets a new trip, one out of the vehicle is seated again first. kRestarts times.
				if (now - p.watchAt < kWatchMs) return;
				p.watchAt = now;
				const uptr v = Ref(p.vehicle), d = Ref(p.driver);
				if (!v || !d || npcvehicles::IsDead(d) || npcvehicles::UsesVehicle(p.vehicle)) { p.idleSince = 0; return; }
				const auto vp = At<game::Vec3>(v, off::kRefPosition);
				if (std::hypot(vp.x - s.x, vp.y - s.y) > kNearSpot || p.restarts >= kRestarts) return;  // elsewhere: a trip ended there
				if (!p.idleSince) { p.idleSince = now; return; }
				if (now - p.idleSince < kIdleMs) return;
				p.idleSince = 0;
				++p.restarts;
				const bool aboard = npcvehicles::VehicleOf(d) == v;
				logger::Write("Traffic pool: spot %d: vehicle %08X has had no trip for %llu s (driver %s): %s (restart %d)", i, p.vehicle,
					static_cast<unsigned long long>(kIdleMs / 1000), aboard ? "aboard" : "out", aboard ? "new trip" : "seating again", p.restarts);
				if (aboard) { p.st = St::Ready; p.tripAt = 0; }
				else { p.st = St::Created; p.at = now; p.tries = 0; p.seatAt = 0; }
				return;
			}
			default: return;
			}
		}
	}

	void Reset()
	{
		g_world.clear(); g_spots.clear(); g_pairs.clear(); g_birth = {};
	}

	void Tick()
	{
		if (g_off || !features::On(features::Id::Traffic)) return;
		uptr cell = 0, ws = 0; game::Vec3 pp{}; char name[201];
		if (!PlayerWorld(cell, ws, pp, name)) return;
		{
			std::lock_guard lk(g_lock);
			if (g_wantWorld != name) return;  // the addon's spots are for another world (or none sent)
			if (g_world != name) {
				g_world = name; g_spots = g_wantSpots; g_pairs.assign(g_spots.size(), Pair{});
				logger::Write("Traffic pool: %s: %zu spawn spot(s)", g_world.c_str(), g_spots.size());
			}
		}
		world::Snapshot snap;
		if (!world::Vehicles(snap)) return;
		const ULONGLONG now = GetTickCount64();
		for (int i = 0; i < int(g_pairs.size()); ++i) Step(i, g_pairs[i], g_spots[i], cell, ws, pp, snap, now);
	}
}

extern "C" __declspec(dllexport) void FVO_PoolSet(const char* world, const FvoPoolSpot* spots, int count, FvoPoolTrip trip)
{
	std::lock_guard lk(trafficpool::g_lock);
	trafficpool::g_wantWorld = world ? world : "";
	trafficpool::g_wantSpots.assign(spots, spots + std::clamp(count, 0, trafficpool::kMaxSpots));
	trafficpool::g_trip = trip;
}
