#pragma once
// How a route's line ends (2026-11-06, format agreed by the editor and driving sessions; notes/VEHICLE_DECISION_DESIGN_20261106.md
// section 4). Keys, additive: `line_end = pass | stop | arrive` and, for arrive, `line_arrive = x y z heading park` (metres, degrees
// clockwise from north, 0 or 1). A route without the keys reads exactly as before: pass. Only an open path ends; a loop never does, but a
// stored value is kept as written. Text this build cannot read is kept whole and written back unchanged. Pure; tests: tests/routeend_test.cpp.
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace routeend {
enum End { Pass = 0, Stop = 1, Arrive = 2 };
inline const char* Token(int e) { return e == Stop ? "stop" : e == Arrive ? "arrive" : "pass"; }
inline const char* Name(int e) { return e == Stop ? "Stop" : e == Arrive ? "Arrive" : "Pass"; }
struct Place { double x = 0, y = 0, z = 0, heading = 0; int park = 0; };
struct Plan {
    int end = Pass;
    bool hasPlace = false;       // line_arrive was read or set
    Place place;
    std::string endUnsupported, placeUnsupported;  // text this build cannot read
};
inline bool ValidPlace(const Place& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && std::fabs(p.x) < 1e8 && std::fabs(p.y) < 1e8 && std::fabs(p.z) < 1e8 &&
           std::isfinite(p.heading) && p.heading >= -360 && p.heading <= 360 && (p.park == 0 || p.park == 1);
}
inline std::string Trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
// Both keys as read from the file (empty = missing).
inline Plan Read(const std::string& endText, const std::string& placeText) {
    Plan p;
    std::string e = Trim(endText); for (auto& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (e.empty() || e == "pass") p.end = Pass; else if (e == "stop") p.end = Stop; else if (e == "arrive") p.end = Arrive; else p.endUnsupported = endText;
    const std::string t = Trim(placeText);
    if (!t.empty()) {
        std::istringstream in(t); Place q; std::string extra;
        if ((in >> q.x >> q.y >> q.z >> q.heading >> q.park) && !(in >> extra) && ValidPlace(q)) { p.place = q; p.hasPlace = true; }
        else p.placeUnsupported = placeText;
    }
    return p;
}
inline std::string Num(double v) { std::ostringstream o; o << std::setprecision(17) << v; return o.str(); }
// The lines to write, each ending in "\n"; nothing for a plain pass with no place.
inline std::string Lines(const Plan& p) {
    std::string out;
    if (!p.endUnsupported.empty()) out += "line_end = " + Trim(p.endUnsupported) + "\n";
    else if (p.end != Pass) out += std::string("line_end = ") + Token(p.end) + "\n";
    if (!p.placeUnsupported.empty()) out += "line_arrive = " + Trim(p.placeUnsupported) + "\n";
    else if (p.hasPlace) out += "line_arrive = " + Num(p.place.x) + " " + Num(p.place.y) + " " + Num(p.place.z) + " " + Num(p.place.heading) + " " + std::to_string(p.place.park) + "\n";
    return out;
}
// An anchored city laid down elsewhere: the place moves with the route's points.
inline void Shift(Plan& p, double dx, double dy, double dz) { if (p.hasPlace) { p.place.x += dx; p.place.y += dy; p.place.z += dz; } }
}
