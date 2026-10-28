#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <string_view>
#include <vector>

// The autopilot's decision core (2026-10-27). One authority per vehicle: it picks the heading, the speed, drive or
// hover, and the way out when stuck. It replaces 0.8's stacked rules (route worker, MPC, detours, lane offsets, assist
// states, travel-mode flips, stuck/lost blocks), which the 2026-10-25 22:18 log showed fighting each other:
//   - 330 "replan: local corridor blocked" loops and 106 "no route" while the vehicle braked and waited;
//   - "ahead 2 need 41 m, want 0, brake 1" with the target 20-30 m away: a hillside read as a wall (horizontal rays
//     that hit terrain are stored as solid in navmap, and the forward ray hit set wallAhead);
//   - drive/hover flipping (38 drive on, 25 off, ~36 each travel change);
//   - a stuck vehicle giving up for good (EscapeMove holds "for Retry").
// Rules here: never wait for a planner (there is always a heading); unknown or uncertain ground slows, it does not stop;
// a surface hit near the ground is terrain judged by its slope, not a wall; stopped vehicles are obstacles to steer
// round; modes change with minimum dwell times; stuck escalates (reverse, other side, hover, climb) and never gives up.
// No engine access: the caller supplies the map and the vehicle list. Main thread.
namespace autodrive
{
	constexpr double kPi = 3.14159265358979323846;
	inline double Wrap(double a) { return std::remainder(a, 2.0 * kPi); }
	inline double Bearing(double dx, double dy) { return std::atan2(dx, dy); }  // the game's yaw: atan2(x, y)

	enum class Mode : std::uint8_t { Drive, Hover };
	enum class Permit : std::uint8_t { Both, DriveOnly, HoverOnly };
	enum class Phase : std::uint8_t { Travel, Arrived, Yield, Reverse, Turn, Climb, Boxed };
	enum class Verdict : std::uint8_t { Clear, Unknown, Caution, Blocked };

	inline const char* Name(Mode m) { return m == Mode::Drive ? "drive" : "hover"; }
	inline const char* Name(Phase p)
	{
		switch (p) {
		case Phase::Travel: return "travel";
		case Phase::Arrived: return "arrived";
		case Phase::Yield: return "yield";
		case Phase::Reverse: return "reverse";
		case Phase::Turn: return "turning out";
		case Phase::Climb: return "climb";
		default: return "boxed";
		}
	}
	inline const char* Name(Verdict v)
	{
		switch (v) {
		case Verdict::Clear: return "clear";
		case Verdict::Unknown: return "unknown";
		case Verdict::Caution: return "caution";
		default: return "blocked";
		}
	}

	// What the shared map remembers at one point (navmap::Sample, reduced). solid: a remembered ray hit at solidZ;
	// object: a game collision box covers the height asked.
	struct Cell
	{
		bool known = false, haveGround = false, solid = false, object = false;
		// A game collision box the vehicle itself is inside of nearby: the boxes there are a poor fit (it stands where one
		// says solid), so they mean "go slowly", not "blocked" (2026-10-27 16:03 run: a rover read every heading closed
		// by "object" at 2 m and never left a settlement).
		bool objectSoft = false;
		double ground = 0, solidZ = 0;
	};

	struct Shape
	{
		double halfWidth = 1.25, halfLength = 2.3, height = 2.0;
		double originAbove = 0.8;  // the reference's height over the ground it stands on
	};

	struct Limits
	{
		double driveTop = 20.0, hoverTop = 26.0;  // m/s
		double brake = 6.0;                       // m/s^2 the vehicle can count on
		double climbRate = 5.0;                   // m/s the hover can climb
		double hoverClear = 2.5;                  // m kept over the surface ahead when hovering
		double maxSlope = 0.62;                   // rise/run the wheels take (about 32 degrees)
		double maxStep = 1.1;                     // m a single 2 m step may rise when driving
		double maxDrop = 1.5;                     // m a step may fall when driving (or 0.9 m per metre of run, whichever is more)
		double unknownDrive = 9.0, unknownHover = 12.0;  // m/s over ground nobody has seen
	};

	struct PathInfo
	{
		Verdict v = Verdict::Unknown;
		double free = 0;     // m open along the heading (the whole length when not blocked)
		const char* why = "";
	};

	// A vehicle's path along `heading` for `len` metres, three lanes (centre and both sides of the hull), 2 m steps.
	// Drive: each lane's ground must not rise faster than the wheels climb or fall off an edge; a solid hit is a wall only
	// when it stands clear of the ground under it (a hillside hit is terrain, judged by the slope). A hit where the ground
	// is unknown is Caution (approach slowly: the ground probes along the heading tell hill from wall on the way).
	// Hover: the surface plus clearance must stay within what the climb rate gives at `speed`; walls as in Drive.
	template <class MapFn>
	PathInfo EvalPath(MapFn&& map, double x, double y, double z, double groundUnder, double heading, double len,
		const Shape& shape, const Limits& lim, Mode mode, double speed)
	{
		PathInfo out;
		out.free = len;
		if (!(len > 0.5)) { out.v = Verdict::Clear; return out; }
		if (len < std::max(2.0, std::ceil((shape.halfLength + 1.0) / 2.0) * 2.0)) len = std::ceil((shape.halfLength + 1.0) / 2.0) * 2.0;
		const double ux = std::sin(heading), uy = std::cos(heading), rx = uy, ry = -ux;
		// The side lanes keep a margin past the hull: the map's cells are 4 m, so a wall edge can sit inside a cell
		// whose centre reads open.
		const double side = shape.halfWidth + 0.9;
		const double lateral[3] = { 0.0, side, -side };
		double prevG[3] = { groundUnder, groundUnder, groundUnder }, prevS[3] = { 0, 0, 0 };
		int known = 0, total = 0;
		bool caution = false;
		double cautionAt = len;
		// The first step is past the vehicle's own nose: cells under its own hull hold its own roof, its own probes'
		// misses and whatever it stands on, none of which is in its way.
		const double first = std::max(2.0, std::ceil((shape.halfLength + 1.0) / 2.0) * 2.0);
		for (double s = first; s <= len + 1e-9; s += 2.0) {
			for (int k = 0; k < 3; ++k) {
				const double px = x + ux * s + rx * lateral[k], py = y + uy * s + ry * lateral[k];
				// The height the vehicle would be at here: on the wheels over this lane's ground, or the hover's height.
				const double laneGround = prevG[k];
				const double zv = mode == Mode::Drive ? laneGround + shape.originAbove : z;
				const Cell c = map(px, py, zv);
				++total;
				if (c.known) ++known;
				if (c.haveGround) {
					if (mode == Mode::Drive) {
						const double run = s - prevS[k], rise = c.ground - prevG[k];
						if (rise > std::max(lim.maxStep, lim.maxSlope * run)) { out.v = Verdict::Blocked; out.free = s - 2.0; out.why = "steep"; return out; }
						if (-rise > std::max(lim.maxDrop, 0.9 * run)) { out.v = Verdict::Blocked; out.free = s - 2.0; out.why = "drop"; return out; }
						prevG[k] = c.ground;
						prevS[k] = s;
					}
					else if (c.ground + lim.hoverClear - z > lim.climbRate * s / std::max(speed, 5.0)) {
						out.v = Verdict::Blocked; out.free = s - 2.0; out.why = "rising ground"; return out;
					}
				}
				if (c.object && !c.objectSoft) { out.v = Verdict::Blocked; out.free = s - 2.0; out.why = "object"; return out; }
				if (c.object && c.objectSoft) { if (!caution || s < cautionAt) cautionAt = s; caution = true; }
				if (c.solid) {
					const double base = c.haveGround ? c.ground : laneGround;
					const double zAt = mode == Mode::Drive ? (c.haveGround ? c.ground : laneGround) + shape.originAbove : z;
					const bool terrain = c.haveGround && c.solidZ <= c.ground + 1.5;  // the ray met the ground itself
					const bool inBand = c.solidZ >= zAt - shape.originAbove + 0.6 && c.solidZ <= zAt + shape.height + 1.0;
					if (!terrain && inBand) {
						if (c.haveGround && c.solidZ > base + 1.5) { out.v = Verdict::Blocked; out.free = s - 2.0; out.why = "wall"; return out; }
						if (!caution || s < cautionAt) cautionAt = s;
						caution = true;
					}
				}
			}
		}
		if (caution) { out.v = Verdict::Caution; out.free = std::max(0.0, cautionAt - 2.0); out.why = "hit, ground unknown"; return out; }
		if (total > 0 && known * 10 < total * 3) { out.v = Verdict::Unknown; out.why = "unmapped"; return out; }
		out.v = Verdict::Clear;
		return out;
	}

	// A coarse ground route over the map (4 m cells, 244 m square round the vehicle), for what the heading fan cannot see
	// round: a cliff with one ramp, a canyon, a walled yard. Synchronous and bounded (one search touches at most 3,721
	// cells); a goal outside the square or unreachable gives the route to the reachable cell nearest it. Drive rules:
	// a step between cells may not rise faster than the wheels climb or fall off an edge; walls and bumps are closed;
	// unmapped cells are open but cost more (optimistic, the fan still checks every metre it drives).
	struct Route
	{
		bool ok = false, reaches = false;
		int n = 0;
		double px[96] = {}, py[96] = {};
		double planAt = -1e9, goalX = 0, goalY = 0;
	};

	// One route search a frame across every pilot (the FPS pass, 2026-10-27): a pilot that misses its turn keeps its
	// current route and tries again next frame.
	inline double& LastRouteSearch() { static double at = -1e9; return at; }
	inline double& WorstRouteMs() { static double ms = 0; return ms; }

	template <class MapFn>
	bool PlanRoute(MapFn&& map, double x, double y, double groundUnder, double gx, double gy, const Shape& shape, const Limits& lim, Route& r)
	{
		constexpr int N = 61, H = N / 2;
		constexpr double kC = 4.0;
		r.ok = r.reaches = false;
		r.n = 0;
		const double ox = (std::floor(x / kC) + 0.5) * kC, oy = (std::floor(y / kC) + 0.5) * kC;  // the vehicle's cell centre
		auto idx = [](int i, int j) { return j * N + i; };
		auto wx = [&](int i) { return ox + (i - H) * kC; };
		auto wy = [&](int j) { return oy + (j - H) * kC; };
		int ti = H + int(std::lround((gx - ox) / kC)), tj = H + int(std::lround((gy - oy) / kC));
		const bool inside = ti >= 1 && ti < N - 1 && tj >= 1 && tj < N - 1;
		if (!inside) {  // aim at the square's edge on the way to the goal
			const double d = std::hypot(gx - ox, gy - oy), reach = (H - 1) * kC;
			ti = H + int(std::lround((gx - ox) / d * reach / kC));
			tj = H + int(std::lround((gy - oy) / d * reach / kC));
			ti = std::clamp(ti, 1, N - 2); tj = std::clamp(tj, 1, N - 2);
		}
		std::vector<Cell> cell(N * N);
		std::vector<std::uint8_t> got(N * N, 0), closed(N * N, 0);
		std::vector<float> g(N * N, 1e30f);
		std::vector<int> from(N * N, -1);
		auto at = [&](int i, int j) -> const Cell& {
			const int k = idx(i, j);
			if (!got[k]) {
				got[k] = 1;
				cell[k] = map(wx(i), wy(j), groundUnder + shape.originAbove);
				if (i == H && j == H) { cell[k].haveGround = true; cell[k].ground = groundUnder; cell[k].object = false; cell[k].solid = false; cell[k].known = true; }
			}
			return cell[k];
		};
		auto wall = [&](const Cell& c) { return (c.object && !c.objectSoft) || (c.solid && c.haveGround && c.solidZ > c.ground + 1.5); };
		auto heur = [&](int i, int j) { return float(std::hypot(double(i - ti), double(j - tj)) * kC); };
		using Item = std::pair<float, int>;
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
		const int start = idx(H, H), goal = idx(ti, tj);
		g[start] = 0;
		open.push({ heur(H, H), start });
		int bestNode = start;
		float bestH = heur(H, H);
		static constexpr int di[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, dj[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
		while (!open.empty()) {
			const auto [f, k] = open.top();
			open.pop();
			if (closed[k]) continue;
			closed[k] = 1;
			const int i = k % N, j = k / N;
			const float hk = heur(i, j);
			if (hk < bestH) { bestH = hk; bestNode = k; }
			if (k == goal) { bestNode = k; break; }
			const Cell& a = at(i, j);
			for (int n = 0; n < 8; ++n) {
				const int ni = i + di[n], nj = j + dj[n];
				if (ni < 0 || nj < 0 || ni >= N || nj >= N) continue;
				const int nk = idx(ni, nj);
				if (closed[nk]) continue;
				const Cell& b = at(ni, nj);
				if (wall(b)) continue;
				if (n >= 4 && (wall(at(i + di[n], j)) || wall(at(i, j + dj[n])))) continue;  // no cutting a wall's corner
				const double run = n >= 4 ? kC * 1.41421356 : kC;
				if (a.haveGround && b.haveGround) {
					const double rise = b.ground - a.ground;
					if (rise > std::max(lim.maxStep, lim.maxSlope * run)) continue;
					if (-rise > std::max(lim.maxDrop, 0.9 * run)) continue;
				}
				double cost = run * (b.known ? 1.0 : 1.4);
				if (b.haveGround) {  // next to a ledge or a wall: keep a cell away when there is room
					for (int e = 0; e < 4; ++e) {
						const int ei = ni + di[e], ej = nj + dj[e];
						if (ei < 0 || ej < 0 || ei >= N || ej >= N) continue;
						const Cell& c = at(ei, ej);
						if (wall(c)) { cost *= 2.5; break; }
						if (c.haveGround && std::fabs(c.ground - b.ground) > std::max(lim.maxStep, lim.maxSlope * kC)) { cost *= 1.6; break; }
					}
				}
				if (b.solid && !b.haveGround) cost *= 3.0;  // a hit with no ground under it: maybe a wall
				const float ng = g[k] + float(cost);
				if (ng < g[nk]) { g[nk] = ng; from[nk] = k; open.push({ ng + heur(ni, nj), nk }); }
			}
		}
		if (bestNode == start) return false;
		// Walk back, then store from the vehicle outward.
		int chain[N * N / 2];
		int len = 0;
		for (int k = bestNode; k != -1 && len < N * N / 2; k = from[k]) chain[len++] = k;
		const int keep = std::min(len, 96);
		for (int m = 0; m < keep; ++m) {
			const int k = chain[len - 1 - m];
			r.px[m] = wx(k % N);
			r.py[m] = wy(k / N);
		}
		r.n = keep;
		r.ok = keep >= 2;
		r.reaches = inside && bestNode == goal;
		return r.ok;
	}

	struct Other
	{
		std::uint32_t id = 0;
		double x = 0, y = 0, z = 0, vx = 0, vy = 0, radius = 2.5;
		int priority = 0;
	};

	struct Traffic
	{
		double cost = 0;          // added to the candidate's cost
		double free = 1e9;        // m to the first stopped vehicle in the lane
		double cap = 1e9;         // m/s: yield to a moving vehicle with right of way
		std::uint32_t yieldTo = 0;
	};

	// Other vehicles against a candidate heading at `speed`: the closest approach in the next 4 s. A stopped vehicle in
	// the lane is an obstacle (steer round it, never wait behind it); a moving one with right of way is yielded to.
	// A parked vehicle blocks only the lane it stands in, judged with the real hull widths (2026-10-27 15:28 run: rovers
	// parked in a row read every heading as "stopped vehicle" with the old two-radii circle and never pulled out).
	inline Traffic TrafficFor(double x, double y, double z, double heading, double speed, double radius, std::uint32_t self, int priority,
		const Other* others, int count, std::uint32_t ignoreYield, bool drive, double halfWidth = 1.25, double halfLength = 2.3)
	{
		Traffic t;
		const double ux = std::sin(heading), uy = std::cos(heading);
		const double v = std::max(speed, 3.0), svx = ux * v, svy = uy * v;
		for (int i = 0; i < count; ++i) {
			const Other& o = others[i];
			if (o.id == self) continue;
			const double dx = o.x - x, dy = o.y - y;
			const double d = std::hypot(dx, dy);
			if (d > 50.0 || std::fabs(o.z - z) > (drive ? 4.0 : 6.0)) continue;
			const double R = 0.8 * (radius + o.radius) + 1.0;
			const bool stopped = std::hypot(o.vx, o.vy) < 0.6;
			if (stopped) {
				const double along = dx * ux + dy * uy, lat = std::fabs(dx * uy - dy * ux);
				const double otherHalf = 0.8 * o.radius;  // its half extent, from the footprint radius (a box's diagonal)
				if (along > 0 && lat < halfWidth + otherHalf + 0.4) {
					t.free = std::min(t.free, std::max(0.0, along - halfLength - otherHalf - 0.5));
					t.cost += 3.0 * std::clamp(1.0 - along / 50.0, 0.2, 1.0);
				}
				continue;
			}
			const double wx = o.vx - svx, wy = o.vy - svy, w2 = wx * wx + wy * wy;
			const double tc = w2 > 1e-6 ? std::clamp(-(dx * wx + dy * wy) / w2, 0.0, 4.0) : 0.0;
			const double miss = std::hypot(dx + wx * tc, dy + wy * tc);
			if (miss >= R) continue;
			t.cost += 2.0 * (1.0 - tc / 4.0) + 0.5;
			const bool theyGo = o.priority > priority || (o.priority == priority && o.id < self);
			const bool ahead = dx * ux + dy * uy > 0;
			if (theyGo && ahead && tc < 3.0 && o.id != ignoreYield) {
				const double cap = std::sqrt(2.0 * 5.0 * std::max(0.0, d - R));
				if (cap < t.cap) { t.cap = cap; t.yieldTo = o.id; }
			}
		}
		return t;
	}

	struct Input
	{
		double now = 0;                 // s (monotonic)
		double x = 0, y = 0, z = 0, heading = 0, vx = 0, vy = 0;
		double groundUnder = 0;         // world height of the ground under the vehicle
		bool groundOk = false;
		double above = 0;               // m over that ground
		double gx = 0, gy = 0, gz = std::numeric_limits<double>::quiet_NaN(), stop = 3.0;
		bool keepGoing = false, hold = false, cruise = false;
		double cruiseSpeed = 0;
		double targetVx = 0, targetVy = 0;  // a followed target's velocity
		Permit permit = Permit::Both;
		double speedLimit = 0;          // 0: none
		bool waterAhead = false;
		bool hoverPossible = true;      // false under 30 % health (the hover fails)
		double ceiling = 1e9;           // m free above the vehicle's top (the ceiling rays); 1e9: open sky
		// What this moment prefers when both are allowed (permit Both): the air (a fight, fleeing, a long summon) or the
		// ground (traffic, a settlement, the final approach). A preference, not a limit: water, a high goal, a stuck spot
		// still lift a ground preference; dwell times still apply.
		bool preferAir = false;
		const char* preferWhy = "";
		// Drive mode on the game's own wheels (true), or the hover held low (false: an empty vehicle "drives" that way).
		// Only wheels sit on the ground and need room to swing round; the hover turns on the spot.
		bool onWheels = true;
		bool contact = false;           // a side contact this frame
		std::uint32_t self = 0;
		int priority = 60;
		const Other* others = nullptr;
		int otherCount = 0;
		Shape shape;
		Limits limits;
	};

	struct Output
	{
		Mode mode = Mode::Drive;
		Phase phase = Phase::Travel;
		double bearing = 0;     // world heading to steer to
		double speed = 0;       // m/s wanted along it
		bool brake = false;
		bool reverse = false;   // back up (the nose turns toward `bearing` + pi)
		double climb = 0;       // m added to the hover height
		bool modeChanged = false;
		const char* why = "";   // the reason behind mode / phase this frame
		PathInfo path;          // the chosen heading's path
		int usable = 0, blocked = 0;
		std::uint32_t yieldTo = 0;
		unsigned escapeLevel = 0;
		bool stuckEvent = false;
		bool trapped = false;
		const char* blockedWhy = "";  // the commonest reason headings were closed (on the last decision)
		int blockedWhyCount = 0;
		bool routed = false;    // a coarse route set the aim this frame
		bool routeReaches = false;
	};

	class Pilot
	{
	public:
		void Reset() { *this = Pilot{}; }

		template <class MapFn>
		Output Step(const Input& in, MapFn&& map)
		{
			Output out;
			const double dx = in.gx - in.x, dy = in.gy - in.y;
			const double dist = std::hypot(dx, dy);
			const double toGoal = Bearing(dx, dy);
			const double sp = std::hypot(in.vx, in.vy);
			// On the wheels the vehicle stands on the ground: a reading more than 1 m off it (a probe that met a roof, a
			// platform or a cave floor; 2026-10-27 15:15: an enemy rover read the ground 3.2 m over itself and saw a drop on
			// every side) loses to the vehicle's own height.
			const double ownGround = in.z - in.shape.originAbove;
			double groundUnder = in.groundOk ? in.groundUnder : ownGround;
			if (mode_ == Mode::Drive && in.onWheels && std::fabs(groundUnder - ownGround) > 1.0) groundUnder = ownGround;
			const bool goalHigh = std::isfinite(in.gz) && in.gz - groundUnder > 6.0 + in.shape.originAbove;
			if (!started_) Start(in, goalHigh);
			if (std::hypot(in.gx - goalX_, in.gy - goalY_) > 8.0) NewGoal(in);
			// What it ran into itself counts as solid for a minute, whatever the shared map says (an obstacle the rays
			// never saw stops being found again and again).
			const bool insideBox = map(in.x, in.y, in.z).object;
			auto view = [&](double x, double y, double z) {
				Cell c = map(x, y, z);
				if (c.object && insideBox && std::hypot(x - in.x, y - in.y) < 25.0) c.objectSoft = true;
				for (const auto& b : bumps_) {
					if (in.now < b.until && std::hypot(x - b.x, y - b.y) < 2.2) { c.object = true; c.known = true; break; }
				}
				return c;
			};
			Crumb(in);

			// ---- Arrival, with hysteresis (never re-arms on a few centimetres of drift) ----
			const bool follow = in.keepGoing && !in.hold && !in.cruise;
			if (arrived_) { if (dist > in.stop + 3.0) arrived_ = false; }
			else if (dist <= in.stop + 0.75 && !in.cruise) arrived_ = true;

			// ---- Mode: drive unless something calls for the hover; minimum dwell both ways ----
			const bool forced = in.now < forceHoverUntil_;
			bool wantHover = in.permit == Permit::HoverOnly || (in.permit == Permit::Both && (goalHigh || in.waterAhead || forced || climb_ > 0.5 || in.preferAir));
			if (in.permit == Permit::DriveOnly) wantHover = false;
			const char* modeWhy = in.permit == Permit::HoverOnly ? "hover only" : goalHigh ? "goal above the ground" : in.waterAhead ? "water ahead" :
				forced ? "getting out of a stuck spot" : climb_ > 0.5 ? "climbing over" : in.preferAir ? (in.preferWhy && *in.preferWhy ? in.preferWhy : "the air is preferred") :
				(in.preferWhy && *in.preferWhy ? in.preferWhy : "on the ground");
			if (mode_ == Mode::Drive && wantHover && (in.now - modeSince_ >= 2.0 || forced || in.permit == Permit::HoverOnly)) SetMode(in, Mode::Hover, modeWhy, out);
			else if (mode_ == Mode::Hover && !wantHover && in.permit != Permit::HoverOnly && in.now - modeSince_ >= 8.0 && in.groundOk && in.above < 3.5) {
				// Back on the wheels only where the wheels can go: the chosen way, judged for driving, is clear.
				const double reach = std::clamp(std::max(sp, 5.0) * 3.0, 25.0, 50.0);
				const PathInfo p = EvalPath(view, in.x, in.y, in.z, groundUnder, chosen_, std::min(reach, std::max(dist, 8.0)), in.shape, in.limits, Mode::Drive, sp);
				if (p.v == Verdict::Clear || (p.v == Verdict::Unknown && in.above < 3.0)) SetMode(in, Mode::Drive, "the way ahead drives", out);
			}
			// ---- Progress watch: commanded to move, but not moving ----
			const double since = in.now - lastNow_;
			lastNow_ = in.now;
			Watch(in, sp, since > 1.0, out);
			out.mode = mode_;

			const double top = std::min(mode_ == Mode::Drive ? in.limits.driveTop : in.limits.hoverTop, in.speedLimit > 0 ? in.speedLimit : 1e9);
			out.climb = climb_;
			out.escapeLevel = level_;

			if (arrived_ && !follow) {
				out.phase = Phase::Arrived;
				out.bearing = in.heading;
				out.speed = 0;
				out.brake = sp > 0.3;
				out.why = "at the goal";
				commanded_ = 0;
				return out;
			}
			if (pendingReverse_) TryReverse(in, view, groundUnder, pendingAway_);
			if (in.now < waitUntil_) {
				out.phase = Phase::Boxed;
				out.bearing = in.heading;
				out.speed = 0;
				out.brake = true;
				out.why = "no room to move: waiting";
				commanded_ = 0;
				return out;
			}
			if (in.now < turnUntil_) {
				out.phase = Phase::Turn;
				out.bearing = turnBearing_;
				out.speed = 2.0;
				out.why = "turning out";
				commanded_ = 2.0;
				return out;
			}
			if (in.now < reverseUntil_) {
				out.phase = Phase::Reverse;
				out.reverse = true;
				out.bearing = reverseBearing_;
				out.speed = 3.0;
				out.why = "backing out";
				commanded_ = 3.0;
				return out;
			}

			// ---- The coarse route (driving, goal over 16 m away): the fan aims at a point along it, not straight at the goal ----
			double aim = toGoal;
			if (mode_ == Mode::Drive && dist > 16.0) {
				if ((in.now - route_.planAt > 1.5 || std::hypot(in.gx - route_.goalX, in.gy - route_.goalY) > 8.0) && in.now - LastRouteSearch() >= 0.03) {
					LastRouteSearch() = in.now;
					route_.planAt = in.now;
					route_.goalX = in.gx; route_.goalY = in.gy;
					const auto t0 = std::chrono::steady_clock::now();
					PlanRoute(view, in.x, in.y, groundUnder, in.gx, in.gy, in.shape, in.limits, route_);
					const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
					WorstRouteMs() = std::max(WorstRouteMs(), ms);
				}
				if (route_.ok) {
					// The point 14 m along the route from the cell nearest the vehicle.
					int near = 0;
					double nd = 1e18;
					for (int m = 0; m < route_.n; ++m) {
						const double d = std::hypot(route_.px[m] - in.x, route_.py[m] - in.y);
						if (d < nd) { nd = d; near = m; }
					}
					int m = near;
					double walked = 0;
					while (m + 1 < route_.n && walked < 14.0) { walked += std::hypot(route_.px[m + 1] - route_.px[m], route_.py[m + 1] - route_.py[m]); ++m; }
					if (std::hypot(route_.px[m] - in.x, route_.py[m] - in.y) > 2.0) aim = Bearing(route_.px[m] - in.x, route_.py[m] - in.y);
					// Slow ahead of a sharp bend in the route (within 30 m): a corner taken at speed overshoots.
					routeCap_ = 1e9;
					double along = 0;
					for (int k = near; k + 2 < route_.n && along < 30.0; ++k) {
						const double a1 = Bearing(route_.px[k + 1] - route_.px[k], route_.py[k + 1] - route_.py[k]);
						const double a2 = Bearing(route_.px[k + 2] - route_.px[k + 1], route_.py[k + 2] - route_.py[k + 1]);
						along += std::hypot(route_.px[k + 1] - route_.px[k], route_.py[k + 1] - route_.py[k]);
						const double bend = std::fabs(Wrap(a2 - a1)) + std::fabs(Wrap(a1 - in.heading)) * (k == near ? 1.0 : 0.0);
						if (bend > 0.6) {
							const double vb = bend > 1.2 ? 5.0 : 9.0;
							routeCap_ = std::min(routeCap_, std::sqrt(vb * vb + 2.0 * in.limits.brake * 0.4 * std::max(0.0, along - 2.0)));
						}
					}
				}
			}
			else route_.ok = false;

			// ---- Heading: the cheapest usable direction in a fan round the aim (10 Hz, held otherwise) ----
			const double range = std::clamp(std::max(sp, 4.0) * 2.5 + 10.0, 20.0, 60.0);
			const double needFree = std::max(6.0, sp * sp / (2.0 * in.limits.brake) + in.shape.halfLength + 2.0);
			if (in.now >= decideAt_ || !haveChoice_) {
				decideAt_ = in.now + 0.1;
				static constexpr double kOffsets[] = { 0, 15, -15, 30, -30, 45, -45, 60, -60, 80, -80, 100, -100, 125, -125, 150, -150, 180 };
				double bestCost = 1e18, keptCost = 1e18, bestFree = -1, widest = aim;
				PathInfo bestPath, keptPath;
				double best = aim;
				Traffic bestTraffic, keptTraffic;
				usable_ = blocked_ = 0;
				whyN_ = 0;
				creepFree_ = -1; creepH_ = aim; creepOff_ = 1e9; creepWhy_ = "";
				for (const double deg : kOffsets) {
					const double off = deg * (kPi / 180.0);
					const double h = Wrap(aim + off);
					double len = range;
					if (deg == 0 && aim == toGoal) len = std::clamp(dist - in.stop + in.shape.halfLength, 4.0, range);
					PathInfo p = EvalPath(view, in.x, in.y, in.z, groundUnder, h, len, in.shape, in.limits, mode_, sp);
					const Traffic t = TrafficFor(in.x, in.y, in.z, h, std::max(sp, 4.0), Radius(in.shape), in.self, in.priority, in.others, in.otherCount,
						in.now < passUntil_ ? passId_ : 0, mode_ == Mode::Drive,
						in.shape.halfWidth, in.shape.halfLength);
					if (t.free < p.free) { p.free = t.free; p.v = Verdict::Blocked; p.why = "stopped vehicle"; }
					const double free = p.free;
					if (free > bestFree) { bestFree = free; widest = h; }
					const bool usable = p.v != Verdict::Blocked || free >= needFree || (deg == 0 && free >= dist - in.stop - 1.0);
					if (!usable) {
						++blocked_;
						const char* w = p.why && *p.why ? p.why : "?";
						int slot = 0;
						while (slot < whyN_ && std::string_view(whyName_[slot]) != w) ++slot;
						if (slot == whyN_ && whyN_ < 8) { whyName_[whyN_] = w; whyCount_[whyN_] = 0; ++whyN_; }
						if (slot < 8) ++whyCount_[slot];
						if (free > creepFree_ + 0.5 || (free > creepFree_ - 0.5 && std::fabs(off) < std::fabs(creepOff_))) { creepFree_ = free; creepH_ = h; creepOff_ = off; creepWhy_ = w; }
						continue;
					}
					++usable_;
					double cost = (trapped_ ? 0.45 : 1.2) * (1.0 - std::cos(off));
					cost += Visited(in, h, free, trapped_ ? 2.5 : 0.4);
					cost += p.v == Verdict::Unknown ? 0.25 : p.v == Verdict::Caution ? 0.35 : 0.0;
					cost += 0.8 * (1.0 - std::min(1.0, free / len));
					cost += 0.35 * std::fabs(Wrap(h - chosen_)) / kPi;
					cost += t.cost + Bad(h, in.now);
					if (sp > 4.0 && std::fabs(Wrap(h - in.heading)) > 2.2) cost += 0.5;
					if (cost < bestCost) { bestCost = cost; best = h; bestPath = p; bestTraffic = t; }
					if (haveChoice_ && std::fabs(Wrap(h - chosen_)) < 0.14) { keptCost = cost; keptPath = p; keptTraffic = t; }
				}
				boxed_ = usable_ == 0;
				// Everything reads closed but nothing touches the vehicle: the readings are more likely wrong than a vehicle
				// walled in on every side (2026-10-27 run: enemy rovers froze "boxed" with nothing round them). Creep toward
				// the most open heading (2 m/s); a real obstacle then gives contacts, the bump memory and the escapes.
				if (boxed_ && !boxedSince0_) boxedSince0_ = in.now;
				if (!boxed_) boxedSince0_ = 0;
				// Boxed in: go the way with the most freedom. The candidates are every horizontal heading (how far each is open)
				// and up (the room under any ceiling), up only when this vehicle may leave the ground. Up wins when it is more
				// open than any heading; otherwise ease toward the most open heading rather than freeze.
				const bool airAllowed = in.permit != Permit::DriveOnly && in.hoverPossible;
				upWins_ = boxed_ && airAllowed && in.ceiling > std::max(4.0, creepFree_);
				creep_ = boxed_ && !upWins_ && in.now - boxedSince0_ > 1.0 && creepFree_ > -1 &&
					(creepFree_ > in.shape.halfLength + 1.0 || in.now - lastContact_ > 2.0);
				if (creep_) {
					boxed_ = false;
					chosen_ = creepH_;
					haveChoice_ = true;
					chosenPath_ = {};
					chosenPath_.v = Verdict::Caution;
					chosenPath_.free = std::max(creepFree_, in.shape.halfLength + 2.5);
					chosenPath_.why = "creeping: every heading read closed, nothing touching";
					chosenTraffic_ = {};
				}
				else if (boxed_) {
					// Boxed in: hover over it when allowed, else back out toward the widest opening.
					if (!boxedSince_) boxedSince_ = in.now;
					if (upWins_ && in.now - boxedSince_ > 0.8) {
						if (mode_ == Mode::Drive) { forceHoverUntil_ = in.now + 10.0; SetMode(in, Mode::Hover, "boxed in on the ground", out); out.mode = mode_; }
						else if (climb_ < 30.0 && in.now >= climbAt_) { climb_ = std::min(30.0, climb_ + 6.0); climbAt_ = in.now + 1.5; }
					}
					else if (!upWins_ && in.now - boxedSince_ > 1.5) {
						TryReverse(in, view, groundUnder, widest);
						boxedSince_ = 0;
					}
					chosen_ = widest;
					haveChoice_ = true;
					chosenPath_ = {};
					chosenPath_.v = Verdict::Blocked;
					chosenPath_.free = std::max(0.0, bestFree);
					chosenPath_.why = "boxed in";
					chosenTraffic_ = {};
				}
				else {
					boxedSince_ = 0;
					if (keptCost < 1e17 && keptCost <= bestCost + 0.12) { bestCost = keptCost; bestPath = keptPath; bestTraffic = keptTraffic; best = chosen_; }
					chosen_ = best;
					haveChoice_ = true;
					chosenPath_ = bestPath;
					chosenTraffic_ = bestTraffic;
				}
				// Three-point turn: the wheels need room to swing round. A big turn with the nose against something
				// backs out first (when the way behind is open).
				// Hemmed in at the sides (a parking row) with the way round behind it: the nose would swing into a neighbour,
				// so it backs out straight first, the way a driver leaves a parking space.
				if (mode_ == Mode::Drive && in.onWheels && sp < 3.0 && std::fabs(Wrap(chosen_ - in.heading)) > 1.2) {
					const PathInfo ahead = EvalPath(view, in.x, in.y, in.z, groundUnder, in.heading, 8.0, in.shape, in.limits, Mode::Drive, 3.0);
					const Traffic ta = TrafficFor(in.x, in.y, in.z, in.heading, 3.0, Radius(in.shape), in.self, in.priority, in.others, in.otherCount, 0, true, in.shape.halfWidth, in.shape.halfLength);
					if ((ahead.v == Verdict::Blocked && ahead.free < 5.0) || ta.free < 4.0 || (std::fabs(Wrap(chosen_ - in.heading)) > 1.8 && SidesTight(in))) {
						TryReverse(in, view, groundUnder, chosen_);
						if (in.now < reverseUntil_ || in.now < turnUntil_ || in.now < waitUntil_) return Step(in, map);
					}
				}
			}
			out.bearing = chosen_;
			out.path = chosenPath_;
			out.climb = climb_;
			out.usable = usable_;
			{
				int best = -1;
				for (int k = 0; k < whyN_; ++k) if (best < 0 || whyCount_[k] > whyCount_[best]) best = k;
				if (best >= 0) { out.blockedWhy = whyName_[best]; out.blockedWhyCount = whyCount_[best]; }
			}
			out.trapped = trapped_;
			out.routed = route_.ok;
			out.routeReaches = route_.reaches;
			out.blocked = blocked_;
			if (boxed_) out.phase = Phase::Boxed;

			// ---- Speed: what the goal wants, capped by the path, the turn and traffic; creeping, never parked on doubt ----
			double want;
			if (follow) {
				const double along = in.targetVx * std::sin(toGoal) + in.targetVy * std::cos(toGoal);
				want = std::clamp(along + 0.6 * (dist - in.stop), 0.0, top);
			}
			else if (in.cruise) want = in.cruiseSpeed > 0 ? in.cruiseSpeed : top;
			else want = std::max(dist > in.stop + 1.0 ? 1.5 : 0.0, std::sqrt(2.0 * in.limits.brake * 0.4 * std::max(0.0, dist - in.stop - 0.5 * sp)));
			want = std::min(want, top);
			double cap = top;
			const auto& p = chosenPath_;
			if (p.v == Verdict::Blocked || p.v == Verdict::Caution) {
				const double room = std::max(0.0, p.free - in.shape.halfLength - 1.0);
				cap = std::min(cap, std::sqrt(2.0 * in.limits.brake * 0.5 * room));
				if (p.v == Verdict::Caution) cap = std::max(cap, 3.0);
			}
			else if (p.v == Verdict::Unknown) cap = std::min(cap, mode_ == Mode::Drive ? in.limits.unknownDrive : in.limits.unknownHover);
			if (route_.ok && mode_ == Mode::Drive) cap = std::min(cap, std::max(routeCap_, 4.0));
			const double turn = std::fabs(Wrap(chosen_ - in.heading));
			if (turn > 1.6) cap = std::min(cap, mode_ == Mode::Drive ? 4.0 : 3.0);
			else if (turn > 0.3) cap = std::min(cap, top * std::clamp(1.0 - (turn - 0.3) / 1.5, 0.2, 1.0));
			// Yield to a moving vehicle with right of way, at most 3 s, then pass it.
			if (chosenTraffic_.yieldTo && chosenTraffic_.cap < cap) {
				if (chosenTraffic_.yieldTo != yieldId_) { yieldId_ = chosenTraffic_.yieldTo; yieldSince_ = in.now; }
				if (in.now - yieldSince_ > 3.0) { passId_ = yieldId_; passUntil_ = in.now + 5.0; }
				else { cap = chosenTraffic_.cap; out.phase = Phase::Yield; out.yieldTo = yieldId_; }
			}
			else yieldId_ = 0;
			double speed = std::min(want, cap);
			const bool movingOn = !(arrived_ && follow && want < 0.5);
			if (movingOn && out.phase != Phase::Yield && out.phase != Phase::Boxed && speed < 1.5 && dist > in.stop + 1.0) speed = 1.5;
			if (out.phase == Phase::Boxed && mode_ == Mode::Hover) speed = 0.0;  // climb in place, then go on
			if (creep_) speed = std::min(speed, 2.0);
			out.speed = std::max(0.0, speed);
			out.brake = sp > out.speed + 2.0;
			if (out.phase == Phase::Travel) {
				if (mode_ == Mode::Hover && climb_ > 0.5) out.phase = Phase::Climb;
				else if (arrived_) out.phase = Phase::Arrived;
			}
			out.why = out.phase == Phase::Yield ? "giving way" : out.phase == Phase::Boxed ? "boxed in" : p.why && *p.why ? p.why : "open";
			commanded_ = out.speed;

			// Hovering at rising ground: climb ahead of it instead of pressing into it.
			if (mode_ == Mode::Hover && p.v == Verdict::Blocked && p.why && std::string_view(p.why) == "rising ground" && p.free < 25.0 && in.now >= climbAt_ && climb_ < 30.0) {
				climb_ = std::min(30.0, climb_ + 4.0);
				climbAt_ = in.now + 1.0;
			}
			// Climb decays once the way stays clear.
			if (mode_ == Mode::Hover && climb_ > 0 && p.v == Verdict::Clear) {
				if (!clearSince_) clearSince_ = in.now;
				if (in.now - clearSince_ > 3.0) climb_ = std::max(0.0, climb_ - 1.0 * std::clamp(since, 0.0, 0.2));
			}
			else clearSince_ = 0;
			out.climb = climb_;
			return out;
		}

		Mode CurrentMode() const { return mode_; }
		double Chosen() const { return chosen_; }
		unsigned Level() const { return level_; }

	private:
		bool started_ = false, arrived_ = false, haveChoice_ = false, boxed_ = false;
		int usable_ = 0, blocked_ = 0;
		Mode mode_ = Mode::Drive;
		double modeSince_ = 0, goalX_ = 0, goalY_ = 0;
		double chosen_ = 0, decideAt_ = 0, boxedSince_ = 0, climbAt_ = 0;
		PathInfo chosenPath_;
		Traffic chosenTraffic_;
		double forceHoverUntil_ = 0, reverseUntil_ = 0, reverseBearing_ = 0;
		double turnUntil_ = 0, turnBearing_ = 0, waitUntil_ = 0, pendingAway_ = 0;
		bool pendingReverse_ = false;
		bool creep_ = false, upWins_ = false;
		int waits_ = 0;
		double creepFree_ = -1, creepH_ = 0, creepOff_ = 1e9, lastContact_ = -1e9, boxedSince0_ = 0;
		const char* creepWhy_ = "";
		const char* whyName_[8] = {};
		int whyCount_[8] = {}, whyN_ = 0;
		double contactSince_ = 0;
		double climb_ = 0, clearSince_ = 0, lastNow_ = 0;
		// progress watch
		double watchAt_ = 0, watchX_ = 0, watchY_ = 0, commanded_ = 0, lastStuck_ = -1e9, goodSince_ = 0;
		unsigned level_ = 0;
		// avoided headings (where it got stuck or bumped)
		struct Sector { double heading = 0, half = 0, until = 0; } bad_[4];
		int badNext_ = 0;
		// what it ran into (solid for a minute) and where it has been (to stop circling in a pocket)
		struct Mark { double x = 0, y = 0, until = 0; } bumps_[24];
		int bumpNext_ = 0;
		struct Visit { double x = 0, y = 0, at = -1e9; } crumbs_[48];
		int crumbNext_ = 0;
		double crumbX_ = 1e18, crumbY_ = 1e18;
		bool trapped_ = false;
		Route route_;
		double routeCap_ = 1e9;
		double bestDist_ = 1e18, bestAt_ = 0, trappedAt_ = 0;
		// traffic
		std::uint32_t yieldId_ = 0, passId_ = 0;
		double yieldSince_ = 0, passUntil_ = 0;

		static double Radius(const Shape& s) { return std::hypot(s.halfWidth, s.halfLength); }

		void Start(const Input& in, bool goalHigh)
		{
			started_ = true;
			mode_ = in.permit == Permit::HoverOnly || (in.permit == Permit::Both && (goalHigh || in.above > 2.5 || in.preferAir)) ? Mode::Hover : Mode::Drive;
			modeSince_ = in.now;
			chosen_ = Bearing(in.gx - in.x, in.gy - in.y);
			goalX_ = in.gx; goalY_ = in.gy;
			watchAt_ = in.now; watchX_ = in.x; watchY_ = in.y;
			lastNow_ = in.now;
			bestAt_ = in.now;
		}
		void NewGoal(const Input& in)
		{
			goalX_ = in.gx; goalY_ = in.gy;
			arrived_ = false;
			trapped_ = false; bestDist_ = 1e18; bestAt_ = in.now;
			watchAt_ = in.now; watchX_ = in.x; watchY_ = in.y;
		}
		void SetMode(const Input& in, Mode m, const char* why, Output& out)
		{
			if (m == mode_) return;
			mode_ = m;
			modeSince_ = in.now;
			out.modeChanged = true;
			out.why = why;
			decideAt_ = 0;  // re-judge every heading for the new mode at once
			// A wheel manoeuvre is invalid after takeoff: the hover can turn in
			// place and must choose its escape using the airspace at its new height.
			if (m == Mode::Hover) {
				reverseUntil_ = turnUntil_ = waitUntil_ = 0;
				pendingReverse_ = false;
				contactSince_ = 0;
				watchAt_ = in.now; watchX_ = in.x; watchY_ = in.y;
			}
			if (m == Mode::Drive) climb_ = 0;
		}
		double Bad(double h, double now) const
		{
			double c = 0;
			for (const auto& b : bad_) if (now < b.until && std::fabs(Wrap(h - b.heading)) < b.half) c += 1.5;
			return c;
		}
		void AddBad(double heading, double half, double until)
		{
			bad_[badNext_] = { heading, half, until };
			badNext_ = (badNext_ + 1) % 4;
		}
		void AddBump(const Input& in, double heading)
		{
			const double reach = in.shape.halfLength + 1.2;
			const double fx = std::sin(heading), fy = std::cos(heading), rx = fy, ry = -fx;
			for (const double side : { 0.0, in.shape.halfWidth + 0.5, -(in.shape.halfWidth + 0.5) }) {
				const double bx = in.x + fx * reach + rx * side, by = in.y + fy * reach + ry * side;
				bool have = false;
				for (auto& b : bumps_) if (in.now < b.until && std::hypot(bx - b.x, by - b.y) < 1.5) { b.until = in.now + 60.0; have = true; break; }
				if (have) continue;
				bumps_[bumpNext_] = { bx, by, in.now + 60.0 };
				bumpNext_ = (bumpNext_ + 1) % 24;
			}
		}
		void Crumb(const Input& in)
		{
			if (std::hypot(in.x - crumbX_, in.y - crumbY_) < 3.0) return;
			crumbX_ = in.x; crumbY_ = in.y;
			crumbs_[crumbNext_] = { in.x, in.y, in.now };
			crumbNext_ = (crumbNext_ + 1) % 48;
		}
		// Recently visited ground at the end of a candidate's open run costs extra (more when trapped).
		double Visited(const Input& in, double h, double free, double weight) const
		{
			const double reach = std::clamp(free, 4.0, 16.0);
			const double ex = in.x + std::sin(h) * reach, ey = in.y + std::cos(h) * reach;
			int n = 0;
			for (const auto& c : crumbs_) if (in.now - c.at < 60.0 && std::hypot(ex - c.x, ey - c.y) < 7.0) ++n;
			return weight * std::min(n, 4) / 4.0;
		}
		// No gain toward the goal for 10 s while trying: trapped (a pocket, a dead end). The goal pulls less and fresh
		// ground pulls more until it gains 6 m on its best, or 40 s pass.
		void Trap(const Input& in)
		{
			const double dist = std::hypot(in.gx - in.x, in.gy - in.y);
			if (dist < bestDist_ - 3.0) {
				if (trapped_ && dist < bestDist_ - 6.0) trapped_ = false;
				bestDist_ = dist; bestAt_ = in.now;
				return;
			}
			if (trapped_ && in.now - trappedAt_ > 40.0) { trapped_ = false; bestDist_ = dist; bestAt_ = in.now; }
			if (!trapped_ && in.now - bestAt_ > 10.0) { trapped_ = true; trappedAt_ = in.now; decideAt_ = 0; }
		}
		void RequestReverse(double away) { pendingReverse_ = true; pendingAway_ = away; }
		// Something solid right beside the hull (a stopped vehicle within 1.5 m of its side, alongside it).
		bool SidesTight(const Input& in) const
		{
			const double ux = std::sin(in.heading), uy = std::cos(in.heading);
			for (int i = 0; i < in.otherCount; ++i) {
				const Other& o = in.others[i];
				if (o.id == in.self || std::hypot(o.vx, o.vy) > 0.6) continue;
				const double dx = o.x - in.x, dy = o.y - in.y;
				const double along = dx * ux + dy * uy, lat = std::fabs(dx * uy - dy * ux);
				const double half = 0.8 * o.radius;
				if (std::fabs(along) < in.shape.halfLength + half && lat < in.shape.halfWidth + half + 1.5) return true;
			}
			return false;
		}
		// Back out only when the way behind is open (map and vehicles); otherwise turn out forward toward the most open
		// heading at a crawl; with no room either way, hold a moment (a vehicle in the way may move) and try again.
		template <class ViewFn>
		void TryReverse(const Input& in, ViewFn&& view, double groundUnder, double away)
		{
			pendingReverse_ = false;
			const double rear = Wrap(in.heading + kPi);
			const PathInfo back = EvalPath(view, in.x, in.y, in.z, groundUnder, rear, 6.0, in.shape, in.limits, mode_, 3.0);
			const Traffic tb = TrafficFor(in.x, in.y, in.z, rear, 3.0, Radius(in.shape), in.self, in.priority, in.others, in.otherCount, 0, mode_ == Mode::Drive, in.shape.halfWidth, in.shape.halfLength);
			const double rearFree = std::min(back.v == Verdict::Blocked ? back.free : 6.0, tb.free);
			const bool rearOpen = rearFree >= 3.5 && tb.cost < 1.5;
			double bestFree = -1, bestH = in.heading;
			for (const double deg : { 30.0, -30.0, 60.0, -60.0, 90.0, -90.0, 120.0, -120.0, 150.0, -150.0, 0.0, 180.0 }) {
				const double h = Wrap(in.heading + deg * (kPi / 180.0));
				PathInfo p = EvalPath(view, in.x, in.y, in.z, groundUnder, h, 12.0, in.shape, in.limits, mode_, 3.0);
				const Traffic t = TrafficFor(in.x, in.y, in.z, h, 3.0, Radius(in.shape), in.self, in.priority, in.others, in.otherCount, 0, mode_ == Mode::Drive, in.shape.halfWidth, in.shape.halfLength);
				const double free = std::min(p.v == Verdict::Blocked ? p.free : 12.0, t.free) - 0.2 * std::fabs(deg) / 30.0;
				if (free > bestFree) { bestFree = free; bestH = h; }
			}
			if (mode_ == Mode::Hover || !in.onWheels) {
				// A hover turns to an open heading. Negative thrust while turning
				// toward the escape would instead carry it away from that escape.
				if (bestFree >= 3.0 || in.now - lastContact_ > 1.0 || waits_ >= 2) {
					turnUntil_ = in.now + 2.5; turnBearing_ = bestH; waits_ = 0;
				}
				else { waitUntil_ = in.now + 1.5; ++waits_; }
				return;
			}
			// The way with the most room wins: behind, or forward/sideways. Only a vehicle actually pressed against something
			// pauses, and never more than twice running (2026-10-27 run: an enemy rover sat in "no room: waiting" for good).
			// Nose pressed against something: a forward turn cannot swing round it (the wheels need room), so any room
			// behind wins.
			const bool pressed = in.now - lastContact_ < 0.6;
			if (rearOpen || (rearFree > 1.0 && rearFree >= bestFree) || (pressed && rearFree > 1.2)) {
				const bool tight = in.onWheels && SidesTight(in);
				reverseUntil_ = in.now + (tight ? 3.0 : 2.0);
				// Backing up turns the nose toward `away`; hemmed in at the sides it backs out straight (nose kept).
				reverseBearing_ = tight ? in.heading : away;
				waits_ = 0;
				return;
			}
			if (bestFree >= 3.0 || in.now - lastContact_ > 1.0 || waits_ >= 2) { turnUntil_ = in.now + 2.5; turnBearing_ = bestH; waits_ = 0; }
			else { waitUntil_ = in.now + 1.5; ++waits_; }
		}
		void Watch(const Input& in, double sp, bool pause, Output& out)
		{
			// Turning out is a forward try: the stuck watch still runs (2026-10-27 run: a rover "turned out" at 0 m/s for 40 s).
			const bool reversing = in.now < reverseUntil_ || in.now < waitUntil_;
			if (in.contact) lastContact_ = in.now;
			if (in.contact && !reversing) {
				AddBump(in, in.heading);
				if (sp > 1.0) AddBad(in.heading, 0.45, in.now + 6.0);
				if (!contactSince_) contactSince_ = in.now;
				if (in.now - contactSince_ > 0.6 && commanded_ > 0.5) {
					// Pressing into something: back off now rather than after the stuck watch's 3.5 s.
					contactSince_ = 0;
					RequestReverse(chosen_);
					AddBad(in.heading, 0.6, in.now + 10.0);
					decideAt_ = 0;
				}
			}
			else if (!in.contact) contactSince_ = 0;
			const bool trying = commanded_ >= 1.2 && !reversing && !(arrived_ && !(in.keepGoing && !in.hold));
			if (!trying || pause) {  // a pause (menu, load) restarts the window
				watchAt_ = in.now; watchX_ = in.x; watchY_ = in.y;
				return;
			}
			Trap(in);
			const double moved = std::hypot(in.x - watchX_, in.y - watchY_);
			if (moved > 4.0) {
				watchAt_ = in.now; watchX_ = in.x; watchY_ = in.y;
				if (!goodSince_) goodSince_ = in.now;
				if (level_ && in.now - goodSince_ > 12.0 && in.now - lastStuck_ > 12.0) level_ = 0;
				return;
			}
			if (in.now - watchAt_ < 3.5) return;
			// Stuck: under 4 m in 3.5 s while asking to move.
			goodSince_ = 0;
			watchAt_ = in.now; watchX_ = in.x; watchY_ = in.y;
			if (in.now - lastStuck_ > 25.0) level_ = 0;
			lastStuck_ = in.now;
			++level_;
			out.stuckEvent = true;
			const double h = in.heading;
			AddBump(in, commanded_ < 0 ? h + kPi : h);
			const double toGoal = Bearing(in.gx - in.x, in.gy - in.y);
			// The other side from the last attempt; the side away from the blocked heading first.
			const double side = Wrap(chosen_ - h) >= 0.05 ? 1.0 : Wrap(chosen_ - h) <= -0.05 ? -1.0 : (Wrap(toGoal - h) >= 0 ? 1.0 : -1.0);
			AddBad(h, level_ >= 2 ? 0.85 : 0.6, in.now + 12.0);
			switch (std::min(level_, 4u)) {
			case 1:
				RequestReverse(Wrap(h + side * 1.0));
				break;
			case 2:
				if (in.permit != Permit::DriveOnly && in.hoverPossible && in.ceiling > 4.0 && mode_ == Mode::Drive) { forceHoverUntil_ = in.now + 10.0; SetMode(in, Mode::Hover, "stuck on the ground; open above", out); }
				else if (mode_ == Mode::Hover) climb_ = std::min(30.0, climb_ + 6.0);
				else RequestReverse(Wrap(h + side * 1.4));
				break;
			case 3:
				if (mode_ == Mode::Hover) { climb_ = std::min(30.0, climb_ + 10.0); forceHoverUntil_ = in.now + 10.0; }
				else RequestReverse(Wrap(h - side * 1.4));
				break;
			default:
				// Keep cycling: back out and try the far side; the hover keeps rising (capped).
				RequestReverse(Wrap(h + side * 2.0));
				if (mode_ == Mode::Hover) climb_ = std::min(30.0, climb_ + 6.0);
				level_ = 1;
				break;
			}
			decideAt_ = 0;
		}
	};
}
