// Unit tests for the autopilot's stuck sense, trail memory and backtrack planner (stucksense.h, 2026-10-31).
#include "stucksense.h"

#include <cstdio>

using namespace stucksense;

static int g_failures = 0;
static void Check(bool ok, const char* name, const char* detail = "")
{
	std::printf("%s %s %s\n", ok ? "PASS" : "FAIL", name, detail);
	if (!ok) ++g_failures;
}

constexpr double kDt = 1.0 / 30.0;
constexpr double kPi = 3.14159265358979323846;

// Feeds a motion for `seconds`; pos(t) gives the position, the goal sits at (gx, gy). Returns the first time a rule fired
// (and which), or -1.
template <class PosFn>
static double Run(Sense& s, double t0, double seconds, PosFn&& pos, double gx, double gy, Kind& kind, bool trying = true, bool countProgress = true)
{
	kind = Kind::None;
	for (double t = t0; t < t0 + seconds; t += kDt) {
		double x, y;
		pos(t, x, y);
		const Reading r = s.Feed(t, x, y, std::hypot(gx - x, gy - y), trying, countProgress);
		if (r.kind != Kind::None) { kind = r.kind; return t; }
	}
	return -1;
}

int main()
{
	// ---------------- Sense ----------------
	{  // Trying, not moving at all: pinned at 3.5 s, not before.
		Sense s;
		Kind k;
		const double t = Run(s, 0, 10, [](double, double& x, double& y) { x = 0; y = 0; }, 0, 100, k);
		char b[64]; std::snprintf(b, sizeof b, "(fired at %.2f s)", t);
		Check(k == Kind::Pinned && t >= 3.5 && t < 3.6, "sense: standing still while trying = pinned at 3.5 s", b);
	}
	{  // Driving at 3 m/s toward the goal for 60 s: never fires.
		Sense s;
		Kind k;
		const double t = Run(s, 0, 60, [](double t, double& x, double& y) { x = 0; y = 3.0 * t; }, 0, 1000, k);
		Check(t < 0, "sense: steady progress never fires");
	}
	{  // Slow but steady (1.3 m/s): neither rule (4.5 m per 3.5 s, 11.7 m per 9 s).
		Sense s;
		Kind k;
		const double t = Run(s, 0, 60, [](double t, double& x, double& y) { x = 0; y = 1.3 * t; }, 0, 1000, k);
		Check(t < 0, "sense: slow steady progress never fires");
	}
	{  // A legitimate detour away from the goal at 3 m/s (the goal behind a wall): moving well, never fires.
		Sense s;
		Kind k;
		const double t = Run(s, 0, 30, [](double t, double& x, double& y) { x = 3.0 * t; y = -0.5 * t; }, 0, 100, k);
		Check(t < 0, "sense: a detour that moves away from the goal is not stuck");
	}
	{  // Circling (radius 4 m at 3 m/s): initial movement brings it closer; then nine seconds without a new best fires.
		Sense s;
		Kind k;
		const double t = Run(s, 0, 20, [](double t, double& x, double& y) { x = 4.0 * std::sin(t * 0.75); y = 4.0 - 4.0 * std::cos(t * 0.75); }, 0, 100, k);
		char b[64]; std::snprintf(b, sizeof b, "(fired at %.2f s, %s)", t, Name(k));
		Check(k == Kind::NoProgress && t >= 12.0 && t < 13.0, "sense: circling = getting nowhere after initial progress expires", b);
	}
	{  // Sliding to and fro along a wall (±7 m at a steady 3 m/s, the goal straight past the wall): getting nowhere.
		Sense s;
		Kind k;
		const double t = Run(s, 0, 20, [](double t, double& x, double& y) {
			const double u = std::fmod(t * 3.0, 28.0);  // 28 m there and back
			x = u < 14.0 ? -7.0 + u : 21.0 - u;
			y = 40.0;
		}, 0, 100, k);
		char b[64]; std::snprintf(b, sizeof b, "(fired at %.2f s, %s)", t, Name(k));
		Check(k == Kind::NoProgress, "sense: sliding to and fro = getting nowhere", b);
	}
	{  // The reading carries what it measured.
		Sense s;
		Reading got;
		for (double t = 0; t < 20; t += kDt) {
			const double x = 4.0 * std::sin(t * 0.75), y = 4.0 - 4.0 * std::cos(t * 0.75);
			const Reading r = s.Feed(t, x, y, std::hypot(x, 100 - y), true, true);
			if (r.kind != Kind::None) { got = r; break; }
		}
		char b[128]; std::snprintf(b, sizeof b, "(window %.1f s, moved %.1f m, travelled %.1f m, gain %.1f m)", got.window, got.moved, got.travelled, got.gain);
		Check(got.window >= 9.0 && got.moved < 8.0 && got.travelled > 20.0 && got.gain < 2.0, "sense: reading reports window, net move, path length, gain", b);
	}
	{  // Waiting on purpose (not trying) for 60 s, standing still: never fires.
		Sense s;
		Kind k;
		const double t = Run(s, 0, 60, [](double, double& x, double& y) { x = 0; y = 0; }, 0, 100, k, false, false);
		Check(t < 0, "sense: waiting on purpose never fires");
	}
	{  // Giving way (trying, but not counting progress) while circling slowly: only pinned may fire, not getting nowhere.
		Sense s;
		Kind k;
		const double t = Run(s, 0, 30, [](double t, double& x, double& y) { x = 4.0 * std::sin(t * 0.75); y = 4.0 - 4.0 * std::cos(t * 0.75); }, 0, 100, k, true, false);
		Check(k != Kind::NoProgress, "sense: frames that do not count progress never give getting nowhere");
		(void)t;
	}
	{  // A pause (2 s gap, the paused map or a loading screen) restarts the window: no fire until 3.5 s after it.
		Sense s;
		for (double t = 0; t < 3.0; t += kDt) s.Feed(t, 0, 0, 100, true, true);
		Reading r = s.Feed(5.0, 0, 0, 100, true, true);
		Check(r.kind == Kind::None, "sense: the first frame after a pause never fires");
		double fired = -1;
		for (double t = 5.0 + kDt; t < 12.0; t += kDt) { r = s.Feed(t, 0, 0, 100, true, true); if (r.kind != Kind::None) { fired = t; break; } }
		char b[64]; std::snprintf(b, sizeof b, "(fired at %.2f s)", fired);
		Check(fired >= 8.5 && fired < 8.6, "sense: pause restarts the window (3.5 s after it)", b);
	}
	{  // One event per episode: after a pinned event the next needs a fresh 3.5 s.
		Sense s;
		int events = 0;
		for (double t = 0; t < 10.0; t += kDt) if (s.Feed(t, 0, 0, 100, true, true).kind != Kind::None) ++events;
		Check(events == 2, "sense: standing still 10 s = two pinned events (3.5 s each), not one per frame");
	}

	{  // Pinned scales with the asked speed (2026-10-31 live run): asked 2 m/s, moving 0.6 m/s: not pinned; standing: pinned.
		Sense s;
		bool fired = false;
		for (double t = 0; t < 20.0; t += kDt) fired |= s.Feed(t, 0, 0.6 * t, 100 - 0.6 * t, true, false, 2.0).kind == Kind::Pinned;
		Check(!fired, "sense: asked 2 m/s and creeping at 0.6 m/s is not pinned");
		Sense still;
		double at = -1;
		for (double t = 0; t < 10.0 && at < 0; t += kDt) if (still.Feed(t, 0, 0.1 * t, 100, true, false, 2.0).kind == Kind::Pinned) at = t;
		Check(at >= 3.5 && at < 3.6, "sense: asked 2 m/s and (almost) still is pinned at 3.5 s");
		Sense fast;
		bool f2 = false;
		for (double t = 0; t < 20.0; t += kDt) f2 |= fast.Feed(t, 0, 1.0 * t, 100, true, false, 10.0).kind == Kind::Pinned;
		Check(f2, "sense: asked 10 m/s and moving 1 m/s is pinned (the 4 m rule)");
	}
	{  // The reading reports the vehicle's own mean speed over the window.
		Sense s;
		Reading got;
		for (double t = 0; t < 10.0; t += kDt) { const Reading r = s.Feed(t, 0, 0, 100, true, false, 5.0, 20.0); if (r.kind != Kind::None) { got = r; break; } }
		Check(got.kind == Kind::Pinned && std::fabs(got.reported - 20.0) < 0.5, "sense: pinned reading carries the reported speed (reference not following the body)");
	}

	// ---------------- Trail ----------------
	{
		Trail tr;
		for (int i = 0; i <= 100; ++i) tr.Record(i * 0.1, 0, i * 1.0);  // 1 m steps
		Check(tr.Size() == 26, "trail: points kept 4 m apart");
		Check(tr.At(1).y - tr.At(0).y >= 4.0, "trail: spacing at least 4 m");
		Trail big;
		for (int i = 0; i < 200; ++i) big.Record(i, 0, i * 5.0);
		Check(big.Size() == Trail::kMax && big.At(Trail::kMax - 1).y == 199 * 5.0 && big.At(0).y == (200 - Trail::kMax) * 5.0, "trail: bounded, keeps the newest");
	}
	{  // PickBack: first point at least minBack, newest first.
		Trail tr;
		for (int i = 0; i <= 10; ++i) tr.Record(i, 0, i * 4.0);  // 0, 4, ... 40
		auto none = [](double, double) { return false; };
		const int i = tr.PickBack(0, 40, 8.0, none);
		Check(i >= 0 && tr.At(i).y == 32.0, "trail: picks the newest point 8 m back");
		const int j = tr.PickBack(0, 40, 8.0, [](double, double y) { return y > 25.0 && y < 35.0; });
		Check(j >= 0 && tr.At(j).y == 24.0, "trail: skips points the caller says are closed");
		const int k = tr.PickBack(0, 40, 100.0, none);
		Check(k == 0, "trail: none that far: falls back to the oldest");
		Trail shortTr;
		shortTr.Record(0, 0, 0);
		Check(shortTr.PickBack(0, 1.0, 8.0, none) == -1, "trail: nothing 3 m away: no point");
		Check(std::fabs(tr.Along(8, 0, 41) - 9.0) < 1e-9, "trail: distance along it to the vehicle");
		Trail drop = tr;
		drop.DropSince(7.5);
		Check(drop.Size() == 8 && drop.At(7).y == 28.0, "trail: points recorded while stuck are dropped");
		tr.TrimAfter(8);
		Check(tr.Size() == 9 && tr.At(8).y == 32.0, "trail: trimmed after the point backed to");
	}

	// ---------------- Backtracker ----------------
	auto open = [](double, double) { return false; };
	{  // Escalation, give-up bound, cool-off, then fresh attempts.
		Backtracker b;
		for (int i = 0; i <= 25; ++i) b.trail.Record(i, 0, i * 4.0);  // 0 .. 100 north
		Plan p1 = b.OnStuck(0, 0, 100, 1, 0.0, 2.3, 50, true, open);
		Check(p1.ok && p1.haveCrumb && p1.attempt == 1 && p1.alt == Alt::SideDetour && p1.back >= 8.0 && p1.back < 12.5, "backtrack 1: side detour, about 8 m back");
		Check(b.Active() && b.TargetY() == p1.ty, "backtrack 1: active toward the point");
		Check(std::hypot(p1.markX - 0, p1.markY - 100) >= p1.markR + 2.3, "backtrack 1: the mark is ahead, never over the vehicle");
		Check(p1.markY > 100 && std::fabs(p1.markX) < 1e-9, "backtrack 1: the mark lies along the way it was pushing");
		Check(b.Step(1, 0, p1.ty + 10) == Backtracker::Update::None, "backtrack: not there yet");
		Check(b.Step(2, 0, p1.ty + 1) == Backtracker::Update::Reached && !b.Active(), "backtrack: reached within 3 m");
		Plan p2 = b.OnStuck(10, 0, p1.ty, 1, 0.0, 2.3, 50, true, open);
		Check(p2.attempt == 2 && p2.alt == Alt::Climb && p2.back >= 16.0, "backtrack 2: climb when the air is allowed, 16 m back");
		b.Cancel();
		Plan p3 = b.OnStuck(20, 0, p2.ty, 1, 0.0, 2.3, 50, true, open);
		Check(p3.attempt == 3 && p3.alt == Alt::OtherRoute && p3.back >= 30.0 && p3.markR > p2.markR, "backtrack 3: a different route, 30 m back, wider mark");
		b.Cancel();
		Plan p4 = b.OnStuck(30, 0, p3.ty, 1, 0.0, 2.3, 50, true, open);
		Check(p4.gaveUp && !p4.ok && !b.Active(), "backtrack 4: gives up after 3 tries");
		Plan p5 = b.OnStuck(60, 0, p3.ty, 1, 0.0, 2.3, 50, true, open);
		Check(p5.coolingOff && !p5.ok, "backtrack: cooling off for 60 s after giving up (nothing done)");
		Plan p6 = b.OnStuck(91, 0, p3.ty, 1, 0.0, 2.3, 50, true, open);
		Check(p6.ok && p6.attempt == 1, "backtrack: after the cool-off, attempts start again at 1");
	}
	{  // Drive only (no air): attempt 2 is a wider detour.
		Backtracker b;
		for (int i = 0; i <= 25; ++i) b.trail.Record(i, 0, i * 4.0);
		b.OnStuck(0, 0, 100, 1, 0.0, 2.3, 50, false, open);
		b.Cancel();
		Plan p2 = b.OnStuck(5, 0, 100, 1, 0.0, 2.3, 50, false, open);
		Check(p2.alt == Alt::WiderDetour, "backtrack 2, no air: a wider detour instead of climbing");
	}
	{  // No trail: the attempt still counts, marks the spot, has no point to back to.
		Backtracker b;
		Plan p = b.OnStuck(0, 5, 5, 1, 1.0, 2.3, 50, true, open);
		Check(p.ok && !p.haveCrumb && !b.Active(), "backtrack: no trail = alternative from here, nothing to steer to");
	}
	{  // Out of time on the way back.
		Backtracker b;
		for (int i = 0; i <= 10; ++i) b.trail.Record(i, 0, i * 4.0);
		b.OnStuck(0, 0, 40, 1, 0.0, 2.3, 50, true, open);
		Check(b.Step(10, 0, 40) == Backtracker::Update::None && b.Step(26, 0, 40) == Backtracker::Update::TimedOut && !b.Active(), "backtrack: times out after 25 s");
	}
	{  // Recovery: 15 m closer than at the first stuck resets the attempts; a new goal does too.
		Backtracker b;
		for (int i = 0; i <= 25; ++i) b.trail.Record(i, 0, i * 4.0);
		b.OnStuck(0, 0, 100, 1, 0.0, 2.3, 50, true, open);
		Check(!b.NoteGoalDist(20), "backtrack: no recovery while still backing off");
		b.Step(1, b.TargetX(), b.TargetY());
		Check(!b.NoteGoalDist(40), "backtrack: 10 m closer is not yet recovered");
		Check(b.NoteGoalDist(30) && b.Attempts() == 0 && b.Gained() >= 15.0, "backtrack: 20 m closer = recovered, attempts reset");
		b.OnStuck(5, 0, 100, 1, 0.0, 2.3, 50, true, open);
		b.NewGoal();
		Check(b.Attempts() == 0 && !b.Active(), "backtrack: a new goal resets attempts and stops backing");
	}
	{  // The trail point is never inside the new mark (backing toward the stuck direction).
		Backtracker b;
		// The trail came from the north (the vehicle drove south), stuck pushing north again (turned round): the newest
		// points lie ahead, inside the mark.
		for (int i = 0; i <= 10; ++i) b.trail.Record(i, 0, 40.0 - i * 4.0);  // 40 .. 0
		Plan p = b.OnStuck(0, 0, 0, 1, 0.0, 2.3, 50, true, open);
		Check(!p.haveCrumb || std::hypot(p.tx - p.markX, p.ty - p.markY) >= p.markR + 2.0, "backtrack: never picks a point inside the blocked spot");
	}

	std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
	return g_failures ? 1 : 0;
}
