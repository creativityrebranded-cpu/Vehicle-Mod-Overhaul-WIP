#pragma once
// The local surface map's own camera maths (2026-10-28, traffic route editor). Pure: tests/routemap_test.cpp.
// From the 1.16.244 decompile (notes/traffic-route-editor-20261028/ENGINE_RESEARCH.md, "Local map"):
//  - StarMap::SurfaceMapState (vtable 0x144C99A00) slot 4 = its per-frame update FUN_1416DA0B0. state+0x78 is the map
//    camera rig; rig+0x20 the NiCamera; rig+0x30 the focus point and rig+0x50 the eye, both in world coordinates.
//  - Native markers (FUN_1416DF1A0): FUN_142BE45C0(camera+0x180 4x4 world->clip, camera+0x1E4 port {l, r, t, b}, world
//    point) gives x01 / y01 (y up), then stage x = (s.r - s.l) x01 + s.l, stage y = (s.b - s.t)(1 - y01) + s.t with the
//    state's rect at state+0xA0 {l, t, r, b}. Its divisor is |w| (mask 0x7FFFFFFF) and must exceed 1e-5.
//  - The custom marker (FUN_1416D8CF0 -> FUN_141A43220) writes the map's hit point straight into the marker reference's
//    position (+0x8C): the map scene is in world coordinates.
//  - The cursor (FUN_1416A8980): the global at 0x1461DE010 points to the cursor in pixels (+0x60, +0x64) and the screen
//    size (+0x70, +0x74); x01 = x / width, y01 = 1 - y / height (FUN_142BE3800).
#include <cmath>

namespace routemap {
struct Vec3 { double x = 0, y = 0, z = 0; };
struct Vec2 { double x = 0, y = 0; };
struct Camera {
    float m[16]{};            // NiCamera +0x180, row-major: clip = m * (x, y, z, 1)
    float port[4]{0, 1, 1, 0};  // NiCamera +0x1E4: left, right, top, bottom
    float stage[4]{0, 0, 1, 1}; // SurfaceMapState +0xA0: left, top, right, bottom (the movie's marker space)
    Vec3 focus, eye;          // rig +0x30, rig +0x50
    double mouseX = 0, mouseY = 0, width = 0, height = 0;  // the cursor in screen pixels and the screen size
};
inline bool Finite(Vec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
inline double Row(const Camera& c, int r, Vec3 p) { const float* m = c.m + 4 * r; return m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3]; }

// FUN_142BE45C0 exactly: normalised screen position (x01 right, y01 up) and depth.
inline bool Screen01(const Camera& c, Vec3 p, double& x01, double& y01, double& depth)
{
    if (!Finite(p)) return false;
    const double w = std::fabs(Row(c, 3, p));
    if (!(w > 1e-5)) return false;
    const double inv = 1.0 / w, l = c.port[0], r = c.port[1], t = c.port[2], b = c.port[3];
    x01 = (r - l) * 0.5 * (Row(c, 0, p) * inv) + (l + r) * 0.5;
    y01 = (t - b) * 0.5 * (Row(c, 1, p) * inv) + (b + t) * 0.5;
    depth = Row(c, 2, p) * inv;
    return std::isfinite(x01) && std::isfinite(y01) && std::isfinite(depth);
}
// FUN_1416DF1A0's mapping into the marker space; `inside` as its on-screen flag (0..1 on both axes).
inline bool ToStage(const Camera& c, Vec3 p, Vec2& out, bool* inside = nullptr)
{
    double x01, y01, depth;
    if (!Screen01(c, p, x01, y01, depth)) return false;
    out = { (c.stage[2] - c.stage[0]) * x01 + c.stage[0], (c.stage[3] - c.stage[1]) * (1.0 - y01) + c.stage[1] };
    if (inside) *inside = x01 >= 0 && x01 <= 1 && y01 >= 0 && y01 <= 1;
    return std::isfinite(out.x) && std::isfinite(out.y);
}
inline Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
// The line under a normalised screen point. On it row0 = xn * s * row3 and row1 = yn * s * row3, where s is the sign of
// row3 in front of the camera (taken at the focus point, which the rig always keeps in view). Origin: the point of the
// line nearest the eye; direction: away from the eye, toward the focus side.
inline bool Ray01(const Camera& c, double x01, double y01, Vec3& origin, Vec3& dir)
{
    const double l = c.port[0], r = c.port[1], t = c.port[2], b = c.port[3];
    if (!(std::fabs(r - l) > 1e-9) || !(std::fabs(t - b) > 1e-9)) return false;
    const double xn = (2 * x01 - (l + r)) / (r - l), yn = (2 * y01 - (t + b)) / (t - b);
    const double s = Row(c, 3, c.focus) < 0 ? -1.0 : 1.0;
    // Planes a.p + a4 = 0 for both rows.
    double a[4], e[4];
    for (int k = 0; k < 4; ++k) { a[k] = c.m[k] - xn * s * c.m[12 + k]; e[k] = c.m[4 + k] - yn * s * c.m[12 + k]; }
    const Vec3 na{ a[0], a[1], a[2] }, ne{ e[0], e[1], e[2] };
    Vec3 d = Cross(na, ne);
    const double len = std::sqrt(Dot(d, d));
    if (!(len > 1e-12)) return false;
    d = { d.x / len, d.y / len, d.z / len };
    // The line's point nearest the eye: minimise |p - eye| with both plane equations (closed form for two planes).
    const double da = Dot(na, c.eye) + a[3], de = Dot(ne, c.eye) + e[3];
    const double aa = Dot(na, na), ae = Dot(na, ne), ee = Dot(ne, ne), det = aa * ee - ae * ae;
    if (!(std::fabs(det) > 1e-18)) return false;
    const double ka = (da * ee - de * ae) / det, ke = (de * aa - da * ae) / det;
    origin = { c.eye.x - ka * na.x - ke * ne.x, c.eye.y - ka * na.y - ke * ne.y, c.eye.z - ka * na.z - ke * ne.z };
    const Vec3 view{ c.focus.x - c.eye.x, c.focus.y - c.eye.y, c.focus.z - c.eye.z };
    if (Dot(d, view) < 0) d = { -d.x, -d.y, -d.z };
    dir = d;
    return Finite(origin) && Finite(dir);
}
inline bool MouseRay(const Camera& c, Vec3& origin, Vec3& dir)
{
    if (!(c.width > 0) || !(c.height > 0)) return false;
    return Ray01(c, c.mouseX / c.width, 1.0 - c.mouseY / c.height, origin, dir);
}
// Where the cursor's ray crosses the height z (a waypoint dropped or dragged at its own height).
inline bool MouseAt(const Camera& c, double z, Vec3& out)
{
    Vec3 o, d;
    if (!MouseRay(c, o, d) || !std::isfinite(z) || std::fabs(d.z) < 1e-9) return false;
    const double t = (z - o.z) / d.z;
    if (!std::isfinite(t) || t < 0) return false;
    out = { o.x + t * d.x, o.y + t * d.y, z };
    return Finite(out);
}
// The cursor in the marker space (for the panel's hit tests).
inline bool MouseStage(const Camera& c, Vec2& out)
{
    if (!(c.width > 0) || !(c.height > 0)) return false;
    out = { (c.stage[2] - c.stage[0]) * (c.mouseX / c.width) + c.stage[0], (c.stage[3] - c.stage[1]) * (c.mouseY / c.height) + c.stage[1] };
    return true;
}
}
