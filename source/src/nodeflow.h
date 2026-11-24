#pragma once
// Node Flow, Scripted Node Event Bus & Quest/Encounter Dispatch Registry (2026-11-24):
// Pure, versioned bay-occupancy registry, whole-city node-event bus (near physics + far FleetRail),
// bounded pad-hold timeouts, and director overrides (link share/divert/close, departure stagger,
// dual-write calm zone toggles). Zero ABI breaks (strictly additive exports), single-arbiter safe
// (events only request intents; actuator stays locked).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

extern "C" {

enum FvoBayStateV1 : int {
	FVO_BAY_FREE = 0,
	FVO_BAY_INBOUND = 1,
	FVO_BAY_PARKED = 2,
	FVO_BAY_DEPARTING = 3
};

struct FvoLandingStateV1 {
	std::uint32_t size = sizeof(FvoLandingStateV1);
	std::uint32_t version = 1;
	char place[96]{};
	char purpose[32]{};
	int bayIndex = 0;
	int bayCount = 1;
	double x = 0.0, y = 0.0, z = 0.0;
	double radius = 40.0;
	std::uint32_t occupantVehicle = 0;
	int state = FVO_BAY_FREE;
	double dwellRemainingS = 0.0;
};

enum FvoNodeKindV1 : int {
	FVO_NODE_LANDING = 0,
	FVO_NODE_LINK = 1,
	FVO_NODE_ZONE = 2,
	FVO_NODE_WAYPOINT = 3
};

enum FvoNodeEventKindV1 : int {
	FVO_EVT_ENTER = 0,
	FVO_EVT_PASS = 1,
	FVO_EVT_INBOUND = 2,
	FVO_EVT_PARKED = 3,
	FVO_EVT_DWELL_END = 4,
	FVO_EVT_DEPART = 5,
	FVO_EVT_HOLD_TIMEOUT = 6,
	FVO_EVT_DIVERT = 7,
	FVO_EVT_ZONE_TOGGLE = 8
};

enum FvoSimTierV1 : int {
	FVO_TIER_PHYS = 0,
	FVO_TIER_RAIL = 1
};

struct FvoNodeEventV1 {
	std::uint32_t size = sizeof(FvoNodeEventV1);
	std::uint32_t version = 1;
	std::uint64_t seq = 0;
	std::uint64_t timestampMs = 0;
	int nodeKind = FVO_NODE_LANDING;
	int eventKind = FVO_EVT_ENTER;
	int simTier = FVO_TIER_PHYS;
	std::uint32_t vehicleId = 0;
	int lane = 0;
	int dir = 1;
	double speed = 0.0;
	double u = 0.0;
	double heldS = 0.0;
	char node[96]{};
	char tag[64]{};
	char eventId[64]{};
	char route[64]{};
	char reason[96]{};
};

using FvoLandingStateFnV1 = int (*)(int index, FvoLandingStateV1* out);
using FvoDispatchToLandingFnV1 = int (*)(std::uint32_t vehicleId, const char* placeId, double dwellSeconds, std::uint32_t triggerId);
using FvoReleaseLandingFnV1 = int (*)(std::uint32_t vehicleId, const char* placeId);
using FvoNodeEventReadFnV1 = int (*)(std::uint64_t afterSeq, FvoNodeEventV1* out);
using FvoSetLinkRuleFnV1 = int (*)(const char* linkId, double shareOverride, int divertAll, int closed, double durationSeconds, const char* reason);
using FvoSetZoneCalmFnV1 = int (*)(const char* zoneId, int active, double speedMps, double minRoadZ, double maxRoadZ, const char* reason);
using FvoDelayDepartureFnV1 = int (*)(std::uint32_t vehicleId, double extraSeconds, const char* reason);

}  // extern "C"

namespace nodeflow {

// Bounded pad-hold timeout prevents stalled scripts from wedging a landing spot and recreating hover queues.
constexpr double kDefaultMaxHoldS = 90.0;
constexpr double kHardMaxHoldS = 300.0;
constexpr double kMaxStaggerDelayS = 60.0;

inline double ClampHoldSeconds(double requestedS, double fallbackS = kDefaultMaxHoldS) {
	if (!std::isfinite(requestedS) || requestedS <= 0.0) return std::clamp(fallbackS, 1.0, kHardMaxHoldS);
	return std::clamp(requestedS, 1.0, kHardMaxHoldS);
}

inline const char* NodeKindName(int k) {
	switch (k) {
		case FVO_NODE_LANDING: return "landing";
		case FVO_NODE_LINK: return "link";
		case FVO_NODE_ZONE: return "zone";
		case FVO_NODE_WAYPOINT: return "waypoint";
		default: return "unknown";
	}
}

inline const char* EventKindName(int k) {
	switch (k) {
		case FVO_EVT_ENTER: return "enter";
		case FVO_EVT_PASS: return "pass";
		case FVO_EVT_INBOUND: return "inbound";
		case FVO_EVT_PARKED: return "parked";
		case FVO_EVT_DWELL_END: return "dwell_end";
		case FVO_EVT_DEPART: return "depart";
		case FVO_EVT_HOLD_TIMEOUT: return "hold_timeout";
		case FVO_EVT_DIVERT: return "divert";
		case FVO_EVT_ZONE_TOGGLE: return "zone_toggle";
		default: return "unknown";
	}
}

inline const char* SimTierName(int t) {
	return t == FVO_TIER_RAIL ? "rail" : "phys";
}

struct Bay {
	std::string place;
	std::string purpose;
	int bayIndex = 0;
	int bayCount = 1;
	double x = 0.0, y = 0.0, z = 0.0;
	double radius = 40.0;
	std::uint32_t occupantVehicle = 0;
	FvoBayStateV1 state = FVO_BAY_FREE;
	double dwellRemainingS = 0.0;
};

// Pure bay-occupancy registry: guarantees at most 1 vehicle owns a bay at a time.
struct Registry {
	std::vector<Bay> bays;

	void Clear() { bays.clear(); }

	void AddBay(const std::string& place, const std::string& purpose, int bayIndex, int bayCount,
	            double x, double y, double z, double radius) {
		Bay b{};
		b.place = place;
		b.purpose = purpose;
		b.bayIndex = bayIndex;
		b.bayCount = std::max(1, bayCount);
		b.x = x; b.y = y; b.z = z;
		b.radius = radius;
		bays.push_back(std::move(b));
	}

	// Reserve a free bay at `place` for `vehicleId`. Returns bay index (>= 0) or -1 if full/unknown.
	// Idempotent if `vehicleId` already holds a bay at `place`.
	int Reserve(const std::string& place, std::uint32_t vehicleId, double dwellS = 0.0) {
		if (!vehicleId || place.empty()) return -1;
		for (auto& b : bays) {
			if (b.place == place && b.occupantVehicle == vehicleId) {
				if (b.state == FVO_BAY_FREE) b.state = FVO_BAY_INBOUND;
				if (dwellS > 0.0) b.dwellRemainingS = dwellS;
				return b.bayIndex;
			}
		}
		for (auto& b : bays) {
			if (b.place == place && b.occupantVehicle == 0 && b.state == FVO_BAY_FREE) {
				b.occupantVehicle = vehicleId;
				b.state = FVO_BAY_INBOUND;
				b.dwellRemainingS = std::max(0.0, dwellS);
				return b.bayIndex;
			}
		}
		return -1;
	}

	bool MarkParked(const std::string& place, std::uint32_t vehicleId, double dwellRemainingS) {
		for (auto& b : bays) {
			if (b.place == place && b.occupantVehicle == vehicleId) {
				b.state = FVO_BAY_PARKED;
				b.dwellRemainingS = std::max(0.0, dwellRemainingS);
				return true;
			}
		}
		return false;
	}

	bool MarkDeparting(const std::string& place, std::uint32_t vehicleId) {
		for (auto& b : bays) {
			if (b.place == place && b.occupantVehicle == vehicleId) {
				b.state = FVO_BAY_DEPARTING;
				b.dwellRemainingS = 0.0;
				return true;
			}
		}
		return false;
	}

	int ReleaseVehicle(std::uint32_t vehicleId, const std::string& placeFilter = {}) {
		int released = 0;
		for (auto& b : bays) {
			if (b.occupantVehicle != vehicleId) continue;
			if (!placeFilter.empty() && b.place != placeFilter) continue;
			b.occupantVehicle = 0;
			b.state = FVO_BAY_FREE;
			b.dwellRemainingS = 0.0;
			++released;
		}
		return released;
	}

	int FreeCount(const std::string& place) const {
		int n = 0;
		for (const auto& b : bays) if (b.place == place && b.occupantVehicle == 0 && b.state == FVO_BAY_FREE) ++n;
		return n;
	}

	bool Read(int index, FvoLandingStateV1& out) const {
		if (index < 0 || index >= int(bays.size())) return false;
		const auto& b = bays[std::size_t(index)];
		out = FvoLandingStateV1{};
		std::snprintf(out.place, sizeof out.place, "%s", b.place.c_str());
		std::snprintf(out.purpose, sizeof out.purpose, "%s", b.purpose.c_str());
		out.bayIndex = b.bayIndex;
		out.bayCount = b.bayCount;
		out.x = b.x; out.y = b.y; out.z = b.z;
		out.radius = b.radius;
		out.occupantVehicle = b.occupantVehicle;
		out.state = int(b.state);
		out.dwellRemainingS = b.dwellRemainingS;
		return true;
	}
};

// Whole-city node event ring buffer (phys + FleetRail cars).
struct EventBus {
	static constexpr std::size_t kCapacity = 512;
	std::deque<FvoNodeEventV1> ring;
	std::uint64_t nextSeq = 1;

	void Clear() { ring.clear(); nextSeq = 1; }

	FvoNodeEventV1 Emit(std::uint64_t timestampMs, FvoNodeKindV1 nodeKind, FvoNodeEventKindV1 eventKind,
	                    FvoSimTierV1 simTier, std::uint32_t vehicleId, const std::string& node,
	                    const std::string& tag, const std::string& eventId, const std::string& route,
	                    const std::string& reason, int lane = 0, int dir = 1,
	                    double speed = 0.0, double u = 0.0, double heldS = 0.0) {
		FvoNodeEventV1 ev{};
		ev.seq = nextSeq++;
		ev.timestampMs = timestampMs;
		ev.nodeKind = int(nodeKind);
		ev.eventKind = int(eventKind);
		ev.simTier = int(simTier);
		ev.vehicleId = vehicleId;
		ev.lane = lane;
		ev.dir = dir;
		ev.speed = std::isfinite(speed) ? speed : 0.0;
		ev.u = std::isfinite(u) ? u : 0.0;
		ev.heldS = std::isfinite(heldS) ? heldS : 0.0;
		std::snprintf(ev.node, sizeof ev.node, "%s", node.c_str());
		std::snprintf(ev.tag, sizeof ev.tag, "%s", tag.c_str());
		std::snprintf(ev.eventId, sizeof ev.eventId, "%s", eventId.c_str());
		std::snprintf(ev.route, sizeof ev.route, "%s", route.c_str());
		std::snprintf(ev.reason, sizeof ev.reason, "%s", reason.c_str());
		if (ring.size() >= kCapacity) ring.pop_front();
		ring.push_back(ev);
		return ev;
	}

	// Read the earliest event with seq > afterSeq. Returns false if none.
	bool ReadAfter(std::uint64_t afterSeq, FvoNodeEventV1& out) const {
		for (const auto& ev : ring) {
			if (ev.seq > afterSeq) { out = ev; return true; }
		}
		return false;
	}
};

// Temporary director override for a link (share throttle, convoy divert for both lap + landing cars, or closure).
struct LinkOverride {
	std::string link;
	double shareOverride = -1.0;  // < 0: keep authored share; [0..1]: override share
	bool divertAll = false;       // true: force both lap AND landing-bound cars to take this link
	bool closed = false;          // true: block turning onto this link
	std::uint64_t expiresMs = 0;  // 0: until cleared; else auto-expires at expiresMs
	std::string reason;
};

struct LinkDirector {
	std::vector<LinkOverride> rules;

	void Prune(std::uint64_t nowMs) {
		rules.erase(std::remove_if(rules.begin(), rules.end(), [&](const LinkOverride& r) {
			return r.expiresMs > 0 && nowMs >= r.expiresMs;
		}), rules.end());
	}

	void Set(const std::string& link, double shareOverride, bool divertAll, bool closed,
	         std::uint64_t nowMs, double durationS, const std::string& reason) {
		Prune(nowMs);
		const bool emptyRule = (shareOverride < 0.0 && !divertAll && !closed);
		rules.erase(std::remove_if(rules.begin(), rules.end(), [&](const LinkOverride& r) {
			return r.link == link;
		}), rules.end());
		if (emptyRule || link.empty()) return;
		LinkOverride ov{};
		ov.link = link;
		ov.shareOverride = shareOverride >= 0.0 ? std::clamp(shareOverride, 0.0, 1.0) : -1.0;
		ov.divertAll = divertAll && !closed;
		ov.closed = closed;
		if (std::isfinite(durationS) && durationS > 0.0) {
			ov.expiresMs = nowMs + std::uint64_t(std::clamp(durationS, 1.0, 3600.0) * 1000.0);
		}
		ov.reason = reason;
		rules.push_back(std::move(ov));
	}

	const LinkOverride* Find(const std::string& link, std::uint64_t nowMs) {
		Prune(nowMs);
		for (const auto& r : rules) if (r.link == link) return &r;
		return nullptr;
	}
};

}  // namespace nodeflow
