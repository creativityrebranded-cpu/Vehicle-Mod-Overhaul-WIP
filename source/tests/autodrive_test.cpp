// Scenario tests for the autopilot decision core (autodrive.h): a small kinematic world with terrain, walls, hidden
// obstacles and other vehicles. The map the pilot reads is built the way navmap fills in game: ground probes give the
// highest surface (a wall's top), and horizontal rays that meet a hillside are stored as solid hits at the ground.
#include "autodrive.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

using namespace autodrive;

static int g_failures = 0;
static void Check(bool ok, const char* name, const char* detail = "")
{
	std::printf("%s %s %s\n", ok ? "PASS" : "FAIL", name, detail);
	if (!ok) ++g_failures;
}

struct Box { double x0, y0, x1, y1, top; bool hidden = false; };  // a wall / building (top: metres over the terrain)

struct World
{
	std::function<double(double, double)> terrain = [](double, double) { return 0.0; };
	std::vector<Box> boxes;
	std::vector<Box> objectZones;  // game collision boxes that are a poor fit: solid in the map only
	double water0 = 1e9, water1 = -1e9;  // a band of x that is water
	bool mapKnown = true;
	double senseRange = 1e9;  // cells beyond this from the vehicle are unknown

	double Ground(double x, double y, bool includeHidden = true) const
	{
		double g = terrain(x, y);
		for (const auto& b : boxes)
			if ((includeHidden || !b.hidden) && x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) g = std::max(g, terrain(x, y) + b.top);
		return g;
	}
	bool InBox(double x, double y, bool includeHidden) const
	{
		for (const auto& b : boxes)
			if ((includeHidden || !b.hidden) && x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) return true;
		return false;
	}
	// What the shared map would hold (4 m cells, like navmap).
	Cell MapCell(double x, double y, double vx, double vy) const
	{
		Cell c;
		const double cx = (std::floor(x / 4.0) + 0.5) * 4.0, cy = (std::floor(y / 4.0) + 0.5) * 4.0;
		if (!mapKnown || std::hypot(cx - vx, cy - vy) > senseRange) return c;
		c.known = true;
		c.haveGround = true;
		c.ground = Ground(cx, cy, false);
		for (const auto& z : objectZones) if (cx >= z.x0 && cx <= z.x1 && cy >= z.y0 && cy <= z.y1) c.object = true;
		if (InBox(cx, cy, false)) { c.solid = true; c.solidZ = terrain(cx, cy) + 1.5; }
		else {
			// A horizontal ray at the vehicle's height meeting a hillside: stored solid at the ground (navmap does this).
			const double slope = std::hypot(terrain(cx + 1, cy) - terrain(cx - 1, cy), terrain(cx, cy + 1) - terrain(cx, cy - 1)) / 2.0;
			if (slope > 0.05) { c.solid = true; c.solidZ = c.ground + 0.3; }
		}
		return c;
	}
};

struct Car
{
	double x = 0, y = 0, z = 0, heading = 0, v = 0, vx = 0, vy = 0;
	Shape shape;
	Limits limits;
	Permit permit = Permit::Both;
	Pilot pilot;
	std::uint32_t id = 1;
	int priority = 60;
	bool contact = false;
	bool parked = false;  // a stopped vehicle (no pilot)
	double gx = 0, gy = 0, gz = std::numeric_limits<double>::quiet_NaN(), stop = 3.0;
	bool keepGoing = false;
	double tvx = 0, tvy = 0;  // a followed target's velocity
	double groundBias = 0;    // error added to the ground reading the pilot gets
	bool ownRoof = false;     // the map shows this vehicle's own roof and hull hits under it
	bool hoverPossible = true;
	double ceiling = 1e9;
	bool onWheels = true;
	bool preferAir = false;
	Output last;
	int modeChanges = 0, stuckEvents = 0, contacts = 0;
	double minGap = 1e9;
};

struct Sim
{
	World w;
	std::vector<Car> cars;
	double t = 0;
	double dt = 1.0 / 30.0;
	bool trace = false;
	double accelMax = 4.0, yawMax = 1.0, lag = 1.5;  // vehicle response (sluggish variants lower them)

	double Radius(const Car& c) const { return std::hypot(c.shape.halfWidth, c.shape.halfLength); }

	bool Blocked(const Car& c, double nx, double ny, double nz, Mode mode, size_t self) const
	{
		// Walls (hidden ones too: they exist even when the map lacks them).
		for (const auto& b : w.boxes) {
			const double top = w.terrain(nx, ny) + b.top;
			const bool inside = nx >= b.x0 - 1.0 && nx <= b.x1 + 1.0 && ny >= b.y0 - 1.0 && ny <= b.y1 + 1.0;
			if (inside && mode == Mode::Hover && nz < top + 0.5) return true;
		}
		if (mode == Mode::Drive) {
			// Ground (a box's top included) rising too steeply for the wheels, sampled 1 m past the hull's centre.
			const double run = std::hypot(nx - c.x, ny - c.y);
			if (run > 1e-6) {
				const double ux = (nx - c.x) / run, uy = (ny - c.y) / run;
				const double g0 = w.Ground(c.x, c.y), g1 = w.Ground(nx + ux * 1.0, ny + uy * 1.0);
				if (g1 - g0 > std::max(1.0, 0.75 * (run + 1.0))) return true;
			}
			if (nx >= w.water0 && nx <= w.water1) return true;  // wheels do not cross water
		}
		for (size_t i = 0; i < cars.size(); ++i) {
			if (i == self) continue;
			const auto& o = cars[i];
			if (std::hypot(o.x - nx, o.y - ny) < Radius(c) + Radius(o) - 0.8 && std::fabs(o.z - nz) < 3.0) return true;
		}
		return false;
	}

	void Step()
	{
		std::vector<Other> others;
		for (const auto& c : cars) others.push_back({ c.id, c.x, c.y, c.z, c.vx, c.vy, Radius(c), c.priority });
		for (size_t i = 0; i < cars.size(); ++i) {
			Car& c = cars[i];
			if (c.parked) continue;
			Input in;
			in.now = t;
			in.x = c.x; in.y = c.y; in.z = c.z; in.heading = c.heading; in.vx = c.vx; in.vy = c.vy;
			in.groundUnder = w.Ground(c.x, c.y, false) + c.groundBias;
			in.hoverPossible = c.hoverPossible;
			in.ceiling = c.ceiling;
			in.onWheels = c.onWheels;
			in.preferAir = c.preferAir;
			in.preferWhy = c.preferAir ? "test: air" : "";
			in.groundOk = true;
			in.above = c.z - in.groundUnder;
			in.gx = c.gx; in.gy = c.gy; in.gz = c.gz; in.stop = c.stop; in.keepGoing = c.keepGoing;
			in.targetVx = c.tvx; in.targetVy = c.tvy;
			in.permit = c.permit;
			in.waterAhead = c.x < w.water0 && w.water0 - c.x < 60.0 && c.gx > w.water1;
			in.contact = c.contact;
			in.self = c.id; in.priority = c.priority;
			in.others = others.data(); in.otherCount = int(others.size());
			in.shape = c.shape; in.limits = c.limits;
			const double vx0 = c.x, vy0 = c.y;
			const double cz0 = c.z;
			const bool roof = c.ownRoof;
			const Output o = c.pilot.Step(in, [&](double x, double y, double) {
				Cell m = w.MapCell(x, y, vx0, vy0);
				if (roof && std::hypot(x - vx0, y - vy0) < 3.0) { m.known = m.haveGround = true; m.ground = cz0 + 1.2; m.solid = true; m.solidZ = cz0 + 0.5; }
				return m;
			});
			if (o.modeChanged) ++c.modeChanges;
			if (o.stuckEvent) ++c.stuckEvents;
			c.last = o;
			// Vehicle response.
			const double target = o.reverse ? -o.speed : o.speed;
			const double acc = std::clamp((target - c.v) * lag, -c.limits.brake, accelMax);
			c.v += acc * dt;
			if (o.brake && c.v > 0) c.v = std::max(0.0, c.v - c.limits.brake * dt);
			const double err = Wrap(o.bearing - c.heading);
			double yawRate = std::clamp(2.0 * err, -yawMax, yawMax);
			if (o.mode == Mode::Drive && c.onWheels) yawRate *= std::min(1.0, std::fabs(c.v) / 3.0);  // the hover turns on the spot
			c.heading = Wrap(c.heading + yawRate * dt);
			const double nx = c.x + std::sin(c.heading) * c.v * dt, ny = c.y + std::cos(c.heading) * c.v * dt;
			double nz;
			if (o.mode == Mode::Drive) nz = w.Ground(nx, ny) + c.shape.originAbove;
			else {
				double ahead = w.Ground(nx, ny);
				for (double d = 2.0; d <= 16.0; d += 2.0) ahead = std::max(ahead, w.Ground(nx + std::sin(c.heading) * d, ny + std::cos(c.heading) * d));
				const double want = std::max(ahead + c.limits.hoverClear + o.climb, std::isfinite(c.gz) ? c.gz : -1e9);
				nz = c.z + std::clamp(want - c.z, -5.0 * dt, 5.0 * dt);
			}
			c.contact = false;
			if (Blocked(c, nx, ny, nz, o.mode, i)) {
				c.v = 0; c.vx = c.vy = 0;
				c.contact = true;
				++c.contacts;
				if (o.mode == Mode::Hover) c.z = c.z + std::clamp(std::max(w.Ground(c.x, c.y), -1e9) + c.limits.hoverClear + o.climb - c.z, -5.0 * dt, 5.0 * dt);
			}
			else {
				c.vx = (nx - c.x) / dt; c.vy = (ny - c.y) / dt;
				c.x = nx; c.y = ny; c.z = nz;
			}
			if (trace && (int(std::lround(t * 30)) % 15 == 0 || c.contact))
				std::printf("  [%zu] t=%.2f pos %.1f,%.1f z %.1f hd %.0f v %.1f %s %s want %.1f brg %.0f path %s/%s free %.1f usable %d lvl %u trap %d route %d%s\n", i, t, c.x, c.y, c.z,
					c.heading * 57.3, c.v, Name(o.mode), Name(o.phase), o.speed, o.bearing * 57.3, Name(o.path.v), o.path.why, o.path.free, o.usable, o.escapeLevel,
					o.trapped ? 1 : 0, o.routed ? 1 : 0, c.contact ? " CONTACT" : "");
			for (size_t j = 0; j < cars.size(); ++j)
				if (j != i) c.minGap = std::min(c.minGap, std::hypot(cars[j].x - c.x, cars[j].y - c.y) - Radius(c) - Radius(cars[j]));
		}
		t += dt;
	}

	// Runs until car 0 (or every piloted car) is within stop + 1.5 m and slow, or the time runs out. Returns the time.
	double RunUntilArrived(double limit, bool all = false)
	{
		while (t < limit) {
			Step();
			bool done = true;
			for (const auto& c : cars) {
				if (c.parked) continue;
				const bool there = std::hypot(c.gx - c.x, c.gy - c.y) <= c.stop + 1.5 && std::fabs(c.v) < 1.0;
				if (!there) done = false;
				if (!all) break;
			}
			if (done) return t;
		}
		return -1;
	}
};

static Car MakeCar(double x, double y, double heading, double gx, double gy, Permit permit = Permit::DriveOnly)
{
	Car c;
	c.x = x; c.y = y; c.heading = heading; c.gx = gx; c.gy = gy; c.permit = permit;
	c.z = c.shape.originAbove;
	return c;
}

static void Report(const char* name, const Sim& s, double t, double limit)
{
	char buf[256];
	const auto& c = s.cars[0];
	std::snprintf(buf, sizeof buf, "(t=%.1f s of %.0f, end at %.1f,%.1f, %d contacts, %d stuck events, %d mode changes)", t, limit, c.x, c.y, c.contacts,
		c.stuckEvents, c.modeChanges);
	Check(t >= 0, name, buf);
}

int main()
{
	{  // 1. Open flat ground: drive 150 m and stop at the goal.
		Sim s;
		s.cars.push_back(MakeCar(0, 0, 0, 0, 150));
		const double t = s.RunUntilArrived(40);
		Report("flat: arrives", s, t, 40);
		Check(s.cars[0].contacts == 0, "flat: no contacts");
		Check(t > 0 && t < 25, "flat: no dawdling (under 25 s for 150 m)");
		// Stays arrived (no re-arming, no creeping off).
		const double x = s.cars[0].x, y = s.cars[0].y;
		for (int i = 0; i < 300; ++i) s.Step();
		Check(std::hypot(s.cars[0].x - x, s.cars[0].y - y) < 1.0 && s.cars[0].last.phase == Phase::Arrived, "flat: holds at the goal");
	}
	{  // 2. A long hill (20 % grade, 30 m rise). In game 0.8 stopped here: hillside ray hits read as walls.
		Sim s;
		s.w.terrain = [](double, double y) { return std::clamp((y - 40.0) * 0.2, 0.0, 30.0); };
		s.cars.push_back(MakeCar(0, 0, 0, 0, 260));
		const double t = s.RunUntilArrived(60);
		Report("hill: drives over it", s, t, 60);
		Check(s.cars[0].stuckEvents == 0, "hill: never stuck");
	}
	{  // 3. A wall across the way (drive only): goes round it.
		Sim s;
		s.w.boxes.push_back({ -25, 60, 25, 64, 6 });
		s.cars.push_back(MakeCar(0, 0, 0, 0, 120));
		const double t = s.RunUntilArrived(60);
		Report("wall: detours round it", s, t, 60);
		Check(s.cars[0].contacts == 0, "wall: no contact with the mapped wall");
	}
	{  // 4. A cliff band too steep for wheels with a gap 25 m to the side.
		Sim s;
		s.w.terrain = [](double x, double y) { return (std::fabs(x - 30.0) < 6.0) ? std::clamp((y - 30.0) * 0.2, 0.0, 12.0) : std::clamp((y - 50.0) * 2.0, 0.0, 12.0); };
		s.cars.push_back(MakeCar(0, 0, 0, 0, 90));
		const double t = s.RunUntilArrived(90);
		Report("cliff: finds the ramp gap", s, t, 90);
	}
	{  // 5. A parked vehicle right in the lane: steers round, never waits behind it.
		Sim s;
		s.cars.push_back(MakeCar(0, 0, 0, 0, 100));
		Car parked = MakeCar(0, 50, 0, 0, 50);
		parked.parked = true; parked.id = 2;
		s.cars.push_back(parked);
		const double t = s.RunUntilArrived(40);
		Report("parked vehicle: passes it", s, t, 40);
		Check(s.cars[0].contacts == 0 && s.cars[0].minGap > 0.0, "parked vehicle: no contact");
	}
	{  // 6. Two vehicles head-on in the same lane: both get through, no collision.
		Sim s;
		s.cars.push_back(MakeCar(0, 0, 0, 0, 120));
		Car b = MakeCar(0.5, 120, 3.14159, 0.5, 0);
		b.id = 2;
		s.cars.push_back(b);
		const double t = s.RunUntilArrived(60, true);
		Report("head-on: both arrive", s, t, 60);
		Check(s.cars[0].contacts == 0 && s.cars[1].contacts == 0, "head-on: no collision");
	}
	{  // 7. A dead end (cul-de-sac) open away from the goal: escapes and arrives.
		Sim s;
		s.w.boxes.push_back({ -20, 40, 20, 43, 6 });   // the back wall, facing the goal
		s.w.boxes.push_back({ -20, 0, -17, 43, 6 });   // the sides
		s.w.boxes.push_back({ 17, 0, 20, 43, 6 });
		s.cars.push_back(MakeCar(0, 10, 0, 0, 100));
		const double t = s.RunUntilArrived(120);
		Report("dead end: gets out and arrives", s, t, 120);
	}
	{  // 8. Both: the goal is on a mesa 25 m up with sheer sides: it takes off and arrives.
		Sim s;
		s.w.boxes.push_back({ -15, 80, 15, 110, 25 });
		Car c = MakeCar(0, 0, 0, 0, 95, Permit::Both);
		c.gz = 25.0 + 0.8;
		s.cars.push_back(c);
		double t = -1;
		while (s.t < 60) {
			const int before = s.cars[0].contacts;
			s.Step();
			const auto& q = s.cars[0];
			if (std::getenv("AD_TRACE") && (int(s.t * 30) % 15 == 0 || q.contacts != before))
				std::printf("  t=%.2f pos %.1f,%.1f z %.1f v %.1f mode %s phase %s want %.1f path %s free %.1f climb %.1f %s\n", s.t, q.x, q.y, q.z, q.v,
					Name(q.last.mode), Name(q.last.phase), q.last.speed, Name(q.last.path.v), q.last.path.free, q.last.climb, q.contacts != before ? "CONTACT" : "");
			if (std::hypot(q.gx - q.x, q.gy - q.y) <= q.stop + 1.5 && std::fabs(q.v) < 1.0) { t = s.t; break; }
		}
		Report("mesa: hovers up to the goal", s, t, 60);
		Check(s.cars[0].contacts < 10, "mesa: climbs before reaching the face");
		Check(s.cars[0].z >= 25.0, "mesa: up on the top (hovering, or settled onto it and driving)");
	}
	{  // 9. Nothing mapped at all: still drives (slowly), never parks waiting for sensing.
		Sim s;
		s.w.mapKnown = false;
		s.cars.push_back(MakeCar(0, 0, 0, 0, 100));
		const double t = s.RunUntilArrived(40);
		Report("unmapped: moves anyway", s, t, 40);
	}
	{  // 10. An obstacle the map never saw (hidden): contacts, stuck recovery, then round it.
		Sim s;
		s.w.boxes.push_back({ -12, 40, 12, 44, 6, true });
		s.cars.push_back(MakeCar(0, 0, 0, 0, 100));
		const double t = s.RunUntilArrived(120);
		Report("hidden obstacle: recovers and arrives", s, t, 120);
		Check(s.cars[0].contacts > 0 && s.cars[0].contacts < 200, "hidden obstacle: backs off its contacts instead of grinding");
	}
	{  // 11. Both, flat open ground: no drive/hover flapping.
		Sim s;
		s.cars.push_back(MakeCar(0, 0, 0, 0, 300, Permit::Both));
		const double t = s.RunUntilArrived(60);
		Report("both, flat: arrives", s, t, 60);
		Check(s.cars[0].modeChanges == 0, "both, flat: stays on the wheels");
	}
	{  // 12. Both: water band across the way: hovers over it, back on the wheels after.
		Sim s;
		s.w.water0 = 40; s.w.water1 = 120;
		s.cars.push_back(MakeCar(0, 0, 1.5708, 200, 0, Permit::Both));
		const double t = s.RunUntilArrived(80);
		Report("water: hovers across", s, t, 80);
		Check(s.cars[0].modeChanges >= 1 && s.cars[0].modeChanges <= 3, "water: one or two mode changes, no flapping");
	}
	{  // 13. Following a target moving at 8 m/s: keeps up without overtaking.
		Sim s;
		Car c = MakeCar(0, 0, 0, 0, 20);
		c.keepGoing = true; c.stop = 8.0; c.tvy = 8.0;
		s.cars.push_back(c);
		double worst = 0, closest = 1e9;
		for (int i = 0; i < 30 * 40; ++i) {
			const double ty = 20.0 + 8.0 * s.t;
			s.cars[0].gy = ty;
			s.Step();
			const double gap = ty - s.cars[0].y;
			if (s.t > 15) { worst = std::max(worst, gap); closest = std::min(closest, gap); }
		}
		char buf[96];
		std::snprintf(buf, sizeof buf, "(gap %.1f..%.1f m)", closest, worst);
		Check(worst < 30.0 && closest > 2.0, "follow: keeps up without running into the target", buf);
	}
	{  // 14. Hover only: a 20 m wall: flies over or round it.
		Sim s;
		s.w.boxes.push_back({ -40, 60, 40, 64, 20 });
		s.cars.push_back(MakeCar(0, 0, 0, 0, 120, Permit::HoverOnly));
		const double t = s.RunUntilArrived(80);
		Report("hover only: past a tall wall", s, t, 80);
	}
	{  // 15. Three vehicles to the same area through a gap: nobody deadlocks.
		Sim s;
		s.w.boxes.push_back({ -60, 60, -6, 64, 6 });
		s.w.boxes.push_back({ 6, 60, 60, 64, 6 });
		for (int k = 0; k < 3; ++k) {
			Car c = MakeCar(-12.0 + 12.0 * k, 0, 0, -12.0 + 12.0 * k, 120);
			c.id = std::uint32_t(10 + k);
			s.cars.push_back(c);
		}
		const double t = s.RunUntilArrived(120, true);
		Report("three through a gap: all arrive", s, t, 120);
	}
	{  // 16. Sluggish vehicle (slow pedals and steering) through the dead end.
		Sim s;
		s.accelMax = 1.5; s.yawMax = 0.45; s.lag = 0.7;
		s.trace = std::getenv("AD_TRACE16") != nullptr;
		s.w.boxes.push_back({ -20, 40, 20, 43, 6 });
		s.w.boxes.push_back({ -20, 0, -17, 43, 6 });
		s.w.boxes.push_back({ 17, 0, 20, 43, 6 });
		s.cars.push_back(MakeCar(0, 10, 0, 0, 100));
		const double t = s.RunUntilArrived(150);
		Report("sluggish: dead end", s, t, 150);
	}
	{  // 17. Short sensing (only 40 m round the vehicle is mapped): the cliff ramp.
		Sim s;
		s.w.senseRange = 40;
		s.trace = std::getenv("AD_TRACE17") != nullptr;
		s.w.terrain = [](double x, double y) { return (std::fabs(x - 30.0) < 6.0) ? std::clamp((y - 30.0) * 0.2, 0.0, 12.0) : std::clamp((y - 50.0) * 2.0, 0.0, 12.0); };
		s.cars.push_back(MakeCar(0, 0, 0, 0, 90));
		const double t = s.RunUntilArrived(150);
		Report("short sensing: cliff ramp", s, t, 150);
	}
	{  // 18. Rough rolling terrain over 400 m (hills up to 14 m, slopes under the wheels' limit).
		Sim s;
		s.w.terrain = [](double x, double y) { return 7.0 * std::sin(x * 0.045) * std::cos(y * 0.035) + 4.0 * std::sin(y * 0.06 + 1.0); };
		s.cars.push_back(MakeCar(0, 0, 0.3, 120, 380));
		const double t = s.RunUntilArrived(90);
		Report("rolling terrain: 400 m trip", s, t, 90);
		Check(s.cars[0].stuckEvents == 0, "rolling terrain: never stuck");
	}
	{  // 19. Six vehicles converging on points round one spot: no deadlock, no pile-up.
		Sim s;
		for (int k = 0; k < 6; ++k) {
			const double a = k * 1.0472;
			Car c = MakeCar(80 * std::sin(a), 80 * std::cos(a), a + 3.14159, 9 * std::sin(a), 9 * std::cos(a));
			c.id = std::uint32_t(20 + k);
			c.stop = 2.0;
			s.cars.push_back(c);
		}
		s.trace = std::getenv("AD_TRACE19") != nullptr;
		const double t = s.RunUntilArrived(120, true);
		Report("six converge: all arrive", s, t, 120);
		int contacts = 0;
		for (const auto& c : s.cars) contacts += c.contacts;
		char buf[64];
		std::snprintf(buf, sizeof buf, "(%d contacts)", contacts);
		Check(contacts < 30, "six converge: little or no bumping", buf);
	}
	{  // 20. Following a target that turns (a square loop at 6 m/s).
		Sim s;
		Car c = MakeCar(0, 0, 0, 0, 15);
		c.keepGoing = true; c.stop = 8.0;
		s.cars.push_back(c);
		double tx = 0, ty = 15, worst = 0;
		for (int i = 0; i < 30 * 60; ++i) {
			const int leg = int(s.t / 10.0) % 4;
			const double vx = leg == 1 ? 6 : leg == 3 ? -6 : 0, vy = leg == 0 ? 6 : leg == 2 ? -6 : 0;
			tx += vx * s.dt; ty += vy * s.dt;
			s.cars[0].gx = tx; s.cars[0].gy = ty; s.cars[0].tvx = vx; s.cars[0].tvy = vy;
			s.Step();
			if (s.t > 8) worst = std::max(worst, std::hypot(tx - s.cars[0].x, ty - s.cars[0].y));
		}
		char buf[64];
		std::snprintf(buf, sizeof buf, "(farthest %.1f m)", worst);
		Check(worst < 30.0 && s.cars[0].contacts == 0, "follow turns: keeps close through the corners", buf);
	}
	{  // 21. The ground reading is 3.2 m too high (a probe met something over the vehicle; 2026-10-27 enemy rover 1).
		Sim s;
		Car c = MakeCar(0, 0, 0, 0, 100);
		c.groundBias = 3.2;
		s.cars.push_back(c);
		const double t = s.RunUntilArrived(40);
		Report("ground read 3.2 m high: still drives", s, t, 40);
		Check(s.cars[0].last.phase != Phase::Boxed, "ground read high: not boxed");
	}
	{  // 22. The map shows the vehicle's own roof and hull under it (cells round the vehicle).
		Sim s;
		Car c = MakeCar(0, 0, 0, 0, 100);
		c.ownRoof = true;
		s.cars.push_back(c);
		const double t = s.RunUntilArrived(40);
		Report("own roof in the map: still drives", s, t, 40);
	}
	{  // 23. Drive only, walled in on every side (4 m walls, 6 m away): never hovers, never freezes; keeps working the most open way.
		Sim s;
		s.w.boxes.push_back({ -8, 6, 8, 8, 4 });
		s.w.boxes.push_back({ -8, -8, 8, -6, 4 });
		s.w.boxes.push_back({ -8, -8, -6, 8, 4 });
		s.w.boxes.push_back({ 6, -8, 8, 8, 4 });
		s.cars.push_back(MakeCar(0, 0, 0, 0, 80));
		double travelled = 0, px = 0, py = 0;
		for (int i = 0; i < 30 * 40; ++i) { s.Step(); travelled += std::hypot(s.cars[0].x - px, s.cars[0].y - py); px = s.cars[0].x; py = s.cars[0].y; }
		char buf[64];
		std::snprintf(buf, sizeof buf, "(%.0f m worked in 40 s, %d mode changes)", travelled, s.cars[0].modeChanges);
		Check(s.cars[0].modeChanges == 0 && travelled > 20.0, "walled pit, drive only: stays on the wheels and keeps trying", buf);
	}
	{  // 24. Both, the same pit with open sky: up has the most freedom, so it rises out and arrives.
		Sim s;
		s.w.boxes.push_back({ -8, 6, 8, 8, 4 });
		s.w.boxes.push_back({ -8, -8, 8, -6, 4 });
		s.w.boxes.push_back({ -8, -8, -6, 8, 4 });
		s.w.boxes.push_back({ 6, -8, 8, 8, 4 });
		s.cars.push_back(MakeCar(0, 0, 0, 0, 80, Permit::Both));
		const double t = s.RunUntilArrived(90);
		Report("walled pit, both, open sky: rises out and arrives", s, t, 90);
	}
	{  // 25. Both, the pit under a roof (1 m of room above): up is not the freest way; it never lifts into the roof.
		Sim s;
		s.w.boxes.push_back({ -8, 6, 8, 8, 4 });
		s.w.boxes.push_back({ -8, -8, 8, -6, 4 });
		s.w.boxes.push_back({ -8, -8, -6, 8, 4 });
		s.w.boxes.push_back({ 6, -8, 8, 8, 4 });
		Car c = MakeCar(0, 0, 0, 0, 80, Permit::Both);
		c.ceiling = 1.0;
		s.cars.push_back(c);
		for (int i = 0; i < 30 * 30; ++i) s.Step();
		Check(s.cars[0].modeChanges == 0, "walled pit under a roof: does not lift into it");
	}
	{  // 26. Damaged (the hover would fail), Both, open sky: stays on the wheels.
		Sim s;
		s.w.boxes.push_back({ -8, 6, 8, 8, 4 });
		s.w.boxes.push_back({ -8, -8, 8, -6, 4 });
		s.w.boxes.push_back({ -8, -8, -6, 8, 4 });
		s.w.boxes.push_back({ 6, -8, 8, 8, 4 });
		Car c = MakeCar(0, 0, 0, 0, 80, Permit::Both);
		c.hoverPossible = false;
		s.cars.push_back(c);
		for (int i = 0; i < 30 * 30; ++i) s.Step();
		Check(s.cars[0].modeChanges == 0, "damaged in the pit: never hovers");
	}
	{  // 27. Parked in a tight row (other vehicles 3 m to each side): pulls out ahead and arrives.
		Sim s;
		s.cars.push_back(MakeCar(0, 0, 0, 0, 80));
		for (int k = 0; k < 2; ++k) {
			Car p = MakeCar(k ? 5.5 : -5.5, 0, 0, 0, 0);
			p.parked = true; p.id = std::uint32_t(50 + k);
			s.cars.push_back(p);
		}
		const double t = s.RunUntilArrived(40);
		Report("tight parking row: pulls out", s, t, 40);
		Check(s.cars[0].contacts == 0, "tight parking row: no contact with the neighbours");
	}
	{  // 28. Same row, the goal behind it: backs or turns out and arrives.
		Sim s;
		s.cars.push_back(MakeCar(0, 0, 0, 0, -60));
		s.trace = std::getenv("AD_TRACE28") != nullptr;
		for (int k = 0; k < 2; ++k) {
			Car p = MakeCar(k ? 5.5 : -5.5, 0, 0, 0, 0);
			p.parked = true; p.id = std::uint32_t(60 + k);
			s.cars.push_back(p);
		}
		const double t = s.RunUntilArrived(60);
		Report("tight parking row, goal behind: gets out", s, t, 60);
	}
	{  // 29. Drive mode on the hover held low (an empty vehicle), 2.7 m up, goal behind, a parked vehicle beside:
		//     no false drops from its own height, no endless reversing; it turns on the spot and goes.
		Sim s;
		Car c = MakeCar(0, 0, 0, 0, -60, Permit::Both);
		c.onWheels = false; c.groundBias = 0; c.z = 2.7;
		s.cars.push_back(c);
		Car p = MakeCar(5.5, 0, 0, 0, 0); p.parked = true; p.id = 70;
		s.cars.push_back(p);
		const double t = s.RunUntilArrived(40);
		Report("hover held low, goal behind: turns and goes", s, t, 40);
		Check(s.cars[0].last.phase != Phase::Boxed, "hover held low: never boxed by false drops");
	}
	{  // 30. Both, combat preference (air): takes off and fights its way there in the hover, no flapping back to wheels.
		Sim s;
		Car c = MakeCar(0, 0, 0, 0, 200, Permit::Both);
		c.preferAir = true;
		s.cars.push_back(c);
		const double t = s.RunUntilArrived(60);
		Report("combat preference: flies", s, t, 60);
		Check(s.cars[0].last.mode == Mode::Hover && s.cars[0].modeChanges <= 1, "combat preference: in the hover, one change at most");
	}
	{  // 31. Both, ground preference over open ground: stays on the wheels the whole way.
		Sim s;
		s.cars.push_back(MakeCar(0, 0, 0, 0, 200, Permit::Both));
		const double t = s.RunUntilArrived(60);
		Report("traffic preference: drives", s, t, 60);
		Check(s.cars[0].modeChanges == 0 && s.cars[0].last.mode == Mode::Drive, "traffic preference: never lifts off");
	}
	{  // 32. Parked inside a badly fitting collision box (the map says solid all round, nothing physical): drives out.
		Sim s;
		s.w.objectZones.push_back({ -20, -20, 20, 20, 3 });
		s.cars.push_back(MakeCar(0, 0, 0, 0, 80));
		const double t = s.RunUntilArrived(40);
		Report("inside a poor collision box: drives out", s, t, 40);
	}
	{  // 33. A real collision box ahead (the vehicle is not inside it): still avoided.
		Sim s;
		s.w.objectZones.push_back({ -14, 40, 14, 48, 3 });
		s.w.boxes.push_back({ -14, 40, 14, 48, 3 });
		s.cars.push_back(MakeCar(0, 0, 0, 0, 100));
		const double t = s.RunUntilArrived(60);
		Report("collision box ahead: goes round it", s, t, 60);
		Check(s.cars[0].contacts == 0, "collision box ahead: no contact");
	}
	{  // Airborne recovery must not classify the ground far below as a drop.
		Pilot pilot;
		Input in; in.gy=100; in.z=20; in.above=20; in.groundOk=true;
		in.onWheels=false; in.permit=Permit::HoverOnly;
		// A ground ledge closes every wheel route; all of it is below the aircraft.
		auto map=[](double, double, double) { Cell c; c.known=c.haveGround=true; c.ground=5; return c; };
		Output out;
		for (int k=0;k<=38;++k) { in.now=k*0.1; out=pilot.Step(in,map); }
		Check(out.phase==Phase::Turn && !out.reverse && out.speed>0,
			"airborne stuck recovery turns toward open air instead of reversing or waiting on ground slopes");
	}
	{  // Switching from a wheel escape to flight cancels the old reverse timer.
		Pilot pilot;
		Input in; in.gy=100; in.z=0.8; in.above=0.8; in.groundOk=true;
		auto map=[](double, double, double) { Cell c; c.known=c.haveGround=true; return c; };
		Output out;
		for (int k=0;k<=38;++k) { in.now=k*0.1; out=pilot.Step(in,map); }
		Check(out.reverse,"wheel recovery fixture begins by backing out");
		in.now=3.9; in.preferAir=true; in.onWheels=false;
		out=pilot.Step(in,map);
		Check(out.mode==Mode::Hover && !out.reverse && out.phase!=Phase::Turn,
			"takeoff discards the wheel escape and replans in the air");
	}
    { // A speed floor must never defeat a wall/traffic braking cap.
        Pilot pilot;Input in;in.gy=100;in.z=10;in.above=10;in.groundOk=true;
        in.permit=Permit::HoverOnly;in.onWheels=false;in.ceiling=0;in.strictTraffic=true;
        auto blocked=[](double,double,double){Cell c;c.known=true;c.object=true;return c;};
        Output out;for(int i=0;i<100;++i){in.now=1+i*0.1;out=pilot.Step(in,blocked);}
        Check(out.speed==0,"proven blocked corridor never gets the minimum forward-speed override");
        Other oncoming;oncoming.id=2;oncoming.z=10;oncoming.y=100;oncoming.vy=-30;
        const auto t=TrafficFor(0,0,10,0,30,3,1,100,&oncoming,1,2,false);
        Check(t.yieldTo==2&&t.cap<1e8,"imminent collision beyond old 50m limit is detected even with right of way / ignore timer");
    }
	std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
	return g_failures ? 1 : 0;
}

