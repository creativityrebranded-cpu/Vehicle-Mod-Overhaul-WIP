#include "routeguidance.h"
#include "trafficprofile.h"
#include "trafficprofilestore.h"
#include "trafficpermanent.h"
#include "pilotlayout.h"
#include "fvopaths.h"
#include "log.h"
#include "notify.h"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <mutex>

namespace trafficprofile {
namespace {
std::atomic<bool> g_allowed{false};
std::atomic<bool> g_saveReady{false};
std::recursive_mutex g_lock;
std::uint32_t g_cachedId = 0;
std::wstring g_cachedRoutes;
bool g_reported = false;
unsigned g_saveDepth = 0;
std::atomic<const char*> g_notice{nullptr};
#ifndef FVO_TRAFFICPROFILE_TEST
std::atomic<std::uintptr_t> g_savedForm{0}, g_seenForm{0};
std::atomic<bool> g_formsStarted{false}, g_formsReady{false};
bool WarmForms() {
    bool expected = false;
    if (!g_formsStarted.compare_exchange_strong(expected, true)) return g_formsReady.load();
    // Resolve static plugin forms before taking the file mutex. Save callbacks must
    // never wait on an engine lookup while holding the file mutex (lock inversion).
    g_savedForm = runtimeforms::Lookup(runtimeforms::Resolve(pilotlayout::kTrafficProfile));
    g_seenForm = runtimeforms::Lookup(runtimeforms::Resolve(pilotlayout::kMapMessageSeen));
    g_formsReady = true;
    return true;
}
#endif
#ifdef FVO_TRAFFICPROFILE_TEST
std::wstring g_testStorage, g_testDefaults;
float* g_testSaved = nullptr;
bool g_testEstablished = false;
#endif
const std::wstring& Storage() {
#ifdef FVO_TRAFFICPROFILE_TEST
    return g_testStorage;
#else
    return fvopaths::Dir();
#endif
}
bool Established() {
#ifdef FVO_TRAFFICPROFILE_TEST
    return g_testEstablished;
#else
    const auto seen = g_seenForm.load();
    return seen && *reinterpret_cast<const std::uint8_t*>(seen + 0x2E) == 10 &&
                  *reinterpret_cast<const float*>(seen + 0x48) != 0.f;
#endif
}
std::uint32_t Seed() {
    FILETIME time{}; GetSystemTimeAsFileTime(&time);
    return time.dwLowDateTime ^ time.dwHighDateTime ^ GetCurrentProcessId();
}
float& SavedValue() {
#ifdef FVO_TRAFFICPROFILE_TEST
    if (!g_testSaved) throw std::runtime_error("Saved traffic identity unavailable");
    float& value = *g_testSaved;
#else
    const auto form = g_savedForm.load();
    if (!form || *reinterpret_cast<const std::uint8_t*>(form + 0x2E) != 10)
        throw std::runtime_error("Update FVO_NPCPilots.esm for saved traffic snapshots");
    float& value = *reinterpret_cast<float*>(form + 0x48);
#endif
    if (!std::isfinite(value) || value < 0 || value > float(trafficprofilestore::kMaxId) || std::floor(value) != value)
        throw std::runtime_error("Invalid saved traffic snapshot identity");
    return value;
}
std::filesystem::path Defaults() {
#ifdef FVO_TRAFFICPROFILE_TEST
    return std::filesystem::path(g_testDefaults);
#else
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&Defaults), &self);
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(self, path, 32768)) throw std::runtime_error("Could not locate Traffic Addon defaults");
    return std::filesystem::path(path).parent_path() / L"FVO_Traffic";
#endif
}
}
std::recursive_mutex& Mutex() { return g_lock; }
void SaveReady(bool ready) {
    g_saveReady = ready;
    if (!ready) {
        g_allowed = false;
        g_notice = "Traffic snapshots unavailable: game save hook could not be installed. Traffic is paused.";
    }
}
void Report() { if (const auto text = g_notice.exchange(nullptr)) notify::Show("%s", text); }
void Allow(bool allowed) {
    bool ready = allowed && g_saveReady.load();
#ifndef FVO_TRAFFICPROFILE_TEST
    if (ready) ready = WarmForms();
#endif
    g_allowed.store(ready, std::memory_order_release);
}
void BeginLoad() {
    Allow(false);
    Guard lock(g_lock);
    // Reloading the same save also discards unsaved route changes and branches anew.
    g_cachedId = 0; g_cachedRoutes.clear(); g_reported = false;
}
void BeforeSave() {
    Guard lock(g_lock);
    if (g_saveDepth++) return;
    if (g_cachedRoutes.empty()) return; // traffic was not initialized: existing saved identity stays valid
    try {
        if(routeguidance::beforeSave && !routeguidance::beforeSave()) throw std::runtime_error("Active traffic journeys could not be recorded");
        float& value = SavedValue();
        if (static_cast<std::uint32_t>(value) != g_cachedId)
            throw std::runtime_error("Traffic snapshot identity changed before saving");
        const std::filesystem::path working(g_cachedRoutes);
        // Flush the Win32 INI writer before cloning the fleet registry.
        for (const auto* name : {L"trafficpool.ini", L"ringroad.ini"})
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, (working.parent_path() / name).c_str());
        const auto saved = trafficprofilestore::Clone(std::filesystem::path(Storage()) / L"TrafficProfiles", working, Seed());
        // Publish a complete immutable snapshot BEFORE the game serializes the GLOB.
        // The active working path stays unchanged, so a normal save never resets traffic.
        value = static_cast<float>(saved.id); g_cachedId = saved.id;
        logger::Write("Traffic snapshot: game save captures %u", saved.id);
    } catch (const std::exception& error) {
        logger::Important("Traffic snapshot: %s; this game save retains the previous traffic snapshot", error.what());
        g_notice = "Traffic snapshot could not be saved. This game save keeps the previous traffic setup; your current edits remain active.";
    }
}
void AfterSave() { Guard lock(g_lock); if (g_saveDepth) --g_saveDepth; }
namespace {
void WritePermanentCache(const std::filesystem::path& path,const std::string& text){const auto temporary=path.wstring()+L".permanent.tmp";const auto backup=path.wstring()+L".bak";const HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Could not stage permanent traffic cache");DWORD written=0;const bool ok=WriteFile(file,text.data(),DWORD(text.size()),&written,nullptr)&&written==text.size()&&FlushFileBuffers(file);CloseHandle(file);if(!ok){DeleteFileW(temporary.c_str());throw std::runtime_error("Could not flush permanent traffic cache");}if(std::filesystem::exists(path)&&!CopyFileW(path.c_str(),backup.c_str(),FALSE)){DeleteFileW(temporary.c_str());throw std::runtime_error("Could not back up permanent traffic cache");}if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temporary.c_str());throw std::runtime_error("Could not replace permanent traffic cache");}}
std::filesystem::path PermanentRoot(){return std::filesystem::path(Storage())/L"TrafficProfiles"/L"Permanent"/L"routes";}
}
std::wstring PermanentDir(){Guard lock(g_lock);if(Routes().empty())return {};const auto path=PermanentRoot();try{std::filesystem::create_directories(path);return path.wstring();}catch(...){return {};}}
bool RefreshPermanent(std::string& why){Guard lock(g_lock);if(g_cachedRoutes.empty()){why="No editable traffic profile is active";return false;}try{trafficpermanent::Sync(PermanentRoot(),g_cachedRoutes,WritePermanentCache);return true;}catch(const std::exception& error){why=error.what();return false;}}
std::wstring Routes() {
    if (!g_allowed.load(std::memory_order_acquire)) return {};
    std::lock_guard lock(g_lock);
    if (!g_allowed.load(std::memory_order_acquire)) return {};
    try {
        float& value = SavedValue();
        auto id = static_cast<std::uint32_t>(value);
        if (id && id == g_cachedId) return g_cachedRoutes;
        if (Storage().empty()) throw std::runtime_error("Could not locate traffic profile storage");
        const std::filesystem::path legacy(Storage()), root = legacy / L"TrafficProfiles";
        std::filesystem::path routes;
        if (!id) {
            const auto created = trafficprofilestore::Create(root, Defaults(), legacy, Established(),
                                     Seed());
            id = created.id; routes = created.routes;
            // Same game-persisted GLOB value mechanism as the introduction's seen flag.
            // Initial baseline for a save with no identity yet. It will never be edited directly.
            value = static_cast<float>(id);
            logger::Write("Traffic profile: created %u (%s)", id, created.imported ? "legacy setup imported" : "fresh defaults");
        } else routes = trafficprofilestore::Open(root, id);
        const auto working = trafficprofilestore::Clone(root / L"Working", routes, Seed());
        trafficpermanent::Sync(PermanentRoot(),working.routes,WritePermanentCache);
        g_cachedId = id; g_cachedRoutes = working.routes.wstring(); g_reported = false;
        return g_cachedRoutes;
    } catch (const std::exception& error) {
        if (!g_reported) {
            g_reported = true;
            logger::Important("Traffic profile: %s; traffic remains paused", error.what());
            g_notice = "Traffic setup unavailable. Check FVO_NPCPilots.esm and this save's TrafficProfiles folder.";
        }
        return {};
    }
}
std::wstring File(const wchar_t* name) {
    const auto routes = Routes();
    return routes.empty() ? std::wstring() : (std::filesystem::path(routes).parent_path() / name).wstring();
}
#ifdef FVO_TRAFFICPROFILE_TEST
namespace testing {
void Configure(const std::wstring& storage, const std::wstring& defaults, float* saved, bool established) {
    Guard lock(g_lock);
    g_allowed = false; g_saveReady = false; g_cachedId = 0; g_cachedRoutes.clear(); g_saveDepth = 0;
    g_reported = false; g_notice = nullptr;
    g_testStorage = storage; g_testDefaults = defaults; g_testSaved = saved; g_testEstablished = established;
}
}
#endif
}
extern "C" __declspec(dllexport) void FVO_TrafficFilesLock(int acquire) {
    if (acquire) trafficprofile::Mutex().lock(); else trafficprofile::Mutex().unlock();
}
extern "C" __declspec(dllexport) int FVO_TrafficProfileDir(wchar_t* out, int size) {
    if (!out || size < 1) return 0;
    out[0] = 0;
    const auto routes = trafficprofile::Routes();
    if (routes.empty() || routes.size() >= std::size_t(size)) return 0;
    std::copy(routes.begin(), routes.end(), out); out[routes.size()] = 0;
    return 1;
}
