#pragma once
// Traffic Manager "Design by Look" cinematic control layer (2026-11-24, notes/TRAFFIC_MANAGER_LOOK_DESIGN_20261124.md):
// Pure conversions between human-readable cinematic presets (Spacing, Pace, Link Share, Landing Dwell, Calm Zone)
// and existing route/zone file fields. Zero new required keys; 100% backwards-compatible with older builds.
#include "routeschedule.h"
#include "trafficdata.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace trafficlook {

enum SpacingPreset : int { Sparse = 0, Calm = 1, Lively = 2, Busy = 3, SpacingCustom = 4 };
inline constexpr double kSpacingGapS[4] = { 25.0, 18.0, 12.0, 7.0 };
inline const char* SpacingName(int p) {
	constexpr const char* names[] = { "Sparse", "Calm", "Lively", "Busy", "Custom" };
	return p >= 0 && p <= 4 ? names[p] : "Custom";
}

enum PacePreset : int { Stroll = 0, City = 1, Express = 2, PaceCustom = 3 };
inline constexpr double kGroundPaceMps[3] = { 11.0, 16.0, 22.0 };
inline constexpr double kSkyPaceMps[3]    = { 15.0, 22.0, 27.0 };
inline const char* PaceName(int p) {
	constexpr const char* names[] = { "Stroll", "City", "Express", "Custom" };
	return p >= 0 && p <= 3 ? names[p] : "Custom";
}

enum SharePreset : int { Few = 0, Some = 1, Many = 2, ShareCustom = 3 };
inline constexpr double kSharePreset[3] = { 0.15, 0.30, 0.45 };

enum DwellPreset : int { ShortDwell = 0, MediumDwell = 1, LongDwell = 2, UntilNeeded = 3, DwellCustom = 4 };
inline constexpr double kDwellSeconds[4] = { 60.0, 120.0, 300.0, 0.0 };

// Convert Spacing preset (time gap in seconds between cars in one lane) -> existing traffic_level + traffic_ranges.
// Leaves density untouched so older builds without traffic_level continue reading it unchanged.
inline void ApplySpacing(routeschedule::Schedule& sched, int preset, double speedMps, int lanes, double lengthM) {
	if (preset < Sparse || preset > Busy) return;
	const double gapS = kSpacingGapS[preset];
	const double spd = std::max(4.0, speedMps);
	const double lenKm = std::max(0.1, lengthM / 1000.0);
	const double d = spd * gapS;  // metres between cars in one lane
	const double n = std::max(1.0, double(std::max(1, lanes)) * (lengthM / std::max(10.0, d)));
	const int k = std::clamp(int(std::lround(n / lenKm)), 1, fvolimits::kVehicles - 1);
	int lvl = routeschedule::Steady;
	if (k <= 4) lvl = routeschedule::Quiet;
	else if (k >= 9) lvl = routeschedule::Busy;
	sched.trafficLevel = lvl;
	sched.trafficBands[lvl] = { std::max(1, k - 1), std::min(fvolimits::kVehicles, k + 1) };
}

inline int DetectSpacing(const routeschedule::Schedule& sched, double speedMps, int lanes, double lengthM) {
	if (!routeschedule::HasLevel(sched) || !(lengthM > 50.0) || !(speedMps > 1.0)) return SpacingCustom;
	const auto band = sched.trafficBands[sched.trafficLevel];
	const double k = (band.low + band.high) * 0.5;
	if (!(k > 0.0)) return SpacingCustom;
	const double cars = k * (lengthM / 1000.0);
	const double gapM = (double(std::max(1, lanes)) * lengthM) / std::max(1.0, cars);
	const double gapS = gapM / speedMps;
	for (int p = Sparse; p <= Busy; ++p) {
		if (std::fabs(gapS - kSpacingGapS[p]) <= kSpacingGapS[p] * 0.12) return p;
	}
	return SpacingCustom;
}

inline double PaceSpeed(int preset, bool isSky) {
	const int idx = std::clamp(preset, 0, 2);
	return isSky ? kSkyPaceMps[idx] : kGroundPaceMps[idx];
}

inline int DetectPace(double speedMps, bool isSky) {
	for (int p = Stroll; p <= Express; ++p) {
		const double target = isSky ? kSkyPaceMps[p] : kGroundPaceMps[p];
		if (std::fabs(speedMps - target) <= 1.5) return p;
	}
	return PaceCustom;
}

inline bool SpeedJumpTooLarge(double speedA, double speedB) {
	return std::fabs(speedA - speedB) > 8.0;
}

inline int DetectShare(double share) {
	for (int p = Few; p <= Many; ++p) {
		if (std::fabs(share - kSharePreset[p]) <= 0.04) return p;
	}
	return ShareCustom;
}

inline int DetectDwell(double staySeconds) {
	if (staySeconds <= 0.5) return UntilNeeded;
	for (int p = ShortDwell; p <= LongDwell; ++p) {
		if (std::fabs(staySeconds - kDwellSeconds[p]) <= 15.0) return p;
	}
	return DwellCustom;
}

// Wait-above (queue) is only safe on hover roads with >= 3 landing spaces (prevents hovering pile-ups).
inline bool QueueAllowed(bool isHoverRoad, int totalSpacesOnRoad) {
	return isHoverRoad && totalSpacesOnRoad >= 3;
}

// Calm-here slow zone helper: writes BOTH kind=slow/speed AND height-banded restrictions so
// trip-goal, lane-following, and FleetRail cars all slow smoothly without leaking to other levels.
inline void ApplyCalmZone(trafficdata::Zone& z, double speedMps, double minRoadZ, double maxRoadZ) {
	const double v = std::clamp(speedMps, 4.0, 40.0);
	z.slow = true;
	z.speed = v;
	z.restrictionFloor = minRoadZ - 10.0;
	z.restrictionTop = maxRoadZ + 10.0;
	// Zone::restrictions is trafficconstraints::Rules (no enabled flag: a non-empty rule is active). Written as
	// "1 3 0 <v> 0 200 -1 -1 0" by routefiles.h: all modes, open, speed cap v, full flight band, all day, lane changes allowed.
	z.restrictions = trafficconstraints::Rules{};
	z.restrictions.speed = v;
	z.restrictions.maxHeight = 200.0;
}

struct RoadLook {
	int cars = 0;
	int moving = 0;
	double gapM = 0.0;
	double gapS = 0.0;
	double inView = 0.0;
	const char* feel = "Calm";
};

inline RoadLook EvaluateRoadLook(const routeschedule::Schedule& sched, double speedMps, int lanes, double lengthM, double parkedShare = 0.35, double viewWindowM = 550.0) {
	RoadLook out{};
	out.cars = routeschedule::VehiclesFor(sched, sched.allDayVehicles, lengthM);
	out.moving = std::max(0, int(std::lround(out.cars * (1.0 - std::clamp(parkedShare, 0.0, 0.85)))));
	if (out.cars > 0 && lengthM > 1.0) {
		out.gapM = (double(std::max(1, lanes)) * lengthM) / double(out.cars);
		out.gapS = speedMps > 0.5 ? out.gapM / speedMps : 0.0;
	}
	if (lengthM > 1.0) {
		out.inView = (double(out.moving) / lengthM) * std::min(lengthM, viewWindowM);
	}
	if (out.inView < 1.0) out.feel = "Empty";
	else if (out.inView <= 3.0) out.feel = "Calm";
	else if (out.inView <= 6.0) out.feel = "Lively";
	else out.feel = "Crowded";
	return out;
}

inline std::string FormatLookLine(const RoadLook& lk) {
	char buf[160];
	std::snprintf(buf, sizeof buf, "%s | %d cars (~%d moving) | 1 every %.0f m (%.0fs) | ~%.0f in view",
		lk.feel, lk.cars, lk.moving, lk.gapM, lk.gapS, std::max(0.0, lk.inView));
	return buf;
}

struct CityBudget {
	int wanted = 0;
	int cap = fvolimits::kVehicles;
	bool overCap = false;
};

inline CityBudget EvaluateCityBudget(int totalWanted, int cityCap) {
	CityBudget b{};
	b.wanted = std::max(0, totalWanted);
	b.cap = std::max(0, cityCap);
	b.overCap = b.cap > 0 && b.wanted > b.cap;
	return b;
}

}  // namespace trafficlook
