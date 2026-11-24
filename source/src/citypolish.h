#pragma once
// Production City Polish, Time-of-Day Rhythm, Lights Intent, Gap-Aware Lift-Off & Self-Heal Watchdog (2026-11-24):
// Pure, zero-dependency helpers that keep ambient traffic calm, readable, and self-healing over multi-hour sessions.
// Single-arbiter safe: every helper only produces intents/timings; never writes vehicle physics or bypasses energyplan.h.
#include <algorithm>
#include <cmath>
#include <cstdint>

extern "C" {

// Additive visual-intent flags for vehicle lights/cues (queried by native light hooks or extensions).
enum FvoLightFlagsV1 : std::uint32_t {
	FVO_LIGHT_NONE = 0u,
	FVO_LIGHT_HEADLIGHTS = 1u << 0,   // Dusk/night headlights (19:00..06:30)
	FVO_LIGHT_BRAKE = 1u << 1,        // Decelerating / slow-zone / yielding
	FVO_LIGHT_TURN_BEACON = 1u << 2,  // Turning at a link or descending to a landing pad
	FVO_LIGHT_LANDING_GLOW = 1u << 3  // Touchdown / lift-off thruster flare
};

struct FvoVehicleVisualIntentV1 {
	std::uint32_t size = sizeof(FvoVehicleVisualIntentV1);
	std::uint32_t version = 1;
	std::uint32_t vehicleId = 0;
	std::uint32_t lightFlags = FVO_LIGHT_NONE;
	int localMinute = -1;
	double speedScale = 1.0;
};

using FvoVehicleVisualIntentFnV1 = int (*)(std::uint32_t vehicleId, FvoVehicleVisualIntentV1* out);

}  // extern "C"

namespace citypolish {

enum class DayPhase : int {
	LateNight = 0,  // 22:00..05:30 — quiet glide, headlights on, longer calm pad dwell
	Dawn = 1,       // 05:30..07:30 — waking up, headlights on until 06:30
	MorningRush = 2,// 07:30..09:30 — crisp pace, brisker pad turnover
	Midday = 3,     // 09:30..17:00 — calm baseline
	EveningRush = 4,// 17:00..19:30 — lively commute, headlights on from 19:00
	Evening = 5     // 19:30..22:00 — headlights on, settling into night
};

struct Rhythm {
	DayPhase phase = DayPhase::Midday;
	bool headlightsOn = false;
	double speedScale = 1.0;
	double dwellScale = 1.0;
};

inline const char* PhaseName(DayPhase p) {
	switch (p) {
		case DayPhase::LateNight: return "late_night";
		case DayPhase::Dawn: return "dawn";
		case DayPhase::MorningRush: return "morning_rush";
		case DayPhase::Midday: return "midday";
		case DayPhase::EveningRush: return "evening_rush";
		case DayPhase::Evening: return "evening";
		default: return "midday";
	}
}

// Compute time-of-day rhythm and headlight state from planet local minute [0..1439].
inline Rhythm EvaluateTimeOfDay(bool timeKnown, int localMinute) {
	Rhythm r{};
	if (!timeKnown || localMinute < 0) return r;
	const int m = ((localMinute % 1440) + 1440) % 1440;
	r.headlightsOn = (m >= 19 * 60) || (m < 6 * 60 + 30);
	if (m >= 22 * 60 || m < 5 * 60 + 30) {
		r.phase = DayPhase::LateNight;
		r.speedScale = 0.90;
		r.dwellScale = 1.30;
	} else if (m < 7 * 60 + 30) {
		r.phase = DayPhase::Dawn;
		r.speedScale = 0.96;
		r.dwellScale = 1.05;
	} else if (m < 9 * 60 + 30) {
		r.phase = DayPhase::MorningRush;
		r.speedScale = 1.02;
		r.dwellScale = 0.85;
	} else if (m < 17 * 60) {
		r.phase = DayPhase::Midday;
		r.speedScale = 1.00;
		r.dwellScale = 1.00;
	} else if (m < 19 * 60 + 30) {
		r.phase = DayPhase::EveningRush;
		r.speedScale = 1.02;
		r.dwellScale = 0.85;
	} else {
		r.phase = DayPhase::Evening;
		r.speedScale = 0.95;
		r.dwellScale = 1.15;
	}
	return r;
}

inline std::uint32_t ComputeLightFlags(bool headlightsOn, bool brakingOrHolding, bool turningOrApproaching, bool landingOrLiftoff) {
	std::uint32_t f = FVO_LIGHT_NONE;
	if (headlightsOn) f |= FVO_LIGHT_HEADLIGHTS;
	if (brakingOrHolding) f |= FVO_LIGHT_BRAKE;
	if (turningOrApproaching) f |= FVO_LIGHT_TURN_BEACON;
	if (landingOrLiftoff) f |= FVO_LIGHT_LANDING_GLOW;
	return f;
}

// Gap-aware lift-off: defer departure by 3 s (up to 3 deferrals = 9 s max) if an oncoming car on the target
// road/lane is within kLiftoffClearGapM of the pad/join point, preventing 2-car mini-platoons.
constexpr double kLiftoffClearGapM = 40.0;
constexpr double kLiftoffDeferS = 3.0;
constexpr int kLiftoffMaxDefers = 3;

inline bool ShouldDeferLiftoff(double nearestOncomingDistM, int deferCount) {
	if (deferCount >= kLiftoffMaxDefers) return false;
	if (!std::isfinite(nearestOncomingDistM)) return false;
	return nearestOncomingDistM < kLiftoffClearGapM;
}

// Silent self-heal watchdog: triggers only when an active moving vehicle (not parked, not intentionally held/queued)
// fails to move >= 0.5 m for >= kStuckHealSeconds, or after a cooldown has elapsed since its last self-heal.
constexpr double kStuckMoveEpsM = 0.5;
constexpr double kStuckHealSeconds = 18.0;
constexpr double kSelfHealCooldownS = 30.0;

struct Watchdog {
	double anchorX = 0.0, anchorY = 0.0, anchorZ = 0.0;
	std::uint64_t anchorMs = 0;
	std::uint64_t lastHealMs = 0;
	std::uint32_t healCount = 0;

	void Reset(double x, double y, double z, std::uint64_t nowMs) {
		anchorX = x; anchorY = y; anchorZ = z;
		anchorMs = nowMs;
	}

	// Returns stuck duration in seconds (>= kStuckHealSeconds) when self-heal should run, else 0.0.
	double Update(double x, double y, double z, std::uint64_t nowMs, bool movingExpected) {
		if (!movingExpected || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
			Reset(x, y, z, nowMs);
			return 0.0;
		}
		if (anchorMs == 0 || std::hypot(std::hypot(x - anchorX, y - anchorY), z - anchorZ) >= kStuckMoveEpsM) {
			Reset(x, y, z, nowMs);
			return 0.0;
		}
		const double stuckS = double(nowMs - anchorMs) * 0.001;
		if (stuckS < kStuckHealSeconds) return 0.0;
		if (lastHealMs != 0 && double(nowMs - lastHealMs) * 0.001 < kSelfHealCooldownS) return 0.0;
		lastHealMs = nowMs;
		++healCount;
		Reset(x, y, z, nowMs);
		return stuckS;
	}
};

}  // namespace citypolish
