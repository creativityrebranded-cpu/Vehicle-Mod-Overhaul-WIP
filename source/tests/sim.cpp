// Headless test of the flight model on synthetic terrain.
// Ground samples arrive LATE like the engine's ray cast (requests made after frame k-1 are answered in frame k and
// read in frame k+1), and ground further than `reach` metres from the ship returns "no data" (collision not loaded).
#include "flightmodel.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

using namespace fm;

namespace
{
	using Height = std::function<double(double, double)>;

	struct Scenario
	{
		std::string name;
		Height ground;
		double x = 0, y = 0, z = 100, yaw = 0, speed = 100;
		double dt = 1.0 / 60.0;
		double seconds = 60;
		double reach = 350;
		// scripted input: called each frame with time, fills the input
		std::function<void(double, Input&)> script;
		// optional parameter tweak
		std::function<void(Model&)> tweak;
	};

	struct Result
	{
		double minGap = 1e9;          // smallest (z - highest ground under the hull): must stay above the hull half-height
		int clamps = 0;
		double maxClampLift = 0;
		double tAssist = 0, tWarn = 0, tScrape = 0, tStall = 0;
		double maxSpeed = 0, maxSpeedLate = 0, endSpeed = 0, endZ = 0, minZ = 1e9, maxZ = -1e9;
		double firstAssist = -1, minYawRate = 1e9, maxYawRate = -1e9;
		State end;
	};

	double HullGround(const Scenario& sc, const Model& m)
	{
		const double c = std::cos(m.s.yaw), sn = std::sin(m.s.yaw);
		double g = -1e18;
		const double L = m.p.hullHalfLen, W = m.p.hullHalfSpan;
		const double pts[5][2] = { { 0, 0 }, { L, 0 }, { -L, 0 }, { 0, W }, { 0, -W } };
		for (auto& p : pts) {
			const double fx = p[0], rx = p[1];
			const double x = m.s.x + sn * fx + c * rx, y = m.s.y + c * fx - sn * rx;
			g = std::max(g, sc.ground(x, y));
		}
		return g;
	}

	bool g_easy = false;  // scenarios 1-13: the handoff's scheme (bank turns, stall); 14+: the easy scheme

	void Scheme(Model& m)
	{
		m.p.mouseYaw = g_easy;
		m.p.stallEnabled = !g_easy;
		m.p.minClearance = g_easy ? 50.0 : 0.0;  // tests pinned to 50 m (the in-game default is tunable); none in the handoff's model
		m.p.simpleSpeed = g_easy;
		if (!g_easy) m.p.brakeDecel = 0;  // the handoff's model had no predictive braking
	}

	Result Run(const Scenario& sc)
	{
		Model m;
		Scheme(m);
		if (sc.tweak) sc.tweak(m);
		m.Reset(sc.x, sc.y, sc.z, sc.yaw, sc.speed);
		Result r;
		struct Pending { std::vector<Request> req; std::vector<fm::Result> res; };
		std::deque<Pending> q;
		const int n = int(sc.seconds / sc.dt);
		for (int i = 0; i < n; ++i) {
			const double t = i * sc.dt;
			if (q.size() >= 2) {  // results from requests made two steps ago (one frame to service, one to read)
				m.Feed(q.front().req, q.front().res);
				q.pop_front();
			}
			Input in;
			if (sc.script) sc.script(t, in);
			m.Step(sc.dt, in);
			// the engine answers these requests during the next frame, relative to where the ship is then
			Pending p;
			p.req = m.Requests();
			for (auto& rq : p.req) {
				fm::Result rs;
				rs.valid = std::hypot(rq.x - m.s.x, rq.y - m.s.y) <= sc.reach;
				rs.h = rs.valid ? sc.ground(rq.x, rq.y) : 0;
				p.res.push_back(rs);
			}
			q.push_back(std::move(p));

			const State& s = m.s;
			r.minGap = std::min(r.minGap, s.z - HullGround(sc, m));
			r.maxSpeed = std::max(r.maxSpeed, s.speed);
			if (t > 3.0) r.maxSpeedLate = std::max(r.maxSpeedLate, s.speed);
			r.minYawRate = std::min(r.minYawRate, s.yawRate);
			r.maxYawRate = std::max(r.maxYawRate, s.yawRate);
			r.minZ = std::min(r.minZ, s.z);
			r.maxZ = std::max(r.maxZ, s.z);
			if (s.mode == Mode::Assist) { r.tAssist += sc.dt; if (r.firstAssist < 0) r.firstAssist = t; }
			if (s.mode == Mode::Warning) r.tWarn += sc.dt;
			if (s.mode == Mode::Scraping) r.tScrape += sc.dt;
			if (s.stalled) r.tStall += sc.dt;
		}
		r.clamps = m.s.clampEvents;
		r.maxClampLift = m.s.maxClampLift;
		r.endSpeed = m.s.speed;
		r.endZ = m.s.z;
		r.end = m.s;
		return r;
	}

	int g_fail = 0;
	void Check(bool ok, const char* what)
	{
		std::printf("    [%s] %s\n", ok ? "PASS" : "FAIL", what);
		if (!ok) ++g_fail;
	}
	double p_landing_h() { return 15.0; }
	const double kHullHalfHeight = 4.0;  // physical contact if the gap to the highest ground under the hull is below this

	void Print(const char* name, const Result& r)
	{
		std::printf("%s\n    minGap %.1f m | clamps %d (max lift %.1f m) | assist %.1fs warn %.1fs scrape %.1fs stall %.1fs | z %.0f..%.0f | speed max %.1f end %.1f\n",
			name, r.minGap, r.clamps, r.maxClampLift, r.tAssist, r.tWarn, r.tScrape, r.tStall, r.minZ, r.maxZ, r.maxSpeed, r.endSpeed);
	}
}

int main()
{
	const double pi = 3.14159265358979323846;
	const Height flat = [](double, double) { return 0.0; };

	// 1. Flat, full throttle, no input.
	{
		Scenario sc{ "flat cruise", flat };
		sc.z = 80; sc.seconds = 40;
		auto r = Run(sc);
		Print("1. flat cruise at 100 m/s, hands off", r);
		Check(r.tAssist == 0 && r.tScrape == 0, "no assist and no scraping over flat ground");
		Check(r.maxSpeed <= 100.0 + 1e-6, "never exceeds the 100 m/s cap");
		Check(r.minGap > 60, "stays at its altitude");
	}
	// 2. Ramp rising at 20 degrees from x = 2000, flying +x.
	for (double dt : { 1.0 / 60.0, 1.0 / 30.0 }) {
		Scenario sc{ "ramp", [](double x, double) { return x > 2000 ? (x - 2000) * std::tan(20.0 * 3.14159265 / 180.0) : 0.0; } };
		sc.z = 100; sc.yaw = pi / 2; sc.seconds = 45; sc.dt = dt;
		auto r = Run(sc);
		char nm[96]; std::snprintf(nm, sizeof nm, "2. 20 degree ramp, 100 m/s, hands off, dt=1/%.0f", 1.0 / dt);
		Print(nm, r);
		Check(r.minGap > kHullHalfHeight, "never touches the ground");
		Check(r.firstAssist > 0, "the assist engaged before the ramp");
	}
	// 3. Vertical cliff 150 m high at y = 3000, flying +y.
	for (double dt : { 1.0 / 60.0, 1.0 / 30.0 }) {
		Scenario sc{ "cliff", [](double, double y) { return y > 3000 ? 150.0 : 0.0; } };
		sc.z = 60; sc.seconds = 40; sc.dt = dt;
		auto r = Run(sc);
		char nm[96]; std::snprintf(nm, sizeof nm, "3. 150 m vertical cliff, 100 m/s, z=60, hands off, dt=1/%.0f", 1.0 / dt);
		Print(nm, r);
		Check(r.minGap > kHullHalfHeight, "clears the cliff without touching");
		Check(r.clamps == 0, "no hard-floor correction needed (the assist did the work)");
	}
	// 4. Gaussian ridge 400 m high.
	{
		Scenario sc{ "ridge", [](double, double y) { return 400.0 * std::exp(-(y - 3000) * (y - 3000) / (2 * 250.0 * 250.0)); } };
		sc.z = 60; sc.seconds = 50;
		auto r = Run(sc);
		Print("4. 400 m ridge, 100 m/s, z=60, hands off", r);
		Check(r.minGap > kHullHalfHeight, "clears the ridge without touching");
		Check(r.clamps == 0, "no hard-floor correction needed");
	}
	// 5. Ridge with a short reach (collision loaded only 150 m ahead): speed must be capped.
	{
		Scenario sc{ "short reach", [](double, double y) { return 200.0 * std::exp(-(y - 3000) * (y - 3000) / (2 * 150.0 * 150.0)); } };
		sc.z = 50; sc.seconds = 80; sc.reach = 150;
		auto r = Run(sc);
		Print("5. 200 m ridge, only 150 m of collision loaded", r);
		Check(r.minGap > kHullHalfHeight, "does not hit the ridge");
		Check(r.maxSpeedLate < 80.0, "speed was capped (about reach / 2 s = 75 m/s) once the short reach was known");
	}
	// 6. Mouse: pitch up with the mouse, hold the climb, check it climbs and slows.
	{
		Scenario sc{ "mouse pitch", flat };
		sc.z = 200; sc.seconds = 12;
		sc.script = [](double t, Input& in) { if (t < 1.0) in.mouseDy = 8; };
		auto r = Run(sc);
		Print("6. mouse up for 1 s (nose up), throttle unchanged", r);
		Check(r.maxZ > 260, "the ship climbs after the nose is raised");
		Check(r.end.speed < 100.0, "climbing costs speed");
	}
	// 7. All three axes at once: mouse rolls (bank right) and pitches up while A (yaw left) is held against the bank.
	{
		Scenario sc{ "three axes", flat };
		sc.z = 300; sc.seconds = 6;
		sc.script = [](double t, Input& in) {
			if (t < 0.5) { in.mouseDx = 6; in.mouseDy = 4; }  // bank right and nose up with the mouse
			if (t >= 1.0 && t < 3.0) in.rudder = -1.0;        // A held: yaw left, while the bank is still on
		};
		auto r = Run(sc);
		Print("7. mouse bank right + nose up, then A (yaw left) held at the same time", r);
		Check(r.end.roll > 0.1, "roll is still held while yawing");
		Check(r.maxZ > 310, "the mouse pitch input climbed at the same time");
		Check(r.minYawRate < -0.05, "A pushed the yaw rate negative (left) against the right bank");
	}
	// 8. Yaw momentum: a short A/D tap; the yaw rate must coast and decay, not stop dead.
	{
		Model m;
		Scheme(m);
		m.Reset(0, 0, 300, 0, 100);
		double t = 0, dt = 1.0 / 60.0, peak = 0, tPeak = 0, t10 = -1;
		for (int i = 0; i < 360; ++i, t += dt) {
			Input in;
			if (t < 0.3) in.rudder = 1.0;  // D tapped for 0.3 s
			m.Step(dt, in);
			const double yr = std::fabs(m.s.yawRate);
			if (yr > peak) { peak = yr; tPeak = t; }
			if (peak > 0 && t > tPeak && t10 < 0 && yr < 0.1 * peak) t10 = t - tPeak;
		}
		std::printf("8. yaw momentum: peak %.2f rad/s, falls to 10%% after %.2f s\n", peak, t10);
		Check(t10 > 0.5, "yaw rate coasts for more than half a second after the key is released");
		Check(std::fabs(m.s.roll) < 1e-9, "roll untouched while only yaw was commanded");
	}
	// 9. Roll holds: a bank keeps turning the ship until it is rolled back.
	{
		Scenario sc{ "bank turn", flat };
		sc.z = 400; sc.seconds = 10;
		sc.script = [](double t, Input& in) { if (t < 0.4) in.mouseDx = 6; };
		auto r = Run(sc);
		Print("9. bank right for 0.4 s then hands off", r);
		Check(r.end.roll > 0.2, "roll is held (no auto-level)");
		Check(std::fabs(r.end.yaw) > 0.5, "the held bank keeps turning the ship");
	}
	// 10. Stall and recovery.
	{
		Scenario sc{ "stall", flat };
		sc.z = 800; sc.speed = 60; sc.seconds = 40;
		sc.script = [](double t, Input& in) { if (t < 10) in.slower = true; else in.faster = true; };
		auto r = Run(sc);
		Print("10. slow right down at 800 m for 10 s (S), then W to recover", r);
		Check(r.tStall > 0.5, "stalled when too slow");
		Check(r.minGap > 100, "recovered with plenty of height to spare");
		Check(!r.end.stalled, "stall cleared after throttle was restored");
	}
	// 11. Slow approach to land: no stall near the ground, the hard floor holds.
	{
		Scenario sc{ "landing", flat };
		sc.z = 40; sc.speed = 20; sc.seconds = 30;
		sc.script = [](double, Input& in) { in.slower = true; };
		auto r = Run(sc);
		Print("11. slow approach near the ground (landing)", r);
		Check(r.minGap > kHullHalfHeight - 1e-6, "never below the hull floor");
		Check(r.end.elevation >= 0 && r.end.elevation < p_landing_h(), "hovers near the ground, inside the autoland envelope");
		Check(r.end.speed < 15.0 && !r.end.stalled, "slow, not stalled, not falling");
	}

	// 12. Pilot dives at the ground with the mouse: the hidden controller must pull up.
	{
		Scenario sc{ "dive", flat };
		sc.z = 300; sc.seconds = 20;
		sc.script = [](double t, Input& in) { if (t > 1.0 && t < 4.0) in.mouseDy = -12; };
		auto r = Run(sc);
		Print("12. mouse pushed forward for 3 s (dive at the ground), at 300 m", r);
		Check(r.minGap > kHullHalfHeight, "never touches the ground");
		Check(r.tAssist > 0, "the assist took over");
	}

	// 13. Gentle touchdown: slow, nose slightly down, onto the floor. Must settle as LANDED with no assist fight.
	{
		Scenario sc{ "touchdown", flat };
		sc.z = 25; sc.speed = 22; sc.seconds = 25;
		sc.script = [](double t, Input& in) { in.slower = true; if (t < 0.4) in.mouseDy = -5; };
		auto r = Run(sc);
		Print("13. gentle touchdown approach: 22 m/s, nose slightly down, S held", r);
		Check(r.minGap > kHullHalfHeight - 1e-6, "never below the hull floor");
		Check(r.end.elevation >= 0 && r.end.elevation < 15.0 && r.end.speed < 15.0, "ends inside the autoland envelope (below 15 m, under 15 m/s) so autoland can finish");
		Check(r.tScrape < 1.0, "no scraping on a gentle touchdown");
	}

	// ---- Easy scheme (mouse = pitch and yaw; A / D strafe; Space / Ctrl up / down; W / S throttle).
	g_easy = true;
	// 14. Mouse right turns the nose directly; the hull banks for looks; release stops the turn.
	{
		Scenario sc{ "easy yaw", flat };
		sc.z = 300; sc.seconds = 6;
		sc.script = [](double t, Input& in) { if (t < 0.5) in.mouseDx = 8; };
		auto r = Run(sc);
		Print("14. easy: mouse right for 0.5 s", r);
		std::printf("    peak yaw rate %.2f rad/s, end yaw %.2f rad\n", r.maxYawRate, r.end.yaw);
		Check(r.maxYawRate > 0.15 && r.end.yaw > 0.1, "the nose turned right from the mouse");
		Check(std::fabs(r.end.yawRate) < 0.05, "the turn stops after the mouse is released");
		Check(std::fabs(r.end.roll) < 0.1, "the cosmetic bank levels out again");
	}
	// 15. Hover, then Space: straight up with the nose level; Ctrl: straight down to the floor, no stall.
	{
		Scenario sc{ "easy lift", flat };
		sc.z = 50; sc.speed = 0; sc.seconds = 20;
		sc.script = [](double t, Input& in) { if (t < 4) in.lift = 1; else if (t > 6) in.lift = -1; };
		auto r = Run(sc);
		Print("15. easy: hover, Space 4 s, then Ctrl", r);
		Check(r.maxZ > 140, "Space lifted the ship straight up");
		Check(std::fabs(r.end.pitch) < 1e-6, "the nose never moved");
		std::printf("    end elevation %.1f m, end z %.1f m\n", r.end.elevation, r.end.z);
		Check(r.end.elevation > 45.0 && r.end.elevation < 60.0, "Ctrl brought it down only to the minimum distance (about 50 m)");
		Check(r.tStall == 0, "no stall while hovering");
	}
	// 16. Strafe: D slides right while the nose keeps pointing north.
	{
		Scenario sc{ "easy strafe", flat };
		sc.z = 100; sc.speed = 0; sc.seconds = 5;
		sc.script = [](double, Input& in) { in.strafe = 1; };
		auto r = Run(sc);
		Print("16. easy: D held 5 s from a hover", r);
		Check(r.end.x > 80 && std::fabs(r.end.y) < 1.0, "slid right (east) without moving forward");
		Check(std::fabs(r.end.yaw) < 1e-6, "the nose kept its heading");
	}
	// 17. Terrain assist still works in the easy scheme (cliff at 100 m/s).
	{
		Scenario sc{ "easy cliff", [](double, double y) { return y > 3000 ? 150.0 : 0.0; } };
		sc.z = 60; sc.seconds = 40;
		auto r = Run(sc);
		Print("17. easy: 150 m cliff, 100 m/s, hands off", r);
		Check(r.minGap > 25.0, "clears the cliff by a wide margin (height keeping)");
	}
	// 18. Launch low (3 m above the ground, from a hover): the height keeping lifts the ship to the minimum.
	{
		Scenario sc{ "easy launch", flat };
		sc.z = 9; sc.speed = 0; sc.seconds = 10;
		auto r = Run(sc);
		Print("18. easy: launch from a hover 9 m up, hands off", r);
		Check(r.end.elevation > 45.0 && r.end.elevation < 60.0, "rose to about the 50 m minimum by itself");
	}
	// 19. Fly at a 300 m ridge at 100 m/s, 50 m up, hands off: kept well above it all the way.
	{
		Scenario sc{ "easy ridge", [](double, double y) { return 300.0 * std::exp(-(y - 3000) * (y - 3000) / (2 * 200.0 * 200.0)); } };
		sc.z = 50; sc.seconds = 50;
		auto r = Run(sc);
		Print("19. easy: 300 m ridge at 100 m/s, hands off", r);
		Check(r.minGap > 25.0, "never closer than 25 m to the ground under the hull");
	}
	// 20. Landing: the minimum is off and the ship settles onto the floor.
	{
		Scenario sc{ "easy landing", flat };
		sc.z = 60; sc.speed = 0; sc.seconds = 15;
		sc.script = [](double, Input& in) { in.landing = true; in.slower = true; };
		auto r = Run(sc);
		Print("20. easy: landing from 60 m", r);
		Check(r.end.elevation >= 0 && r.end.elevation < 7.0, "settled onto the hull floor");
		Check(r.minGap > kHullHalfHeight - 1e-6, "never through the floor");
	}

	// 21. An unclimbable 400 m wall at 100 m/s, 50 m up: must stop (harshly if needed) and never touch it.
	{
		Scenario sc{ "easy wall", [](double, double y) { return y > 3000 ? 400.0 : 0.0; } };
		sc.z = 50; sc.seconds = 60;
		auto r = Run(sc);
		Print("21. easy: 400 m vertical wall at 100 m/s", r);
		Check(r.minGap > kHullHalfHeight, "never touched the wall or the ground");
	}
	// 22. Collision loaded only 100 m ahead at 100 m/s: slows to what can stop within what is known; never collides.
	{
		Scenario sc{ "easy short reach", [](double, double y) { return 250.0 * std::exp(-(y - 2500) * (y - 2500) / (2 * 120.0 * 120.0)); } };
		sc.z = 50; sc.seconds = 80; sc.reach = 100;
		auto r = Run(sc);
		Print("22. easy: 250 m ridge, only 100 m of collision loaded", r);
		Check(r.minGap > kHullHalfHeight, "never collided");
	}
	// 23. Mouse hard down at full speed from 400 m (dive at the ground): braked and pulled up, never collides.
	{
		Scenario sc{ "easy dive", flat };
		sc.z = 400; sc.seconds = 20;
		sc.script = [](double t, Input& in) { if (t > 0.5 && t < 4.0) in.mouseDy = -20; };
		auto r = Run(sc);
		Print("23. easy: full mouse dive from 400 m at 100 m/s", r);
		Check(r.minGap > kHullHalfHeight, "never collided");
	}

	// 24. Mouse rate control: a jitter below the threshold does nothing; a real move turns; still = stabilise to level.
	{
		Scenario sc{ "easy stabilise", flat };
		sc.z = 300; sc.seconds = 8;
		sc.script = [](double t, Input& in) {
			if (t < 1.0) in.mouseDx = 0.3;                                   // 18 counts/s: below the threshold
			else if (t < 1.6) { in.mouseDx = 6; in.mouseDy = 4; }            // a real move: turn right, nose up
		};
		auto r = Run(sc);
		Print("24. easy: jitter, then a mouse move, then still", r);
		std::printf("    end yaw %.2f rad, yaw rate %.3f, pitch %.3f rad\n", r.end.yaw, r.end.yawRate, r.end.pitch);
		Check(r.end.yaw > 0.2, "the real move turned the ship");
		Check(std::fabs(r.end.yawRate) < 0.02 && std::fabs(r.end.pitch) < 0.05, "still: the turn stopped and the nose levelled");
	}

	std::printf("\n%s (%d failed)\n", g_fail ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", g_fail);
	return g_fail ? 1 : 0;
}
