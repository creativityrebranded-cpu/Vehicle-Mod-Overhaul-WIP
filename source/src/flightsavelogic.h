#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

// Saved hover (2026-11-17, the user: "save flight state so that when loading I don't just drop from whatever height I was
// floating at"). Pure rules, tested offline (tests/flightsave_test.cpp); flightsave.cpp does the files and the game.
// Mode (notes/VEHICLE_DECISION_DESIGN_20261106.md section 6): the player's hover on/off stays the hover key's path in
// vehicleflight::PlayerTick. A load only submits a request there; it decides nothing else.
namespace flightsavelogic {
// What a save holds for the player's own hover. Speed and attitude are not kept: Start keeps the body's own.
struct Record { std::uint32_t vehicle = 0; float z = 0; int gear = 0; };

// One key per save for both routines: SaveGame (FUN_141814220 param_4, "---Finished saving game: %s") and LoadGame
// (FUN_141815620 param_2, copied into a 0x104 buffer). Folder and ".sfs" dropped, lower case, file-name safe.
inline std::string Key(const char* name) {
    if (!name) return {};
    std::string s(name);
    const auto slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s.erase(0, slash + 1);
    if (s.size() >= 4) {
        std::string ext = s.substr(s.size() - 4);
        for (auto& c : ext) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
        if (ext == ".sfs") s.resize(s.size() - 4);
    }
    std::string out;
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') c = char(c + 32);
        const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        out += keep ? c : '_';
    }
    if (out.size() > 120) out.resize(120);
    return out;
}

inline std::string Format(const Record& r) {
    char line[160];
    std::snprintf(line, sizeof line, "FVO saved hover v1\nvehicle=%08X\nz=%.3f\ngear=%d\n", unsigned(r.vehicle), double(r.z), r.gear);
    return line;
}
// False for anything that is not a complete v1 record (then nothing is restored).
inline bool Parse(const std::string& text, Record& out) {
    unsigned vehicle = 0; double z = 0; int gear = 0;
    if (text.rfind("FVO saved hover v1\n", 0) != 0) return false;
    if (std::sscanf(text.c_str() + 19, "vehicle=%X\nz=%lf\ngear=%d", &vehicle, &z, &gear) != 3) return false;
    if (!vehicle || !std::isfinite(z) || gear < 0 || gear > 4) return false;
    out = { vehicle, float(z), gear };
    return true;
}

// The load asks once: same vehicle, the player at its wheel, and still where the save left it (a moved or replaced
// vehicle keeps today's behaviour: no hover).
enum class Verdict { Wait, Restore, Expired, OtherVehicle, Moved };
constexpr double kMaxMove = 5.0;          // m between the saved and the loaded height
constexpr unsigned long long kWaitMs = 15000; // from the first frame with a world: the seat comes back or the request ends
inline Verdict Decide(const Record& r, std::uint32_t driven, double z, unsigned long long waitedMs) {
    if (!driven) return waitedMs >= kWaitMs ? Verdict::Expired : Verdict::Wait;
    if (driven != r.vehicle) return Verdict::OtherVehicle;
    if (!std::isfinite(z) || std::fabs(z - r.z) > kMaxMove) return Verdict::Moved;
    return Verdict::Restore;
}
inline const char* Name(Verdict v) {
    switch (v) { case Verdict::Wait: return "wait"; case Verdict::Restore: return "restore"; case Verdict::Expired: return "no driver seat";
    case Verdict::OtherVehicle: return "another vehicle"; case Verdict::Moved: return "vehicle moved"; }
    return "?";
}
}
