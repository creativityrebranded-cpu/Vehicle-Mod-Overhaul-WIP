// Standalone check of the nav map against a simulated world (no game).
#include "navmap.h"
#include <cstdarg>
#include <cstdio>
namespace logger { void Write(const char* fmt, ...) { va_list a; va_start(a, fmt); std::vprintf(fmt, a); std::printf("\n"); va_end(a); } bool Verbose() { return false; } }

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
	// (Sections 1-4, the old heading sampler navmap::ChooseHeading, were removed with it on 2026-11-14.)

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
	// Hit normals (2026-11-05): an up-facing hit (a deck, a ramp) is not a wall; a sideways one (a railing) is; no normal still counts.
	navmap::Reset();
	{
		const float from[3] = { 0, 0, 12 }, to[3] = { 0, 20, 8 };
		navmap::NoteRay(from, to, true, 0, 10, 10, 1 /* static */, true, 0.98f);
		double z = 0;
		Check(navmap::HitNear(0, 10, 9, 11, 1.0, &z), "an up-facing hit is still a hit");
		Check(!navmap::WallNear(0, 10, 9, 11, 1.0, &z), "but not a wall: a deck or a ramp");
		Check(navmap::TakeFloorSkips() >= 1, "and the probe counts it as a floor ignored");
		navmap::NoteRay(from, to, true, 2, 12, 10, 1, true, 0.05f);
		Check(navmap::WallNear(2, 12, 9, 11, 1.0, &z), "a sideways hit (a railing) is a wall");
		navmap::NoteRay(from, to, true, 4, 14, 10, 1, false);
		Check(navmap::WallNear(4, 14, 9, 11, 1.0, &z), "a hit with no normal still counts as a wall (no change from before)");
		bool face = true;
		navmap::WallNear(4, 14, 9, 11, 1.0, &z, &face);
		Check(!face, "a hit with no normal is not known to be a face");
		navmap::NoteRay(from, to, true, 6, 16, 9.5f, 1, true, 0.02f);
		navmap::NoteRay(from, to, true, 6.3f, 16, 10.6f, 1, true, 0.02f);
		Check(navmap::WallNear(6, 16, 9, 11, 1.0, &z, &face) && face && z > 10.5, "a barrier face: reported as a face, at its highest hit");
	}

	{	// Boxes kept whole, beside the bands.
		navmap::Reset();
		navmap::NoteObject(100, 100, 0.5, 3, 8, 10, 14);
		navmap::NoteObject(100, 100, 0.5, 3, 8, 10, 14);
		navmap::KeptBox kb[8];
		int n = navmap::BoxesNear(101, 99, 20, kb, 8);
		Check(n == 1, "a box noted twice is kept once");
		Check(n == 1 && kb[0].hy == 8 && kb[0].yaw == 0.5 && kb[0].zHi == 14, "its real size and heading survive");
		Check(navmap::BoxesNear(400, 400, 20, kb, 8) == 0, "nothing far away");
		navmap::NoteObject(127, 64, 0, 10, 10, 0, 2);  // straddles two buckets
		Check(navmap::BoxesNear(120, 62, 5, kb, 8) >= 1 && navmap::BoxesNear(130, 66, 5, kb, 8) >= 1, "a box across a bucket edge is found from both sides");
		// Stacked decks: every level is remembered, `ground` still reads the highest as before.
		navmap::NoteGround(300, 300, 50);
		navmap::NoteGround(300, 300, 62);
		navmap::NoteGround(300, 300, 50.4);
		float lv[3]; const int m = navmap::GroundLevels(300, 300, lv, 3);
		Check(m == 2, "two decks over one spot are two levels");
		Check(navmap::GroundLevels(900, 900, lv, 3) == 0, "no levels where nothing was seen");
		navmap::Reset();
		Check(navmap::BoxesNear(101, 99, 20, kb, 8) == 0, "a reset clears kept boxes");
	}

	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}





