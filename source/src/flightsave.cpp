#include "flightsave.h"
#include "flightsavelogic.h"
#include "fvopaths.h"
#include "log.h"

#include <Windows.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace flightsave {
namespace {
std::mutex g_lock;
flightsavelogic::Record g_now;          // published by PlayerTick
bool g_pending = false;                 // a loaded save's record waits for the seat
flightsavelogic::Record g_request;
std::string g_requestKey;
unsigned long long g_firstAsk = 0;      // first PlayerTick with a world after the load

// The name pointers come from the game's own save and load routines; copy them under SEH so a bad pointer only costs
// the record. No C++ objects in this function (SEH).
bool CopyName(const char* name, char* out, std::size_t size) {
    out[0] = 0;
    if (!name) return false;
    __try {
        std::size_t i = 0;
        for (; i + 1 < size && name[i]; ++i) out[i] = name[i];
        out[i] = 0;
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}
std::wstring Folder() { return fvopaths::Dir() + L"\\FlightStates"; }
std::wstring FileFor(const std::string& key) { return Folder() + L"\\" + std::wstring(key.begin(), key.end()) + L".txt"; }
}

void Publish(std::uint32_t vehicle, float z, int gear) {
    std::lock_guard lk(g_lock);
    g_now = vehicle ? flightsavelogic::Record{ vehicle, z, gear } : flightsavelogic::Record{};
}

void OnSave(const char* saveName) {
    char raw[300];
    const bool named = CopyName(saveName, raw, sizeof raw);
    const std::string key = named ? flightsavelogic::Key(raw) : std::string();
    flightsavelogic::Record now;
    { std::lock_guard lk(g_lock); now = g_now; }
    if (key.empty()) { logger::Write("Saved hover: save name unreadable; no record (hover %s)", now.vehicle ? "on" : "off"); return; }
    if (fvopaths::Dir().empty()) return;
    const auto file = FileFor(key);
    if (!now.vehicle) {
        // Hover off: an older record under this name (a reused quicksave or autosave slot) must not come back.
        if (DeleteFileW(file.c_str())) logger::Write("Saved hover: save '%s' (key %s): hover off, old record removed", raw, key.c_str());
        else logger::Write("Saved hover: save '%s' (key %s): hover off, nothing recorded", raw, key.c_str());
        return;
    }
    CreateDirectoryW(Folder().c_str(), nullptr);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << flightsavelogic::Format(now);
    out.close();
    logger::Write("Saved hover: save '%s' (key %s): vehicle %08X at z %.1f, gear %d %s", raw, key.c_str(), now.vehicle, double(now.z), now.gear,
        out ? "recorded" : "NOT recorded (file write failed)");
}

void OnLoad(const char* saveName) {
    char raw[300];
    const bool named = CopyName(saveName, raw, sizeof raw);
    const std::string key = named ? flightsavelogic::Key(raw) : std::string();
    std::lock_guard lk(g_lock);
    g_now = {};
    g_pending = false; g_firstAsk = 0; g_requestKey = key;
    if (key.empty()) { logger::Write("Saved hover: load name unreadable; the vehicle loads as before"); return; }
    std::ifstream in(FileFor(key), std::ios::binary);
    if (!in) { logger::Write("Saved hover: load '%s' (key %s): no record", raw, key.c_str()); return; }
    std::stringstream text; text << in.rdbuf();
    flightsavelogic::Record r;
    if (!flightsavelogic::Parse(text.str(), r)) { logger::Write("Saved hover: load '%s' (key %s): record unreadable; ignored", raw, key.c_str()); return; }
    g_request = r; g_pending = true;
    logger::Write("Saved hover: load '%s' (key %s): vehicle %08X was hovering at z %.1f, gear %d; waiting for you at its wheel",
        raw, key.c_str(), r.vehicle, double(r.z), r.gear);
}

bool TakeRestore(std::uint32_t driven, double z, int& gear) {
    std::lock_guard lk(g_lock);
    if (!g_pending) return false;
    const unsigned long long now = GetTickCount64();
    if (!g_firstAsk) g_firstAsk = now;
    const auto verdict = flightsavelogic::Decide(g_request, driven, z, now - g_firstAsk);
    if (verdict == flightsavelogic::Verdict::Wait) return false;
    g_pending = false;
    if (verdict != flightsavelogic::Verdict::Restore) {
        logger::Write("Saved hover: not restored (%s): saved vehicle %08X z %.1f, now driving %08X z %.1f, %.1f s after the world came back",
            flightsavelogic::Name(verdict), g_request.vehicle, double(g_request.z), driven, z, (now - g_firstAsk) / 1000.0);
        return false;
    }
    gear = g_request.gear;
    logger::Write("Saved hover: restoring on vehicle %08X at z %.1f (saved %.1f), gear %d, %.1f s after the world came back",
        driven, z, double(g_request.z), gear, (now - g_firstAsk) / 1000.0);
    return true;
}
}
