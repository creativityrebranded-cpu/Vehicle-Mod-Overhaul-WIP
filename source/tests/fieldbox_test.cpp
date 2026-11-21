#include "fieldbox.h"
#include <cstdio>
using namespace fieldbox;
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
static bool Near(double a, double b, double e = 1e-6) { return std::fabs(a - b) <= e; }

int main() {
    Box b; b.centre = { 0, 0, 2.5 }; b.halfWidth = 4; b.halfLength = 10; b.halfHeight = 2.5;
    Check(Contains(b, { 3, 9, 1 }) && !Contains(b, { 5, 0, 1 }), "a level box contains what is inside it");
    {   Box t = b; t.yaw = 1.5707963267948966;  // facing +x
        const auto a = AxesOf(t);
        Check(Near(a.forward.x, 1) && Near(a.right.y, -1) && Near(a.up.z, 1), "yaw follows the game: forward (sin, cos), right (cos, -sin)");
        Check(Contains(t, { 9, 3, 1 }) && !Contains(t, { 3, 9, 1 }), "orientation turns the box");
    }
    {   Box t = b; t.pitch = 0.3; t.roll = 0.2; const auto a = AxesOf(t);
        Check(Near(Dot(a.forward, a.right), 0) && Near(Dot(a.forward, a.up), 0) && Near(Dot(a.right, a.up), 0) && Near(Dot(a.up, a.up), 1), "axes stay square when pitched and rolled");
        const V3 p{ 1.5, -3, 2 }; const V3 back = World(t, Local(t, p));
        Check(Near(back.x, p.x) && Near(back.y, p.y) && Near(back.z, p.z), "local and world agree");
    }
    Body car; car.centre = { 0, 0, 1 }; car.halfWidth = 1; car.halfLength = 2; car.halfHeight = 0.9;
    Check(Near(Share(&b, 1, car), 1), "a car fully inside has share 1");
    {   Body out = car; out.centre.x = 20; Check(Near(Share(&b, 1, out), 0), "a car far outside has share 0"); }
    {   Box chain[2] = { b, b }; chain[1].centre.y = 20; Body span = car; span.centre.y = 10;
        Check(Near(Share(chain, 2, span), 1), "a car spanning two tiles is inside the chain"); }

    Check(Judge(0, 0.4) == Move::Entering, "outside to part way is entering");
    Check(Judge(0.4, 1) == Move::Entering, "part way to inside is entering");
    Check(Judge(1, 1) == Move::Inside, "inside stays inside");
    Check(Judge(1, 0.6) == Move::Exiting, "inside to part way is exiting");
    Check(Judge(0, 0) == Move::Outside, "outside stays outside");
    Check(Judge(0.5, 0.5) == Move::Entering, "holding part way is not leaving");

    {   Containment c;
        Check(!c.Update(0, 0.5), "not contained while entering");
        Check(!c.Refuses(0.5, 0.2), "a car not yet contained may back out: no trap on the face");
        Check(!c.Update(1, 1) && !c.Update(1.1, 1), "the whole car must settle first");
        Check(c.Update(1.3, 1), "contained once settled");
        Check(c.Refuses(1, 0.7), "a contained car may not leave");
        Check(!c.Refuses(1, 1), "moving about inside is allowed");
        Check(c.Update(2, 0.8), "a small slip outside keeps containment");
        Check(!c.Refuses(0.7, 1), "coming back in is allowed");
        Check(!c.Update(3, 0.2), "pushed mostly out: containment ends");
        Check(!c.Refuses(0.2, 0), "and it is not held to the field");
        Check(!c.Refuses(0.2, 1), "it may enter again");
    }

    {   const auto s = SurfaceAt(b, 1, 2, 3); Check(s.inside && s.touch && !s.wall && Near(s.z, 0), "a Touch bottom is the ground reference"); }
    {   Box ramp = b; ramp.pitch = std::atan(0.1); const auto s = SurfaceAt(ramp, 0, 5, 3);
        Check(s.inside && s.touch && s.z > 0.3, "a pitched box gives the ramp's height ahead"); }
    {   Box air = b; FromKind(air, 6, 10); const auto s = SurfaceAt(air, 0, 0, 3);
        Check(s.inside && !s.touch, "open air touches nothing"); Check(Near(s.baseZ, -2.5), "and still gives the height to fly from"); }
    {   Box lift = b; lift.faces[Bottom] = State::Avoid; lift.faces[Back] = State::Touch; const auto s = SurfaceAt(lift, 0, 0, 3);
        Check(s.touch && s.wall && s.face == Back && Near(s.z, 3), "a Touch wall gives the context for rising up it"); }
    {   Box flat = b; FromKind(flat, 1, 0);
        Check(Near(flat.halfHeight, 2.5) && flat.faces[Bottom] == State::Touch && flat.faces[Top] == State::Avoid, "an old flat tile reads as a 5 m box, bottom Touch"); }
    {   Box tube = b; const double was = tube.halfHeight; Check(!FromKind(tube, 4, 8) && tube.halfHeight == was, "D11 not approved: an old tube gives no box and is left as it was");
        Box r = b; Check(!FromKind(r, 3, 8) && !FromKind(r, 5, 8), "nor rounded or trapezoid tiles"); Box bx = b; Check(FromKind(bx, 2, 6) && Near(bx.halfHeight, 3), "a box tile reads as itself"); }
    {   // D13: neighbouring boxes meet at a bend; straight neighbours and boxes far apart stay as drawn.
        fieldbox::Box a[2]; a[0].centre = { 0, 0, 0 }; a[0].halfLength = 5; a[0].halfWidth = 3; a[1].centre = { 3, 9, 0 }; a[1].halfLength = 5; a[1].halfWidth = 3; a[1].yaw = .5;
        fieldbox::JoinAtBends(a, 2); const double need = 3 * std::tan(.25);
        Check(std::fabs(a[0].halfLength - 5 - need) < 1e-9 && std::fabs(a[1].halfLength - 5 - need) < 1e-9, "a bend: both boxes lengthened to meet");
        fieldbox::Box s[2]; s[0].halfLength = 5; s[1].centre = { 0, 10, 0 }; s[1].halfLength = 5; fieldbox::JoinAtBends(s, 2); Check(s[0].halfLength == 5, "a straight: unchanged");
        fieldbox::Box far_[2]; far_[0].halfLength = 5; far_[1].centre = { 0, 200, 0 }; far_[1].halfLength = 5; far_[1].yaw = .5; fieldbox::JoinAtBends(far_, 2); Check(far_[0].halfLength == 5, "far apart: unchanged"); }
    std::printf("%s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
