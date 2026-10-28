#include "flightscript.h"
#include <cmath>
#include <cstdio>

using namespace flightscript;

namespace {
int failures = 0;
void Check(bool ok, const char* name) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; }

// A plain vehicle: thrust accelerates along the nose (8 m/s^2 at full throttle, capped by the gear's top speed),
// brake decelerates 10 m/s^2, yaw follows the commanded rate, vertical speed follows the command with a 0.4 s lag.
struct Sim {
	State s;
	double speed = 0;
	void Step(const Out& o, double dt) {
		const double tops[5] = { 0, 5, 18, 50, 150 };
		const double top = tops[std::clamp(o.gear, 1, 4)];
		if (o.brake) speed -= std::copysign(std::min(std::fabs(speed), 10 * dt), speed);
		else speed += o.throttle * 8 * dt;
		speed = std::clamp(speed, -top, top);
		s.heading = Wrap(s.heading + o.yawRate * dt);
		s.vz += (o.vertical - s.vz) * std::min(1.0, dt / 0.4);
		s.vx = std::sin(s.heading) * speed; s.vy = std::cos(s.heading) * speed;
		s.x += s.vx * dt; s.y += s.vy * dt; s.z += s.vz * dt;
	}
};

// Runs one step until done (or the time limit); returns the seconds it took, -1 when it never finished.
double RunStep(Sim& sim, const Step& st, double limit = 120) {
	Progress p; const double dt = 1.0 / 30;
	for (double t = 0; t < limit; t += dt) {
		const Out o = Run(st, p, sim.s, dt);
		if (o.done) return t;
		sim.Step(o, dt);
	}
	return -1;
}
}

int main() {
	{
		Sim sim; Step st; st.op = Op::Drive; st.distance = 15; st.speed = 4;
		const double t = RunStep(sim, st);
		Check(t > 0 && std::fabs(sim.s.y - 15) < 1.5 && std::fabs(sim.s.x) < 0.5, "drive out 15 m forward and stop");
	}
	{
		Sim sim; sim.s.heading = 1.0; Step st; st.op = Op::Drive; st.distance = -10; st.speed = 3;
		const double t = RunStep(sim, st);
		const double back = -(sim.s.x * std::sin(1.0) + sim.s.y * std::cos(1.0));
		Check(t > 0 && std::fabs(back - 10) < 1.5, "reverse 10 m along the start heading");
	}
	{
		Sim sim; Step st; st.op = Op::Rise; st.z = 40;
		const double t = RunStep(sim, st);
		Check(t > 0 && t < 20 && std::fabs(sim.s.z - 40) < 1.0 && std::hypot(sim.s.x, sim.s.y) < 0.5, "rise straight up 40 m");
	}
	{
		Sim sim; sim.s.heading = 0; Step st; st.op = Op::Fly; st.x = -200; st.y = -50; st.z = 0; st.speed = 18; st.radius = 5;
		const double t = RunStep(sim, st);
		Check(t > 0 && std::hypot(sim.s.x + 200, sim.s.y + 50) < 5.5, "turn around and fly to a point behind, stopping there");
	}
	{
		Sim sim; Step st; st.op = Op::Fly; st.x = 0; st.y = 300; st.z = 0; st.speed = 18; st.radius = 20; st.stopAtEnd = false;
		const double t = RunStep(sim, st);
		Check(t > 0 && sim.speed > 10, "pass-through point keeps its speed");
	}
	{
		Sim sim; sim.s.heading = 2.5; Step st; st.op = Op::Face; st.x = -1.0;
		const double t = RunStep(sim, st);
		Check(t > 0 && std::fabs(Wrap(sim.s.heading + 1.0)) < 0.07, "face a heading on the spot");
	}
	{
		Sim sim; sim.s.z = 40; Step st; st.op = Op::Descend; st.z = 1;
		const double t = RunStep(sim, st);
		Check(t > 0 && std::fabs(sim.s.z - 1) < 1.0, "descend straight down");
	}
	{
		// A blocked vehicle (never moves) is reported stuck.
		Progress p; State s; Step st; st.op = Op::Drive; st.distance = 15;
		for (int k = 0; k < 300; ++k) Run(st, p, s, 1.0 / 30);
		Check(Stuck(p, 8.0), "no progress for 10 s counts as stuck");
	}
	{
		Progress p; State s; Step st; st.op = Op::Fly; st.x = 0; st.y = 100; st.drive = true;
		Step r; r.op = Op::Rise; r.z = 10; r.drive = true; Progress q;
		Check(Run(st, p, s, 1.0 / 30).drive && !Run(r, q, s, 1.0 / 30).drive, "drive flag: Fly drives, height steps never do");
	}
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
