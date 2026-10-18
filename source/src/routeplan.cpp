#include "routeplan.h"

#include "log.h"
#include "vehicleworld.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>

namespace routeplan
{
	namespace
	{
		constexpr int kPilots = 8;
		constexpr float kHorizon = 10.0f, kStep = 0.5f;     // s
		constexpr float kMargin = 0.5f, kMarginZ = 2.5f;    // m clearance around the boxes (the radii are circles around the boxes)
		constexpr float kRange = 600.0f;                    // m: vehicles farther than this are ignored
		constexpr ULONGLONG kHoldMs = 3000;
		constexpr float kLaneRamp = 5.0f;                   // m/s the lane change climbs at (the path predicts it)
		constexpr int kPlayerPriority = 200;
		constexpr int kLanes = 4, kYaws = 7;
		const float kLaneZ[kLanes] = { 0.0f, 8.0f, 16.0f, 24.0f };
		const float kYaw[kYaws] = { 0.0f, 0.35f, -0.35f, 0.7f, -0.7f, 1.05f, -1.05f };  // + is right: keep right first

		world::Snapshot g_snap;
		bool g_have = false;

		struct PState
		{
			std::uint32_t id = 0;
			int priority = 60;
			float laneZ = 0, yaw = 0;
			ULONGLONG changedAt = 0, clearSince = 0, lastLog = 0;
			float lastSpeed = 0;
			float dir[2] = {};
		} g_ps[kPilots];

		int PriorityOf(const world::VehicleInfo& v)
		{
			if (v.who == world::VehicleInfo::Who::Player) return kPlayerPriority;
			for (const auto& p : g_ps) {
				if (p.id && p.id == v.formId) return p.priority;
			}
			if (v.who == world::VehicleInfo::Who::Npc) return 40;
			return 1000;  // parked: never moves, everything yields
		}

		bool Wins(int myPri, std::uint32_t myId, int otherPri, std::uint32_t otherId)
		{
			if (myPri != otherPri) return myPri > otherPri;
			return myId < otherId;
		}

		// The first predicted conflict of the path (heading rotated by `yaw`, lane `laneZ`) with any other vehicle.
		bool FirstConflict(const Query& q, float laneZ, float yaw, float& tOut, std::uint32_t& otherOut, float& distOut)
		{
			const float c = std::cos(yaw), s = std::sin(yaw);
			const float dx = q.dir[0] * c + q.dir[1] * s, dy = q.dir[1] * c - q.dir[0] * s;  // heading + yaw (clockwise = right)
			const float sp = std::max(q.speed, 4.0f);
			const float rSelf = std::hypot(q.half[0], q.half[1]);
			const float rampT = std::fabs(laneZ) / kLaneRamp;
			for (float t = kStep; t <= kHorizon; t += kStep) {
				const float travelled = std::min(sp * t, std::max(q.distToGoal, 0.0f));
				const float px = q.pos[0] + dx * travelled, py = q.pos[1] + dy * travelled;
				const float pz = q.pos[2] + (rampT > 0.0f ? laneZ * std::min(1.0f, t / rampT) : 0.0f);
				for (std::uint32_t i = 0; i < g_snap.count; ++i) {
					const world::VehicleInfo& o = g_snap.v[i];
					if (o.formId == q.selfId) continue;
					const float ox = o.pos[0] + o.vel[0] * t, oy = o.pos[1] + o.vel[1] * t, oz = o.pos[2] + o.vel[2] * t;
					const float R = rSelf + std::hypot(o.half[0], o.half[1]) + kMargin;
					const float hz = q.half[2] + o.half[2] + kMarginZ;
					const float dNow = std::hypot(q.pos[0] - o.pos[0], q.pos[1] - o.pos[1]), dT = std::hypot(px - ox, py - oy);
					// Only a pair that is closing counts: a formation (side by side, one behind the other) at a steady gap is no conflict.
					if (dT >= dNow - 0.25f) continue;
					if (dT < R && std::fabs(pz - oz) < hz) {
						tOut = t;
						otherOut = o.formId;
						distOut = travelled;
						return true;
					}
				}
			}
			return false;
		}

		const world::VehicleInfo* Find(std::uint32_t id)
		{
			for (std::uint32_t i = 0; i < g_snap.count; ++i) {
				if (g_snap.v[i].formId == id) return &g_snap.v[i];
			}
			return nullptr;
		}
	}

	void Frame()
	{
		g_have = world::Vehicles(g_snap);
	}

	void Reset()
	{
		for (auto& p : g_ps) p = PState{};
		g_have = false;
	}

	void Forget(int pilot)
	{
		if (pilot >= 0 && pilot < kPilots) g_ps[pilot] = PState{};
	}

	Plan Update(int pilot, const Query& q)
	{
		Plan plan;
		if (pilot < 0 || pilot >= kPilots || !g_have || !q.selfId) return plan;
		PState& s = g_ps[pilot];
		if (q.distToGoal < 3.0f) {  // arrived / holding: nothing to plan
			s.id = q.selfId;
			s.priority = q.priority;
			return plan;
		}
		s.id = q.selfId;
		s.priority = q.priority;
		s.lastSpeed = q.speed;
		s.dir[0] = q.dir[0];
		s.dir[1] = q.dir[1];
		const ULONGLONG now = GetTickCount64();
		const float laneMax = q.ground ? 0.0f : 1e9f;

		// The current plan, then the plain path.
		float tCur = 0, tPlain = 0, dist = 0;
		std::uint32_t oCur = 0, oPlain = 0;
		const bool curBad = FirstConflict(q, s.laneZ, s.yaw, tCur, oCur, dist);
		const bool plainBad = (s.laneZ == 0.0f && s.yaw == 0.0f) ? curBad : FirstConflict(q, 0.0f, 0.0f, tPlain, oPlain, dist);
		if (s.laneZ == 0.0f && s.yaw == 0.0f) { tPlain = tCur; oPlain = oCur; }

		if (!plainBad) {
			// Clear: release a held lane / side offset once the plain path has been clear for kHoldMs.
			if (!s.clearSince) s.clearSince = now;
			if ((s.laneZ != 0.0f || s.yaw != 0.0f) && now - s.clearSince >= kHoldMs && now - s.changedAt >= kHoldMs) {
				logger::Write("Route plan: [%d] %08X path clear; lane back to the cruise height", pilot, q.selfId);
				s.laneZ = 0.0f;
				s.yaw = 0.0f;
				s.changedAt = now;
			}
		}
		else {
			s.clearSince = 0;
			const world::VehicleInfo* o = Find(oPlain);
			const bool iWin = o && Wins(q.priority, q.selfId, PriorityOf(*o), o->formId);
			if (iWin) {
				// Keep right of an oncoming vehicle; otherwise hold course (the other yields).
				if (o) {
					const float ovl = std::hypot(o->vel[0], o->vel[1]);
					if (ovl > 2.0f && (o->vel[0] * q.dir[0] + o->vel[1] * q.dir[1]) / ovl < -0.5f) plan.yawOffset = 0.2f;
				}
			}
			else if (curBad && (now - s.changedAt >= kHoldMs || (s.laneZ == 0.0f && s.yaw == 0.0f))) {
				// The current plan still conflicts: pick the smallest change that clears every predicted path.
				bool found = false;
				float bestLane = 0, bestYaw = 0;
				for (int li = 0; li < kLanes && !found; ++li) {            // altitude first
					if (kLaneZ[li] > laneMax) break;
					float t; std::uint32_t oo; float d;
					if (!FirstConflict(q, kLaneZ[li], 0.0f, t, oo, d)) { found = true; bestLane = kLaneZ[li]; bestYaw = 0; }
				}
				for (int yi = 1; yi < kYaws && !found; ++yi) {             // then sideways
					float t; std::uint32_t oo; float d;
					if (!FirstConflict(q, 0.0f, kYaw[yi], t, oo, d)) { found = true; bestLane = 0; bestYaw = kYaw[yi]; }
				}
				for (int li = 1; li < kLanes && !found; ++li) {            // then both
					if (kLaneZ[li] > laneMax) break;
					for (int yi = 1; yi < kYaws && !found; ++yi) {
						float t; std::uint32_t oo; float d;
						if (!FirstConflict(q, kLaneZ[li], kYaw[yi], t, oo, d)) { found = true; bestLane = kLaneZ[li]; bestYaw = kYaw[yi]; }
					}
				}
				if (found) {
					if (bestLane != s.laneZ || bestYaw != s.yaw) {
						logger::Write("Route plan: [%d] %08X conflict with %08X in %.1f s -> lane +%.0f m, %+.0f deg (%s)", pilot, q.selfId, oPlain, tPlain, bestLane,
							bestYaw * 57.2958f, o ? (o->who == world::VehicleInfo::Who::Parked ? "parked" : o->who == world::VehicleInfo::Who::Player ? "yours" : "moving") : "?");
						s.laneZ = bestLane;
						s.yaw = bestYaw;
						s.changedAt = now;
					}
				}
				else {
					// Nothing clears it: stop short of the conflict.
					const float stop = std::max(0.0f, dist - (std::hypot(q.half[0], q.half[1]) + kMargin));
					plan.speedCap = tCur < 1.5f ? 0.0f : std::max(0.0f, stop / 4.0f);
					if (now - s.lastLog > 3000) {
						s.lastLog = now;
						logger::Write("Route plan: [%d] %08X boxed in by %08X in %.1f s; speed cap %.0f m/s", pilot, q.selfId, oCur, tCur, plan.speedCap);
					}
				}
			}
		}
		plan.laneZ = s.laneZ;
		plan.yawOffset += s.yaw;
		plan.conflictT = curBad ? tCur : -1.0f;
		plan.other = curBad ? oCur : 0;
		if (curBad && plan.speedCap < 0.0f && tCur < 1.5f && !(plainBad && Find(oCur) && Wins(q.priority, q.selfId, PriorityOf(*Find(oCur)), oCur))) {
			plan.speedCap = 0.0f;  // the held plan is about to hit something: brake
		}
		return plan;
	}
}
