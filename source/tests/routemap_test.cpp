#include "routemap.h"
#include "photoeditorinput.h"
#include "inputownership.h"
#include <cmath>
#include <cstdio>

using namespace routemap;
namespace {
// A look-at perspective camera written as one world->clip matrix; `flip` puts w = -z_view (the other sign convention),
// which the game's |w| divisor accepts as well.
Camera MakeCamera(Vec3 eye, Vec3 focus, double fovY, double aspect, bool flip)
{
    Vec3 f{ focus.x - eye.x, focus.y - eye.y, focus.z - eye.z };
    const double fl = std::sqrt(Dot(f, f)); f = { f.x / fl, f.y / fl, f.z / fl };
    Vec3 up{ 0, 0, 1 };
    if (std::fabs(Dot(f, up)) > 0.999) up = { 0, 1, 0 };
    Vec3 r = Cross(f, up); const double rl = std::sqrt(Dot(r, r)); r = { r.x / rl, r.y / rl, r.z / rl };
    const Vec3 u = Cross(r, f);
    const double sy = 1.0 / std::tan(fovY / 2), sx = sy / aspect, s = flip ? -1.0 : 1.0;
    // rows: x = sx r.(p - eye), y = sy u.(p - eye), z = f.(p - eye) (any depth row works), w = s f.(p - eye)
    const Vec3 rows[4] = { {sx * r.x, sx * r.y, sx * r.z}, {sy * u.x, sy * u.y, sy * u.z}, f, {s * f.x, s * f.y, s * f.z} };
    Camera c;
    for (int k = 0; k < 4; ++k) {
        c.m[4 * k] = float(rows[k].x); c.m[4 * k + 1] = float(rows[k].y); c.m[4 * k + 2] = float(rows[k].z);
        c.m[4 * k + 3] = float(-Dot(rows[k], eye));
    }
    c.port[0] = 0; c.port[1] = 1; c.port[2] = 1; c.port[3] = 0;
    c.stage[0] = 0; c.stage[1] = 0; c.stage[2] = 1920; c.stage[3] = 1080;
    c.focus = focus; c.eye = eye; c.width = 2560; c.height = 1440;
    return c;
}
}

int main()
{
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } };
    for (int flip = 0; flip < 2; ++flip) {
        const Vec3 focus{ -800, 1600, -160 }, eye{ -800, 1200, 400 };
        Camera c = MakeCamera(eye, focus, 0.9, 16.0 / 9.0, flip != 0);
        Vec2 s; bool inside = false;
        check(ToStage(c, focus, s, &inside) && inside && std::fabs(s.x - 960) < 0.5 && std::fabs(s.y - 540) < 0.5, "focus projects to the middle of the stage");
        // A point to the right and above the focus lands right and higher up on the stage (y grows downward).
        Vec2 a, b;
        check(ToStage(c, { focus.x + 50, focus.y, focus.z }, a) && a.x > 960, "east of the focus is right of centre");
        check(ToStage(c, { focus.x, focus.y, focus.z + 50 }, b) && b.y < 540, "higher is up on the stage");
        // Round trip: project a point, put the cursor there, intersect at its height: the same point.
        const Vec3 p{ -760, 1650, -100 };
        double x01, y01, depth;
        check(Screen01(c, p, x01, y01, depth), "point projects");
        c.mouseX = x01 * c.width; c.mouseY = (1 - y01) * c.height;
        Vec3 back;
        check(MouseAt(c, p.z, back) && std::fabs(back.x - p.x) < 0.05 && std::fabs(back.y - p.y) < 0.05, flip ? "cursor round trip (w = -z)" : "cursor round trip (w = +z)");
        Vec2 ms;
        check(MouseStage(c, ms) && std::fabs(ms.x - (c.mouseX / c.width) * 1920) < 1e-6, "cursor in marker space");
        // A drag at another height: the cursor's ray at z = 200 is still on the same line of sight.
        Vec3 high; Vec2 hs, ps;
        check(MouseAt(c, 200, high) && ToStage(c, high, hs) && ToStage(c, p, ps) && std::fabs(hs.x - ps.x) < 0.05 && std::fabs(hs.y - ps.y) < 0.05, "points along the ray share a screen spot");
        Vec3 o, d;
        check(MouseRay(c, o, d) && Dot(d, { focus.x - eye.x, focus.y - eye.y, focus.z - eye.z }) > 0, "ray points into the scene");
    }
    // Top-down view (the map's default pitch) and an orthographic camera (w row 0 0 0 1).
    {
        Camera c = MakeCamera({ 0, -1, 1000 }, { 0, 0, 0 }, 0.8, 1.6, false);
        c.mouseX = c.width * 0.75; c.mouseY = c.height * 0.5;
        Vec3 hit;
        check(MouseAt(c, 0, hit) && hit.x > 0, "top-down cursor right of centre hits east");
        Camera o{};
        o.m[0] = 0.01f; o.m[5] = 0.01f; o.m[10] = -0.001f; o.m[15] = 1;
        o.port[0] = -1; o.port[1] = 1; o.port[2] = 1; o.port[3] = -1;  // x01 = x_ndc etc.
        o.stage[2] = 100; o.stage[3] = 100; o.focus = { 0, 0, 0 }; o.eye = { 0, 0, 500 }; o.width = 100; o.height = 100;
        o.mouseX = 75; o.mouseY = 25;  // x01 0.75, y01 0.75 -> x_ndc 0.75 -> 75 m east, 75 m north
        Vec3 g;
        check(MouseAt(o, 10, g) && std::fabs(g.x - 75) < 1e-3 && std::fabs(g.y - 75) < 1e-3 && g.z == 10, "orthographic pick");
    }
    // Degenerate input is refused, never NaN.
    {
        Camera z{};
        Vec2 s; Vec3 h;
        check(!ToStage(z, { 1, 2, 3 }, s) && !MouseAt(z, 0, h), "empty camera refused");
        Camera c = MakeCamera({ 0, -100, 100 }, { 0, 0, 0 }, 0.8, 1.6, false);
        check(!ToStage(c, { NAN, 0, 0 }, s), "NaN refused");
    }
    {   // marker shapes: only what faces the eye is drawn, lit from above
        const Vec3 p{ 10, 20, 30 };
        const auto pad = Pad(p, 5, 1, 0);
        const auto above = Visible(pad, { 10, 20, 29.5 }, { 10, 20, 500 });
        check(pad.size() == 6 && above.size() == 1 && (*above[0].face)[0].z == 30, "a pad seen from straight above shows only its top");
        const auto slant = Visible(pad, { 10, 20, 29.5 }, { 500, 20, 500 });
        check(slant.size() == 2, "from an angle: its top and one side");
        check(slant[0].light > slant[1].light, "the top is lit more than the side");
        const auto turned = Pad(p, 5, 1, 1.5707963267948966);
        check(std::fabs(turned[0][0].x - 15) < 1e-9 && std::fabs(turned[0][0].y - 15) < 1e-9, "a pad turned a quarter clockwise has its front to the east");
        const auto cone = Cone(p, 4, 10, 3);
        check(cone.size() == 11 && Visible(cone, p, { 10, 20, 500 }).size() == 10, "a cone from above: all its sides, not its base");
        check(Visible(cone, p, { 10, 20, -500 }).size() == 1, "a cone from below: only its base");
        check(std::fabs((*Visible(cone, p, { 10, 20, 500 })[0].face)[2].z - 37) < 1e-9, "its point stands above the base");
        const auto gem = Diamond(p, 4);
        check(gem.size() == 8 && Visible(gem, p, { 10, 20, 500 }).size() == 4, "a diamond from above: its four upper faces");
        check(Visible(gem, p, { 500, 500, 30 }).size() == 4, "and from the side: the four that face that way");
    }
    for(bool flip : {false,true}) {
        auto c=MakeCamera({0,0,0},{0,10,0},.8,1.6,flip);
        Vec2 p; c.frontOnly=true;
        check(ToStage(c,{0,10,0},p),"photo front point projects with either matrix sign");
        check(!ToStage(c,{0,-10,0},p),"photo behind point is rejected with either matrix sign");
        c.frontOnly=false;
        check(ToStage(c,{0,-10,0},p),"legacy map projection remains compatible");
    }
    {
        using photoeditorinput::Block;
        check(!Block(false,false,false,0,0,'W'),"closed photo editor leaves camera alone");
        check(Block(true,false,false,0,0,'W'),"editing owns movement keys");
        check(!Block(true,true,false,0,0,'W'),"camera hold passes movement keys");
        check(Block(true,false,false,1,1,0xA00),"editing owns mouse look");
        check(!Block(true,true,false,1,1,0xA00),"camera hold passes mouse look");
        check(Block(true,false,false,2,4,0),"editing owns sticks");
        check(!Block(true,true,false,2,4,0),"camera hold passes sticks");
        check(Block(true,true,false,1,0,1),"RMB gesture must not lift camera");
        check(!Block(true,true,true,1,0,1),"Ctrl RMB deliberately lifts camera");

        check(Block(true,true,false,2,0,0x100)&&Block(true,false,false,2,0,0x100),"pad LB is the speed key, never the native tab switch");
        check(!Block(false,false,false,2,0,0x100),"editor closed: LB stays native");
        check(Block(true,true,false,0,0,' ')&&Block(true,true,false,0,0,'T')&&Block(true,true,false,0,0,0x1B)&&Block(true,true,false,0,0,0x26),"camera hold: Space, T, Esc, arrows never reach photo mode");
        check(Block(true,true,false,2,0,0x1000)&&Block(true,true,false,2,0,0x8000)&&Block(true,true,false,2,0,0x200),"camera hold: pad A, Y, RB never reach photo mode");
        check(!Block(true,true,false,0,0,'A')&&!Block(true,true,false,0,0,'D')&&!Block(true,true,false,1,0,0)&&!Block(true,true,false,1,0,8),"camera hold: A, D, LMB down, wheel pass");
        check(Block(true,true,false,2,0,0x40),"L3 gesture must not toggle helmet");
        check(!Block(true,true,false,2,0,9)&&!Block(true,true,false,2,0,10),"camera hold preserves triggers");
        std::uint8_t owner=0;
        check(!inputownership::Own(owner,true,true,false),"native camera key press delivered");
        inputownership::Own(owner,true,false,true);
        check(!inputownership::Own(owner,false,false,true),"native key release survives editing transition");
        check(inputownership::Own(owner,true,true,true),"edit key press captured");
        check(inputownership::Own(owner,false,false,false),"edit release remains captured after close");
    }
    for(bool flip : {false,true}) {
        auto c=MakeCamera({0,0,0},{0,10,0},.8,1.6,flip);c.frontOnly=true;
        const auto tri=ClipPolygon(c,{{0,20,0},{5,20,0},{0,-20,0}});
        bool all=tri.size()>=3;Vec2 p;for(const auto& v:tri)all=all&&ToStage(c,v,p);
        check(all,"photo triangle reaching behind the camera is cut, not dropped");
        Vec3 a{0,-30,0},b{0,30,0};
        check(ClipSegment(c,a,b)&&ToStage(c,a,p)&&ToStage(c,b,p)&&a.y>0,"segment through the camera is cut at its front part");
        Vec3 d{0,-30,0},e{1,-40,0};
        check(!ClipSegment(c,d,e),"segment wholly behind the camera is skipped");
        c.frontOnly=false;
        check(ClipPolygon(c,{{0,20,0},{5,20,0},{0,-20,0}}).size()==3,"legacy map shapes are left as they were");
    }
    for(bool flip : {false,true}) {
        const Vec3 eye{-1057,1574,-146},focus{-1050,1600,-180};
        auto c=MakeCamera(eye,focus,.8,16.0/9,flip);
        Vec3 e,f;
        check(ViewFromMatrix(c,e,f),"view recovered from the clip matrix");
        check(std::fabs(e.x-eye.x)<1e-2&&std::fabs(e.y-eye.y)<1e-2&&std::fabs(e.z-eye.z)<1e-2,"matrix eye is the camera position");
        const Vec3 want{focus.x-eye.x,focus.y-eye.y,focus.z-eye.z};const double l=std::sqrt(Dot(want,want));
        check(Dot(f,{want.x/l,want.y/l,want.z/l})>.999,"matrix forward points at the view, either matrix sign");
    }
    for(bool flip : {false,true}) {
        // A camera-relative matrix (eye at the origin) re-based onto the world eye draws world points where they are.
        const Vec3 eye{-1057,1574,-146},focus{-1050,1600,-180};
        auto world=MakeCamera(eye,focus,.8,16.0/9,flip);
        auto rel=MakeCamera({0,0,0},{focus.x-eye.x,focus.y-eye.y,focus.z-eye.z},.8,16.0/9,flip);
        Vec3 e,f;ViewFromMatrix(rel,e,f);
        ShiftMatrix(rel,{e.x-eye.x,e.y-eye.y,e.z-eye.z});
        Vec2 a,b;const Vec3 p{-1053,1681,-149};
        check(ToStage(world,p,a)&&ToStage(rel,p,b)&&std::fabs(a.x-b.x)<.5&&std::fabs(a.y-b.y)<.5,"re-based camera-relative matrix draws the route where it was built");
    }
    std::printf("routemap: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
