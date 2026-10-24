#include "../src/vtolassist.h"
#include <cstdio>

static int failures = 0, checks = 0;
static void check(bool ok, const char* what) { ++checks; std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }

int main()
{
    using namespace vtolassist;
    Input in;
    in.speed = 25; in.haveView = true; in.heading = 0; in.rollRate = 1.5; in.yawLimit = 1.0; in.pitchLimit = 1.0;

    // A camera turn to the right banks right, as far as a level turn needs.
    in.view = 0.4;
    auto r = Solve(in);
    check(r.yaw > 0 && r.bankWant > 0 && r.rollRate > 0, "a camera turn right banks right");
    check(std::fabs(std::tan(r.bankWant) - in.speed * r.yaw / kGravity) < 1e-9 || r.bankWant == vtolai::kMaxBank, "the bank is the level-turn bank");
    check(r.world.z < 0, "turning right is negative about world up");
    in.view = -0.4;
    r = Solve(in);
    check(r.yaw < 0 && r.bankWant < 0 && r.rollRate < 0, "a camera turn left banks left");

    // Hovering in place: a camera turn does not bank.
    in.view = 0.4; in.speed = 0;
    r = Solve(in);
    check(r.yaw > 0 && r.bankWant == 0, "no forward speed: the nose turns flat");

    // Banking with A / D at speed turns the nose into the curve.
    in.speed = 25; in.view = 0; in.roll = 0.3; in.rollIn = 1;
    r = Solve(in);
    check(r.yaw > 0 && r.rollRate > 0, "banking right at speed turns the nose right");
    check(std::fabs(r.yaw - kGravity * std::tan(0.3) / 25) < 1e-9, "the nose follows the curve of that bank");
    in.roll = -0.3; in.rollIn = -0.5;
    r = Solve(in);
    check(r.yaw < 0 && std::fabs(r.rollRate + 0.75) < 1e-9, "a half stick left rolls at half rate and turns left");
    in.speed = 2; in.roll = 0.3; in.rollIn = 1;
    check(std::fabs(Solve(in).yaw) < 0.12, "hovering slowly a bank barely turns the nose");

    // Released at the same bank with the camera straight ahead: the assist rolls level.
    in.speed = 25; in.roll = 0.3; in.rollIn = 0; in.view = 0;
    r = Solve(in);
    check(r.rollRate < 0, "released with no turn asked: the bank levels");

    // Near the ground and landing: level, the camera alone turns.
    in.nearGround = true; in.view = 0.4; in.roll = 0.2;
    r = Solve(in);
    check(r.rollRate < 0 && r.bankWant == 0 && r.yaw > 0, "near the ground: no turn bank, the chassis levels");
    in.nearGround = false; in.landing = true; in.rollIn = 1;
    check(Solve(in).rollRate < 0, "landing always levels");
    in.landing = false;

    // Limits and the aerobatic band.
    in.view = 3.0; in.roll = 0; in.rollIn = 0;
    r = Solve(in);
    check(std::fabs(r.yaw) <= in.yawLimit && std::fabs(r.bankWant) <= vtolai::kMaxBank, "yaw and bank stay inside their limits");
    check(Applies(0.5) && !Applies(1.2) && !Applies(-1.2), "past 60 degrees of roll the body-axis controls take over");
    check(BankYaw(0.3, 0, 1) == 0 && BankYaw(NAN, 20, 1) == 0, "no speed or bad input: no bank yaw");

    // The camera's pitch is measured in the world, so a bank does not change it.
    in.view = 0; in.haveAim = true; in.pitchAim = 0.2; in.pitch = 0;
    in.roll = 0.0; const double level = Solve(in).world.z;
    in.roll = 0.4; in.right = { std::cos(0.4), 0, -std::sin(0.4) };
    check(std::fabs(Solve(in).world.z - level) < 0.2, "a bank does not drag the pitch aim into the yaw");

    std::printf("vtolassist: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
