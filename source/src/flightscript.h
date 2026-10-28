#pragma once

// Scripted flight steps (2026-10-24, the user: "drive out, float all the way up, then turn around and drive"; the
// autopilot's open planner could not get traffic out of roofed bays). Pure logic: one step turns the vehicle's state
// into a direct command (throttle, yaw rate, vertical speed, gear, brake) and says when it is done.
// The caller sends the command through vehicleflight::PilotCommand with Command::kScripted every frame.
// Heading convention as the game: atan2(x, y), + turns right; forward = (sin h, cos h).
#include <algorithm>
#include <cmath>

namespace flightscript
{
	enum class Op { Drive, Rise, Fly, Face, Descend };

	struct Step
	{
		Op op = Op::Rise;
		double x = 0, y = 0, z = 0;  // Fly: target point; Rise/Descend: target height; Face: heading in x
		double distance = 0;         // Drive: metres along the start heading (negative = reverse)
		double speed = 0;            // Fly / Drive: wanted ground speed (m/s)
		double radius = 5;           // Fly: done within this many metres (pass-through points use a larger one)
		bool stopAtEnd = true;       // Fly: brake to a stop at the point (false: keep the speed through it)
		bool drive = false;          // Fly / Drive: on the surface (the hover held low follows it; no height command) instead of floating
	};

	struct State { double x = 0, y = 0, z = 0, heading = 0, vx = 0, vy = 0, vz = 0; };

	struct Out
	{
		double throttle = 0, yawRate = 0, vertical = 0;
		int gear = 1;
		bool brake = false, done = false;
		bool drive = false;  // the step drives on the surface: send no height command, travel = drive
	};

	// What the step measured, for the log: how well the vehicle followed the commands.
	struct Metrics
	{
		double seconds = 0, maxHeightError = 0, maxHeadingError = 0, maxSpeed = 0, climbAsked = 0, climbGot = 0;
		unsigned samples = 0;
	};

	struct Progress
	{
		bool started = false;
		double startX = 0, startY = 0, startZ = 0, startHeading = 0, holdZ = 0;
		double bestRemaining = 1e18, sinceProgress = 0;  // stuck detection
		Metrics m;
	};

	constexpr double kPi = 3.14159265358979323846;
	inline double Wrap(double a) { while (a > kPi) a -= 2 * kPi; while (a < -kPi) a += 2 * kPi; return a; }
	inline double Forward(const State& s) { return s.vx * std::sin(s.heading) + s.vy * std::cos(s.heading); }
	inline int GearFor(double speed) { const double v = std::fabs(speed); return v <= 4.5 ? 1 : v <= 16 ? 2 : v <= 45 ? 3 : 4; }

	// Speed control: throttle toward the wanted forward speed; brake when clearly too fast or asked to stop.
	inline void Speed(Out& o, double want, double forward)
	{
		o.gear = GearFor(std::max(std::fabs(want), std::fabs(forward)));
		const double err = want - forward;
		o.throttle = std::clamp(err * 0.35, -1.0, 1.0);
		o.brake = (std::fabs(want) < 0.3 && std::fabs(forward) > 0.3) || (want >= 0 && forward > want + 2.0) || (want < 0 && forward < want - 2.0);
		if (o.brake) o.throttle = 0;
	}
	inline double HoldHeight(double target, double z, double limit = 3.0) { return std::clamp((target - z) * 0.8, -limit, limit); }
	inline double Steer(double err, double limit) { return std::clamp(err * 1.6, -limit, limit); }

	// One frame of a step. dt: seconds since the last call (for the metrics and stuck timer).
	inline Out Run(const Step& st, Progress& p, const State& s, double dt)
	{
		Out o;
		o.drive = st.drive && (st.op == Op::Fly || st.op == Op::Drive);
		if (!p.started) {
			p = Progress{};
			p.started = true; p.startX = s.x; p.startY = s.y; p.startZ = s.z; p.startHeading = s.heading; p.holdZ = s.z;
		}
		const double forward = Forward(s);
		double remaining = 0;
		p.m.seconds += dt; ++p.m.samples;
		p.m.maxSpeed = std::max(p.m.maxSpeed, std::hypot(s.vx, s.vy));
		switch (st.op) {
		case Op::Drive: {
			const double along = (s.x - p.startX) * std::sin(p.startHeading) + (s.y - p.startY) * std::cos(p.startHeading);
			const double sign = st.distance < 0 ? -1.0 : 1.0;
			remaining = std::fabs(st.distance) - along * sign;
			const double want = remaining > 0.5 ? sign * std::min(st.speed > 0 ? st.speed : 4.0, std::max(1.0, remaining * 0.8)) : 0.0;
			Speed(o, want, forward);
			const double herr = Wrap(p.startHeading - s.heading);
			o.yawRate = Steer(herr, 0.4);
			o.vertical = HoldHeight(p.holdZ, s.z, 1.5);
			p.m.maxHeadingError = std::max(p.m.maxHeadingError, std::fabs(herr));
			p.m.maxHeightError = std::max(p.m.maxHeightError, std::fabs(s.z - p.holdZ));
			o.done = remaining <= 0.5 && std::fabs(forward) < 0.8;
			break;
		}
		case Op::Rise:
		case Op::Descend: {
			remaining = std::fabs(st.z - s.z);
			const double up = st.op == Op::Rise ? 4.0 : 2.5;
			o.vertical = std::clamp((st.z - s.z) * 0.8, -up, up);
			if (st.op == Op::Rise && st.z - s.z > 3) o.vertical = up;
			if (st.op == Op::Descend && s.z - st.z > 3) o.vertical = -up;
			Speed(o, 0.0, forward);
			const double herr = Wrap(p.startHeading - s.heading);
			o.yawRate = Steer(herr, 0.3);
			p.m.climbAsked += std::fabs(o.vertical) * dt; p.m.climbGot += std::fabs(s.vz) * dt;
			p.m.maxHeadingError = std::max(p.m.maxHeadingError, std::fabs(herr));
			o.done = remaining < 0.8 && std::fabs(s.vz) < 1.0;
			break;
		}
		case Op::Face: {
			const double herr = Wrap(st.x - s.heading);
			remaining = std::fabs(herr) * 10;
			o.yawRate = Steer(herr, 0.8);
			Speed(o, 0.0, forward);
			o.vertical = HoldHeight(p.holdZ, s.z);
			p.m.maxHeightError = std::max(p.m.maxHeightError, std::fabs(s.z - p.holdZ));
			o.done = std::fabs(herr) < 0.06;
			break;
		}
		case Op::Fly: {
			const double dx = st.x - s.x, dy = st.y - s.y, dist = std::hypot(dx, dy);
			remaining = dist;
			const double herr = dist > 1.0 ? Wrap(std::atan2(dx, dy) - s.heading) : 0.0;
			o.yawRate = Steer(herr, 0.6);
			// Turn on the spot when facing away; speed builds as the nose lines up, and eases into a stop at the point.
			const double align = std::clamp(1.0 - (std::fabs(herr) - 0.15) / 0.5, 0.0, 1.0);
			double want = (st.speed > 0 ? st.speed : 18.0) * align;
			if (st.stopAtEnd) want = std::min(want, std::sqrt(std::max(0.0, 2.0 * 2.0 * (dist - st.radius * 0.5))));
			Speed(o, want, forward);
			o.vertical = HoldHeight(st.z, s.z);
			p.m.maxHeightError = std::max(p.m.maxHeightError, std::fabs(s.z - st.z));
			if (align > 0.9) p.m.maxHeadingError = std::max(p.m.maxHeadingError, std::fabs(herr));
			o.done = dist <= st.radius && (!st.stopAtEnd || std::fabs(forward) < 1.0);
			// Turning toward the point is progress too (2026-10-25: a car turning on the spot at the first hop counted as blocked
			// after 3 s and floated for nothing): the heading still to turn counts like the Face step's.
			if (align < 1.0) remaining += std::fabs(herr) * 10;
			break;
		}
		}
		// Stuck: no step-progress of 0.5 m (Face: ~3 deg) for this long while commanding.
		if (remaining < p.bestRemaining - 0.5) { p.bestRemaining = remaining; p.sinceProgress = 0; }
		else p.sinceProgress += dt;
		return o;
	}
	inline bool Stuck(const Progress& p, double seconds) { return p.started && p.sinceProgress > seconds; }
}
