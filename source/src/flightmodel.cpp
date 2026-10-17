#include "flightmodel.h"

#include <algorithm>
#include <cmath>

namespace fm
{
	namespace
	{
		constexpr double kPi = 3.14159265358979323846;
		double Clamp(double v, double a, double b) { return v < a ? a : (v > b ? b : v); }
		double Lerp(double a, double b, double t) { return a + (b - a) * t; }
		double Approach(double cur, double target, double dt, double tau) { return cur + (target - cur) * (1.0 - std::exp(-dt / tau)); }
	}

	const char* ModeName(Mode m)
	{
		switch (m) {
		case Mode::Cruise: return "CRUISE";
		case Mode::Warning: return "TERRAIN AHEAD";
		case Mode::Assist: return "TERRAIN ASSIST";
		case Mode::Scraping: return "SCRAPING";
		case Mode::Stall: return "STALL";
		case Mode::Grounded: return "LANDED";
		}
		return "?";
	}

	Model::Model() { Reset(0, 0, 100, 0, 50); }

	void Model::Reset(double x, double y, double z, double yaw, double speed)
	{
		s = State{};
		s.x = x;
		s.y = y;
		s.z = z;
		s.yaw = yaw;
		s.speed = speed;
		s.throttle = Clamp(speed / p.vMax, 0.0, 1.0);
		s.fx = std::sin(yaw);
		s.fy = std::cos(yaw);
		s.fz = 0;
		s.vx = s.fx * speed;
		s.vy = s.fy * speed;
		m_req.clear();
		m_lastReq.clear();
		m_lastRes.clear();
		PlanRequests(1.0 / 60.0);
	}

	void Model::Feed(const std::vector<Request>& reqs, const std::vector<Result>& res)
	{
		m_lastReq = reqs;
		m_lastRes = res;
	}

	void Model::PlanRequests(double dt)
	{
		m_req.clear();
		// Hull points at where the ship will be in about two frames (results arrive late).
		const double px = s.x + s.vx * dt * 2.0, py = s.y + s.vy * dt * 2.0;
		const double fwdx = std::sin(s.yaw), fwdy = std::cos(s.yaw);
		const double rgtx = std::cos(s.yaw), rgty = -std::sin(s.yaw);
		m_req.push_back({ px, py, 2, 0 });  // centre
		m_req.push_back({ px + fwdx * p.hullHalfLen, py + fwdy * p.hullHalfLen, 0, 0 });
		m_req.push_back({ px - fwdx * p.hullHalfLen, py - fwdy * p.hullHalfLen, 0, 0 });
		m_req.push_back({ px + rgtx * p.hullHalfSpan, py + rgty * p.hullHalfSpan, 0, 0 });
		m_req.push_back({ px - rgtx * p.hullHalfSpan, py - rgty * p.hullHalfSpan, 0, 0 });
		// Look-ahead along the horizontal travel direction.
		double dx = fwdx, dy = fwdy;
		const double hh = std::hypot(s.vx, s.vy);
		if (hh > 1.0) {
			dx = s.vx / hh;
			dy = s.vy / hh;
		}
		for (double t : p.lookTimes) {
			const double d = std::max(t * std::max(s.speed, 20.0), 40.0);
			m_req.push_back({ s.x + dx * d, s.y + dy * d, 1, d });
		}
	}

	void Model::Step(double dt, const Input& in)
	{
		State& st = s;

		// --- Virtual stick: the mouse pushes it, it springs back to centre (this is the momentum). ---
		st.stickX += in.mouseDx * p.mouseGain;
		st.stickY += in.mouseDy * p.mouseGain * (p.invertPitch ? -1.0 : 1.0);
		const double decay = std::exp(-dt / p.stickTau);
		st.stickX = Clamp(st.stickX * decay, -1.0, 1.0);
		st.stickY = Clamp(st.stickY * decay, -1.0, 1.0);

		const double auth = Lerp(p.authLow, 1.0, Clamp(st.speed / p.vAuth, 0.0, 1.0));

		// --- Pilot commands (rates). Easy scheme: mouse = pitch and yaw. Handoff scheme: mouse = pitch and roll,
		// rudder = yaw. ---
		double userPitch = p.pitchRateMax * st.stickY * (p.mouseYaw ? 1.0 : auth);
		double easyYaw = p.easyYawRateMax * st.stickX;
		if (p.mouseYaw && p.mouseRateControl) {
			const double sx = dt > 1e-6 ? in.mouseDx / dt : 0.0;
			const double sy = dt > 1e-6 ? in.mouseDy / dt * (p.invertPitch ? -1.0 : 1.0) : 0.0;
			st.mouseSX = Approach(st.mouseSX, sx, dt, p.mouseFilterTau);
			st.mouseSY = Approach(st.mouseSY, sy, dt, p.mouseFilterTau);
			const double mag = std::hypot(st.mouseSX, st.mouseSY);
			if (!st.mouseActive && mag > p.mouseOn) {
				st.mouseActive = true;
			}
			else if (st.mouseActive && mag < p.mouseOff) {
				st.mouseActive = false;
			}
			if (st.mouseActive) {
				easyYaw = Clamp(st.mouseSX * p.yawPerCount, -p.easyYawRateMax, p.easyYawRateMax);
				userPitch = Clamp(st.mouseSY * p.pitchPerCount, -p.pitchRateMax, p.pitchRateMax);
			}
			else {
				easyYaw = 0.0;  // still: stabilise
				userPitch = Clamp(-st.pitch * p.levelRate, -p.pitchRateMax * 0.5, p.pitchRateMax * 0.5);
			}
		}
		double userRoll = p.rollRateMax * st.stickX * auth;
		if (p.autoLevelRate > 0 && std::fabs(st.stickX) < 0.02) {
			userRoll = -Clamp(st.roll * 3.0, -p.autoLevelRate, p.autoLevelRate);
		}
		const double rudder = p.yawRateMax * Clamp(in.rudder, -1.0, 1.0) * auth;
		if (p.mouseYaw) {
			// The hull banks into the turn for looks; the turn itself comes straight from the mouse.
			const double bank = Clamp(st.yawRate * p.bankPerYawRate, -p.maxRoll * 0.6, p.maxRoll * 0.6);
			userRoll = Clamp((bank - st.roll) * 4.0, -p.rollRateMax, p.rollRateMax);
		}

		// --- Read the (late) ground samples. ---
		double dirx = std::sin(st.yaw), diry = std::cos(st.yaw);
		const double hh = std::hypot(st.vx, st.vy);
		if (hh > 1.0) {
			dirx = st.vx / hh;
			diry = st.vy / hh;
		}
		const double gammaCur = std::atan2(st.vz, std::max(hh, 1.0));
		bool violation = false;
		double worstGamma = -1e9, ttc = 1e9;
		double knownReach = 0;
		bool anyLookValid = false;
		double hullFloor = -1e18;
		bool haveHull = false;
		st.elevation = -1;
		double prevAlong = 0;
		double aheadAlong[8], aheadH[8];
		int aheadN = 0;
		for (std::size_t i = 0; i < m_lastReq.size() && i < m_lastRes.size(); ++i) {
			const Request& rq = m_lastReq[i];
			const Result& rs = m_lastRes[i];
			if (rq.kind == 1) {
				const double along = (rq.x - st.x) * dirx + (rq.y - st.y) * diry;
				if (!rs.valid || along <= 1.0) {
					continue;
				}
				anyLookValid = true;
				knownReach = std::max(knownReach, along);
				if (aheadN < 8) {
					aheadAlong[aheadN] = along;
					aheadH[aheadN] = rs.h;
					++aheadN;
				}
				// The obstacle may start anywhere between the previous sample and this one: be conservative and
				// assume it starts right after the previous sample.
				const double near = std::max(prevAlong, 15.0);
				prevAlong = along;
				const double H = rs.h + p.clearance;
				const double pathZ = st.z + std::tan(gammaCur) * near;
				if (pathZ < H) {
					violation = true;
					worstGamma = std::max(worstGamma, std::atan2(H - st.z, near));
					ttc = std::min(ttc, near / std::max(st.speed, 1.0));
				}
			}
			else if (rs.valid) {
				hullFloor = std::max(hullFloor, rs.h + p.clearance);
				haveHull = true;
				if (rq.kind == 2) {
					st.elevation = st.z - rs.h;
				}
			}
		}
		st.ttc = ttc;

		// Unknown ground ahead limits speed (collision that is not loaded cannot be avoided).
		st.speedCap = 1e9;
		if (!m_lastRes.empty() && !anyLookValid) {
			st.speedCap = p.minSpeedCap;
		}
		else if (anyLookValid) {
			st.speedCap = std::max(p.minSpeedCap, knownReach / p.reachTime);
		}

		// --- Predictive braking: the nearest point ahead the ship cannot clear in time, or where nothing is known. ---
		// A speed limit, not a yes/no: for every point ahead the ship is not yet above (with the hull clearance and a
		// margin), approach no faster than it can climb over it in time (room / climb time), and never faster than it
		// could stop within the room. Unknown ground: stop within what is known. Close to an unclearable wall this is a
		// stop, then a slow approach while the lift raises the ship.
		double obstacle = 1e18, allowAhead = 1e18;
		bool climbToClear = false;
		if (p.brakeDecel > 0 && !in.landing) {
			for (int i = 0; i < aheadN; ++i) {
				const double need = aheadH[i] + p.clearance + p.brakeHeightMargin - st.z;
				if (need <= 0) {
					continue;
				}
				const double room = std::max(0.0, aheadAlong[i] - p.hullHalfLen - p.brakeMargin);
				const double tClimb = need / p.assistLiftMax + p.tauAssistLift + 0.3;
				const double allow = std::min(room / tClimb, std::sqrt(2.0 * p.brakeDecel * room));
				if (allow < allowAhead) {
					allowAhead = allow;
					obstacle = aheadAlong[i];
				}
				if (allow < st.speed + 1.0) {
					climbToClear = true;  // held back by it: climb at full lift to get over
				}
			}
			if (!m_lastRes.empty()) {
				const double room = std::max(0.0, (anyLookValid ? knownReach : 40.0) - p.hullHalfLen);
				const double allow = std::sqrt(2.0 * p.brakeDecel * room);
				if (allow < allowAhead) {
					allowAhead = allow;
					obstacle = anyLookValid ? knownReach : 40.0;
				}
			}
		}

		// --- Height keeping: the climb rate needed to stay minClearance above what is under and ahead of the ship. ---
		const bool keepHeight = p.minClearance > 0 && !in.landing;
		const double hullGround = haveHull ? hullFloor - p.clearance : -1e18;
		double vReq = -1e18;
		if (keepHeight) {
			if (haveHull) {
				vReq = p.floorGain * (hullGround + p.minClearance - st.z);
			}
			for (int i = 0; i < aheadN; ++i) {
				const double need = aheadH[i] + p.minClearance - st.z;
				if (need <= 0) {
					continue;  // already above the minimum there: descent over it is the hull term's business
				}
				const double t = std::max(aheadAlong[i] / std::max(st.speed, 1.0), 0.3);
				vReq = std::max(vReq, need / t);
				if (need > 0 && need / t > p.assistLiftMax) {
					// Full lift cannot clear it in time at this speed: slow down so it can.
					st.speedCap = std::min(st.speedCap, std::max(p.minSpeedCap * 0.5, aheadAlong[i] * p.assistLiftMax / need));
				}
			}
		}

		// --- Hidden controller: terrain assist adds a pitch-up command through the same path as the pilot. ---
		double assistW = 0, assistRate = 0;
		if (violation) {
			const double thetaReq = std::min(worstGamma + p.assistMargin, p.maxPitch);
			const double missing = thetaReq - st.pitch;
			const double tPull = std::max(0.0, missing) / p.pitchRateMax + p.tauPitch + p.tauAlign;
			assistW = Clamp((2.0 * tPull - ttc) / tPull, 0.0, 1.0);  // starts at twice the pull-up time, full at 1x
			assistRate = Clamp(p.assistGain * missing, 0.0, p.pitchRateMax);
		}
		if (st.elevation >= 0 && st.elevation < p.landingHeight && st.speed < p.landingSpeed) {
			assistW = 0;  // landing envelope
		}
		if (p.mouseYaw) {
			// Easy scheme: the height keeping (lift) and predictive braking keep the ship clear; the nose only moves
			// with the mouse (a pitch-up assist left the nose up, so the ship kept climbing after every obstacle).
			assistW = 0;
		}
		double pitchTarget = userPitch;
		if (assistW > 0 && assistRate > userPitch) {
			pitchTarget = Lerp(userPitch, assistRate, assistW);  // only ever pushes the nose up
		}

		if (st.recovering > 0 && std::fabs(st.stickY) < 0.05) {
			pitchTarget += Clamp(-st.pitch * 1.5, -p.stallRecoveryRate, p.stallRecoveryRate);
			st.recovering -= dt;
		}

		// --- Momentum on every axis. ---
		st.pitchRate = Approach(st.pitchRate, pitchTarget, dt, p.tauPitch);
		st.rollRate = Approach(st.rollRate, userRoll, dt, p.tauRoll);
		const double yawTarget = p.mouseYaw ? easyYaw : p.turnGain * std::sin(st.roll) * auth + rudder;
		st.yawRate = Approach(st.yawRate, yawTarget, dt, p.mouseYaw ? p.easyTauYaw : p.tauYaw);
		st.pitch = Clamp(st.pitch + st.pitchRate * dt, -p.maxPitch, p.maxPitch);
		st.roll = Clamp(st.roll + st.rollRate * dt, -p.maxRoll, p.maxRoll);
		st.yaw += st.yawRate * dt;
		if (st.yaw > kPi) st.yaw -= 2 * kPi;
		if (st.yaw < -kPi) st.yaw += 2 * kPi;

		// --- Speed: throttle lever (W / S), climbing costs speed, hard turns cost speed. ---
		if (in.faster) st.throttle += p.throttleRate * dt;
		if (in.slower) st.throttle -= p.throttleRate * dt;
		st.throttle = Clamp(st.throttle, 0.0, 1.0);
		if (p.simpleSpeed) {
			st.speed += Clamp(st.throttle * p.vMax - st.speed, -p.accel * dt, p.accel * dt);
		}
		else {
			double target = std::min(st.throttle * p.vMax, st.speedCap);
			st.speed += Clamp(p.thrustGain * (target - st.speed), -p.accel, p.accel) * dt;
			st.speed -= p.gravityAlong * std::sin(st.pitch) * dt;
			const double n = 1.0 / std::max(std::cos(st.roll), 0.2);
			st.speed -= p.turnBleed * (n * n - 1.0) * dt;
		}
		st.speed = Clamp(st.speed, 0.0, p.vMax);
		// Predictive braking: never faster than what stops before the obstacle (or the ground when diving).
		st.braking = false;
		st.obstacle = obstacle < 1e17 ? obstacle : -1;
		if (p.brakeDecel > 0 && !in.landing) {
			double allow = 1e18;
			allow = allowAhead;
			// Diving: the path's downward speed must be able to stop above the hull floor (lift helps at assistLiftMax).
			if (haveHull && st.fz < -0.05) {
				const double above = st.z - (hullGround + p.clearance + 5.0);
				const double decel = p.brakeDecel + p.assistLiftMax;
				const double vDownMax = std::sqrt(2.0 * decel * std::max(0.0, above));
				allow = std::min(allow, vDownMax / std::max(-st.fz, 0.05));
			}
			if (st.speed > allow) {
				st.speed = std::max(allow, st.speed - p.brakeDecel * dt);
				st.braking = true;
			}
		}

		// --- Stall: slow and not near the ground -> nose drops and the ship sinks. ---
		const bool nearGround = st.elevation >= 0 && st.elevation < p.nearGround;
		st.stalled = p.stallEnabled && st.speed < p.vStall && !nearGround;
		const double sev = Clamp(1.0 - st.speed / p.vStall, 0.0, 1.0);
		if (st.stalled) {
			st.sink = std::min(st.sink + p.stallSink * sev * dt, p.terminalSink);
			if (st.pitch > -p.stallNoseDownLimit) {
				st.pitch = std::max(st.pitch - p.stallNoseDown * sev * dt, -p.stallNoseDownLimit);
			}
			st.recovering = p.stallRecoveryTime;
		}
		else {
			st.sink = std::max(0.0, st.sink - 15.0 * dt);
		}
		if (nearGround) {
			st.sink = std::min(st.sink, p.nearGroundSink);
		}

		// --- The flight path chases the nose (so climb / descent come from where the nose points). ---
		const double nx = std::sin(st.yaw) * std::cos(st.pitch), ny = std::cos(st.yaw) * std::cos(st.pitch), nz = std::sin(st.pitch);
		const double k = 1.0 - std::exp(-dt / p.tauAlign);
		st.fx += (nx - st.fx) * k;
		st.fy += (ny - st.fy) * k;
		st.fz += (nz - st.fz) * k;
		const double fl = std::sqrt(st.fx * st.fx + st.fy * st.fy + st.fz * st.fz);
		st.fx /= fl;
		st.fy /= fl;
		st.fz /= fl;

		// Strafe and lift: thrust along the ship's level right axis and straight up; the nose is not touched.
		st.vStrafe = Approach(st.vStrafe, Clamp(in.strafe, -1.0, 1.0) * p.strafeSpeed, dt, p.tauThrust);
		if (st.braking && st.speed < 1.0 && obstacle < p.brakeMargin + p.hullHalfLen + 10.0) {
			st.vStrafe = 0;  // stopped at an obstacle reached sideways: no further slide into it
		}
		double liftIn = Clamp(in.lift, -1.0, 1.0);
		if (keepHeight && haveHull && liftIn < 0 && st.z < hullGround + p.minClearance + 2.0) {
			liftIn = 0;  // the pilot cannot push below the minimum distance (F7 lands)
		}
		double liftTarget = liftIn * p.liftSpeed;
		if (in.landing) {
			liftTarget = -p.landingSink;
		}
		st.vLift = Approach(st.vLift, liftTarget, dt, p.tauThrust);
		double assistTarget = 0;
		if (climbToClear) {
			vReq = std::max(vReq, p.assistLiftMax);  // held back by something ahead: full lift to get over it
		}
		if (keepHeight && vReq > -1e17) {
			const double baseVz = st.fz * st.speed - st.sink + st.vLift;
			if (vReq > baseVz) {
				assistTarget = std::min(vReq - baseVz, p.assistLiftMax);
			}
		}
		st.assistLift = Approach(st.assistLift, assistTarget, dt, p.tauAssistLift);
		const double rx = std::cos(st.yaw), ry = -std::sin(st.yaw);
		st.vx = st.fx * st.speed + rx * st.vStrafe;
		st.vy = st.fy * st.speed + ry * st.vStrafe;
		st.vz = st.fz * st.speed - st.sink + st.vLift + st.assistLift;
		st.x += st.vx * dt;
		st.y += st.vy * dt;
		st.z += st.vz * dt;

		// --- Backstop: the hard floor (only reached if the assist could not pull up in time). ---
		bool scraping = false, grounded = false;
		if (haveHull && st.z < hullFloor) {
			const double lift = hullFloor - st.z;
			st.z = hullFloor;
			if (st.fz < 0) {
				st.fz = 0;
				const double l2 = std::sqrt(st.fx * st.fx + st.fy * st.fy);
				if (l2 > 1e-6) {
					st.fx /= l2;
					st.fy /= l2;
				}
			}
			st.sink = 0;
			st.vLift = std::max(0.0, st.vLift);
			st.speed *= 1.0 - 0.3 * dt;
			// Resting on the floor slowly is a landing, not a scrape.
			if (lift < 0.5 && st.speed < 25.0) grounded = true;
			else scraping = true;
			++st.clampEvents;
			st.maxClampLift = std::max(st.maxClampLift, lift);
		}

		if (grounded) st.mode = Mode::Grounded;
		else if (scraping) st.mode = Mode::Scraping;
		else if (assistW > 0 || st.assistLift > 2.0 || st.braking) st.mode = Mode::Assist;
		else if (st.stalled) st.mode = Mode::Stall;
		else if (violation && ttc < p.warnTTC) st.mode = Mode::Warning;
		else st.mode = Mode::Cruise;

		PlanRequests(dt);
	}
}
