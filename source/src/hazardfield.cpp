// Hazard field, game side (hazardfield.h). Reads this world's route files the way routesurvey.cpp does (trafficprofile::Routes(), trafficdata)
// and keeps their hazard boxes. Only re-read when a file changed, the world changed or the editor saved. No engine call, no game object.
#include "hazardfield.h"
#include "log.h"
#include "trafficdata.h"
#include "trafficprofile.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

extern "C" int FVO_WorldName(char* out, int size);  // context.cpp

namespace hazardfield {
namespace {
std::mutex g_lock;
std::vector<Entry> g_all;
std::string g_world, g_dir; std::filesystem::file_time_type g_newest{}; int g_files = -1; ULONGLONG g_checkAt = 0;
std::atomic<bool> g_dirty{ true };
bool ReadText(const std::filesystem::path& p, std::string& out) { std::ifstream f(p, std::ios::binary); if (!f) return false; std::ostringstream s; s << f.rdbuf(); out = s.str(); return true; }
struct Watch { std::string route; ULONGLONG since = 0; };
std::unordered_map<unsigned, Watch> g_test;  // vehicle -> the route it is testing (a test flies the route once: the watch ends after 20 minutes)
std::vector<Suggestion> g_suggest;
}
void TestSet(unsigned vehicle, const std::string& route) { std::lock_guard lk(g_lock); g_test[vehicle] = { route, GetTickCount64() }; }
void TestClear(unsigned vehicle) { std::lock_guard lk(g_lock); g_test.erase(vehicle); }
void Invalidate() { g_dirty.store(true); }
void Tick(bool loading) {
    if (loading) return;
    const auto now = GetTickCount64(); if (now < g_checkAt) return; g_checkAt = now + 1500;
    char w[256]{}; if (!FVO_WorldName(w, sizeof w)) return;  // interiors / no player: keep what there is
    try {
        const std::wstring dirW = trafficprofile::Routes(); std::filesystem::file_time_type latest{}; int count = 0;
        for (const auto& e : std::filesystem::directory_iterator(dirW)) { if (!e.is_regular_file() || e.path().extension() != ".ini") continue; ++count; latest = std::max(latest, e.last_write_time()); }
        const std::string dir(dirW.begin(), dirW.end());
        if (!g_dirty.exchange(false) && g_world == w && g_dir == dir && g_newest == latest && g_files == count) return;
        trafficdata::Data d;
        for (const auto& e : std::filesystem::directory_iterator(dirW)) {
            if (!e.is_regular_file() || e.path().extension() != ".ini") continue;
            std::string text; if (ReadText(e.path(), text)) trafficdata::Read(text, e.path().filename().string(), d);
        }
        trafficdata::PreferEditor(d);
        std::vector<Entry> all;
        for (const auto& r : d.routes) {
            if (r.removed || r.hazards.boxes.empty() || !trafficdata::SameWorld(d, r.world, w)) continue;
            for (const auto& b : r.hazards.boxes) all.push_back({ b, r.name });
        }
        const std::size_t total = all.size();
        { std::lock_guard lk(g_lock); g_all = std::move(all); g_world = w; g_dir = dir; g_newest = latest; g_files = count; }
        logger::Write("OBS system=world.hazard event=read world=%s boxes=%zu files=%d reason=route_files_changed_or_saved", w, total, count);
    } catch (const std::exception& error) { g_files = -1; logger::Write("OBS system=world.hazard event=unread reason=%s", error.what()); }
}
std::vector<Entry> Snapshot() { std::lock_guard lk(g_lock); return g_all; }
int NearNow(double x, double y, double zLo, double zHi, double radius, routehazard::Box* out, int max) {
    std::lock_guard lk(g_lock); return Near(g_all, x, y, zLo, zHi, radius, out, max);
}
int AppendNow(double x, double y, double zLo, double zHi, double radius, double now, std::vector<planworld::Shape>& shapes, std::size_t cap) {
    std::lock_guard lk(g_lock); return AppendShapes(g_all, x, y, zLo, zHi, radius, now, shapes, cap);
}
bool IsTest(unsigned vehicle, std::string* route) {
    std::lock_guard lk(g_lock); const auto it = g_test.find(vehicle); if (it == g_test.end()) return false; if (GetTickCount64() - it->second.since > 1200000) { g_test.erase(it); return false; } if (route) *route = it->second.route; return true;
}
void NoteSighting(unsigned vehicle, const routehazard::Box& b, double now) {
    std::lock_guard lk(g_lock); const auto it = g_test.find(vehicle); if (it == g_test.end()) return; Sight(g_suggest, b, it->second.route, now);
}
std::vector<Suggestion> Suggestions(const std::string& route) {
    std::lock_guard lk(g_lock); std::vector<Suggestion> out;
    for (const auto& s : g_suggest) if (s.route == route && s.seen >= kSeenToShow) out.push_back(s);
    return out;
}
void Dismiss(const std::string& route, std::size_t index) {
    std::lock_guard lk(g_lock); std::size_t k = 0;
    for (auto it = g_suggest.begin(); it != g_suggest.end(); ++it) if (it->route == route && it->seen >= kSeenToShow) { if (k++ == index) { g_suggest.erase(it); return; } }
}
void ClearSuggestions(const std::string& route) {
    std::lock_guard lk(g_lock); g_suggest.erase(std::remove_if(g_suggest.begin(), g_suggest.end(), [&](const Suggestion& s) { return s.route == route; }), g_suggest.end());
}
}
// The add-on names the vehicle that flies a route test (route empty or null: it stopped). Additive export; older add-ons never call it.
extern "C" __declspec(dllexport) void FVO_HazardTestV1(unsigned vehicle, const char* route) {
    if (route && *route) hazardfield::TestSet(vehicle, route); else hazardfield::TestClear(vehicle);
}
