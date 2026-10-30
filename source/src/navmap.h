#pragma once

#include <cstdint>

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
	void NoteRay(const float from[3], const float to[3], bool hit, float hx, float hy, float hz, std::uint32_t layer, bool normalOk);
	// A contact at the vehicle's place: up = the solver pushed it up (a floor under it), else a side bump toward (dx, dy).
	void NoteContact(double x, double y, double z, bool up, double dx, double dy);
	// The game's own collision objects (navseed, 2026-10-20): a solid object's box (centre, heading, half-extents, bottom and top) marks
	// every cell it covers as solid between zlo and zhi (two height bands per cell, so a floor and a roof above it stay apart). Refreshed
	// every few seconds by the scan; NoteSeeded says which area the last full scan covered (cells there count as known).
	void NoteObject(double cx, double cy, double yaw, double hx, double hy, double zlo, double zhi);
	void NoteSeeded(double cx, double cy, double radius);
	// How far the scanned (known) area reaches from (x, y) along `heading` (0 outside it or before the first scan).
	double KnownAhead(double x, double y, double heading);

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
	// A blocked straight way: tries the sweep ring's compass directions (every 22.5 degrees) within 120 degrees of the
	// bearing to the target, sweeps the vehicle along each against this map for about 2.5 s of travel, and scores them: progress toward the
	// target, how far the run is open, and not changing the current detour more than needed. Headings with no data
	// (map unknown) are not chosen; none usable = the caller keeps its own rule.
	struct HeadingQuery
	{
		float pos[3] = {};
		double bearing = 0;          // world bearing to the target, atan2(dx, dy)
		bool haveAvoid = false;
		double avoidOffset = 0;      // the offset found blocked (rad): candidates within 8 degrees of it are skipped
		double currentOffset = 0;    // the detour in use (rad): staying near it is cheap
		double speed = 0;
		double halfWidth = 2.5;      // vehicle half-width plus margin
		double clearance = 4.0;
		double climbRate = 6.0;
	};
	struct HeadingChoice
	{
		bool found = false;
		double offset = 0, freeDist = 0, score = 0;
		int usable = 0, blocked = 0, unknown = 0;
	};
	HeadingChoice ChooseHeading(const HeadingQuery& q);
	bool RaysEnabled();            // nav.ini [nav] rays=0 stops every engine ray and hull-sweep query (default on); the walls-only nav map then gets nothing from them
	bool NewAutopilot();           // nav.ini [nav] autopilot=0 goes back to 0.8's autopilot rules (default 1: autodrive.h)
	bool HullEnabled();            // nav.ini [nav] shape=0 turns the hull-sized sweeps off (default on); a fault turns them off by itself
	// For the log line: the straight way's thin ray and hull sweep verdicts side by side this frame.
	void NoteHull(bool rayBlocked, bool hullBlocked);
	bool SamplerEnabled();        // Logs\FlyingVehicleOverhaul\nav.ini [nav] sampler=0 turns it off (default on); re-read every 5 s
	void NoteSampler(bool used);  // for the log line: used the sampler's choice / fell back to the old rule

	// ---- Housekeeping ----
	void Tick();    // main thread, once per frame: eviction and the stats line every 10 s
	void Reset();   // a save load
}
