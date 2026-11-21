#pragma once
// Spawn flow step 7, Health: the record store (notes/health-records-20261107/CHANGE_CARDS.md HR1, HR2).
// One general rule for every subject and stage. A pair reports a pass or a fail at a stage (seat, body, release, drive) against its
// model and its spot; the caller only reports a fail when the stage could be tested. A model is blamed only when its fails come from
// several spots, a spot only when its fails come from several models; anything else is "unexplained" (likely our own bug), never a verdict.
// Rejected: the Wilson upper bound of the success rate is below rules::rejectBelow after rules::minTries tries.
// Pure: no engine calls, no files (the caller reads and writes the text). Tests: tests/spawnhealth_test.cpp.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace spawnhealth {
namespace rules {
inline constexpr double z = 1.0;                 // Wilson bound width
inline constexpr double rejectBelow = .35;       // rejected when even the hopeful success rate is below this
inline constexpr int minTries = 2;               // at least this many tries at a stage before any verdict
inline constexpr int blameSpread = 2;            // a model's fails must come from this many spots (a spot's from this many models)
inline constexpr double preferOdds = .4;         // Demand: a model under these odds is used only when nothing better is allowed
inline constexpr int retrialEvery = 10;          // a rejected subject gets one trial in this many asks
inline constexpr int keepWith = 8;               // distinct counterparts kept per stage
inline constexpr double ageKeep = .5;            // a new build keeps this share of the old counts
inline constexpr unsigned long long seatProveMs = 10000;  // minimum observed play time for a terminal seat failure
inline constexpr double driveProof = 50.0;       // m driven after release: the drive stage passed
}
enum class Stage : int { Seat, Body, Release, Drive, Count };
inline const char* Name(Stage s) { constexpr const char* n[]{ "seat", "body", "release", "drive" }; return int(s) >= 0 && int(s) < 4 ? n[int(s)] : "none"; }

enum class SeatResult { Pending, Pass, Fail };
inline SeatResult JudgeSeatEvidence(bool sitting, bool terminalSeatFailure, bool tested) {
    if (sitting) return SeatResult::Pass;
    return terminalSeatFailure && tested ? SeatResult::Fail : SeatResult::Pending;
}
inline bool DriveProved(double dx, double dy) { return std::hypot(dx, dy) >= rules::driveProof; }

struct Tally { double pass = 0, fail = 0; std::vector<std::string> with; };  // with: distinct counterparts of the fails
inline double Upper(double pass, double tries) {
    if (tries <= 0) return 1; const double p = pass / tries, z2 = rules::z * rules::z;
    return (p + z2 / (2 * tries) + rules::z * std::sqrt(p * (1 - p) / tries + z2 / (4 * tries * tries))) / (1 + z2 / tries);
}
struct Record {
    Tally stage[int(Stage::Count)];
    bool rejected = false, allowed = false; int why = -1; int asks = 0; int fp = 0; std::string build;
    double Tries(int s) const { return stage[s].pass + stage[s].fail; }
    // The weakest stage's odds, (pass + 1) / (tries + 2); unknown = .5.
    double Odds() const { double o = 1; bool any = false; for (int s = 0; s < int(Stage::Count); ++s) if (Tries(s) > 0) { any = true; o = std::min(o, (stage[s].pass + 1) / (Tries(s) + 2)); } return any ? o : .5; }
};
inline std::string Clean(std::string s) { for (auto& c : s) if (c == '|' || c == ';' || c == ',' || c == '\n' || c == '\r' || c == ' ') c = '_'; return s; }
inline std::string ModelKey(unsigned id) { char b[12]; std::snprintf(b, sizeof b, "%08X", id); return b; }
// A spot: route, lane and its 10 m cell. Stable when route counts change, and the same for test and normal slots.
inline std::string SpotKey(const std::string& route, int lane, double x, double y) {
    char b[64]; std::snprintf(b, sizeof b, "/%d/%ld_%ld", lane, long(std::floor(x / 10)), long(std::floor(y / 10))); return Clean(route) + b;
}

struct Change { bool modelRejected = false, modelCleared = false, spotRejected = false, spotCleared = false, unexplained = false; };
struct Store {
    std::map<std::string, Record> models, spots;
    std::string build; int unexplained = 0; bool dirty = false;

    static void Note(Tally& t, bool pass, const std::string& other) {
        (pass ? t.pass : t.fail) += 1;
        if (!pass && !other.empty() && std::find(t.with.begin(), t.with.end(), other) == t.with.end() && int(t.with.size()) < rules::keepWith) t.with.push_back(other);
    }
    // Verdict for one record: any stage with enough tries, a low hopeful rate and fails from enough counterparts.
    static void Judge(Record& r) {
        r.rejected = false; r.why = -1;
        for (int s = 0; s < int(Stage::Count); ++s) {
            const auto& t = r.stage[s]; const double n = t.pass + t.fail;
            if (n >= rules::minTries && Upper(t.pass, n) < rules::rejectBelow && int(t.with.size()) >= rules::blameSpread) { r.rejected = true; r.why = s; return; }
        }
    }
    static bool Concentrated(const Record& r, int s) {
        const auto& t = r.stage[s]; const double n = t.pass + t.fail;
        return n >= rules::minTries && Upper(t.pass, n) < rules::rejectBelow && int(t.with.size()) < rules::blameSpread;
    }
    Change Report(Stage stage, unsigned model, const std::string& spot, bool pass) {
        Change c; const int s = int(stage); if (s < 0 || s >= int(Stage::Count)) return c;
        const std::string mk = model ? ModelKey(model) : std::string();
        if (!mk.empty()) { auto& r = models[mk]; const bool was = r.rejected; Note(r.stage[s], pass, spot); if (r.build.empty()) r.build = build; Judge(r);
            c.modelRejected = r.rejected && !was; c.modelCleared = was && !r.rejected; }
        if (!spot.empty()) { auto& r = spots[spot]; const bool was = r.rejected; Note(r.stage[s], pass, mk); if (r.build.empty()) r.build = build; Judge(r);
            c.spotRejected = r.rejected && !was; c.spotCleared = was && !r.rejected; }
        if (!pass) {
            const auto m = mk.empty() ? models.end() : models.find(mk); const auto p = spot.empty() ? spots.end() : spots.find(spot);
            const bool mc = m != models.end() && Concentrated(m->second, s), pc = p != spots.end() && Concentrated(p->second, s);
            if ((mc || m == models.end()) && (pc || p == spots.end()) && (mc || pc)) { c.unexplained = true; ++unexplained; }
        }
        dirty = true; return c;
    }
    // Bounds fingerprint: a remembered id that now has other bounds is another model (ids are reused); its record starts again. true: reset.
    bool Fingerprint(unsigned model, double radius, double halfHeight) {
        const int fp = int(std::lround(radius * 10)) * 1000 + int(std::lround(halfHeight * 10)); auto& r = models[ModelKey(model)];
        if (!r.fp) { r.fp = fp; dirty = true; return false; }
        if (r.fp == fp) return false;
        const bool allowed = r.allowed; r = Record{}; r.fp = fp; r.allowed = allowed; r.build = build; dirty = true; return true;
    }
    const Record* Model(unsigned model) const { const auto it = models.find(ModelKey(model)); return it == models.end() ? nullptr : &it->second; }
    bool ModelRejected(unsigned model) const { const auto* r = Model(model); return r && r->rejected && !r->allowed; }
    bool SpotRejected(const std::string& spot) const { const auto it = spots.find(spot); return it != spots.end() && it->second.rejected; }
    double Odds(unsigned model) const { const auto* r = Model(model); return r ? r->Odds() : .5; }
    // A rejected subject asked for again: true every rules::retrialEvery asks (a trial birth).
    bool Trial(Record& r) { ++r.asks; dirty = true; return r.asks % rules::retrialEvery == 0; }
    bool ModelTrial(unsigned model) { auto it = models.find(ModelKey(model)); return it != models.end() && Trial(it->second); }
    bool SpotTrial(const std::string& spot) { auto it = spots.find(spot); return it != spots.end() && Trial(it->second); }
    // The user's call (F7): allowed models are used whatever Health says.
    void Allow(unsigned model, bool allow) { auto& r = models[ModelKey(model)]; if (r.allowed != allow) { r.allowed = allow; dirty = true; } }

    // Text form, LF only. "fvo-traffic-health 1", then one line per record:
    // m|<key>|fp|build|allowed|asks|p,f,with;with|p,f,...|...   (s| for spots; four stages)
    std::string Save() const {
        std::ostringstream o; o << "fvo-traffic-health 1\n";
        auto row = [&](char kind, const std::string& key, const Record& r) {
            o << kind << '|' << Clean(key) << '|' << r.fp << '|' << Clean(r.build) << '|' << int(r.allowed) << '|' << r.asks;
            for (int s = 0; s < int(Stage::Count); ++s) { const auto& t = r.stage[s]; char b[48]; std::snprintf(b, sizeof b, "|%.2f,%.2f,", t.pass, t.fail); o << b;
                for (std::size_t k = 0; k < t.with.size(); ++k) o << (k ? ";" : "") << Clean(t.with[k]); }
            o << '\n'; };
        for (const auto& [k, r] : models) row('m', k, r);
        for (const auto& [k, r] : spots) row('s', k, r);
        return o.str();
    }
    // Reads a saved text; records from another build keep rules::ageKeep of their counts. false: not this format (nothing read).
    bool Load(const std::string& text) {
        std::istringstream in(text); std::string line;
        if (!std::getline(in, line) || line.rfind("fvo-traffic-health 1", 0) != 0) return false;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> f; std::size_t a = 0; for (;;) { const auto b = line.find('|', a); f.push_back(line.substr(a, b - a)); if (b == std::string::npos) break; a = b + 1; }
            if (f.size() < 6 + std::size_t(Stage::Count) || (f[0] != "m" && f[0] != "s") || f[1].empty()) continue;
            Record r; r.fp = std::atoi(f[2].c_str()); r.build = f[3]; r.allowed = f[4] == "1"; r.asks = std::atoi(f[5].c_str());
            const bool aged = r.build != Clean(build);
            for (int s = 0; s < int(Stage::Count); ++s) {
                const auto& v = f[6 + std::size_t(s)]; auto& t = r.stage[s];
                const auto c1 = v.find(','), c2 = c1 == std::string::npos ? c1 : v.find(',', c1 + 1); if (c2 == std::string::npos) continue;
                t.pass = std::atof(v.substr(0, c1).c_str()); t.fail = std::atof(v.substr(c1 + 1, c2 - c1 - 1).c_str());
                if (!std::isfinite(t.pass) || t.pass < 0) t.pass = 0; if (!std::isfinite(t.fail) || t.fail < 0) t.fail = 0;
                if (aged) { t.pass *= rules::ageKeep; t.fail *= rules::ageKeep; }
                for (std::size_t p = c2 + 1; p < v.size();) { const auto q = v.find(';', p); const auto w = v.substr(p, q == std::string::npos ? std::string::npos : q - p); if (!w.empty() && int(t.with.size()) < rules::keepWith) t.with.push_back(w); if (q == std::string::npos) break; p = q + 1; }
            }
            if (aged) r.build = Clean(build);
            Judge(r); (f[0] == "m" ? models : spots)[f[1]] = std::move(r);
        }
        return true;
    }
    void Clear() { models.clear(); spots.clear(); unexplained = 0; dirty = true; }
};
}
