// Standalone check of the nav map + heading sampler against a simulated world (no game).
#include "navmap.h"
#include <cstdarg>
#include <cstdio>
namespace logger { void Write(const char* fmt, ...) { va_list a; va_start(a, fmt); std::vprintf(fmt, a); std::printf("\n"); va_end(a); } }

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* what) { std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }

// A wall: the segment from (x0, y0) to (x1, y1), solid at every height.
struct Wall { double x0, y0, x1, y1; };

// Distance along the ray from (ox, oy) with unit direction (dx, dy) to the first wall, or -1.
double Cast(double ox, double oy, double dx, double dy, double maxLen, const std::vector<Wall>& walls)
{
	double best = -1;
	for (const auto& w : walls) {
		const double sx = w.x1 - w.x0, sy = w.y1 - w.y0;
		const double den = dx * sy - dy * sx;
		if (std::fabs(den) < 1e-9) continue;
		const double t = ((w.x0 - ox) * sy - (w.y0 - oy) * sx) / den;
		const double u = ((w.x0 - ox) * dy - (w.y0 - oy) * dx) / den;
		if (t >= 0 && t <= maxLen && u >= 0 && u <= 1 && (best < 0 || t < best)) best = t;
	}
	return best;
}

// What one sweep of the 16-direction ring does for a vehicle at (px, py, pz): the rays the flight code would cast.
void Sweep(double px, double py, double pz, double range, const std::vector<Wall>& walls)
{
	for (int i = 0; i < 16; ++i) {
		const double hd = i * (6.283185307179586 / 16.0);
		const double dx = std::sin(hd), dy = std::cos(hd);
		const float from[3] = { float(px + dx * 3.5), float(py + dy * 3.5), float(pz + 1.0) };
		const float to[3] = { float(from[0] + dx * range), float(from[1] + dy * range), from[2] };
		const double t = Cast(from[0], from[1], dx, dy, range, walls);
		navmap::NoteRay(from, to, t >= 0, float(from[0] + dx * (t >= 0 ? t : 0)), float(from[1] + dy * (t >= 0 ? t : 0)), from[2], 1, true);
	}
}
}  // namespace

int main()
{
	const double kPi = 3.14159265358979;
	navmap::HeadingQuery q;
	q.pos[0] = 0; q.pos[1] = 0; q.pos[2] = 10;
	q.bearing = 0;  // the target is north
	q.speed = 10;
	q.halfWidth = 2.5; q.clearance = 4.0; q.climbRate = 6.0;

	// 1. Nothing known: nothing usable (the caller keeps its own rule).
	navmap::Reset();
	{
		const auto c = navmap::ChooseHeading(q);
		Check(!c.found, "empty map: no heading chosen");
	}

	// 2. A wall across the way north, open to the east: it should turn east, not west.
	navmap::Reset();
	std::vector<Wall> walls = { { -80, 30, 6, 30 } };  // y = 30, from x -80 to 6
	for (int k = 0; k < 3; ++k) Sweep(0, 0, 10, 40, walls);
	{
		float p[3] = { 0, 0, 10 };
		double at = 0;
		const auto v = navmap::PathClear(p, 0, 40, 2.5, 4.0, 6.0, 10, &at, nullptr);
		Check(v == navmap::Verdict::Blocked, "straight north is blocked by the wall");
		Check(at > 20 && at < 34, "...about 30 m ahead");
		q.haveAvoid = true; q.avoidOffset = 0; q.currentOffset = 0;
		const auto c = navmap::ChooseHeading(q);
		std::printf("  chosen offset %+.1f deg, open %.0f m, usable %d, blocked %d, unknown %d\n", c.offset * 180 / kPi, c.freeDist, c.usable, c.blocked, c.unknown);
		Check(c.found, "a heading is found");
		Check(c.offset > 0.3, "it turns east (the open side)");
	}

	// 3. The wall now blocks the east too, only the west is open: it must go west.
	navmap::Reset();
	walls = { { -6, 30, 80, 30 } };
	for (int k = 0; k < 3; ++k) Sweep(0, 0, 10, 40, walls);
	{
		const auto c = navmap::ChooseHeading(q);
		std::printf("  mirror: found %d offset %+.1f deg, open %.0f m, usable %d, blocked %d, unknown %d\n", c.found, c.offset * 180 / kPi, c.freeDist, c.usable, c.blocked, c.unknown);
		Check(c.found && c.offset < -0.3, "mirror world: it turns west");
	}

	// 4. Boxed in: walls on every side close by: nothing usable.
	navmap::Reset();
	walls = { { -5, 5, 5, 5 }, { -5, -5, 5, -5 }, { 5, -5, 5, 5 }, { -5, -5, -5, 5 } };
	for (int k = 0; k < 3; ++k) Sweep(0, 0, 10, 40, walls);
	{
		const auto c = navmap::ChooseHeading(q);
		std::printf("  boxed in: found %d, usable %d, blocked %d, unknown %d\n", c.found, c.usable, c.blocked, c.unknown);
		Check(!c.found, "boxed in: no heading (the caller climbs / backs out)");
	}

	// 5. A learned floor (a push-up) blocks a path that would otherwise look clear over low ground.
	navmap::Reset();
	for (int i = 0; i < 20; ++i) navmap::NoteGround(0, 4.0 * i, 0.0);
	{
		float p[3] = { 0, 0, 10 };
		Check(navmap::PathClear(p, 0, 70, 2.5, 4.0, 6.0, 10, nullptr, nullptr) == navmap::Verdict::Clear, "ground 10 m below: clear");
		navmap::NoteContact(0, 14, 25.0, true, 0, 1);  // pushed up at z 25 at y = 14: a floor at z 23.5
		double at = 0;
		const auto v = navmap::PathClear(p, 0, 70, 2.5, 4.0, 6.0, 10, &at, nullptr);
		std::printf("  floor test: verdict %d at %.1f\n", int(v), at);
		Check(v == navmap::Verdict::Blocked && at >= 8 && at < 22, "a remembered floor 13 m above, 14 m ahead, blocks the way");
	}

	// 6. A hit on a character is free space, not a wall.
	navmap::Reset();
	{
		const float from[3] = { 0, 3.5f, 11 }, to[3] = { 0, 43.5f, 11 };
		navmap::NoteRay(from, to, true, 0, 23.5f, 11, 8 /* biped */, true);
		float p[3] = { 0, 0, 10 };
		Check(navmap::PathClear(p, 0, 40, 0.5, 4.0, 6.0, 10, nullptr, nullptr) != navmap::Verdict::Blocked, "a character in the way is not remembered as a wall");
	}

	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}





