#pragma once
// Redesign (2026-11-12): the World the energy planner reads, as plain data. Solid points (each a small box with a base and a top), the
// floor, the field boxes and the route's lane centre. One physical rule decides what a point is to the car: inside the body's height
// band (above what a wheel rolls over, below the roof) it is solid; at or under the climb height it is an edge the wheels roll over.
// No per-hit labels. Used by the game adapter, the offline simulator (tools/plan_sim.cpp) and tests. Pure.
#include "bicycle.h"
#include "fieldbox.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

namespace planscene {
// soft (ED-1, 2026-11-12): a PREDICTION (survey, game box), not a measurement. It never blocks (Clearance and LowEdges skip it); the planner
// pays for passing near or through it (SoftNear), so a prediction shapes the path but cannot stop the car, and a measured solid is never dropped.
// waits (D1, removal step 1, 2026-11-13): an occupied road user in traffic standing (World's steerRound false: a car with a driver that
// stopped less than stoppedWait ago, a person). The car waits behind it; it never lifts the lane edge to go round.
// band (2026-11-19): a KNOWN no-go wall (route edge band or median, BuildKnownBandSolids), not a sensed one. It is a wall to every rule that
// judges a solid (cost to go, corridor margin, rollout refusal); only the trace reason tells them apart ("enters_a_known_band").
struct Solid { double x = 0, y = 0, base = 0, top = 0, half = .2; bool soft = false, waits = false, band = false;
    std::uint32_t floorProbeBody = 0;
    double road = std::numeric_limits<double>::quiet_NaN();  // RF-1: World's fitted road under this solid, else (Index) the lane floor; NaN: not known
    double roadSpread = 0;                                     // RF-1: the road's own pattern about that surface (m); within it: road, not an edge
#ifndef FVO_RELEASE
    const char* sensor = "unattributed";
    int source = -1, layer = -1, support = 0;
    std::uint32_t owner = 0, body = 0;
    double seenAt = 0, normalZ = 0;
    bool haveNormal = false;
#endif
};
struct RoutePoint { double x = 0, y = 0, station = 0, z = 0; };  // z (step 3): the line's height (a flight line holds it)
// D4 (one-authority design step 1): a moving road user (vehicle or person) as World predicts it: positions at kMoverSamples times
// kMoverStep apart from now (its published plan where it has one, else its velocity), its footprint and height.
constexpr int kMoverSamples = 9; constexpr double kMoverStep = .75;  // 0 .. 6 s
struct Mover {
    double x[kMoverSamples] = {}, y[kMoverSamples] = {}, heading = 0, halfWidth = 1, halfLength = 2.3, base = 0, top = 2;
    double margin = .3;            // World's uncertainty about where it will be (grows with time in planworld; one value here)
    bool person = false;
    void At(double t, double& px, double& py) const {
        const double u = std::clamp(t / kMoverStep, 0.0, double(kMoverSamples - 1)); const int i = std::min(int(u), kMoverSamples - 2); const double f = u - i;
        px = x[i] + (x[i + 1] - x[i]) * f; py = y[i] + (y[i + 1] - y[i]) * f;
    }
};

struct Scene {
    std::vector<Solid> solids;
    std::vector<fieldbox::Box> field;
    std::vector<RoutePoint> route;     // the lane centre, in order
    double laneHalf = 3.5;
    // Known no-go (2026-11-19, route edge bands and medians): the lateral intervals (m from the route line, + = right of travel) where a body
    // may stand, sorted and disjoint. A gap between two is a median; an infinite end is an edge with no band (nothing known beyond it).
    // Known from the route's own data, never sensed. Empty: none known (older roads / addons), and nothing below applies.
    std::vector<std::pair<double, double>> legal;
    int floorSetAside = 0;  // GR-2: lane floor samples set aside as outliers when this scene was built (traced)
    std::function<bool(double, double, double&)> floor;  // floor height at (x, y); false: nothing known
    std::function<bool(double, double, double&)> gradeFloor;
    bool GradeFloor(double x, double y, double& z) const {
        return gradeFloor ? gradeFloor(x, y, z) : Floor(x, y, z);
    }
    double reach = 1.5;                // clearance is exact up to this far; further is reported as this (step 2: the energy looks 1 m out at most)
    // D5 (one-authority design step 1): where the car must stop along the route (a route hold, a stop line, the line's end), as a route
    // station in this scene; +inf: nowhere. The Plan treats it as a wall across the road for forward motion: the stopping energy holds
    // the front of the car short of it.
    double stopStation = std::numeric_limits<double>::infinity();
    std::vector<Mover> movers;         // D4
    // D6/D7 (one-authority step 3): rail or flight, and facing.
    int travel = 1;                    // the stretch: 1 drive only (hover only where wheels cannot go, at a high cost), 2 flight only, 0 both
    bool hoverPossible = false;        // the car can hover now (a damaged car cannot)
    bool flightLine = false;           // the route is a 3D line: hovering holds the line's height there, not hoverClear over the floor
    double facing = std::numeric_limits<double>::quiet_NaN();  // where the nose should point (a combat line, an arrive heading); NaN: none
    double facingScale = 1.0;          // step 4 (D13): how much the facing counts now (an arrive heading grows to full over the last 15 m)
    // D13 (one-authority step 4): a car with no lane (summon, follow, combat, a job) drives to a goal point. There is no lane, so no
    // lateral cost (`lanes` false); the route is the way to the goal (the goal's own polyline where one is known, else straight); the
    // cost-to-go field's goal is the goal region (final goal within reach) or everything past the sub-goal mark (far goal).
    struct Goal { bool have = false; double x = 0, y = 0, radius = 2.0, arriveYaw = std::numeric_limits<double>::quiet_NaN(); bool stop = true; };
    Goal goal;
    bool continuousRecovery = false, managerHeld = false;
    struct CheckedSpan { double x0, y0, z0, x1, y1, z1, at; };
    std::vector<CheckedSpan> checkedPath;
    bool lanes = true;                 // false: no lane to keep (goal driving): the energy pays no lateral term
    bool goalFinal = false;            // the field's goal is the goal region itself (the goal is within the sub-goal reach)
    double goalStation = std::numeric_limits<double>::infinity();  // the station of the field's goal (the goal, or the sub-goal mark)
    static constexpr double kSubGoal = 30.0;  // m: a farther goal is approached through the point this far along the way
    static constexpr double kArriveFacing = 15.0;  // m: the arrive heading counts in full from this close
    // Make this scene a goal scene for a car at (carX, carY). `way` (optional): the goal's polyline (navmesh or ground plan), car to goal.
    // Returns the station the cost-to-go field should be built to (goalStation).
    double SetGoal(double carX, double carY, const Goal& g, double halfLength, double stopGap, const std::vector<RoutePoint>* way = nullptr) {
        goal = g; goal.have = true; lanes = false; route.clear();
        route.push_back({ carX, carY });
        if (way) for (const auto& p : *way) if (std::hypot(p.x - route.back().x, p.y - route.back().y) > .05) route.push_back(p);
        if (std::hypot(g.x - route.back().x, g.y - route.back().y) > .05) route.push_back({ g.x, g.y });
        if (route.size() < 2) route.push_back({ g.x + 1e-3, g.y });  // already at the goal: a stub line
        route[0].station = 0;
        for (int i = 1; i < int(route.size()); ++i) route[i].station = route[i - 1].station + std::hypot(route[i].x - route[i - 1].x, route[i].y - route[i - 1].y);
        const double total = route.back().station;
        goalFinal = total <= kSubGoal + g.radius;
        goalStation = goalFinal ? total : kSubGoal;
        // The arrival stop (D5's wall at the goal: the car's centre may reach the goal point) only once the goal is within reach.
        stopStation = goalFinal && g.stop ? total + halfLength + stopGap : std::numeric_limits<double>::infinity();
        facing = g.arriveYaw;
        facingScale = std::isfinite(g.arriveYaw) ? std::clamp(1.0 - (total - g.radius) / kArriveFacing, 0.0, 1.0) : 1.0;
        return goalStation;
    }
    // The height a hovering car holds at (x, y): the line's own height on a flight line, else hoverClear over the floor.
    double HoverTarget(double x, double y, double floorZ, bool haveFloor, const bicycle::Spec& k, int seg) const {
        if (flightLine && route.size() >= 2) {
            const int i = std::clamp(seg, 0, int(route.size()) - 2); const auto& a = route[i]; const auto& b = route[i + 1];
            const double dx = b.x - a.x, dy = b.y - a.y, l2 = std::max(1e-9, dx * dx + dy * dy);
            const double t = std::clamp(((x - a.x) * dx + (y - a.y) * dy) / l2, 0.0, 1.0);
            return a.z + (b.z - a.z) * t;
        }
        return (haveFloor ? floorZ : 0.0) + k.hoverClear;
    }

    // D4: signed distance from the body (x, y, heading) at time t to the nearest moving user in the body's height band (reach if none),
    // and whether avoiding it is ours (closing): everything except a same-direction user coming up from behind our travel.
    // Step 6 (found by shadowcompare_test's goal scene): does mover i's predicted path reach the body standing at (x, y, heading) within the
    // horizon? A follower is only excused when it does: "standing still would not avoid it" is then true. A car crossing the place we drive
    // into, which met the behind-and-same-direction test only because we had turned toward its direction, never reaches where we stand.
    bool MoverReaches(int i, double x, double y, double heading, double floorZ, const bicycle::Spec& k, bool tilt = true) const {
        if (i < 0 || i >= int(movers.size())) return false;
        const auto& m = movers[i];
        const double ref = MoverRef(m.x[0], m.y[0], x, y, floorZ, tilt);  // RP-1: the road where that user is
        if (m.top <= ref + k.climb || m.base >= ref + k.height) return false;
        const double c = std::cos(heading), n = std::sin(heading), turn = m.heading - heading, ct = std::fabs(std::cos(turn)), st = std::fabs(std::sin(turn));
        const double mAcross = m.halfWidth * ct + m.halfLength * st, mAlong = m.halfLength * ct + m.halfWidth * st;
        for (int s = 0; s < kMoverSamples; ++s) {
            const double dx = m.x[s] - x, dy = m.y[s] - y, across = dx * c - dy * n, along = dx * n + dy * c;
            if (std::fabs(across) - k.halfWidth - mAcross - m.margin < 0 && std::fabs(along) - k.halfLength - mAlong - m.margin < 0) return true;
        }
        return false;
    }
    double MoverClearance(double x, double y, double heading, double floorZ, const bicycle::Spec& k, double t, double motionX, double motionY, bool& closing, int* which = nullptr, bool tilt = true) const {
        double best = reach; closing = false;
        for (int mi = 0; mi < int(movers.size()); ++mi) { const auto& m = movers[mi];
            double mx, my; m.At(t, mx, my);
            const double ref = MoverRef(mx, my, x, y, floorZ, tilt);  // RP-1: the road where that user will be
            if (m.top <= ref + k.climb || m.base >= ref + k.height) continue;
            // Both are boxes: the body against the user's box, grown by its uncertainty (corner-to-box distance, conservative).
            const double dx = mx - x, dy = my - y, c = std::cos(heading), n = std::sin(heading);
            const double across = dx * c - dy * n, along = dx * n + dy * c;
            const double turn = m.heading - heading, ct = std::fabs(std::cos(turn)), st = std::fabs(std::sin(turn));
            const double mAcross = m.halfWidth * ct + m.halfLength * st, mAlong = m.halfLength * ct + m.halfWidth * st;
            const double ox = std::fabs(across) - k.halfWidth - mAcross - m.margin, oy = std::fabs(along) - k.halfLength - mAlong - m.margin;
            const double d = ox > 0 || oy > 0 ? std::hypot(std::max(ox, 0.0), std::max(oy, 0.0)) : std::max(ox, oy);
            if (d < best) {
                best = d;
                // Ours unless it is a same-direction user coming up from BEHIND our direction of travel (a follower): standing still would not
                // avoid that one. A user crossing into our side, or one ahead, is ours to avoid (energyplan_test "D4 crossing", first rule
                // excused side impacts and the car drove into the crossing car).
                const double mv = std::hypot(motionX, motionY);
                bool excused = false;
                if (mv > .1) {
                    const double ux = motionX / mv, uy = motionY / mv, vx = (m.x[1] - m.x[0]) / kMoverStep, vy = (m.y[1] - m.y[0]) / kMoverStep, vm = std::hypot(vx, vy);
                    excused = dx * ux + dy * uy < 0 && vm > .1 && (vx * ux + vy * uy) / vm > .7;
                }
                closing = !excused; if (which) *which = mi;
            }
        }
        return best;
    }

    // Known no-go as walls (2026-11-19, user: the planner sees edge bands and dividers as it sees a wall it must not hit). From `legal` (lateral
    // intervals where a body may stand) the complement that is KNOWN (beyond a finite outer edge, and every gap = median) becomes rows of box
    // solids along the route line, face on the edge, so Clearance / Contact / the cost-to-go field / the corridor treat them as any measured wall.
    // Called once per scene before Index() (route shape and floor known). World-fixed: a later line shift does not move them. No legal: none.
    // The band is the band (2026-11-20, 08:00-08:03 S-bend stall of lc_ring_road): rows are laid by offsetting each segment, which on the INSIDE
    // of a corner overshoots (a spur of the row across the road: 0.2-0.3 m at 19 deg, 3 m out) and, where the offset is beyond the radius of a
    // tight arc, folds across the bend; on the OUTSIDE it leaves a wedge hole. A band solid is kept only where the line is no closer than the
    // offset it stands for (its distance to the route polyline equals its lateral, the offset curve's own definition), and the outside of a
    // corner is filled with the arc round the vertex. A straight line builds exactly the rows it always built.
    static constexpr double kBandThickness = 1.0, kBandBox = .25, kBandSpacing = .5, kBandHeight = 2.5, kBandSpur = .05; static constexpr std::size_t kBandMax = 6000;
    int BuildKnownBandSolids() {
        if (legal.empty() || route.size() < 2) return 0;
        std::vector<std::pair<double, double>> nogo;  // lateral intervals, finite
        if (std::isfinite(legal.front().first)) nogo.push_back({ legal.front().first - kBandThickness, legal.front().first });
        for (std::size_t i = 0; i + 1 < legal.size(); ++i) if (std::isfinite(legal[i].second) && std::isfinite(legal[i + 1].first) && legal[i + 1].first > legal[i].second) nogo.push_back({ legal[i].second, legal[i + 1].first });
        if (std::isfinite(legal.back().second)) nogo.push_back({ legal.back().second, legal.back().second + kBandThickness });
        int added = 0, spurs = 0;
        // Distance from (x, y) to the route polyline, only over the segments within `reach` of arc length round segment `seg`.
        auto lineDistance = [&](double x, double y, std::size_t seg, double reach) {
            double best = 1e18;
            auto test = [&](std::size_t j) {
                const double dx = route[j + 1].x - route[j].x, dy = route[j + 1].y - route[j].y, l2 = dx * dx + dy * dy; if (l2 < 1e-12) return;
                const double t = std::clamp(((x - route[j].x) * dx + (y - route[j].y) * dy) / l2, 0.0, 1.0);
                best = std::min(best, std::hypot(x - route[j].x - t * dx, y - route[j].y - t * dy));
            };
            test(seg);
            double run = 0; for (std::size_t j = seg; j > 0 && run <= reach; --j) { test(j - 1); run += std::hypot(route[j].x - route[j - 1].x, route[j].y - route[j - 1].y); }
            run = 0; for (std::size_t j = seg + 1; j + 1 < route.size() && run <= reach; ++j) { test(j); run += std::hypot(route[j + 1].x - route[j].x, route[j + 1].y - route[j].y); }
            return best;
        };
        auto put = [&](std::size_t seg, double px, double py, double lat, double z) {
            if (solids.size() >= kBandMax) return;
            if (lineDistance(px, py, seg, 2 * std::fabs(lat) + 2) < std::fabs(lat) - kBandSpur) { ++spurs; return; }  // across the road: a spur or a fold
            double fz = 0; if (Floor(px, py, fz)) z = fz;
            Solid s; s.x = px; s.y = py; s.base = z; s.top = z + kBandHeight; s.half = kBandBox; s.band = true; s.road = z;
            solids.push_back(s); ++added;
        };
        auto columns = [&](const std::pair<double, double>& r, auto&& each) {
            const double w = r.second - r.first; const int cols = std::max(1, int(std::ceil(w / (2 * kBandBox) - 1e-9)));
            for (int c = 0; c < cols; ++c) each(r.first + (c + .5) * w / cols);
        };
        double carry = 0;
        for (std::size_t i = 0; i + 1 < route.size() && solids.size() < kBandMax; ++i) {
            const double dx = route[i + 1].x - route[i].x, dy = route[i + 1].y - route[i].y, len = std::hypot(dx, dy);
            if (len < 1e-6) continue;
            const double nx = dy / len, ny = -dx / len;  // right of travel
            double d = carry;
            for (; d < len; d += kBandSpacing) {
                const double cx = route[i].x + dx * d / len, cy = route[i].y + dy * d / len; const double z = route[i].z + (route[i + 1].z - route[i].z) * d / len;
                for (const auto& r : nogo) columns(r, [&](double lat) { put(i, cx + nx * lat, cy + ny * lat, lat, z); });
            }
            carry = d - len;  // where the next sample falls in the next segment
            // the outside of the corner at the end of this segment: the arc round the vertex joins the rows of this segment and the next
            if (i + 2 < route.size()) {
                const double ex = route[i + 2].x - route[i + 1].x, ey = route[i + 2].y - route[i + 1].y, el = std::hypot(ex, ey);
                if (el < 1e-6) continue;
                const double mx = ey / el, my = -ex / el, turn = std::atan2(nx * my - ny * mx, nx * mx + ny * my);  // the angle from normal 1 to normal 2 (counter-clockwise +)
                const double cross = dx * ey - dy * ex;  // + = the line turns left, so its right side is the outside
                if (std::fabs(turn) > 1e-3) for (const auto& r : nogo) columns(r, [&](double lat) {
                    if (lat * cross <= 0) return;  // the inside of this corner: the distance test already cut its overshoot
                    const double arc = std::fabs(lat) * std::fabs(turn); const int steps = int(std::ceil(arc / kBandSpacing)); if (steps < 2) return;
                    for (int q = 1; q < steps; ++q) {
                        const double a = turn * double(q) / steps, c = std::cos(a), s = std::sin(a);  // rotate normal 1 toward normal 2
                        const double rx = nx * c - ny * s, ry = nx * s + ny * c;
                        put(i, route[i + 1].x + rx * lat, route[i + 1].y + ry * lat, lat, route[i + 1].z);
                    }
                });
            }
        }
        bandSpurs = spurs;
        return added;
    }
    int bandSpurs = 0;  // band rows left out because they would have stood across the road (a spur or a fold), last build; traced
    // A 2 m grid over the solids, built once per update (Index) so Clearance looks only nearby.
    double cell = 2.0; std::unordered_map<std::uint64_t, std::vector<int>> grid;
    static std::uint64_t Key(int gx, int gy) { return (std::uint64_t(std::uint32_t(gx)) << 32) | std::uint32_t(gy); }
    double maxHalf = 0;                         // step 2: the largest solid half size (bounds every search radius)
    std::vector<fieldbox::Axes> fieldAxes;      // step 2: each field box's axes, computed once (fieldbox::Local recomputed them per call)
    void Index() {
        grid.clear(); maxHalf = 0;
        for (auto& s : solids) { double z = 0; if (!std::isfinite(s.road) && Floor(s.x, s.y, z)) s.road = z; }  // RP-1: lane floor where World had no fit
        for (int i = 0; i < int(solids.size()); ++i) { grid[Key(int(std::floor(solids[i].x / cell)), int(std::floor(solids[i].y / cell)))].push_back(i); maxHalf = std::max(maxHalf, solids[i].half); }
        if (!goal.have) for (int i = 1; i < int(route.size()); ++i) route[i].station = route[i - 1].station + std::hypot(route[i].x - route[i - 1].x, route[i].y - route[i - 1].y);
        fieldAxes.clear(); for (const auto& b : field) fieldAxes.push_back(fieldbox::AxesOf(b));
        BuildDistance();
        BuildRoundFree();
    }
    // D1 in the energy plan (removal step 1, 2026-11-13; VEHICLE_DECISION_DESIGN D1: "parked, wrecked, or not moving for 20 s: branches may
    // go round it"). The old branch planner carried this rule; the energy plan priced leaving the lane as if the lane were open, so standing
    // in front of a fallen tree cost less than going round it (19:55 / 20:14 runs). Where a measured solid that does not wait (a static
    // object, a parked or still car) stands inside the car's own lane, the lane edge is not charged from roundApproach before it to
    // roundRejoin after it, in the rollouts and in the cost-to-go field. A road user that waits (occupied, in traffic) changes nothing.
    static constexpr double roundApproach = 20.0, roundRejoin = 10.0, roundStep = .35;  // m; roundStep: lower than this over the floor is driven over
    std::vector<std::pair<double, double>> roundFree;  // station intervals, merged
    void BuildRoundFree() {
        roundFree.clear();
        if (goal.have || !lanes || route.size() < 2) return;
        for (const auto& s : solids) {
            if (s.soft || s.waits || s.band) continue;  // a known band is the road's edge, never an obstacle to go round
            double fz = s.base; if (floor) floor(s.x, s.y, fz);
            if (s.top <= fz + roundStep) continue;
            double st, lat, half; Route(s.x, s.y, st, lat, half);
            if (std::fabs(lat) - s.half >= half) continue;  // beside the lane, not in it
            if (st < route.front().station - 1 || st > route.back().station + 1) continue;
            roundFree.push_back({ st - roundApproach, st + roundRejoin });
        }
        std::sort(roundFree.begin(), roundFree.end());
        int w = 0;
        for (int i = 0; i < int(roundFree.size()); ++i) {
            if (w > 0 && roundFree[i].first <= roundFree[w - 1].second) roundFree[w - 1].second = std::max(roundFree[w - 1].second, roundFree[i].second);
            else roundFree[w++] = roundFree[i];
        }
        roundFree.resize(w);
    }
    // How far a body at lateral offset `lateral` (heading `relYaw` to the route's) is into known no-go space, in metres; 0 = clear.
    // The interval holding the body's centre (else the nearest) is its road. A finite end of that interval is a hard edge; the body's
    // reach is half its width turned to the road plus half its length across it. With a finite end on both sides the body may use all
    // the room there is: a road narrower than the body is judged from its middle, so a car wider than the lane is never "in" a band
    // just by standing centred. The centre outside every interval counts the whole body as out, plus the way back to the nearest one.
    double NoGoDepth(double lateral, double relYaw, const bicycle::Spec& k) const {
        if (legal.empty() || !std::isfinite(lateral)) return 0;
        const double ext = k.halfWidth * std::fabs(std::cos(relYaw)) + k.halfLength * std::fabs(std::sin(relYaw));
        const std::pair<double, double>* road = nullptr; double away = 1e18;
        for (const auto& r : legal) {
            const double d = lateral < r.first ? r.first - lateral : lateral > r.second ? lateral - r.second : 0.0;
            if (d < away) { away = d; road = &r; }
        }
        if (!road) return 0;
        if (away > 0) return ext + away;
        const bool lowHard = std::isfinite(road->first), highHard = std::isfinite(road->second);
        if (lowHard && highHard) { const double mid = .5 * (road->first + road->second), room = std::max(0.0, .5 * (road->second - road->first) - ext); return std::max(0.0, std::fabs(lateral - mid) - room); }
        if (lowHard) return std::max(0.0, road->first - (lateral - ext));
        if (highHard) return std::max(0.0, (lateral + ext) - road->second);
        return 0;
    }
    bool LaneEdgeFree(double station) const {
        for (const auto& r : roundFree) { if (station < r.first) return false; if (station <= r.second) return true; }
        return false;
    }
    // Step 2: distance from each 0.5 m cell centre to the nearest solid point of any kind (exact Euclidean transform, Felzenszwalb), so a
    // query far from everything is answered without a search. Never used to decide a contact, only to skip a search that could find none.
    double dCell = .5; int dW = 0, dH = 0; double dX0 = 0, dY0 = 0; std::vector<float> dist;
    void BuildDistance() {
        dist.clear(); dW = dH = 0; if (solids.empty() && route.empty()) return;
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (const auto& so : solids) { x0 = std::min(x0, so.x); y0 = std::min(y0, so.y); x1 = std::max(x1, so.x); y1 = std::max(y1, so.y); }
        for (const auto& r : route) { x0 = std::min(x0, r.x); y0 = std::min(y0, r.y); x1 = std::max(x1, r.x); y1 = std::max(y1, r.y); }
        x0 -= 25; y0 -= 25; x1 += 25; y1 += 25;
        dW = int((x1 - x0) / dCell) + 1; dH = int((y1 - y0) / dCell) + 1; dX0 = x0; dY0 = y0;
        if (dW > 1200 || dH > 1200) { dW = dH = 0; return; }  // too wide: no map (every query searches), never a map missing solids
        const float inf = 1e18f; std::vector<float> f(std::size_t(dW) * dH, inf);
        for (const auto& so : solids) { const int ix = int((so.x - dX0) / dCell), iy = int((so.y - dY0) / dCell); if (ix >= 0 && iy >= 0 && ix < dW && iy < dH) f[std::size_t(iy) * dW + ix] = 0; }
        auto edt1 = [&](float* a, int n, int stride) {  // squared distance transform of one row/column (in cells)
            std::vector<float> src(n), out(n); std::vector<int> v(n); std::vector<float> z(n + 1);
            for (int i = 0; i < n; ++i) src[i] = a[std::size_t(i) * stride];
            int k2 = 0; v[0] = 0; z[0] = -inf; z[1] = inf;
            for (int q = 1; q < n; ++q) {
                float sq; while (true) { const int p2 = v[k2]; sq = ((src[q] + float(q) * q) - (src[p2] + float(p2) * p2)) / (2.f * q - 2.f * p2); if (sq <= z[k2] && k2 > 0) { --k2; continue; } break; }
                ++k2; v[k2] = q; z[k2] = sq; z[k2 + 1] = inf;
            }
            k2 = 0; for (int q = 0; q < n; ++q) { while (z[k2 + 1] < q) ++k2; const float d = float(q - v[k2]); out[q] = d * d + src[v[k2]]; }
            for (int i = 0; i < n; ++i) a[std::size_t(i) * stride] = out[i];
        };
        for (int y = 0; y < dH; ++y) edt1(&f[std::size_t(y) * dW], dW, 1);
        for (int x = 0; x < dW; ++x) edt1(&f[x], dH, dW);
        dist.resize(f.size()); for (std::size_t i = 0; i < f.size(); ++i) dist[i] = f[i] >= inf * .5f ? 1e9f : float(std::sqrt(f[i]) * dCell);
    }
    // A lower bound on the distance from (x, y) to any solid point's box (exact transform, minus the cell's own size and the largest half);
    // +large outside the map (no solids there by construction of the bounds).
    double NearestBound(double x, double y) const {
        if (dist.empty()) return 1e9;
        const int ix = int((x - dX0) / dCell), iy = int((y - dY0) / dCell);
        if (ix < 0 || iy < 0 || ix >= dW || iy >= dH) return solids.empty() ? 1e9 : 0;  // outside the map: no shortcut
        return double(dist[std::size_t(iy) * dW + ix]) - dCell * 1.5 - maxHalf;
    }
    template <class F> void Near(double x, double y, double r, F&& f) const {
        const int x0 = int(std::floor((x - r) / cell)), x1 = int(std::floor((x + r) / cell)), y0 = int(std::floor((y - r) / cell)), y1 = int(std::floor((y + r) / cell));
        for (int gx = x0; gx <= x1; ++gx) for (int gy = y0; gy <= y1; ++gy) { auto it = grid.find(Key(gx, gy)); if (it != grid.end()) for (int i : it->second) f(solids[i]); }
    }
    // Signed distance from the body footprint to a point box (negative = inside), in the plane.
    static double BodyDistance(double x, double y, double heading, double hw, double hl, const Solid& s) {
        const double dx = s.x - x, dy = s.y - y, c = std::cos(heading), n = std::sin(heading);
        const double across = dx * c - dy * n, along = dx * n + dy * c;
        const double ox = std::fabs(across) - hw - s.half, oy = std::fabs(along) - hl - s.half;
        return ox > 0 || oy > 0 ? std::hypot(std::max(ox, 0.0), std::max(oy, 0.0)) : std::max(ox, oy);
    }
    // The same distance with the heading's cos/sin given (step 2: computed once per query, not once per solid point).
    static double BodyDistanceCS(double dx, double dy, double c, double n, double hw, double hl, double half) {
        const double across = dx * c - dy * n, along = dx * n + dy * c;
        const double ox = std::fabs(across) - hw - half, oy = std::fabs(along) - hl - half;
        return ox > 0 || oy > 0 ? std::hypot(std::max(ox, 0.0), std::max(oy, 0.0)) : std::max(ox, oy);
    }
    double SearchRadius(const bicycle::Spec& k, double out) const { return std::hypot(k.halfLength, k.halfWidth) + out + maxHalf; }
    // skipBand: leave the known no-go walls (band solids) out, to ask what a SENSED solid alone leaves (the way out of a band, energyplan Score).
    double Clearance(double x, double y, double heading, double floorZ, const bicycle::Spec& k, bool tilt = true, bool skipBand = false) const {
        if (NearestBound(x, y) > std::hypot(k.halfLength, k.halfWidth) + reach) return reach;  // step 2: nothing within reach of the body
        double best = reach; const double c = std::cos(heading), n = std::sin(heading);
        Near(x, y, SearchRadius(k, reach), [&](const Solid& s) {
            if (s.soft || (skipBand && s.band)) return;  // a prediction (or, asked to, a known band)
            const double ref = RoadRef(s.road, s.x - x, s.y - y, floorZ, tilt);
            if (s.top <= ref + k.climb || s.base >= ref + k.height) return;  // rolled over, or above the roof (RP-1: of the road there)
            best = std::min(best, BodyDistanceCS(s.x - x, s.y - y, c, n, k.halfWidth, k.halfLength, s.half));
        });
        return best;
    }
    // Step 2: clearance, wheel-climbable edges under the body and predictions near it, in ONE pass over the nearby points (the planner asks all
    // three at every rollout step). Same answers as Clearance, LowEdges and SoftNear.
    struct Probe {
        double clear = 0; int low = 0, soft = 0;
        const Solid* nearest = nullptr;  // valid while this scene is being scored
    };
    Probe Contact(double x, double y, double heading, double floorZ, const bicycle::Spec& k, double softMargin = .3, bool tilt = true) const {
        Probe p; p.clear = reach;
        if (NearestBound(x, y) > std::hypot(k.halfLength, k.halfWidth) + std::max(reach, softMargin)) return p;  // step 2: nothing near the body
        const double c = std::cos(heading), n = std::sin(heading);
        Near(x, y, SearchRadius(k, std::max(reach, softMargin)), [&](const Solid& s) {
            const double ref = RoadRef(s.road, s.x - x, s.y - y, floorZ, tilt);  // RP-1: the road's height under this solid
            if (s.base >= ref + k.height) return;
            const double d = BodyDistanceCS(s.x - x, s.y - y, c, n, k.halfWidth, k.halfLength, s.half);
            if (s.soft) { if (s.top > ref + k.climb && d < softMargin) ++p.soft; return; }
            if (s.top > ref + k.climb) {
                if (d < p.clear) {
                    p.clear = d;
                    p.nearest = &s;
                }
            }
            else if (s.top > ref + std::max(.05, 2.5 * s.roadSpread) && d < 0) ++p.low;  // RF-1: inside the road's pattern is road
        });
        return p;
    }
    int LowEdges(double x, double y, double heading, double floorZ, const bicycle::Spec& k, bool tilt = true) const {
        int n = 0;
        Near(x, y, k.halfLength + k.halfWidth + 1, [&](const Solid& s) {
            if (s.soft) return;
            const double ref = RoadRef(s.road, s.x - x, s.y - y, floorZ, tilt);
            if (s.top > ref + std::max(.05, 2.5 * s.roadSpread) && s.top <= ref + k.climb && BodyDistance(x, y, heading, k.halfWidth, k.halfLength, s) < 0) ++n;
        });
        return n;
    }
    // ED-1: predicted solids in the body's height band within `margin` of the body (cost only, never a block).
    int SoftNear(double x, double y, double heading, double floorZ, const bicycle::Spec& k, double margin = .3, bool tilt = true) const {
        int n = 0;
        Near(x, y, k.halfLength + k.halfWidth + margin + 1, [&](const Solid& s) {
            if (!s.soft) return;
            const double ref = RoadRef(s.road, s.x - x, s.y - y, floorZ, tilt);
            if (s.top > ref + k.climb && s.base < ref + k.height && BodyDistance(x, y, heading, k.halfWidth, k.halfLength, s) < margin) ++n;
        });
        return n;
    }
    bool Floor(double x, double y, double& z) const { return floor && floor(x, y, z); }
    // RP-1 (2026-11-19): every solid is judged against the road at its own position, not one flat floor at the body's centre. On a ramp
    // the road under the nose is far above the centre floor (23 deg: +0.9 m at the bumper), so the road and the seams of its deck pieces
    // read as walls ahead of the car on every uphill (09:43-09:45 lc_ring_road: blockers +0.5-0.8 m over the centre floor, about 0 over
    // the road). Solid::road is the lane floor under the solid, found once in Index(). It counts only when it could be this road: no
    // further from floorZ than kMaxBandGrade over the distance (another level of a switchback is not). Unknown: the flat floor, as before.
    // Hovering bodies pass tilt=false: a hovering band does not follow the road.
    static constexpr double kMaxBandGrade = .84;  // tan(40 deg): steeper than any road a wheel holds
    static double RoadRef(double road, double dx, double dy, double floorZ, bool tilt) {
        if (!tilt || !std::isfinite(road)) return floorZ;
        return std::fabs(road - floorZ) <= std::hypot(dx, dy) * kMaxBandGrade + .35 ? road : floorZ;
    }
    double MoverRef(double mx, double my, double x, double y, double floorZ, bool tilt) const {
        double road = std::numeric_limits<double>::quiet_NaN();
        if (tilt && !Floor(mx, my, road)) road = std::numeric_limits<double>::quiet_NaN();
        return RoadRef(road, mx - x, my - y, floorZ, tilt);
    }

    double FieldOutside(double x, double y) const {
        if (field.empty()) return 0;
        double best = 1e9; const bool cached = fieldAxes.size() == field.size();
        for (std::size_t i = 0; i < field.size(); ++i) {
            const auto& b = field[i];
            const fieldbox::V3 d = { x - b.centre.x, y - b.centre.y, 0.0 };
            const auto a = cached ? fieldAxes[i] : fieldbox::AxesOf(b);  // step 2: the axes computed once per box
            const fieldbox::V3 l = { fieldbox::Dot(d, a.right), fieldbox::Dot(d, a.forward), 0.0 };
            best = std::min(best, std::hypot(std::max(0.0, std::fabs(l.x) - b.halfWidth), std::max(0.0, std::fabs(l.y) - b.halfLength)));
        }
        return best;
    }
    // hint: a station near the answer (the car's own); only route within [hint - 15, hint + 45] m is considered, so a hairpin's other leg
    // (metres away across the inner barrier) is never taken for this one. NaN: the whole route.
    void Route(double x, double y, double& station, double& lateral, double& half, double hint = std::nan(""), double* yaw = nullptr, double* distance = nullptr, int* segment = nullptr) const {
        half = laneHalf; station = 0; lateral = 0;
        double best = 1e18;
        auto first = route.begin();
        if (std::isfinite(hint))
            first = std::lower_bound(first, route.end(), hint - 15, [](const RoutePoint& p, double s) { return p.station < s; });
        if (first != route.begin()) --first;
        for (int i = int(first - route.begin()); i + 1 < int(route.size()); ++i) {
            const auto& a = route[i]; const auto& b = route[i + 1];
            if (a.station > hint + 45) break;
            const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy; if (len2 < 1e-9) continue;
            const double t = std::clamp(((x - a.x) * dx + (y - a.y) * dy) / len2, 0.0, 1.0);
            const double px = a.x + t * dx, py = a.y + t * dy, d2 = (x - px) * (x - px) + (y - py) * (y - py);
            if (d2 < best) { best = d2; station = a.station + t * std::sqrt(len2); if (segment) *segment = i;
                if (yaw) *yaw = std::atan2(dx, dy);
                const double len = std::sqrt(len2); lateral = ((x - px) * dy - (y - py) * dx) / len; }  // + = right of travel
        }
        if (distance) *distance = std::sqrt(best);
    }
    // LF-1: the point at `station` along the route, `offset` m to the right of travel (clamped to the route's ends). False: no route.
    bool RouteAt(double station, double offset, double& x, double& y) const {
        if (route.size() < 2) return false;
        auto it = std::lower_bound(route.begin(), route.end(), station, [](const RoutePoint& p, double v) { return p.station < v; });
        std::size_t i = it == route.begin() ? 0 : std::size_t(it - route.begin()) - 1; i = std::min(i, route.size() - 2);
        const auto& a = route[i]; const auto& b = route[i + 1];
        const double len = std::max(1e-6, b.station - a.station), t = std::clamp((station - a.station) / len, 0.0, 1.0);
        const double dx = b.x - a.x, dy = b.y - a.y, l = std::max(1e-6, std::hypot(dx, dy));
        x = a.x + dx * t + dy / l * offset; y = a.y + dy * t - dx / l * offset;  // right of travel = (dy, -dx)
        return true;
    }
    // Step 2: the same projection, searched only within `span` segments of `seg` (the last answer). A rollout moves a few metres per step, so
    // its nearest segment stays close; this also keeps a hairpin's other leg out without a station window. Returns the segment found.
    int RouteLocal(double x, double y, int seg, double& station, double& lateral, double& half, int span = 8) const {
        half = laneHalf; station = 0; lateral = 0; double best = 1e18; int found = seg;
        const int lo = std::max(0, seg - span), hi = std::min(int(route.size()) - 2, seg + span);
        for (int i = lo; i <= hi; ++i) {
            const auto& a = route[i]; const auto& b = route[i + 1];
            const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy; if (len2 < 1e-9) continue;
            const double t = std::clamp(((x - a.x) * dx + (y - a.y) * dy) / len2, 0.0, 1.0);
            const double px = a.x + t * dx, py = a.y + t * dy, d2 = (x - px) * (x - px) + (y - py) * (y - py);
            if (d2 < best) { best = d2; found = i; const double len = std::sqrt(len2); station = a.station + t * len; lateral = ((x - px) * dy - (y - py) * dx) / len; }
        }
        return found;
    }
};
}
