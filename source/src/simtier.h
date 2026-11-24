#pragma once
// Structural architecture primitives (2026-11-23):
// 1. SimTier: one authoritative per-vehicle simulation tier (NearPhysical / FarRail / Unloaded) with hysteresis.
// 2. FrameContext: one main-thread read-only snapshot built once per Update() so subsystems read O(1) memory.
// 3. FrameBudget: one shared per-frame CPU budget governor with starvation prevention for background tasks.
// 4. Invariants: zero-overhead atomic proof-of-work + violation counters emitted every 10 s as OBS system=arch.proof.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace simtier {

enum class Tier : std::uint8_t {
	Unloaded = 0,
	NearPhysical = 1,
	FarRail = 2
};

inline const char* Name(Tier t) {
	switch (t) {
	case Tier::NearPhysical: return "near_phys";
	case Tier::FarRail:      return "far_rail";
	default:                 return "unloaded";
	}
}

inline constexpr double kDefaultNearRadiusM = 160.0;
inline constexpr double kDefaultBandM = 20.0;

// Pure hysteresis decision:
// - !hasModel -> Unloaded
// - distM < nearRadiusM - bandM -> NearPhysical
// - distM > nearRadiusM -> FarRail
// - inside [nearRadiusM - bandM, nearRadiusM] -> keep prev (default NearPhysical if Unloaded)
inline Tier Decide(Tier prev, double distM, bool hasModel, double nearRadiusM = kDefaultNearRadiusM, double bandM = kDefaultBandM) {
	if (!hasModel) return Tier::Unloaded;
	if (!std::isfinite(distM)) return Tier::NearPhysical;
	const double enterNear = std::max(10.0, nearRadiusM - std::max(0.0, bandM));
	if (distM < enterNear) return Tier::NearPhysical;
	if (distM > nearRadiusM) return Tier::FarRail;
	return prev == Tier::FarRail ? Tier::FarRail : Tier::NearPhysical;
}

// Adaptive Havok sweep budget: full 30 Hz + side sweep for player, off-corridor, or obstacle-present NPCs;
// clean pre-baked corridor NPCs drop to 15 Hz (<=90 m) or 10 Hz center-only (>90 m) with zero visual pop-in.
struct RadarPolicy {
	unsigned frameInterval = 2;
	bool sideSweep = true;
};

inline RadarPolicy RadarBudget(bool isPlayer, bool floorProofLane, bool hasObstacles, double distM) {
	if (isPlayer) return {1u, true};
	if (hasObstacles || !floorProofLane) return {2u, true};
	if (std::isfinite(distM) && distM > 90.0) return {6u, false};
	return {4u, true};
}

// Staggered NPC planning cadence (ms): 20 Hz right beside player or with obstacles, ~15 Hz for clean near
// in-view cars, ~8 Hz out-of-view, while cars beyond kDefaultNearRadiusM (160 m) glide on FleetRail at ~0 ms.
inline unsigned PlanIntervalMs(int rank, double distM, bool viewed, bool hasObstacles, int nearCap = 10) {
	if (!std::isfinite(distM) || distM <= 45.0 || hasObstacles) return 50u;
	if (rank < nearCap && distM < 150.0) return viewed ? 67u : 120u;
	if (distM < 300.0) return 150u;
	return 300u;
}

// Per-vehicle entry in the once-per-frame read-only FrameContext.
struct VehicleEntry {
	std::uint32_t vehicleId = 0;
	std::uintptr_t ref = 0;
	float pos[3]{};
	float vel[3]{};
	double distM = 0.0;
	bool has3D = false;
	bool playerOwned = false;
	Tier tier = Tier::Unloaded;
};

struct FrameContext {
	static constexpr std::size_t kMaxEntries = 256;
	std::uint64_t frame = 0;
	bool havePlayer = false;
	float playerPos[3]{};
	double colliderRadiusM = kDefaultNearRadiusM;
	double broadphaseRadiusM = 0.0;
	std::uint32_t count = 0;
	std::array<VehicleEntry, kMaxEntries> entries{};
	std::unordered_map<std::uint32_t, std::uint32_t> indexById;
	std::unordered_map<std::uint32_t, Tier> prevTier;

	// Proof-of-work counters
	std::atomic<std::uint64_t> builds{0};
	std::atomic<std::uint64_t> lookups{0};
	std::atomic<std::uint64_t> hits{0};
	std::atomic<std::uint32_t> tierNear{0};
	std::atomic<std::uint32_t> tierRail{0};
	std::atomic<std::uint32_t> tierUnloaded{0};

	void Reset() {
		frame = 0;
		havePlayer = false;
		count = 0;
		indexById.clear();
		prevTier.clear();
		tierNear.store(0, std::memory_order_relaxed);
		tierRail.store(0, std::memory_order_relaxed);
		tierUnloaded.store(0, std::memory_order_relaxed);
	}

	void Begin(std::uint64_t f, bool hasPlayer, float px, float py, float pz, double colliderR, double broadphaseR) {
		frame = f;
		havePlayer = hasPlayer;
		playerPos[0] = px; playerPos[1] = py; playerPos[2] = pz;
		colliderRadiusM = (std::isfinite(colliderR) && colliderR >= 60.0) ? std::min(colliderR, kDefaultNearRadiusM) : kDefaultNearRadiusM;
		broadphaseRadiusM = std::isfinite(broadphaseR) ? broadphaseR : 0.0;
		count = 0;
		indexById.clear();
		builds.fetch_add(1, std::memory_order_relaxed);
	}

	void Add(std::uint32_t id, std::uintptr_t ref, const float pos[3], const float vel[3], bool has3D, bool playerOwned) {
		if (!id || count >= kMaxEntries) return;
		// 3D distance so high-altitude flight above the city keeps distant cars below on zero-pop-in FarRail instead of full physics.
		const double d = havePlayer ? std::hypot(double(pos[0]) - playerPos[0], double(pos[1]) - playerPos[1], double(pos[2]) - playerPos[2]) : 0.0;
		const auto prevIt = prevTier.find(id);
		const Tier prev = prevIt != prevTier.end() ? prevIt->second : Tier::Unloaded;
		const Tier next = playerOwned ? (has3D ? Tier::NearPhysical : Tier::Unloaded)
		                              : Decide(prev, d, has3D, colliderRadiusM, kDefaultBandM);
		prevTier[id] = next;
		VehicleEntry& e = entries[count];
		e.vehicleId = id;
		e.ref = ref;
		e.pos[0] = pos[0]; e.pos[1] = pos[1]; e.pos[2] = pos[2];
		e.vel[0] = vel[0]; e.vel[1] = vel[1]; e.vel[2] = vel[2];
		e.distM = d;
		e.has3D = has3D;
		e.playerOwned = playerOwned;
		e.tier = next;
		indexById[id] = count;
		++count;
	}

	void Finish() {
		std::uint32_t nNear = 0, nRail = 0, nUnl = 0;
		for (std::uint32_t i = 0; i < count; ++i) {
			if (entries[i].tier == Tier::NearPhysical) ++nNear;
			else if (entries[i].tier == Tier::FarRail) ++nRail;
			else ++nUnl;
		}
		tierNear.store(nNear, std::memory_order_relaxed);
		tierRail.store(nRail, std::memory_order_relaxed);
		tierUnloaded.store(nUnl, std::memory_order_relaxed);
	}

	const VehicleEntry* Find(std::uint32_t vehicleId) const {
		const_cast<FrameContext*>(this)->lookups.fetch_add(1, std::memory_order_relaxed);
		const auto it = indexById.find(vehicleId);
		if (it == indexById.end() || it->second >= count) return nullptr;
		const_cast<FrameContext*>(this)->hits.fetch_add(1, std::memory_order_relaxed);
		return &entries[it->second];
	}
};

inline FrameContext& Context() {
	static FrameContext g_ctx;
	return g_ctx;
}

}  // namespace simtier

namespace framebudget {

enum BgTask : int {
	CellBake = 0,
	RouteSurvey = 1,
	HazardField = 2,
	NavMesh = 3,
	NavSeed = 4,
	kBgTasks = 5
};

struct Governor {
	static constexpr double kTotalBudgetMs = 3.5;
	static constexpr std::uint32_t kStarveLimit = 10;

	double totalBudgetMs = kTotalBudgetMs;
	double elapsedMs = 0.0;
	double gameplayReserveMs = 0.0;  // smoothed cost of NPC + flight gameplay ticks so pre-gameplay bg work never stacks
	std::uint32_t consecutiveDefers[kBgTasks]{};

	// Proof-of-work counters (reset every 10 s on report)
	std::uint32_t frames = 0;
	std::uint32_t framesOverBudget = 0;
	std::uint32_t defers[kBgTasks]{};
	std::uint32_t runs[kBgTasks]{};
	std::uint32_t starveGrants = 0;
	std::uint32_t pilotsCapped = 0;

	void BeginFrame(double budgetMs = kTotalBudgetMs) {
		totalBudgetMs = budgetMs;
		elapsedMs = 0.0;
		++frames;
	}

	void NoteGameplayMs(double ms) {
		if (!std::isfinite(ms) || ms < 0.0) return;
		const double clamped = std::min(ms, totalBudgetMs);
		gameplayReserveMs = clamped > gameplayReserveMs ? clamped : (gameplayReserveMs * 0.85 + clamped * 0.15);
	}

	void UpdateElapsed(double ms) {
		elapsedMs = ms;
	}

	void EndFrame(double ms) {
		elapsedMs = ms;
		if (ms > totalBudgetMs) ++framesOverBudget;
	}

	double RemainingMs() const {
		return std::max(0.0, totalBudgetMs - elapsedMs - gameplayReserveMs);
	}

	// Returns true if the background task may run this frame.
	// Accounts for gameplayReserveMs so background tasks running before PilotsTick cannot stack over budget.
	// If deferred kStarveLimit times in a row, grants a starvation-prevention run so nothing is ever secretly starved.
	bool Allow(BgTask task, double currentElapsedMs) {
		elapsedMs = currentElapsedMs;
		const int idx = int(task);
		if (idx < 0 || idx >= kBgTasks) return true;
		if (elapsedMs + gameplayReserveMs < totalBudgetMs) {
			consecutiveDefers[idx] = 0;
			++runs[idx];
			return true;
		}
		++consecutiveDefers[idx];
		if (consecutiveDefers[idx] >= kStarveLimit) {
			consecutiveDefers[idx] = 0;
			++starveGrants;
			++runs[idx];
			return true;
		}
		++defers[idx];
		return false;
	}

	void ResetWindow() {
		frames = 0;
		framesOverBudget = 0;
		for (int i = 0; i < kBgTasks; ++i) { defers[i] = 0; runs[i] = 0; }
		starveGrants = 0;
		pilotsCapped = 0;
	}

	void ResetAll() {
		ResetWindow();
		for (int i = 0; i < kBgTasks; ++i) consecutiveDefers[i] = 0;
		elapsedMs = 0.0;
		gameplayReserveMs = 0.0;
	}
};

inline Governor& Main() {
	static Governor g_gov;
	return g_gov;
}

}  // namespace framebudget

namespace archproof {

struct Counters {
	// FleetRail transition diagnostics (proves why any FarRail car didn't enter Rail)
	std::atomic<std::uint32_t> railActive{0};
	std::atomic<std::uint32_t> railBlockedNoPath{0};
	std::atomic<std::uint32_t> railBlockedNoChassis{0};
	std::atomic<std::uint32_t> railBlockedBirthHold{0};

	// VehicleLifecycle FSM transitions
	std::atomic<std::uint32_t> lifeToPlayer{0};
	std::atomic<std::uint32_t> lifeEnsureDyn{0};
	std::atomic<std::uint32_t> lifeUnseatRefile{0};

	// Structural invariant violations (0 = healthy, >0 = regression immediately visible)
	std::atomic<std::uint32_t> violKeyframedPlayer{0};
	std::atomic<std::uint32_t> violFallguard{0};
	std::atomic<std::uint32_t> violExpiredLane{0};

	// Follower cache proof
	std::atomic<std::uint64_t> hudFollowerCacheUses{0};

	// Corridor envelope & 60 Hz closed-loop barrier proof (OBS system=arch.corridor)
	std::atomic<std::uint32_t> corridorRailClamped{0};
	std::atomic<std::uint32_t> corridorPlanClamped{0};
	std::atomic<std::uint32_t> corridorPhysWallArrest{0};
	std::atomic<std::uint32_t> corridorTyreYawDamped{0};

	void ResetWindow() {
		railBlockedNoPath.store(0, std::memory_order_relaxed);
		railBlockedNoChassis.store(0, std::memory_order_relaxed);
		railBlockedBirthHold.store(0, std::memory_order_relaxed);
		lifeToPlayer.store(0, std::memory_order_relaxed);
		lifeEnsureDyn.store(0, std::memory_order_relaxed);
		lifeUnseatRefile.store(0, std::memory_order_relaxed);
		violKeyframedPlayer.store(0, std::memory_order_relaxed);
		violFallguard.store(0, std::memory_order_relaxed);
		violExpiredLane.store(0, std::memory_order_relaxed);
		hudFollowerCacheUses.store(0, std::memory_order_relaxed);
		corridorRailClamped.store(0, std::memory_order_relaxed);
		corridorPlanClamped.store(0, std::memory_order_relaxed);
		corridorPhysWallArrest.store(0, std::memory_order_relaxed);
		corridorTyreYawDamped.store(0, std::memory_order_relaxed);
	}
};

inline Counters& State() {
	static Counters g_state;
	return g_state;
}

}  // namespace archproof

namespace corridorwall {

struct Barrier {
	double refX = 0.0, refY = 0.0;
	double tx = 0.0, ty = 1.0;        // unit forward tangent along lane travel
	double minLat = -6.0, maxLat = 6.0; // safe car-centre bounds (+ = right of travel, - = left)
	bool valid = false;
};

struct Result {
	bool arrested = false;
	double lat = 0.0;
	double vx = 0.0, vy = 0.0;
	double yawNudge = 0.0;  // world spin[2] nudge (+ = turn left, - = turn right)
};

// Pure 60 Hz virtual guardrail: if the vehicle centre is at or beyond [minLat, maxLat] and moving outward,
// zero the outward lateral velocity component and provide a gentle inward restoring yaw rate.
inline Result Enforce60Hz(const Barrier& b, double x, double y, double vx, double vy) {
	Result out{};
	out.vx = vx;
	out.vy = vy;
	if (!b.valid || !(b.maxLat > b.minLat) || !std::isfinite(x) || !std::isfinite(y) ||
	    !std::isfinite(vx) || !std::isfinite(vy)) {
		return out;
	}
	const double tLen = std::hypot(b.tx, b.ty);
	if (!(tLen > 0.1)) return out;
	const double ux = b.tx / tLen, uy = b.ty / tLen;
	const double rx = uy, ry = -ux;  // + = right of travel
	const double lat = (x - b.refX) * rx + (y - b.refY) * ry;
	out.lat = lat;
	const double vLat = vx * rx + vy * ry;
	if (lat >= b.maxLat && vLat > 0.0) {
		out.vx = vx - vLat * rx;
		out.vy = vy - vLat * ry;
		out.yawNudge = std::clamp(0.45 + (lat - b.maxLat) * 0.6, 0.45, 1.2);  // turn left (inward)
		out.arrested = true;
	} else if (lat <= b.minLat && vLat < 0.0) {
		out.vx = vx - vLat * rx;
		out.vy = vy - vLat * ry;
		out.yawNudge = -std::clamp(0.45 + (b.minLat - lat) * 0.6, 0.45, 1.2);  // turn right (inward)
		out.arrested = true;
	}
	return out;
}

// Pure 60 Hz closed-loop heading overshoot brake for tyre-mode (TY-1) cars:
// eLive = wrap(railAim - liveHeading) (+ = aim is right of nose; spinZ + = turn left).
// Returns true and writes dampedSpinZ when the body's live yaw spin is actively diverging past railAim.
inline bool DampTyreYaw60Hz(double eLive, double liveSpinZ, double maxYaw, double& dampedSpinZ) {
	dampedSpinZ = liveSpinZ;
	if (!std::isfinite(eLive) || !std::isfinite(liveSpinZ)) return false;
	const double cap = std::clamp(maxYaw, 0.3, 1.6);
	// Spinning left (+spinZ) while aim is to the right (+eLive), or spinning right (-spinZ) while aim is to the left (-eLive).
	const bool diverging = (liveSpinZ > 0.20 && eLive > 0.08) || (liveSpinZ < -0.20 && eLive < -0.08);
	if (!diverging) return false;
	dampedSpinZ = std::clamp(-eLive * 1.8, -cap, cap);
	return true;
}

}  // namespace corridorwall
