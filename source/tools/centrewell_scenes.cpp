// CW-1 step 2, part 2 (notes/centreline-well-20261122/PLAN.md section 7.3): re-solves the scenes the energy planner recorded when a car stood
// or was blocked (SD-1 dumps: OBS system=plan.scene ... event=begin / line / shape / end) with the centre-line well, and prints what the well
// would have done there beside what the old planner chose. Offline tool; reads log files given on the command line.
//
// Mapping (the dump's own roles, planinput::AddShapes): hard -> a hard obstacle when it is inside the car's body band (above the climb
// height over the car's base, below its roof); person (any role but skip) -> hard; soft -> a soft push; mover and skip_* -> nothing
// (movers are the speed layer's).
#include "centrewell.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace centrewell;
double Num(const std::string& l, const char* key, double fallback = NAN) {
    const auto at = l.find(key); if (at == std::string::npos) return fallback; return std::strtod(l.c_str() + at + std::strlen(key), nullptr);
}
bool Triple(const std::string& l, const char* key, double& a, double& b, double& c) {
    const auto at = l.find(key); if (at == std::string::npos) return false;
    const char* p = l.c_str() + at + std::strlen(key); char* e = nullptr;
    a = std::strtod(p, &e); if (*e != ',') return false; b = std::strtod(e + 1, &e); if (*e != ',') return false; c = std::strtod(e + 1, &e); return true;
}
std::string Word(const std::string& l, const char* key) {
    const auto at = l.find(key); if (at == std::string::npos) return ""; std::size_t s = at + std::strlen(key), e = s;
    if (s < l.size() && l[s] == '"') { e = l.find('"', s + 1); return l.substr(s + 1, e == std::string::npos ? std::string::npos : e - s - 1); }
    while (e < l.size() && l[e] != ' ') ++e; return l.substr(s, e - s);
}
struct Shape { std::string src, role; double x, y, z, hl, hw, hh, heading, top, base; };
struct Scene {
    std::string file, time, vehicle, why, plan, feasible; int dump = -1;
    double cx = 0, cy = 0, cz = 0, heading = 0, hw = 1, hl = 2, height = 1.5, speed = 0, half = 3.5;
    double floorStep = 0; int floorUnknown = 0;  // the wanted floor: largest measured height change from the car's base; samples not measured
    std::vector<std::pair<double, std::array<double, 2>>> line;  // d -> x, y
    std::vector<Shape> shapes;
};
constexpr double kClimb = 0.3;  // m a wheel rolls over (carcap's car record is about half a tyre)
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: centrewell_scenes <log> [<log> ...]\n"); return 2; }
    std::vector<Scene> scenes; std::map<int, std::size_t> open;
    for (int a = 1; a < argc; ++a) {
        std::ifstream in(argv[a], std::ios::binary); std::string l;
        open.clear();
        while (std::getline(in, l)) {
            if (l.find("system=plan.scene") == std::string::npos) continue;
            const int dump = int(Num(l, " dump=", -1)); if (dump < 0) continue;
            const std::string ev = Word(l, " event=");
            if (ev == "begin") {
                Scene s; s.file = argv[a]; s.time = l.substr(1, 12); s.vehicle = Word(l, " vehicle="); s.why = Word(l, " why="); s.dump = dump;
                Triple(l, " car=", s.cx, s.cy, s.cz); s.heading = Num(l, " heading=", 0); s.hw = Num(l, " half_width=", 1); s.hl = Num(l, " half_length=", 2);
                s.height = Num(l, " height=", 1.5); s.speed = Num(l, " speed=", 0);
                open[dump] = scenes.size(); scenes.push_back(s);
            } else if (open.count(dump)) {
                auto& s = scenes[open[dump]];
                if (ev == "floor") {  // the wanted floor along the lane: a measured sample far from the car's base is a step or a drop
                    if (Num(l, " have=", 0) > 0 && Num(l, " measured=", 0) > 0) { const double z = Num(l, " z=", s.cz); s.floorStep = std::max(s.floorStep, std::fabs(z - s.cz)); }
                    else ++s.floorUnknown;
                }
                else if (ev == "line") { double x, y, z; if (Triple(l, " at=", x, y, z)) s.line.push_back({ Num(l, " d=", 0), { x, y } }); s.half = Num(l, " half=", s.half); }
                else if (ev == "shape") {
                    Shape sh; sh.src = Word(l, " src="); sh.role = Word(l, " role=");
                    Triple(l, " centre=", sh.x, sh.y, sh.z); Triple(l, " half=", sh.hl, sh.hw, sh.hh);
                    sh.heading = Num(l, " heading=", 0); sh.top = Num(l, " top=", sh.z); sh.base = Num(l, " base=", sh.z); s.shapes.push_back(sh);
                } else if (ev == "end") { s.plan = Word(l, " plan="); s.feasible = Word(l, " feasible="); open.erase(dump); }
            }
        }
    }
    int nCentre = 0, nDodge = 0, nBlock = 0, nBad = 0, nRejoin = 0, nFloor = 0; std::map<std::string, int> blockBy;
    std::printf("%-14s %-9s %-26s %-22s %-8s %5s %6s %7s %8s %s\n", "time", "vehicle", "old why", "old plan", "feasible", "hard", "soft", "result", "detail", "blocked by / floor");
    for (const auto& s : scenes) {
        Line line; for (const auto& p : s.line) if (p.first >= -8) line.Add(p.second[0], p.second[1]);
        if (line.x.size() < 3) { ++nBad; continue; }
        Problem p; double station = 0;
        Frame(p, line, s.cx, s.cy, s.heading, s.half, s.hw, std::min(kMaxStations, int(line.Length()) + 1), 1.0, station);
        std::vector<Obstacle> obs; std::vector<std::string> obsSrc; int soft = 0;
        for (std::size_t i = 0; i < s.shapes.size(); ++i) {
            const auto& sh = s.shapes[i];
            if (sh.role.rfind("skip", 0) == 0 || sh.role == "mover") continue;
            const bool person = sh.src == "person";
            const bool inBand = sh.top > s.cz + kClimb && sh.base < s.cz + s.height;
            auto o = FromBox(line, station, unsigned(i + 1), sh.x, sh.y, sh.heading, std::max(.05, sh.hl), std::max(.05, sh.hw), s.hw + (person ? .6 : .3), s.hl);
            if (o.s1 < -1.0 || o.s0 > p.n) continue;
            if (sh.role == "hard" && (inBand || person)) { obs.push_back(o); obsSrc.push_back(sh.src); }
            else if (sh.role == "soft" || (sh.role == "hard" && !inBand)) {
                if (sh.role == "hard") continue;  // below the climb height or above the roof: never in the body's way
                const double mid = .5 * (o.left + o.right); const int side = mid > 0 ? -1 : +1;
                const int i0 = std::max(0, StationOf(o.s0, 1.0)), i1 = std::min(p.n - 1, StationOf(o.s1, 1.0) + 1);
                for (int k = i0; k <= i1; ++k) p.Soft(k, side, side > 0 ? o.right : o.left, .3);
                ++soft;
            }
        }
        const auto sides = Choose(p, obs); ApplySides(p, obs, sides);
        const auto g = SolveWithinGrip(p, std::max(3.0, s.speed), 3.5);
        std::string result, detail, by;
        if (g.r.blockAt >= 0) {
            result = "BLOCK"; detail = std::to_string(g.r.blockAt) + "m"; { char b[96]; const int bi = g.r.blockAt; std::snprintf(b, sizeof b, "[lo=%.2f hi=%.2f y0=%.2f lane=%.2f car_w=%.2f] ", p.lo[bi], p.hi[bi], p.y0, s.half, 2 * s.hw); by += b; } ++nBlock; { int tc = 0; for (const auto& c : sides) tc += c.touching ? 1 : 0; if (tc) by += "(touching " + std::to_string(tc) + ") "; }
            // which obstacles bound that station
            std::map<std::string, int> here;
            for (std::size_t k = 0; k < obs.size(); ++k) { const int i0 = StationOf(obs[k].s0, 1.0), i1 = StationOf(obs[k].s1, 1.0) + 1; if (g.r.blockAt >= i0 && g.r.blockAt <= i1) ++here[obsSrc[k]]; }
            for (const auto& [k, v] : here) { by += k + "x" + std::to_string(v) + " "; ++blockBy[k]; }
            if (here.empty()) { by = "lane_edges"; ++blockBy["lane_edges"]; }
        } else if (g.r.maxOffset < .05) { result = "CENTRE"; ++nCentre; }
        else if (g.r.maxOffset <= std::fabs(p.y0) + .05 || (obs.empty() && soft == 0)) { char b[32]; std::snprintf(b, sizeof b, "y0=%.2f", p.y0); result = "REJOIN"; detail = b; ++nRejoin; }
        else { char b[32]; std::snprintf(b, sizeof b, "%.2fm", g.r.maxOffset); result = "DODGE"; detail = b; ++nDodge; { int tc = 0; for (const auto& c : sides) tc += c.touching ? 1 : 0; if (tc) by += "(touching " + std::to_string(tc) + ") "; } }
        if (s.floorStep > 1.0) { char b[48]; std::snprintf(b, sizeof b, " floor_step=%.1fm", s.floorStep); by += b; ++nFloor; }
        std::printf("%-14s %-9s %-26.26s %-22.22s %-8s %5zu %6d %7s %8s %s\n", s.time.c_str(), s.vehicle.c_str(), s.why.c_str(), s.plan.c_str(), s.feasible.c_str(), obs.size(), soft, result.c_str(), detail.c_str(), by.c_str());
    }
    std::printf("\nscenes %zu: centre line %d, rejoin %d, dodge %d, block %d, unreadable %d; floor step over 1 m ahead in %d\nblocked by:", scenes.size(), nCentre, nRejoin, nDodge, nBlock, nBad, nFloor);
    for (const auto& [k, v] : blockBy) std::printf(" %s=%d", k.c_str(), v);
    std::printf("\n");
    return 0;
}
