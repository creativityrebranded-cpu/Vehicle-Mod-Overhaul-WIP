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
#include <algorithm>
#include <cmath>
#include <vector>

namespace routemap {
struct Vec3 { double x = 0, y = 0, z = 0; };
struct Vec2 { double x = 0, y = 0; };
struct Camera {
    float m[16]{};            // NiCamera +0x180, row-major: clip = m * (x, y, z, 1)
    float port[4]{0, 1, 1, 0};  // NiCamera +0x1E4: left, right, top, bottom
    float stage[4]{0, 0, 1, 1}; // SurfaceMapState +0xA0: left, top, right, bottom (the movie's marker space)
    Vec3 focus, eye;          // rig +0x30, rig +0x50
    double mouseX = 0, mouseY = 0, width = 0, height = 0;  // the cursor in screen pixels and the screen size
    bool frontOnly = false;   // perspective scene view; preserve the legacy map mapping
};
inline bool Finite(Vec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
inline double Row(const Camera& c, int r, Vec3 p) { const float* m = c.m + 4 * r; return m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3]; }

// FUN_142BE45C0 exactly: normalised screen position (x01 right, y01 up) and depth.
inline bool Screen01(const Camera& c, Vec3 p, double& x01, double& y01, double& depth)
{
    if (!Finite(p)) return false;
    const double signedW = Row(c, 3, p);
    if (c.frontOnly && signedW * Row(c, 3, c.focus) <= 0) return false;
    const double w = std::fabs(signedW);
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
// The eye and view direction implied by the clip matrix itself (2026-11-04 run: the World Camera's transform row +0x90
// read ~0.98 up while the view looked down, so a transform-based eye/forward disagreed with the matrix). Eye: the point
// where rows 0, 1 and 3 are all zero. Forward: row 3's direction, signed so a point ahead has depth (row 2 / |w|) in
// 0..1 (the other side falls outside it for both normal and reversed depth); if depth cannot tell, the side with w > 0.
inline bool ViewFromMatrix(const Camera& c, Vec3& eye, Vec3& forward)
{
    const float* m = c.m;
    const double a[3][3] = { { m[0], m[1], m[2] }, { m[4], m[5], m[6] }, { m[12], m[13], m[14] } };
    const double r[3] = { -double(m[3]), -double(m[7]), -double(m[15]) };
    auto det3 = [](const double q[3][3]) { return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) + q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]); };
    const double d = det3(a);
    if (!(std::fabs(d) > 1e-12)) return false;
    double out[3];
    for (int k = 0; k < 3; ++k) { double q[3][3]; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) q[i][j] = j == k ? r[i] : a[i][j]; out[k] = det3(q) / d; }
    eye = { out[0], out[1], out[2] };
    const double len = std::sqrt(a[2][0] * a[2][0] + a[2][1] * a[2][1] + a[2][2] * a[2][2]);
    if (!(len > 1e-12) || !Finite(eye)) return false;
    forward = { a[2][0] / len, a[2][1] / len, a[2][2] / len };
    auto depthOk = [&](Vec3 p) { const double w = Row(c, 3, p); if (!(std::fabs(w) > 1e-9)) return false; const double z = Row(c, 2, p) / std::fabs(w); return z >= -1e-3 && z <= 1.001; };
    const Vec3 plus{ eye.x + forward.x * 10, eye.y + forward.y * 10, eye.z + forward.z * 10 }, minus{ eye.x - forward.x * 10, eye.y - forward.y * 10, eye.z - forward.z * 10 };
    const bool p = depthOk(plus), n = depthOk(minus);
    if (n && !p) forward = { -forward.x, -forward.y, -forward.z };
    else if (p == n && Row(c, 3, plus) < 0) forward = { -forward.x, -forward.y, -forward.z };
    return true;
}
// Re-base a clip matrix so world point p is drawn as the matrix would draw p + shift (t' = t + R shift). Used when the
// matrix's own eye is not at the camera's world position (camera-relative or other-origin rendering): shift = matrix
// eye - world eye puts the matrix eye on the world eye, and everything else follows exactly.
inline void ShiftMatrix(Camera& c, Vec3 shift)
{
    for (int r = 0; r < 4; ++r) c.m[4 * r + 3] = float(c.m[4 * r + 3] + c.m[4 * r] * shift.x + c.m[4 * r + 1] * shift.y + c.m[4 * r + 2] * shift.z);
}
// Photo view (frontOnly): a shape that crosses the camera plane is cut there instead of dropped. Planes, all
// linear in world space so cutting in world space is exact: in front (w >= kNearW) and |ndc x|, |ndc y| <= kGuard
// (keeps cut points to a few screen widths). The legacy map view is left as it was (input returned unchanged).
constexpr double kNearW = 0.05, kGuard = 4.0;
inline double ClipPlane(const Camera& c, int i, Vec3 p)
{
    const double s = Row(c, 3, c.focus) >= 0 ? 1.0 : -1.0, w = s * Row(c, 3, p);
    switch (i) {
    case 0: return w - kNearW;
    case 1: return kGuard * w - Row(c, 0, p);
    case 2: return kGuard * w + Row(c, 0, p);
    case 3: return kGuard * w - Row(c, 1, p);
    default: return kGuard * w + Row(c, 1, p);
    }
}
inline std::vector<Vec3> ClipPolygon(const Camera& c, std::vector<Vec3> poly)
{
    if (!c.frontOnly) return poly;
    for (int plane = 0; plane < 5 && !poly.empty(); ++plane) {
        std::vector<Vec3> out;
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const Vec3 a = poly[i], b = poly[(i + 1) % poly.size()];
            const double da = ClipPlane(c, plane, a), db = ClipPlane(c, plane, b);
            if (da >= 0) out.push_back(a);
            if ((da >= 0) != (db >= 0)) { const double t = da / (da - db); out.push_back({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t }); }
        }
        poly = std::move(out);
    }
    return poly;
}
// The part of segment a-b that is in view; false when none of it is.
inline bool ClipSegment(const Camera& c, Vec3& a, Vec3& b)
{
    if (!c.frontOnly) return true;
    double t0 = 0, t1 = 1;
    const Vec3 a0 = a, d{ b.x - a.x, b.y - a.y, b.z - a.z };
    for (int plane = 0; plane < 5; ++plane) {
        const double da = ClipPlane(c, plane, a0), db = ClipPlane(c, plane, b);
        if (da < 0 && db < 0) return false;
        if (da < 0) t0 = std::max(t0, da / (da - db));
        else if (db < 0) t1 = std::min(t1, da / (da - db));
    }
    if (t0 > t1) return false;
    a = { a0.x + d.x * t0, a0.y + d.y * t0, a0.z + d.z * t0 };
    b = { a0.x + d.x * t1, a0.y + d.y * t1, a0.z + d.z * t1 };
    return true;
}
inline Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
// ---- Marker shapes (2026-10-30, the user: each kind of placed thing has its own 3D look): convex solids in world coordinates ----
using Face = std::vector<Vec3>;
using Solid = std::vector<Face>;
// A flat pad: its top at p.z, `half` metres each way, `thick` deep, turned `radians` clockwise from north.
inline Solid Pad(Vec3 p, double half, double thick, double radians)
{
    const double ax = std::sin(radians), ay = std::cos(radians);
    auto at = [&](double along, double across, double z) { return Vec3{ p.x + along * ax + across * ay, p.y + along * ay - across * ax, z }; };
    const double t = p.z, b = p.z - thick;
    const Vec3 c[4] = { at(half, half, t), at(half, -half, t), at(-half, -half, t), at(-half, half, t) };
    const Vec3 d[4] = { at(half, half, b), at(half, -half, b), at(-half, -half, b), at(-half, half, b) };
    Solid s{ { c[0], c[1], c[2], c[3] }, { d[3], d[2], d[1], d[0] } };
    for (int i = 0; i < 4; ++i) s.push_back({ c[i], d[i], d[(i + 1) % 4], c[(i + 1) % 4] });
    return s;
}
// A cone standing on its base: the base `below` metres under p.z, the point `height` metres above the base.
inline Solid Cone(Vec3 p, double radius, double height, double below)
{
    constexpr int kSides = 10;
    Solid s;
    Face base;
    const Vec3 tip{ p.x, p.y, p.z - below + height };
    auto rim = [&](int i) { const double a = i * 6.283185307179586 / kSides; return Vec3{ p.x + radius * std::cos(a), p.y + radius * std::sin(a), p.z - below }; };
    for (int i = 0; i < kSides; ++i) { base.push_back(rim(i)); s.push_back({ rim(i), rim(i + 1), tip }); }
    s.push_back(base);
    return s;
}
// A diamond: eight faces round p.
inline Solid Diamond(Vec3 p, double r)
{
    const Vec3 top{ p.x, p.y, p.z + r * 1.25 }, low{ p.x, p.y, p.z - r * 1.25 };
    const Vec3 m[4] = { { p.x + r, p.y, p.z }, { p.x, p.y + r, p.z }, { p.x - r, p.y, p.z }, { p.x, p.y - r, p.z } };
    Solid s;
    for (int i = 0; i < 4; ++i) { s.push_back({ m[i], m[(i + 1) % 4], top }); s.push_back({ m[i], m[(i + 1) % 4], low }); }
    return s;
}
struct Lit { const Face* face = nullptr; double light = 0; };
// The faces of a convex solid that face the eye, each with its light 0 .. 1 (lit from above and to one side).
inline std::vector<Lit> Visible(const Solid& solid, Vec3 centre, Vec3 eye)
{
    std::vector<Lit> out;
    const Vec3 sun{ -0.40, -0.30, 0.866 };
    for (const auto& f : solid) {
        if (f.size() < 3) continue;
        Vec3 mid{};
        for (const auto& v : f) { mid.x += v.x; mid.y += v.y; mid.z += v.z; }
        const double n = double(f.size());
        mid = { mid.x / n, mid.y / n, mid.z / n };
        Vec3 normal = Cross({ f[1].x - f[0].x, f[1].y - f[0].y, f[1].z - f[0].z }, { f[2].x - f[0].x, f[2].y - f[0].y, f[2].z - f[0].z });
        const double len = std::sqrt(Dot(normal, normal));
        if (!(len > 1e-12)) continue;
        normal = { normal.x / len, normal.y / len, normal.z / len };
        if (Dot(normal, { mid.x - centre.x, mid.y - centre.y, mid.z - centre.z }) < 0) normal = { -normal.x, -normal.y, -normal.z };  // outward
        if (Dot(normal, { eye.x - mid.x, eye.y - mid.y, eye.z - mid.z }) <= 0) continue;
        out.push_back({ &f, 0.5 + 0.5 * Dot(normal, sun) });
    }
    return out;
}
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
