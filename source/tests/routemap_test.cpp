#include "routemap.h"
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
    std::printf("routemap: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
