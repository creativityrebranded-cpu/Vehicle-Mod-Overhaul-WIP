#include "scriptbuild.h"
#include <cstdio>

using namespace scriptbuild;

namespace {
int failures = 0;
void Expect(bool ok, const char* name) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; }
}

int main() {
	const Hull hull{ 1.5, 2.5, 2.0 };
	// Bay at the origin facing +y (yaw 0), ground z 0, cruise at 40.
	{
		// A wall (one 4 m map cell) 5-9 m in front: drive out backwards.
		Query q = [](double, double y, double z) { Cell c; c.known = true; c.solid = y > 5 && y < 9 && z < 10; return c; };
		const auto d = ChooseDriveOut(q, 0, 0, 0, 0, hull, 1.5, 40);
		Expect(d.ok && d.distance < 0, "wall ahead of the bay: reverses out");
	}
	{
		// A roof 5 m up over the bay reaching 20 m forward and 6 m back: forward 25 m clears it.
		Query q = [](double, double y, double z) { Cell c; c.known = true; c.solid = y > -6 && y < 20 && z > 5 && z < 6; return c; };
		const auto d = ChooseDriveOut(q, 0, 0, 0, 0, hull, 1.5, 40);
		Expect(d.ok && d.distance == 25, "roof over the bay: drives out far enough to rise clear");
	}
	{
		// Enclosed on all sides: not ok, but still the least-blocked choice.
		Query q = [](double x, double y, double z) { Cell c; c.known = true; c.solid = (std::fabs(x) > 4 || std::fabs(y) > 8) && z < 30; return c; };
		const auto d = ChooseDriveOut(q, 0, 0, 0, 0, hull, 1.5, 40);
		Expect(!d.ok, "boxed in: reported not ok");
	}
	{
		// The user's fixed drive-out is kept even when blocked (reported, never replaced).
		Query q = [](double, double y, double z) { Cell c; c.known = true; c.solid = y > 5 && y < 9 && z < 10; return c; };
		const auto d = ChooseDriveOut(q, 0, 0, 0, 0, hull, 1.5, 40, 15);
		Expect(!d.ok && d.distance == 15, "a fixed drive-out is checked, not replaced");
	}
	{
		// A 30 m tall building across a leg at 20 m: the leg goes up to clear it.
		Query q = [](double x, double, double z) { Cell c; c.known = true; c.solid = x > 50 && x < 60 && z < 32; return c; };
		const auto l = ChooseLegHeight(q, 0, 0, 120, 0, 20, hull, 1.5);
		Expect(l.ok && l.z >= 30, "building across a leg: flies over it");
		Expect(ChooseLegHeight(q, 0, 0, 40, 0, 20, hull, 1.5).z == 20, "clear leg keeps its height");
	}
	{
		// No map data: clear, but nothing known.
		Query q = [](double, double, double) { return Cell{}; };
		const auto s = Sweep(q, 0, 0, 100, 0, 10, hull, 1.5);
		Expect(s.Clear() && s.known == 0 && s.samples > 0, "unknown space does not block but counts as unchecked");
	}
	{
		// Tolerance: an obstacle 3.5 m to the side of a 1.5 m half-width vehicle is hit with 2.5 m tolerance, missed with 1 m.
		Query q = [](double x, double, double z) { Cell c; c.known = true; c.solid = x > 3.4 && x < 4.6 && z < 10; return c; };
		Expect(!Sweep(q, 0, 0, 0, 50, 0, hull, 2.5).Clear() && Sweep(q, 0, 0, 0, 50, 0, hull, 1.0).Clear(), "tolerance widens the swept box");
	}
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
