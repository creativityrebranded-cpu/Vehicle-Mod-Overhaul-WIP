#pragma once

// Builds and checks scripted-flight paths against the planner's map (2026-10-24, the user: "build a script and see if
// there's anything that will interrupt it from the data stored from map the planner used ... creating extra tolerance").
// Pure logic over a map query, so it is testable; queuedtraffic adapts navmap::At (4 m cells: solid ray hits and the
// game's collision boxes, by height). A path is swept as the vehicle's box grown by `tolerance` on every side.
// Unknown cells do not block (no data is not a wall) but are counted, so the log says how much was actually checked.
#include <algorithm>
#include <cmath>
#include <functional>

namespace scriptbuild
{
	struct Cell { bool known = false, solid = false; };
	using Query = std::function<Cell(double x, double y, double z)>;
	struct Hull { double halfWidth = 1.5, halfLength = 2.5, height = 2.0; };

	struct Check
	{
		int samples = 0, known = 0, solid = 0;
		double atX = 0, atY = 0, atZ = 0;  // the first solid sample
		bool Clear() const { return solid == 0; }
		void Add(const Cell& c, double x, double y, double z)
		{
			++samples; if (c.known) ++known;
			if (c.solid) { if (!solid) { atX = x; atY = y; atZ = z; } ++solid; }
		}
		void Merge(const Check& o) { if (o.solid && !solid) { atX = o.atX; atY = o.atY; atZ = o.atZ; } samples += o.samples; known += o.known; solid += o.solid; }
	};

	constexpr double kStep = 2.0;  // m between samples across (half a map cell); heights every 1 m (thin roofs)

	// The vehicle's box (half-width + tolerance either side, from z to z + height + tolerance) swept from A to B.
	inline Check Sweep(const Query& q, double ax, double ay, double bx, double by, double z, const Hull& h, double tol)
	{
		Check c;
		const double dx = bx - ax, dy = by - ay, len = std::hypot(dx, dy);
		const double ux = len > 0.01 ? dx / len : 0, uy = len > 0.01 ? dy / len : 1;
		const double side = h.halfWidth + tol, top = h.height + tol;
		const double ext = h.halfLength + tol;  // the nose reaches past the end point
		for (double s = -ext; s <= len + ext + 1e-6; s += kStep)
			for (double w = -side; w <= side + 1e-6; w += std::max(kStep, side))
				for (double hz = 0.5; hz <= top + 1e-6; hz += 1.0)
				{
					const double x = ax + ux * s - uy * w, y = ay + uy * s + ux * w, zz = z + hz;
					c.Add(q(x, y, zz), x, y, zz);
				}
		return c;
	}

	// A vertical column (square of half-size r) from z0 to z1: the space a vehicle rises or descends through.
	inline Check Column(const Query& q, double x, double y, double z0, double z1, double r)
	{
		Check c;
		const double lo = std::min(z0, z1), hi = std::max(z0, z1);
		for (double z = lo; z <= hi + 1e-6; z += 1.0)
			for (double ox = -r; ox <= r + 1e-6; ox += std::max(kStep, r))
				for (double oy = -r; oy <= r + 1e-6; oy += std::max(kStep, r))
					c.Add(q(x + ox, y + oy, z), x + ox, y + oy, z);
		return c;
	}

	// How the vehicle leaves its bay: straight forward or straight back along its parked heading, far enough that the
	// column above the end point is open to cruise height. The first fully clear candidate wins (forward first, short
	// first); with none clear, the one with the fewest solid samples, flagged not ok.
	struct DriveOut
	{
		bool ok = false;
		double distance = 15;  // signed: + forward, - reverse
		double fx = 0, fy = 0; // the end point (where it rises)
		Check lane, column;
	};
	inline DriveOut ChooseDriveOut(const Query& q, double x, double y, double z, double yaw, const Hull& h, double tol, double cruiseZ, double fixed = 0)
	{
		DriveOut best; int bestSolid = 1 << 30;
		const double fwdX = std::sin(yaw), fwdY = std::cos(yaw);
		const double signs[2] = { 1, -1 }, lengths[3] = { 12, 18, 25 };
		for (double sgn : signs)
			for (double d : lengths) {
				const double dist = fixed != 0 ? fixed : sgn * d;
				DriveOut c; c.distance = dist; c.fx = x + fwdX * dist; c.fy = y + fwdY * dist;
				c.lane = Sweep(q, x, y, c.fx, c.fy, z, h, tol);
				c.column = Column(q, c.fx, c.fy, z + 0.5, cruiseZ + h.height, std::max(h.halfWidth, h.halfLength) + tol);
				c.ok = c.lane.Clear() && c.column.Clear();
				const int solid = c.lane.solid + c.column.solid;
				if (c.ok) return c;
				if (solid < bestSolid) { bestSolid = solid; best = c; }
				if (fixed != 0) return best;  // the user's own value: checked and reported, never replaced
			}
		return best;
	}

	// A straight leg at height z; when something stands in it, try higher (z + each raise) and take the first clear one.
	struct Leg { bool ok = false; double z = 0; Check check; };
	inline Leg ChooseLegHeight(const Query& q, double ax, double ay, double bx, double by, double z, const Hull& h, double tol)
	{
		const double raises[4] = { 0, 10, 20, 40 };
		Leg first;
		for (double r : raises) {
			Leg l; l.z = z + r; l.check = Sweep(q, ax, ay, bx, by, l.z, h, tol); l.ok = l.check.Clear();
			if (l.ok) return l;
			if (r == 0) first = l;
		}
		return first;  // nothing clear: the planned height, flagged (the runtime hands that leg to the autopilot)
	}
}
