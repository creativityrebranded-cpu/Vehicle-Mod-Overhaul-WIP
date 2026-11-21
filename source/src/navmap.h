#pragma once

#include <cstdint>
#include <limits>

// The shared local map for predictive navigation (Driving Behavior session, 2026-10-18; the user: "predictive models
// based on the shape of the environment", then "is there anything that can be shared by all vehicles"). Step 1 of the
// plan: SENSE and REMEMBER. Every vehicle's probes feed one world-anchored grid (4 m cells), so a second vehicle in the
// same area reuses what the first one saw, and a bump or push-up is remembered for the next vehicle. Observe-only for
// now: nothing here steers a vehicle; it logs how well it agrees with the existing corridors, so the next step (a
// trajectory sampler checking candidate paths against this map) is built on measured quality.
//
// What a cell remembers:
//   ground   highest ground-layer surface seen from above (the corridor probes)
//   floor    a surface the ground probes miss, learned from a push-up contact (city platforms): held 10 min
//   blocked  a solid, static thing hit at the vehicle's height by a forward ray (characters are never remembered)
//   free     a height band a forward ray passed through without a hit (free space evidence)
//   contacts how many bumps happened here (side contacts)
//
// Main thread only (the frame task: Fly, Tick). No locking.
namespace navmap
{
	// ---- Feeding (vehicleflight, once per probe result) ----
	void NoteGround(double x, double y, double h);
	// A forward ray from `from` to `to` (as asked). hit: it hit something at (hx, hy, hz) on `layer` (collision layer
	// number 0..63, from the hit record); the ray is free from `from` up to the hit (or all of it without one).
	// normalOk: the hit record held a unit vector at +0x10 (counted, to learn whether that is the surface normal).
	// normalZ: the hit surface's normal z (2026-11-05): kept with the hit, so floors (decks, ramps: facing up) and walls can be told apart.
	void NoteRay(const float from[3], const float to[3], bool hit, float hx, float hy, float hz, std::uint32_t layer, bool normalOk, float normalZ = std::numeric_limits<float>::quiet_NaN(), bool keepAnyLayer = false, std::uint32_t owner = 0);  // keepAnyLayer: path rays and hull sweeps keep a hit on any layer they were cast on (2026-11-05)
	// A contact at the vehicle's place: up = the solver pushed it up (a floor under it), else a side bump toward (dx, dy).
	void NoteContact(double x, double y, double z, bool up, double dx, double dy);
	// The game's own collision objects (navseed, 2026-10-20): a solid object's box (centre, heading, half-extents, bottom and top) marks
	// every cell it covers as solid between zlo and zhi (two height bands per cell, so a floor and a roof above it stay apart). Refreshed
	// every few seconds by the scan; NoteSeeded says which area the last full scan covered (cells there count as known).
	void NoteObject(double cx, double cy, double yaw, double hx, double hy, double zlo, double zhi);
	void NoteSeeded(double cx, double cy, double radius);
	// How far the scanned (known) area reaches from (x, y) along `heading` (0 outside it or before the first scan).

	// ---- Questions ----
	struct Sample
	{
		bool known = false;     // anything remembered here
		bool haveGround = false;
		float ground = 0;       // highest surface remembered (ground or learned floor)
		bool blocked = false;   // a solid static hit near height z
		bool objBlocked = false;  // a game collision object's box (navseed) covers height z here (kept apart: a box can be a poor fit)
		bool objOverruled = false;  // a box covered it, but a ray flew through here at this height recently: open (the box was wrong here)
		bool free = false;      // a ray passed through this height
		int contacts = 0;
		float blockedZ = 0;     // the height of the remembered solid hit (valid when blocked): a hillside hit sits at the ground, a wall above it
	};
	Sample At(double x, double y, double z);
	// A real ray hit point (not its whole 4 m cell) within `radius` metres of (x, y), between heights zLo and zHi, still trusted.
	// hitZ: the nearest one's height. Used by waypoint driving so a thin post blocks about its own size (2026-11-04).
	bool HitNear(double x, double y, double zLo, double zHi, double radius, double* hitZ);
	// As HitNear, but a hit on a surface facing up (normal z > 0.7: a deck, a ramp, a floor) is not a wall. Hits with no normal count.
	// hitZ: the highest wall hit near (2026-11-05: was the nearest; the height of what is there). face: one of them had a sideways normal
	// (a vertical face: a barrier, a post), so even a low hit is a wall, not a step.
	bool WallNear(double x, double y, double zLo, double zHi, double radius, double* hitZ, bool* face = nullptr);
	// The real hit points near (x, y), for World (planworld.h): up is |normal z| as WallNear uses it; signedZ keeps the game's sign, unproven until a run
	// shows it (TakeNormalSigns: falling rays normal up, down; rising rays normal up, down).
	struct KeptHit { float x, y, z, up, signedZ; bool haveNormal; std::uint32_t owner; double age; std::uint8_t layer = 255; };  // layer: world truth A (255 unknown)
	int HitsNear(double x, double y, double zLo, double zHi, double radius, KeptHit* out, int max);
	void TakeNormalSigns(unsigned out[4]);
	// Collision boxes kept whole, and every ground level remembered at a spot, for World (planworld.h). Additive: At() still answers from the bands and
	// the highest surface.
	struct KeptBox { double x, y, yaw, hx, hy, zLo, zHi; };
	int BoxesNear(double x, double y, double radius, KeptBox* out, int max);
	int GroundLevels(double x, double y, float* out, int max);
	unsigned TakeFloorSkips();  // hits WallNear ignored as floors since the last call (probe)
	// The same answers for n heights of one column: one cell lookup instead of n (2026-10-29 audit: the flight field asked each of its
	// 33 heights separately, ~515 000 lookups a field).
	void Column(double x, double y, const double* zs, int n, Sample* out);

	enum class Verdict : std::uint8_t { Unknown, Clear, Blocked };
	// Sweeps a vehicle (half-width `halfWidth`) from p along d (horizontal metres dx, dy). A surface ahead blocks it when
	// the climb it needs (surface + `clearance` - p.z) is more than `climbRate` m/s can give over the time to reach it at
	// `speed` (the same test as the corridors: a gentle rise the height hold absorbs is not a wall); a remembered solid
	// hit at its height blocks. Blocked: the first blocked distance in *at, and why in *why. Unknown: no data on most of it.
	Verdict PathClear(const float p[3], double dx, double dy, double halfWidth, double clearance, double climbRate, double speed,
		double* at, const char** why);

	// ---- The heading sampler (step 2) ----
	bool RaysEnabled();            // nav.ini [nav] rays=0 stops every engine ray and hull-sweep query (default on); the walls-only nav map then gets nothing from them
	bool HullEnabled();            // nav.ini [nav] shape=0 turns the hull-sized sweeps off (default on); a fault turns them off by itself
	// For the log line: the straight way's thin ray and hull sweep verdicts side by side this frame.
	void NoteHull(bool rayBlocked, bool hullBlocked);
	bool SamplerEnabled();        // Logs\FlyingVehicleOverhaul\nav.ini [nav] sampler=0 turns it off (default on); re-read every 5 s

	// ---- Housekeeping ----
	void Tick();    // main thread, once per frame: eviction and the stats line every 10 s
	void Reset();   // a save load
}
