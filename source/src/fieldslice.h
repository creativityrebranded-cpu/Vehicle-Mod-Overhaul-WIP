#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

// World's volume (notes/volume-world-20261111/PLAN.md; one-flow plan section 9). One volume per vehicle, approximated from the data the
// existing sources give: the authored field says where legal space is and where it ends, the car's own beams measure what is in it. Beams are
// laid across the field's own width (none where there is no field), densely (redundancy in space) and in interleaved phases carried forward
// between refreshes (redundancy in time). Space nothing measured is cut out of the volume: it is no information, never clear road and never a
// solid (fieldvolume.h gives it no shape). `Unknown` is the code name of "no information".
namespace fieldslice {
inline constexpr int columns = 16, levels = 4;
inline constexpr int rayCount = columns * levels;
inline constexpr int phases = 4;             // redundancy in time: one column in four is cast per refresh, the rest carried forward
inline constexpr double maxDepth = 40.0, minDepth = 10.0, maxColumnGap = 1.6, minColumnGap = .15, agreement = 1.25;
inline constexpr double agreementLateral = 1.2;  // hits agree when within this far across as well as `agreement` along (not by column index)
// The vehicle layer is the car width plus a margin, never narrower; speed lengthens the volume and widens the margin a little.
inline constexpr double baseMargin = .30, speedMargin = .02, maxMargin = .60;
inline constexpr double reuseHitSeconds = 3.0, reuseClearSeconds = 2.5;  // a carried measurement holds this long (a phase returns every 4 refreshes)
inline double Margin(double speed) { return std::min(maxMargin, baseMargin + speedMargin * std::max(0.0, speed)); }
inline double VehicleHalf(double halfWidth, double speed) { return halfWidth + Margin(speed); }
inline double Depth(double speed, double brake, double halfLength) {
    const double v = std::max(0.0, speed), stop = v * v / (2 * std::max(1.0, brake));
    return std::clamp(halfLength + stop + v * .8 + 6.0, minDepth, maxDepth);
}
// Beam heights over the floor, tyre to roof, fitted to the car's own height (the pebble in the fluid: the volume wraps this car).
inline double Height(int level, double carHeight) {
    const double top = std::max(.50, carHeight - .20), low = .30;
    return levels <= 1 ? low : low + (top - low) * double(std::clamp(level, 0, levels - 1)) / double(levels - 1);
}
// How many columns either side a hit looks for agreement, from the lateral distance (dense beams do not change what counts as confirmed).
inline int Neighbours(double spacing) { return spacing <= 0 ? 1 : std::clamp(int(std::ceil(agreementLateral / spacing - 1e-9)), 1, columns - 1); }
// Redundancy in time: which columns this refresh casts.
inline bool InPhase(int column, int phase) { return ((column % phases) + phases) % phases == ((phase % phases) + phases) % phases; }
// Where the beams go: across the legal space the field gives (fieldLeft..fieldRight, lateral offsets from the route line), widened to cover
// the car's own vehicle layer when the car is partly outside it (entering), at most (columns - 1) x maxColumnGap wide around the car.
// left comes back relative to the car (carLateral), as Slice::left is. Returns false when there is no width to lay beams across.
inline bool Place(double fieldLeft, double fieldRight, double carLateral, double vehicleHalf, double& left, double& spacing) {
    double lo = std::min(fieldLeft, carLateral - vehicleHalf), hi = std::max(fieldRight, carLateral + vehicleHalf);
    const double widest = maxColumnGap * (columns - 1);
    if (hi - lo > widest) { lo = std::clamp(carLateral - widest * .5, lo, hi - widest); hi = lo + widest; }
    if (!(hi > lo)) return false;
    spacing = std::max(minColumnGap, (hi - lo) / (columns - 1));
    left = lo - carLateral;
    return true;
}

// World truth (WORKING_MANDATE.md, stop rule): a ray alone on terrain or road shapes the floor and slows, it never confirms a solid. These are
// the game's terrain collision layers (13 and 17); a hit on one is a surface the volume is clear up to, not an obstacle.
inline constexpr unsigned terrainLayers = (1u << 13) | (1u << 17);
inline bool TerrainLayer(unsigned layer) { return layer < 32 && ((1u << layer) & terrainLayers) != 0; }

enum class Answer { Unknown, Clear, Hit };
// A ray's answer. For Clear, `field` says the ray ended where the authored field ends (legal space ended, not an obstacle); a ray that
// never started inside the field has depth <= the volume's start. `at` is when it was measured (seconds, the plan clock).
// `z` is the height of a hit point (world), NaN when it was not recorded: the volume's shapes take their vertical extent from it.
// `aboveGround` is how high a hit stands above the measured ground under it (NaN: ground not measured there); the remembered point keeps it
// (else the beam's nominal height) so a later beam at that height can pass through the spot. It decides nothing: whether a hit is climbed
// or met is Plan's, against World's fitted road (D2/E 2026-11-19; the volume's own pass-height verdict is removed).
// `owner` is the object a hit landed on (its reference id, 0 unknown): the same object hit again agrees with it (OA-1).
struct Ray { Answer answer = Answer::Unknown; double depth = 0; bool field = false; double at = 0; double z = std::numeric_limits<double>::quiet_NaN();
             double aboveGround = std::numeric_limits<double>::quiet_NaN(); unsigned owner = 0;
             float hx = std::numeric_limits<float>::quiet_NaN(), hy = std::numeric_limits<float>::quiet_NaN(); };  // a hit's own world point (float: small rays, the frame lives on the stack)

// OA-1 object memory (user, 2026-11-11 10:49 run: a fallen branch was hit by the beams again and again, refs 06002AB2/AB3/ABA/ABB/085C8,
// but thin twigs met one beam at a time, so the volume called it solid, doubt and nothing in turn and the plan swung route/trail/reverse).
// Redundancy in time and in identity: every hit point is remembered in world space with the object it landed on. The same object hit
// again close by, by another beam or on a later refresh, is agreement. A remembered point stays until a beam is measured passing through
// that same spot (or it ages out); a beam passing beside it, or a turn that re-aims the beams, does not clear it.
inline constexpr int memoryPoints = 48;
inline constexpr int perObjectPoints = 6;      // the most points one object keeps
inline constexpr double memorySeconds = 6.0;   // a remembered point no beam has passed through is forgotten after this long
inline constexpr double sameObjectReach = 2.5; // same object hit within this far (world, across) agrees
inline constexpr double passSpot = .35;        // a beam passes through a point when within this far across and in height
// World truth (2026-11-12, notes/logic-fix-20261112/DESIGN.md section 2).
inline constexpr int downRaysPerRefresh = 8;     // WT-1: down-rays a refresh may spend to measure the ground under hits the survey has no floor for
inline constexpr int lookAgainPerRefresh = 4;    // WT-3: aimed rays a refresh may spend to settle beliefs seen only once
inline constexpr double overheadClear = .10;     // WT-2: an underside this far above the car's roof is passed under
inline constexpr double lookBeyond = .60;        // WT-3: an aimed ray runs this far past the point; clean through = the point is gone
struct Remembered { unsigned owner = 0; double x = 0, y = 0, z = 0, above = 0, at = -1; int hits = 1; };  // hits: times this spot was hit
// The memory lives outside the Slice (one per pilot, static storage): the planner's frame that holds the Slice is on the game thread's stack.
struct Memory { std::array<Remembered, memoryPoints> points{}; };
inline constexpr double groundTolerance = .15;  // a hit no higher than this over the measured ground is the ground itself: the floor, clear up to it
// A measurement not cast this refresh is carried forward as the car moves: depths shorten by the distance travelled (`moved`, forward in
// the old volume's frame). A hit lives longer than a clear. A ray that never started in the field stays so. Returns false when the old
// answer no longer holds (too old, or used up by the car's own travel).
inline bool Carry(Ray old, double now, double moved, double start, double oldStart, Ray& out) {
    const double age = now - old.at;
    if (!((old.answer == Answer::Hit && age <= reuseHitSeconds) || (old.answer == Answer::Clear && age <= reuseClearSeconds))) return false;
    if (old.answer == Answer::Clear && old.field && old.depth <= oldStart + 1e-6) { old.depth = start; out = old; return true; }
    old.depth -= moved;
    if (old.depth <= start + .2) return false;
    out = old;
    return true;
}
// Beams run in segments that follow the measured floor, so a slope edge does not put a beam into the ground: one cast per segment.
inline constexpr double segmentLength = 8.0;
inline int Segments(double start, double end) { return std::max(1, int(std::ceil((end - start) / segmentLength))); }
// Casts one refresh needs at most: one phase of the columns (or all of them on a full refresh), every level, every segment.
inline int CastsPerRefresh(double start, double end, bool full) {
    const int cols = full ? columns : (columns + phases - 1) / phases;
    return cols * levels * Segments(start, end);
}

struct Slice {
    double x = 0, y = 0, yaw = 0, start = 0, end = 0, left = 0, spacing = 0;
    // The volume's vertical profile: heightAbove[h] is how far above the floor a level's beams fly (defaults: a 1.6 m car). The volume has no
    // pass height of its own (E, 2026-11-19): what the wheels climb is the one car record's climb (carcap.h), judged by Plan against World's
    // fitted road; the volume only says where the rays hit.
    std::array<double, levels> heightAbove{ .30, .67, 1.03, 1.40 };
    double base = 0, grade = 0;
    double vehicleHalf = 0, roadLeft = 0, roadRight = 0;  // vehicle layer half width (0: car only); legal space across, from the car's line
    std::array<std::array<Ray, levels>, columns> rays{};
    bool active = false;
    int phase = 0;  // the columns cast this refresh (InPhase); the rest were carried
    Memory* memory = nullptr;  // OA-1: this car's hit points by object, world space (none: no memory, no object agreement)

    bool Live(const Remembered& p, double now) const { return p.owner != 0 && p.at >= 0 && now >= p.at - 1e-6 && now - p.at <= memorySeconds; }
    // Records one hit point. Points older than the memory are dropped first; a full memory replaces its oldest point.
    void Remember(unsigned owner, double px, double py, double pz, double above, double now) {
        if (!owner || !memory) return;
        auto& points = memory->points;
        // The same spot hit again (same object, within a beam's width and height) refreshes that point instead of adding one.
        int mine = 0, mineOldest = -1;
        for (int k = 0; k < memoryPoints; ++k) {
            auto& p = points[k];
            if (p.owner != owner || !Live(p, now)) continue;
            if (std::hypot(p.x - px, p.y - py) <= passSpot && std::abs(p.z - pz) <= passSpot) { const int hits = p.hits + 1; p = { owner, px, py, pz, std::max(above, p.above), now, hits }; return; }
            ++mine; if (mineOldest < 0 || p.at < points[mineOldest].at) mineOldest = k;
        }
        // One big object (a wall) never crowds the others out: it keeps at most its own share, replacing its oldest point.
        if (mine >= perObjectPoints && mineOldest >= 0) { points[mineOldest] = { owner, px, py, pz, above, now }; return; }
        int slot = -1; double oldest = 1e18;
        for (int k = 0; k < memoryPoints; ++k) {
            if (!Live(points[k], now)) { slot = k; break; }
            if (points[k].at < oldest) { oldest = points[k].at; slot = k; }
        }
        points[slot] = { owner, px, py, pz, above, now };
    }
    // The same object hit elsewhere close by (not this very point): how many points.
    int SameObject(unsigned owner, double px, double py, double pz, double at, double now) const {
        int n = 0;
        if (!owner || !memory) return 0;
        for (const auto& p : memory->points) {
            if (p.owner != owner || !Live(p, now)) continue;
            if (std::abs(p.at - at) < 1e-9 && std::abs(p.x - px) < 1e-3 && std::abs(p.y - py) < 1e-3 && std::abs(p.z - pz) < 1e-3) {  // itself:
                if (p.hits >= 2) ++n;  // agreement in time: this spot was hit before
                continue;
            }
            if (std::hypot(p.x - px, p.y - py) > sameObjectReach) continue;
            ++n;
        }
        return n;
    }
    // A beam cast at `now` (rays[i][h] with at == now) passes through every remembered point it went by before its answer: those points are
    // measured gone. Returns how many were cleared.
    int ClearPassed(double now) {
        const double s = std::sin(yaw), c = std::cos(yaw);
        int cleared = 0;
        if (!memory) return 0;
        for (auto& p : memory->points) {
            if (!Live(p, now) || std::abs(p.at - now) < 1e-9) continue;  // points hit this refresh are this refresh's own answers
            const double depth = (p.x - x) * s + (p.y - y) * c, side = (p.x - x) * c - (p.y - y) * s;
            if (depth < start) continue;
            bool gone = false;
            for (int i = 0; i < columns && !gone; ++i) {
                if (std::abs(left + i * spacing - side) > passSpot) continue;
                for (int h = 0; h < levels; ++h) {
                    const auto& r = rays[i][h];
                    if (r.answer == Answer::Unknown || std::abs(r.at - now) > 1e-9) continue;
                    if (r.answer == Answer::Clear && r.field && r.depth <= start + 1e-6) continue;  // never started in the field
                    if (std::abs(heightAbove[h] - p.above) > passSpot) continue;
                    if (r.depth - .3 < depth) continue;  // the beam ended at or before this point
                    if (r.answer == Answer::Hit && r.owner == p.owner) continue;  // hit the same object just beyond: not gone
                    gone = true; break;
                }
            }
            if (gone) { p.owner = 0; p.at = -1; ++cleared; }
        }
        return cleared;
    }
    int MemoryLive(double now) const { int n = 0; if (memory) for (const auto& p : memory->points) n += Live(p, now) ? 1 : 0; return n; }
    enum class Clearance { Unknown, Clear, Blocked };
    // A trace and shadow-check reading only (the planner reads the volume as World shapes). poseInField: is this pose inside the authored
    // field? A clear ray that ended at the field boundary says nothing beyond it where the pose is outside the field.
    Clearance At(double px, double py, double pz, double halfWidth, double halfLength, double heading, bool poseInField = true) const {
        const double s = std::sin(yaw), c = std::cos(yaw);
        const double depth = (px - x) * s + (py - y) * c;
        const double side = (px - x) * c - (py - y) * s;
        const double angle = heading - yaw;
        halfWidth = std::max(halfWidth, vehicleHalf);  // the vehicle layer is never narrower than the car plus its margin
        const double width = halfWidth * std::abs(std::cos(angle)) + halfLength * std::abs(std::sin(angle));
        const double nose = depth + halfLength * std::abs(std::cos(angle)) + halfWidth * std::abs(std::sin(angle));
        if (spacing <= 0 || spacing > maxColumnGap || nose < start || nose > end ||
            std::abs(pz - (base + depth * grade)) > .65) return Clearance::Unknown;
        bool clear = side - width >= left - spacing * .5 &&
                     side + width <= left + spacing * (columns - .5);
        const int reach = Neighbours(spacing);
        for (int i = 0; i < columns; ++i) {
            if (std::abs(left + i * spacing - side) > width + spacing * .5) continue;
            for (int h = 0; h < levels; ++h) {
                const auto& ray = rays[i][h];
                if (ray.answer == Answer::Clear && ray.field && (ray.depth <= start + 1e-6 || !poseInField)) continue;  // outside the legal space
                if ((ray.answer == Answer::Clear && ray.depth >= nose) ||
                    (ray.answer == Answer::Hit && ray.depth > nose)) continue;
                clear = false;
                if (ray.answer != Answer::Hit) continue;
                for (int j = std::max(0, i - reach); j <= std::min(columns - 1, i + reach); ++j) {
                    for (int level = 0; level < levels; ++level) {
                        if (j == i && level == h) continue;
                        const auto& other = rays[j][level];
                        // A hit another hit agrees with is measured there (the volume's own agreement rule); how high it reaches is Plan's.
                        if (other.answer == Answer::Hit && std::abs(other.depth - ray.depth) <= agreement)
                            return Clearance::Blocked;
                    }
                }
            }
        }
        return clear ? Clearance::Clear : Clearance::Unknown;
    }
};
}
