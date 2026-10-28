#include "content.h"

#include "addresses.h"
#include "anchor.h"
#include "game.h"
#include "handoff.h"
#include "log.h"
#include "notify.h"
#include "telemetry.h"

#include <Windows.h>

#include <atomic>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace content
{
	std::atomic<unsigned long long> g_lastPostSteps{ 0 };
	unsigned long long LastPostSteps() { return g_lastPostSteps.load(); }

	namespace
	{
		// Spawning per zone: landing points spread over the whole zone, one terrain block apart (a landing covers
		// about a block: 1-2 placements 3-4 cells out plus 2-3 requests per block), so the count follows the zone's
		// size. Points are generated in order of time to arrival; the zone at a random TTA ahead is queued early.
		constexpr float kPointSpacing = 1200.0f;     // metres (uBlockSize 10 + merge 2 cells)
		constexpr float kSearchSides = 2.2f;         // candidate square half-width, in zone sides
		constexpr float kMinPointDistance = 500.0f;  // never generate right on top of the player
		constexpr float kMaxPointDistance = 3200.0f; // the content area (play area) around the player reaches ~3.8 km
		constexpr float kLandingClear = 800.0f;      // the game's own landing already covered this around the origin
		constexpr float kDeleteSides = 2.5f;         // zones this many sides away lose their content
		constexpr double kTtaMin = 20.0, kTtaMax = 40.0;  // seconds: random time to arrival for the prediction
		constexpr float kPredictMin = 1000.0f, kPredictMax = 10000.0f;
		constexpr float kWalkSpeed = 5.0f;           // m/s floor for arrival estimates
		constexpr ULONGLONG kPointGapMs = 500;       // one landing point per half second at most
		constexpr int kPointsPerFinish = 6;          // run the landing's post-steps after this many points
		constexpr ULONGLONG kScanMs = 2000;
		constexpr ULONGLONG kPredictMs = 1000;

		// BGSPlanetContentManager layout (from FUN_1418b1c60 / FUN_1418bb240 / FUN_1418b30d0).
		constexpr std::uint32_t kPcmLock = 0x300;
		constexpr std::uint32_t kPcmPlanetEntries = 0x378;   // 0x20-byte buckets {uint32 planetId, ..., ptr +8, int next +0x18 (-1 empty)}
		constexpr std::uint32_t kPcmPlanetCapacity = 0x380;
		constexpr std::uint32_t kPlanetBlockEntries = 0x20;  // same bucket shape, value -> BSTArray<record*> {uint32 count, ptr +8}
		constexpr std::uint32_t kPlanetBlockCapacity = 0x28;
		constexpr std::uint32_t kRecordForm = 0x10;          // the overlay's instanced form (null once removed)
		constexpr std::uint32_t kRecordSkip = 0x30;          // the game's clear skips records with this set
		constexpr std::uint32_t kRecordDefinition = 0x18;    // what the content is (kept while dormant); record+0 = its lat/long
		constexpr int kRestoreCells = 35;                    // dormant records within this many cells come back

		struct Zone
		{
			std::uint64_t key = 0;
			float x = 0, y = 0;
			bool generated = false;   // points queued (or done)
			bool landing = false;     // pseudo-zone: content that existed before we started tracking
			ULONGLONG lastDelete = 0;
			ULONGLONG queuedAt = 0;   // age of the oldest unprocessed point in this zone
			std::vector<std::pair<float, float>> points;  // landing points still to generate
			int done = 0;
		};

		struct State
		{
			std::vector<Zone> zones;
			int current = -1;
			std::unordered_map<std::uintptr_t, std::uint64_t> dormant;  // records we put to sleep -> their zone
			std::uint32_t planet = 0;
			ULONGLONG lastScan = 0, lastGenerate = 0, lastPredict = 0;
			int sinceFinish = 0;
			float vx = 0, vy = 0;
			game::Vec3 lastPos{};
		} g;
		std::mt19937 g_rng{ 0x5EA11E55u };
		struct OperationTrace
		{
			const char* name;
			std::uint64_t startedUs;
			explicit OperationTrace(const char* operation) : name(operation), startedUs(telemetry::NowUs())
			{
				telemetry::Record("content_op", "\"name\":\"%s\",\"phase\":\"start\"", name);
			}
			~OperationTrace()
			{
				telemetry::Record("content_op", "\"name\":\"%s\",\"phase\":\"end\",\"us\":%llu", name,
					static_cast<unsigned long long>(telemetry::NowUs() - startedUs));
			}
		};
		void ReportBacklog(const char* reason, ULONGLONG now)
		{
			static ULONGLONG last = 0;
			if (now - last < 5000) return;
			last = now;
			size_t points = 0;
			ULONGLONG oldest = 0;
			for (const auto& zone : g.zones) {
				points += zone.points.size();
				if (!zone.points.empty() && zone.queuedAt && (!oldest || zone.queuedAt < oldest)) oldest = zone.queuedAt;
			}
			if (points) {
				telemetry::Record("content_backlog", "\"reason\":\"%s\",\"points\":%zu,\"zones\":%zu,\"oldest_ms\":%llu,\"since_finish\":%d",
					reason, points, g.zones.size(), static_cast<unsigned long long>(oldest ? now - oldest : 0), g.sinceFinish);
			}
		}

		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		std::uintptr_t Manager() { return Ptr(addr::kPlanetManagerPtr); }
		std::uintptr_t Pcm() { return Ptr(addr::kContentManagerPtr); }

		// The PCM's lock, taken and released exactly as the game's own clear does.
		void Lock(std::uintptr_t pcm) { EngineFn<void (*)(std::uintptr_t)>(addr::kRWLockWriteB)(pcm + kPcmLock); }
		void Unlock(std::uintptr_t pcm)
		{
			auto* lock = reinterpret_cast<volatile LONG*>(pcm + kPcmLock);
			if ((static_cast<std::uint32_t>(lock[1]) & 0x7FFF0000u) == 0x10000u) {
				lock[0] = 0;
				const LONG prev = InterlockedExchangeAdd(&lock[1], -0x10001);
				if (static_cast<std::uint16_t>(prev) > 1) {
					WakeByAddressSingle(const_cast<LONG*>(&lock[1]));
				}
			}
			else {
				InterlockedExchangeAdd(&lock[1], -0x20000);
			}
		}

		// Calls f(record) for every overlay record of the planet (lock held by the caller).
		template <class F>
		void ForEachRecord(std::uintptr_t pcm, std::uint32_t planet, F&& f)
		{
			const auto pEntries = At<std::uintptr_t>(pcm, kPcmPlanetEntries);
			const auto pCap = At<std::uint64_t>(pcm, kPcmPlanetCapacity);
			if (!pEntries || pCap > (1u << 20)) {
				return;
			}
			for (std::uint64_t i = 0; i < pCap; ++i) {
				const auto b = pEntries + i * 0x20;
				if (At<std::int32_t>(b, 0x18) == -1 || At<std::uint32_t>(b, 0) != planet) {
					continue;
				}
				const auto entry = At<std::uintptr_t>(b, 8);
				if (!entry) {
					continue;
				}
				const auto blocks = At<std::uintptr_t>(entry, kPlanetBlockEntries);
				const auto bCap = At<std::uint64_t>(entry, kPlanetBlockCapacity);
				if (!blocks || bCap > (1u << 24)) {
					continue;
				}
				for (std::uint64_t k = 0; k < bCap; ++k) {
					const auto bb = blocks + k * 0x20;
					if (At<std::int32_t>(bb, 0x18) == -1) {
						continue;
					}
					const auto arr = At<std::uintptr_t>(bb, 8);
					if (!arr) {
						continue;
					}
					const auto n = At<std::uint32_t>(arr, 0);
					const auto data = At<std::uintptr_t*>(arr, 8);
					for (std::uint32_t r = 0; data && r < n; ++r) {
						if (data[r]) {
							f(data[r]);
						}
					}
				}
			}
		}

		int ZoneIndex(std::uint64_t key)
		{
			for (int i = 0; i < int(g.zones.size()); ++i) {
				if (!g.zones[i].landing && g.zones[i].key == key) {
					return i;
				}
			}
			return -1;
		}

		void FinishLanding();

		// Planet coordinate helpers (the engine's own: grid, coordinate of a lat/long, cell offset B - A).
		struct Coords
		{
			std::uintptr_t grid = 0;
			std::uint8_t player[0x60]{};
			std::uint8_t ship[0x60]{};
			bool haveShip = false;
		};

		void CoordOf(std::uintptr_t grid, const void* latlong, void* out)
		{
			EngineFn<void* (*)(std::uintptr_t, void*, const void*)>(addr::kCoordFromLatLong)(grid, out, latlong);
		}

		void Offset(std::uintptr_t grid, const void* a, const void* b, std::int32_t d[2])
		{
			EngineFn<void* (*)(std::uintptr_t, std::int32_t*, const void*, const void*)>(addr::kCoordOffset)(grid, d, a, b);
		}

		bool QuestHeld(std::uintptr_t form)
		{
			if (((At<std::uint32_t>(form, 0x20) >> 2) & 1) || At<std::uintptr_t>(form, 0x388)) {
				const auto sys = EngineFn<std::uintptr_t (*)()>(addr::kGameSingleton)();
				alignas(8) std::uint8_t status[16] = {};
				EngineFn<void (*)(std::uintptr_t, void*)>(addr::kQuestSystemStatus)(At<std::uintptr_t>(sys, 0x8B0), status);
				return status[4] && *reinterpret_cast<std::uint32_t*>(status) != 2;
			}
			return false;
		}

		// Brings a dormant record back exactly as the game's block placement does (FUN_1418ad2b0 at 0x1418ad637).
		bool Restore(std::uintptr_t rec, std::uint32_t planet)
		{
			const auto def = At<std::uintptr_t>(rec, kRecordDefinition);
			std::uintptr_t form = def ? At<std::uintptr_t>(def, 0x60) : 0;
			if (form && At<std::uint8_t>(form, 0x2E) != 0x56) {
				form = 0;
			}
			const auto inst = EngineFn<std::uintptr_t (*)(std::uint32_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, int, std::uintptr_t, int, int,
				std::uint32_t, std::uintptr_t)>(addr::kContentInstanceFor)(planet, 0, form, rec, 0, 1, def, 0, 1, At<std::uint32_t>(rec, 0x2C), 0);
			if (!inst) {
				return false;
			}
			if (inst != At<std::uintptr_t>(rec, kRecordForm)) {
				InterlockedExchangeAdd64(reinterpret_cast<volatile LONG64*>(inst + 8), 0x80000200000LL);
				At<std::uintptr_t>(rec, kRecordForm) = inst;  // was empty (dormant)
			}
			EngineFn<void (*)(std::uintptr_t)>(addr::kContentPlaceRecord)(rec);
			return true;
		}

		// Every record of the planet, by its own lat/long (record+0): far from the player -> its instance is removed
		// (the record, i.e. what and where, stays: dormant); dormant and near again -> brought back the same. The
		// ship's zone keeps its content. Counts per zone go to the log.
		void Maintain(std::uintptr_t pcm, std::uintptr_t player, const game::Vec3& pos, double side)
		{
			OperationTrace operation("maintain");
			const auto startedUs = telemetry::NowUs();
			const auto mgr = Manager();
			alignas(16) std::uint8_t latlong[32] = {};
			const float fp[3] = { pos.x, pos.y, pos.z };
			EngineFn<void* (*)(std::uintptr_t, void*, const float*)>(addr::kWorldToLatLong)(mgr, latlong, fp);
			Coords c;
			c.grid = EngineFn<std::uintptr_t (*)(void*, std::uint32_t, int)>(addr::kPlanetGridFor)(latlong, g.planet, 1);
			if (!c.grid) {
				return;
			}
			CoordOf(c.grid, latlong, c.player);
			{
				std::uintptr_t ship = 0;
				EngineFn<void (*)(std::uintptr_t, std::uintptr_t*)>(addr::kGetPlayerHomeShip)(player, &ship);
				if (ship) {
					const auto sc = At<std::uintptr_t>(ship, off::kRefParentCell);
					const auto pc = At<std::uintptr_t>(player, off::kRefParentCell);
					if (sc && pc && At<std::uintptr_t>(sc, off::kCellWorldspace) &&
						At<std::uintptr_t>(sc, off::kCellWorldspace) == At<std::uintptr_t>(pc, off::kCellWorldspace)) {
						alignas(16) std::uint8_t sl[32] = {};
						const auto sp = At<game::Vec3>(ship, off::kRefPosition);
						const float sf[3] = { sp.x, sp.y, sp.z };
						EngineFn<void* (*)(std::uintptr_t, void*, const float*)>(addr::kWorldToLatLong)(mgr, sl, sf);
						CoordOf(c.grid, sl, c.ship);
						c.haveShip = true;
					}
					EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(ship);
				}
			}
			const int zoneCells = side > 0 ? int(side / 100.0) : 36;
			const int deleteCells = int(zoneCells * kDeleteSides);
			std::int32_t shipD[2] = { 0, 0 };
			if (c.haveShip) {
				Offset(c.grid, c.player, c.ship, shipD);
			}
			int removed = 0, kept = 0, restored = 0, live = 0, dormant = 0;
			unsigned recordsSeen = 0;
			std::uint64_t restoreUs = 0;
			std::unordered_map<std::uint64_t, int> removedByZone, restoredByZone;
			Lock(pcm);
			const auto lockedUs = telemetry::NowUs();
			ForEachRecord(pcm, g.planet, [&](std::uintptr_t rec) {
				++recordsSeen;
				if (At<std::uint8_t>(rec, kRecordSkip)) {
					return;
				}
				alignas(16) std::uint8_t rc[0x60] = {};
				CoordOf(c.grid, reinterpret_cast<const void*>(rec), rc);
				std::int32_t d[2];
				Offset(c.grid, c.player, rc, d);
				const int dist = std::max(std::abs(d[0]), std::abs(d[1]));
				const auto inst = At<std::uintptr_t>(rec, kRecordForm);
				const auto def = At<std::uintptr_t>(rec, kRecordDefinition);
				const auto ll = reinterpret_cast<const double*>(rec);
				std::uint64_t key = 0;
				handoff::ZoneKeyAtLatLong(ll[1], ll[0], key);
				if (inst) {
					++live;
					if (dist <= deleteCells) {
						return;
					}
					if (c.haveShip && std::max(std::abs(d[0] - shipD[0]), std::abs(d[1] - shipD[1])) <= zoneCells) {
						return;  // the ship's zone stays a real place
					}
					if (QuestHeld(inst)) {
						++kept;
						return;
					}
					if (EngineFn<char (*)(std::uintptr_t)>(addr::kContentRemoveOverlay)(rec)) {
						++removed;
						++removedByZone[key];
						g.dormant[rec] = key;
					}
					else {
						++kept;  // still in use
					}
				}
				else if (def) {
					++dormant;
					const auto restoreStartUs = dist <= kRestoreCells ? telemetry::NowUs() : 0;
					const bool didRestore = dist <= kRestoreCells && Restore(rec, g.planet);
					if (restoreStartUs) restoreUs += telemetry::NowUs() - restoreStartUs;
					if (didRestore) {
						++restored;
						++restoredByZone[key];
						g.dormant.erase(rec);
					}
				}
			});
			const auto sweptUs = telemetry::NowUs();
			Unlock(pcm);
			telemetry::Record("content_maintain", "\"records\":%u,\"live\":%d,\"dormant\":%d,\"restored\":%d,\"removed\":%d,\"kept\":%d,\"lock_us\":%llu,\"sweep_us\":%llu,\"restore_us\":%llu,\"total_us\":%llu",
				recordsSeen, live, dormant, restored, removed, kept, static_cast<unsigned long long>(lockedUs - startedUs),
				static_cast<unsigned long long>(sweptUs - lockedUs), static_cast<unsigned long long>(restoreUs),
				static_cast<unsigned long long>(telemetry::NowUs() - startedUs));
			for (const auto& [key, n] : removedByZone) {
				logger::Write("Content: zone %d:(%d, %d) left behind: %d content records made dormant (kept for later)", int(key >> 56),
					int((key >> 28) & 0xFFFFFFF), int(key & 0xFFFFFFF), n);
			}
			for (const auto& [key, n] : restoredByZone) {
				logger::Write("Content: zone %d:(%d, %d) near again: %d content records brought back", int(key >> 56), int((key >> 28) & 0xFFFFFFF),
					int(key & 0xFFFFFFF), n);
			}
			if (removed || restored || kept) {
				telemetry::Record("content_maintain", "\"live\":%d,\"dormant\":%d,\"removed\":%d,\"restored\":%d,\"kept\":%d", live, dormant, removed, restored, kept);
			}
			if (removed) {
				notify::Log("Zone content: %d places put to sleep behind you", removed);
			}
			if (restored) {
				notify::Log("Zone content: %d places woke up", restored);
				FinishLanding();
			}
			static ULONGLONG lastLog = 0;
			const ULONGLONG now = GetTickCount64();
			if (now - lastLog > 30000) {
				lastLog = now;
				logger::Write("Content: planet records: %d live, %d dormant", live, dormant);
			}
		}

		// The landing's own sequence (FUN_14076df30): temporary marker, landing-block generation, marker tear-down.
		bool GenerateAt(std::uintptr_t player, float x, float y, float z)
		{
			OperationTrace operation("generate");
			const auto startUs = telemetry::NowUs();
			const auto pcm = Pcm();
			const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
			const auto ws = cell ? At<std::uintptr_t>(cell, off::kCellWorldspace) : 0;
			const auto base = Ptr(addr::kLandingMarkerBasePtr);
			const auto dataHandler = Ptr(addr::kDataHandlerPtr);
			const auto handles = Ptr(addr::kRefHandleTablePtr);
			if (!pcm || !ws || !base || !dataHandler || !handles) {
				logger::Write("Content: cannot generate (pcm %p, worldspace %p, marker base %p)", (void*)pcm, (void*)ws, (void*)base);
				telemetry::Record("content_point_blocked", "\"reason\":\"missing_engine_state\",\"x\":%.0f,\"y\":%.0f", x, y);
				return false;
			}
			alignas(16) std::uint8_t d[0xC0] = {};
			const auto dp = reinterpret_cast<std::uintptr_t>(d);
			At<std::uintptr_t>(dp, 0x00) = game::Base() + addr::kNewRefrDataVtbl;
			At<float>(dp, 0x10) = x;
			At<float>(dp, 0x14) = y;
			At<float>(dp, 0x18) = z;
			At<float>(dp, 0x28) = 1.0f;  // scale
			At<std::uintptr_t>(dp, 0x30) = base;
			At<std::uintptr_t>(dp, 0x38) = cell;
			At<std::uintptr_t>(dp, 0x40) = ws;
			At<std::uint8_t>(dp, 0x8C) = 1;
			At<std::uint8_t>(dp, 0x8D) = 1;
			At<std::uint32_t>(dp, 0x98) = 1;
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			EngineFn<void* (*)(std::uintptr_t, std::uint64_t*, void*)>(addr::kCreateReference)(dataHandler, &handle, d);
			const auto createdUs = telemetry::NowUs();

			std::uintptr_t ref = 0;
			std::uintptr_t* out = &ref;
			struct
			{
				std::uintptr_t vtbl;
				std::uintptr_t** capture;
			} lookup{ game::Base() + addr::kHandleToRefFunctorVtbl, &out };
			EngineFn<void (*)(std::uintptr_t, std::uint32_t, void*)>(addr::kHandleLookup)(handles, static_cast<std::uint32_t>(handle), &lookup);
			if (!ref) {
				logger::Write("Content: the landing marker could not be placed");
				telemetry::Record("content_point_blocked", "\"reason\":\"marker_lookup\",\"x\":%.0f,\"y\":%.0f,\"us\":%llu", x, y,
					static_cast<unsigned long long>(telemetry::NowUs() - startUs));
				return false;
			}
			const auto lookedUpUs = telemetry::NowUs();
			EngineFn<void (*)(std::uintptr_t, std::uintptr_t)>(addr::kContentLandingBlock)(pcm, ref);
			const auto placedUs = telemetry::NowUs();
			At<std::uint16_t>(ref, 0x2C) &= 0xFFBF;
			auto** vt = *reinterpret_cast<void***>(ref);
			reinterpret_cast<void (*)(std::uintptr_t, char)>(vt[0xB8 / 8])(ref, 1);
			EngineFn<void (*)(std::uintptr_t, std::uintptr_t, char)>(addr::kMarkerPost1)(ref, 0, 1);
			EngineFn<void (*)(std::uintptr_t, char)>(addr::kMarkerPost2)(ref, 1);
			reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, char)>(vt[0x538 / 8])(ref, 0, 0, 1);
			EngineFn<void (*)(std::uint32_t, char)>(addr::kMarkerPost3)(static_cast<std::uint32_t>(handle), 0);
			EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(ref);
			const auto finishedUs = telemetry::NowUs();
			telemetry::Record("content_point_stages", "\"x\":%.0f,\"y\":%.0f,\"create_us\":%llu,\"lookup_us\":%llu,\"place_us\":%llu,\"cleanup_us\":%llu,\"total_us\":%llu",
				x, y, static_cast<unsigned long long>(createdUs - startUs), static_cast<unsigned long long>(lookedUpUs - createdUs),
				static_cast<unsigned long long>(placedUs - lookedUpUs), static_cast<unsigned long long>(finishedUs - placedUs),
				static_cast<unsigned long long>(finishedUs - startUs));
			return true;
		}
	}

	namespace
	{
		// Landing points across a zone: a grid around (cx, cy), kept where the zone lattice says the zone is.
		void QueuePoints(int zi, float cx, float cy, float z)
		{
			auto& zn = g.zones[zi];
			zn.generated = true;
			const double side = handoff::ZoneSide();
			if (side <= 0) {
				return;
			}
			const float half = float(side) * kSearchSides;
			const int n = int(half / kPointSpacing);
			for (int a = -n; a <= n; ++a) {
				for (int b = -n; b <= n; ++b) {
					const float x = cx + a * kPointSpacing, y = cy + b * kPointSpacing;
					std::uint64_t key = 0;
					if (!handoff::ZoneKeyAt(x, y, z, key) || key != zn.key) {
						continue;
					}
					if (std::hypot(x, y) < kLandingClear) {
						continue;  // the real landing site
					}
					zn.points.emplace_back(x, y);
				}
			}
			logger::Write("Content: zone #%d queued %zu landing points (zone side %.2f km)", zi, zn.points.size(), side / 1000.0);
			zn.queuedAt = GetTickCount64();
			telemetry::Record("content_queue", "\"zone\":%d,\"points\":%zu", zi, zn.points.size());
		}

		int EnsureZone(std::uint64_t key, float x, float y)
		{
			int idx = ZoneIndex(key);
			if (idx < 0) {
				Zone zn;
				zn.key = key;
				zn.x = x;
				zn.y = y;
				g.zones.push_back(zn);
				idx = int(g.zones.size()) - 1;
			}
			return idx;
		}

		// The landing's post-steps (MoveTo: module slot 7, then slot 8), minus terrain relocation and the content
		// step we ran ourselves: apply overlays, rebuild texture order, flag navmesh regeneration, refresh overlays.
		void FinishLanding()
		{
			OperationTrace operation("finish");
			const auto startUs = telemetry::NowUs();
			const auto mgr = Manager();
			struct
			{
				std::uintptr_t mgr = 0;
				std::uint8_t owned = 0;
			} h;
			EngineFn<void* (*)(void*, std::uintptr_t)>(addr::kPlanetManagerTryLock)(&h, mgr);
			if (!h.owned) {
				telemetry::Record("content_finish_blocked", "\"reason\":\"manager_lock\",\"us\":%llu",
					static_cast<unsigned long long>(telemetry::NowUs() - startUs));
				return;  // busy; try after the next points
			}
			const auto lockedUs = telemetry::NowUs();
			const auto base = game::Base();
			const auto count = At<std::uint32_t>(mgr, off::kMgrModuleCount);
			const auto mods = At<std::uintptr_t*>(mgr, off::kMgrModules);
			std::uintptr_t overlays = 0;
			int ran = 0;
			for (std::uint32_t i = 0; mods && i < count; ++i) {
				const auto m = mods[i];
				if (!m) {
					continue;
				}
				const auto vt = *reinterpret_cast<std::uintptr_t*>(m) - base;
				if (vt == addr::kOverlaysVtbl || vt == addr::kOrderedLandTexturesVtbl || vt == addr::kNavMeshGenVtbl) {
					reinterpret_cast<void (*)(std::uintptr_t)>((*reinterpret_cast<void***>(m))[0x38 / 8])(m);
					++ran;
					if (vt == addr::kOverlaysVtbl) {
						overlays = m;
					}
				}
			}
			if (overlays) {
				reinterpret_cast<void (*)(std::uintptr_t, void*)>((*reinterpret_cast<void***>(overlays))[0x40 / 8])(overlays, &h);
			}
			const auto hooksUs = telemetry::NowUs();
			// Release exactly as the engine does after FUN_14078f480.
			auto* word = reinterpret_cast<volatile LONG*>(mgr + 0x28);
			if ((static_cast<ULONG>(*word) & 0xFFC00000u) == 0x400000u) {
				At<DWORD>(mgr, 0x24) = 0;
				const auto before = static_cast<ULONG>(InterlockedExchangeAdd(word, -0x401000));
				if (before & 0xFFFu) {
					WakeByAddressAll(const_cast<LONG*>(word));
				}
				else if ((before & 0x3FF000u) > 0x1000u) {
					WakeByAddressSingle(const_cast<LONG*>(word));
				}
			}
			else {
				InterlockedExchangeAdd(word, -0x400000);
			}
			g.sinceFinish = 0;
			logger::Write("Content: landing post-steps run (%d module hooks + overlay refresh)", ran);
			g_lastPostSteps = GetTickCount64();
			telemetry::Record("content_finish", "\"modules\":%d,\"lock_us\":%llu,\"hooks_us\":%llu,\"total_us\":%llu", ran,
				static_cast<unsigned long long>(lockedUs - startUs), static_cast<unsigned long long>(hooksUs - lockedUs),
				static_cast<unsigned long long>(telemetry::NowUs() - startUs));
		}
	}

	void OnZoneEnter(std::uint64_t key, float x, float y, float z, float vx, float vy, bool start)
	{
		(void)start;
		const auto mgr = Manager();
		const std::uint32_t planet = mgr ? At<std::uint32_t>(mgr, off::kMgrPlanetId) : 0;
		if (planet != g.planet) {
			g = State{};
			g.planet = planet;
		}
		g.vx = vx;
		g.vy = vy;
		const int idx = EnsureZone(key, x, y);
		g.current = idx;
		if (!g.zones[idx].generated) {
			QueuePoints(idx, x, y, z);
		}
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto mgr = Manager();
		const auto pcm = Pcm();
		if (!player || !mgr || !pcm || At<std::uint8_t>(mgr, off::kMgrState) != 2 || g.planet == 0) {
			ReportBacklog("engine_state", now);
			return;
		}
		if (At<std::uint32_t>(mgr, off::kMgrPlanetId) != g.planet) {
			ReportBacklog("planet_changed", now);
			return;  // OnZoneEnter resets on the next zone
		}
		const auto a = anchor::Current();
		if (!a.ref || !anchor::DemandReady()) {
			ReportBacklog("world_unsettled", now);
			return;
		}
		const auto pos = At<game::Vec3>(a.ref, off::kRefPosition);
		const double side = handoff::ZoneSide();

		// Prediction: the zone at a random time to arrival ahead gets its points queued long before arrival.
		if (now - g.lastPredict >= kPredictMs) {
			const float dt = g.lastPredict ? (now - g.lastPredict) / 1000.0f : 0.0f;
			if (dt > 0.2f && std::hypot(pos.x - g.lastPos.x, pos.y - g.lastPos.y) < 2000.0f) {
				g.vx = (pos.x - g.lastPos.x) / dt;
				g.vy = (pos.y - g.lastPos.y) / dt;
			}
			g.lastPos = pos;
			g.lastPredict = now;
			const float speed = std::hypot(g.vx, g.vy);
			if (speed > 2.0f) {
				const double tta = std::uniform_real_distribution<double>(kTtaMin, kTtaMax)(g_rng);
				const float d = std::clamp(float(speed * tta), kPredictMin, kPredictMax);
				const float px = pos.x + g.vx / speed * d, py = pos.y + g.vy / speed * d;
				std::uint64_t key = 0;
				if (handoff::ZoneKeyAt(px, py, pos.z, key)) {
					const int zi = EnsureZone(key, px, py);
					if (!g.zones[zi].generated) {
						logger::Write("Content: predicted zone #%d %.1f km ahead (TTA %.0f s)", zi, d / 1000.0f, tta);
						QueuePoints(zi, px, py, pos.z);
					}
				}
			}
		}

		// Next landing point: earliest time to arrival (points ahead count as nearer than points behind).
		ReportBacklog(a.ref == player ? "ready_for_placement" : "vehicle_anchor", now);
		if (now - g.lastGenerate >= kPointGapMs && a.ref == player) {
			const auto selectStartUs = telemetry::NowUs();
			const float v = std::hypot(g.vx, g.vy);
			const float speed = std::max(kWalkSpeed, v);
			const float hx = v > 1.0f ? g.vx / v : 0.0f;
			const float hy = v > 1.0f ? g.vy / v : 0.0f;
			int bz = -1;
			size_t bp = 0;
			float best = 1e30f;
			for (int zi = 0; zi < int(g.zones.size()); ++zi) {
				const auto& pts = g.zones[zi].points;
				for (size_t k = 0; k < pts.size(); ++k) {
					const float dx = pts[k].first - pos.x, dy = pts[k].second - pos.y;
					const float dist = std::hypot(dx, dy);
					if (dist < kMinPointDistance || dist > kMaxPointDistance) {
						continue;
					}
					const float cosA = (dx * hx + dy * hy) / dist;
					const float tta = dist / speed * (1.5f - 0.5f * cosA);
					if (tta < best) {
						best = tta;
						bz = zi;
						bp = k;
					}
				}
			}
			if (bz >= 0) {
				const auto selectUs = telemetry::NowUs() - selectStartUs;
				const auto queueAgeMs = g.zones[bz].queuedAt ? now - g.zones[bz].queuedAt : 0;
				const auto pt = g.zones[bz].points[bp];
				g.zones[bz].points.erase(g.zones[bz].points.begin() + bp);
				g.lastGenerate = now;
				const bool generated = GenerateAt(player, pt.first, pt.second, pos.z);
				telemetry::Record("content_point_attempt", "\"zone\":%d,\"ok\":%d,\"queue_ms\":%llu,\"select_us\":%llu,\"tta_s\":%.1f,\"distance_m\":%.0f,\"left\":%zu",
					bz, generated ? 1 : 0, static_cast<unsigned long long>(queueAgeMs), static_cast<unsigned long long>(selectUs),
					best, std::hypot(pt.first - pos.x, pt.second - pos.y), g.zones[bz].points.size());
				if (generated) {
					++g.zones[bz].done;
					++g.sinceFinish;
					telemetry::Record("content_generate", "\"zone\":%d,\"pos\":[%.0f,%.0f],\"tta\":%.0f,\"left\":%zu", bz, pt.first, pt.second, best,
						g.zones[bz].points.size());
					if (g.zones[bz].done == 1) {
						notify::Log("Zone content: spawning across the zone");
					}
				}
				if (g.sinceFinish >= kPointsPerFinish || (g.sinceFinish > 0 && g.zones[bz].points.empty())) {
					FinishLanding();
				}
				g.lastScan = 0;
			}
		}

		if (now - g.lastScan < kScanMs) {
			return;
		}
		g.lastScan = now;
		// The game trims saved content records to this count when saving; keep every zone's records instead.
		auto& maxSaved = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kMaxSavedOverlays);
		if (maxSaved < 4000) {
			logger::Write("Content: saved content records limit %u -> 4000 (zones keep their content)", maxSaved);
			maxSaved = 4000;
		}
		Maintain(pcm, player, pos, side);
	}

	void Reset()
	{
		g = State{};
	}
}
