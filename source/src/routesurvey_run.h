#pragma once
#include <memory>
// Route survey, game side (routesurvey.cpp). World source; decides nothing.
namespace planworld { struct Shape; }
namespace routesurvey {
void Tick(bool loading);  // main thread, frame task: sweeps this world's routes near the player after a load and as the player moves
// The measured floor at (x, y) nearest zRef within `window` m; false when not surveyed there (any thread).
bool FloorAt(double x, double y, double zRef, double window, double& z);
// A measured floor point from elsewhere (the look-ahead doubt probe's down rays), into the same store (any thread).
void NoteFloor(double x, double y, double z, double nz);
// WM-1 (2026-11-18, one world map): the levels car `slot` can reach from where it stands, flooded at most every rules::mapReachEvery s
// (or at once when it moved more than 5 m since); returns the reachable levels and the flood's ms (0 when the last one stands).
int UpdateReach(int slot, double x, double y, double z, double now, double& ms);
// The car's reachable floor at (x, y) nearest zRef within `window` (slope-correct); false: no reachable level measured here.
bool FloorReachAt(int slot, double x, double y, double zRef, double window, double& z);
// For a bounded batch of radar shapes, flag only point hits proven below the current floor.
// The caller initializes the flags; original evidence stays in radar for subsequent checks.
void CheckRadarPoints(int slot, const planworld::Shape* shapes, int count, double step, double carHeight,
                      bool* below, double* floorZ);
// Map verdict for a hit point of car `slot` (one judge for every reader): 1 road (within tol of the reachable floor), 2 stands on it (more
// than tol over it, under carHeight), 3 another level (a floor the car cannot reach, or below its floor), 0 unknown (nothing measured).
int MapVerdict(int slot, double x, double y, double z, double tol, double carHeight, double& floorZ);
// RF-1 (2026-11-19): the road surface under a hit, fitted over car `slot`'s reachable floor 2 m round (x, y) on the level nearest zRef
// within window. ok: a trustworthy plane (5 kept points over both axes, a drivable grade); spread: how far the road's own pattern
// varies about it (m, RMS of the kept points); outliers: points set aside (a tooth too high, a lip). False: no reachable floor there.
struct RoadSurface { bool ok = false; double z = 0, spread = 0; int samples = 0, outliers = 0; };
bool RoadFit(int slot, double x, double y, double zRef, double window, RoadSurface& out);
// The clear-path sweep (any thread). KnownSolid: a live hit here is on a solid the sweep already knows. SolidsNear: the swept solids
// within `radius` of (x, y) at about height z, into out. RowAt: what the sweep measured across the lane here: its across direction and
// blocked scan lines as bits (planbranch::ClearBranch reads them); false: not swept here.
// NoClearNear: a swept row near here has no room for a car (the F7 tile warning).
struct SweptSolid { float x, y, lo, hi; };
struct SweepView;
std::shared_ptr<const SweepView> ReadSweep();
bool KnownSolid(const SweepView& view, double x, double y, double z);
int SolidsNear(const SweepView& view, double x, double y, double z, double radius, SweptSolid* out, int max);
bool RowAt(const SweepView& view, double x, double y, double z, double& ux, double& uy, unsigned& blocked);
bool KnownSolid(double x, double y, double z);
int ForgetSolid(double x, double y);  // rays passed clean through: the swept solid here is dropped
int SolidsNear(double x, double y, double z, double radius, SweptSolid* out, int max);
bool RowAt(double x, double y, double z, double& ux, double& uy, unsigned& blocked);
bool NoClearNear(double x, double y, double radius);
// True on the thread that runs the frame task (where CastNow may be called); false before the first Tick.
bool OnMainThread();
// The frame task's frame number (one per Tick): for per-frame budgets on the main thread.
unsigned long long Frame();
}
