#pragma once
#include "planscene.h"

namespace wheelguard {
constexpr double reaction = .18;          // seconds to replace a published command
constexpr double clearanceMargin = .15;   // do not depend on a centimetre of modeled clearance
constexpr double crawl = 1.25;
constexpr double escape = .75;
constexpr double sampleStep = .25;
constexpr double maxLook = 30.0;

struct Command {
    double steering = 0, want = 0;          // normalized steering and signed requested speed
    double vx = 0, vy = 0;                    // actual velocity; a gear change can leave motion in the old direction
    double railBearing = 0, railYaw = 0;     // direct rail motion, world heading and right-positive yaw
    double snapshotAge = 0;
    bool directRail = false, constrained = false;
};

struct Limit {
    double speed = std::numeric_limits<double>::infinity();
    const char* reason = "clear";
};

inline double SpeedForDistance(double distance, double brake) {
    const double lag = brake * reaction;
    return std::max(0.0, std::sqrt(lag * lag + 2 * brake * std::max(0.0, distance)) - lag);
}

// A fresh measured scene is collision evidence, never a guarantee that an unobserved wall
// is absent. Unverified floor limits a constrained maneuver but does not declare a collision.
inline Limit Check(const planscene::Scene* scene, const bicycle::State& car,
                   const bicycle::Spec& inputSpec, const Command& cmd) {
    if (!scene) return cmd.constrained ? Limit{crawl, "snapshot_unverified"} : Limit{};
    auto spec = inputSpec;
    const double reactionSpeed = std::max(std::hypot(cmd.vx, cmd.vy), std::fabs(cmd.want));
    const double brake = std::clamp(spec.brake * .5, .6, 3.0);
    const double needed = reactionSpeed * reaction + reactionSpeed * reactionSpeed / (2 * brake) + clearanceMargin;
    const double look = std::min(maxLook, needed);
    if (look < .05) return {};
    if (!cmd.constrained && scene->movers.empty() &&
        scene->NearestBound(car.x, car.y) > look + std::hypot(spec.halfLength, spec.halfWidth) + .4)
        return {};

    // Inflate the swept body for tracking and pose discretization. Keep the original width
    // for deciding whether the rover is already touching and may move away.
    // WG-2 (2026-11-19): only on a constrained move (Avoid, Recover, a plan with under 0.65 m clearance). In Follow the planner already
    // scored this corridor with its own margin; a stricter guard there would be a second speed decider that crawls every narrow lane.
    auto envelope = spec;
    if (cmd.constrained) {
        envelope.halfWidth += clearanceMargin;
        envelope.halfLength += clearanceMargin;
    }
    auto clearance = [&](double x, double y, double heading, double z, double t, const bicycle::Spec& body) {
        bool closing = false;
        const double mover = scene->MoverClearance(x, y, heading, z, body, cmd.snapshotAge + t, cmd.vx, cmd.vy, closing);
        // The known route edge bands and medians (planscene band walls) are the planner's: Score refuses a path into one unless it is leaving it,
        // within kNoGoTolerance. The guard judges measured solids only: its own 0.15 m envelope on a constrained move read a band 3-10 cm
        // away as "wheel_path_blocked" (tier 10, speed 0), a second, stricter judge of the same wall (2026-11-20 08:00-08:03 S-bend stall).
        const double fixed = scene->Clearance(x, y, heading, z, body, true, true);
        return closing ? std::min(fixed, mover) : fixed;
    };
    double startZ = car.z;
    const bool haveStart = scene->Floor(car.x, car.y, startZ);
    const double startClear = clearance(car.x, car.y, car.heading, startZ, 0, envelope);
    const double startActual = startClear < 0 ? clearance(car.x, car.y, car.heading, startZ, 0, spec) : startClear;
    const double signedMotion = cmd.vx * std::sin(car.heading) + cmd.vy * std::cos(car.heading);
    const int requestDirection = cmd.want < 0 ? -1 : 1;
    const int movingDirection = signedMotion < 0 ? -1 : 1;
    Limit limit;
    auto restrict = [&](double speed, const char* reason) {
        if (speed < limit.speed) limit = {speed, reason};
    };
    if (cmd.constrained && needed > maxLook) restrict(SpeedForDistance(maxLook - clearanceMargin, brake), "lookahead_limit");
    auto examine = [&](int direction, bool actualMotion, bool rail) {
        if (startActual < 0 && actualMotion && movingDirection != requestDirection) {
            restrict(0, "changing_direction_in_overlap");
            return;
        }
        double x = car.x, y = car.y, heading = car.heading, floor = startZ;
        double previous = startClear, previousActual = startActual;
        const double speed = std::max(.5, reactionSpeed);
        const double wheelYawPerM = direction * std::tan(carmodel::WheelAngle(spec.car, cmd.steering, speed)) /
            std::max(1.0, spec.car.wheelbase);
        const double yawPerM = rail ? cmd.railYaw / speed : wheelYawPerM;
        const double driftBearing = std::atan2(cmd.vx, cmd.vy);
        const int count = std::max(1, int(std::ceil(look / sampleStep)));
        const double step = look / count;
        for (int i = 1; i <= count; ++i) {
            const double dist = i * step;
            heading += yawPerM * step;
            const double bearing = actualMotion && dist <= speed * reaction ?
                driftBearing : rail ? cmd.railBearing : heading + (direction < 0 ? 3.141592653589793 : 0);
            x += step * std::sin(bearing);
            y += step * std::cos(bearing);
            const bool known = scene->Floor(x, y, floor);
            if ((!known || !haveStart) && cmd.constrained)
                restrict(std::max(crawl, SpeedForDistance(dist - step, brake)), "floor_unverified");
            const double gap = clearance(x, y, heading, floor, dist / speed, envelope);
            if (gap < 0) {
                const double actual = clearance(x, y, heading, floor, dist / speed, spec);
                // Permit a real retreat from an initial overlap, never a path that first goes deeper
                // or reaches an additional obstruction after clearing the original overlap.
                if (startActual < 0 && previousActual < 0 && actual > previousActual + .001) {
                    restrict(escape, "backing_out_of_overlap");
                } else if (startActual >= 0 && actual >= 0 && previous < 0 && gap >= previous - .002) {
                    restrict(escape, "inside_clearance_margin");
                } else {
                    restrict(SpeedForDistance(dist - step, brake), "wheel_path_blocked");
                    return;
                }
                previousActual = actual;
            } else {
                previousActual = gap;
            }
            previous = gap;
        }
    };
    examine(requestDirection, false, cmd.directRail);
    const double lateralMotion = cmd.vx * std::cos(car.heading) - cmd.vy * std::sin(car.heading);
    if (std::hypot(cmd.vx, cmd.vy) > .35 &&
        (cmd.directRail || movingDirection != requestDirection || std::fabs(lateralMotion) > .12 * std::fabs(signedMotion)))
        examine(movingDirection, true, cmd.directRail && movingDirection == requestDirection);
    return limit;
}
}  // namespace wheelguard
