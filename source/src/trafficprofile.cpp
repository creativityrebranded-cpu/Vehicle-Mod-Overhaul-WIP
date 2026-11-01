#include "trafficprofile.h"
#include "trafficprofilestore.h"
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
std::mutex g_lock;
std::uint32_t g_cachedId = 0;
std::wstring g_cachedRoutes;
bool g_reported = false;
std::filesystem::path Defaults() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&Defaults), &self);
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(self, path, 32768)) throw std::runtime_error("Could not locate Traffic Addon defaults");
    return std::filesystem::path(path).parent_path() / L"FVO_Traffic";
}
}
void Allow(bool allowed) { g_allowed.store(allowed, std::memory_order_release); }
std::wstring Routes() {
    if (!g_allowed.load(std::memory_order_acquire)) return {};
    std::lock_guard lock(g_lock);
    if (!g_allowed.load(std::memory_order_acquire)) return {};
    try {
        const auto form = runtimeforms::Lookup(runtimeforms::Resolve(pilotlayout::kTrafficProfile));
        if (!form || *reinterpret_cast<const std::uint8_t*>(form + 0x2E) != 10)
            throw std::runtime_error("Update FVO_NPCPilots.esm for playthrough traffic profiles");
        float& value = *reinterpret_cast<float*>(form + 0x48);
        if (!std::isfinite(value) || value < 0 || value > float(trafficprofilestore::kMaxId) || std::floor(value) != value)
            throw std::runtime_error("Invalid saved traffic profile identity");
        auto id = static_cast<std::uint32_t>(value);
        if (id && id == g_cachedId) return g_cachedRoutes;
        if (fvopaths::Dir().empty()) throw std::runtime_error("Could not locate traffic profile storage");
        const std::filesystem::path legacy(fvopaths::Dir()), root = legacy / L"TrafficProfiles";
        std::filesystem::path routes;
        if (!id) {
            const auto seen = runtimeforms::Lookup(runtimeforms::Resolve(pilotlayout::kMapMessageSeen));
            const bool established = seen && *reinterpret_cast<const std::uint8_t*>(seen + 0x2E) == 10 &&
                                     *reinterpret_cast<const float*>(seen + 0x48) != 0.f;
            FILETIME time{}; GetSystemTimeAsFileTime(&time);
            const auto created = trafficprofilestore::Create(root, Defaults(), legacy, established,
                                     time.dwLowDateTime ^ time.dwHighDateTime ^ GetCurrentProcessId());
            id = created.id; routes = created.routes;
            // Same game-persisted GLOB value mechanism as the introduction's seen flag.
            // Publish only after all files are complete. Later saves inherit this identity.
            value = static_cast<float>(id);
            logger::Write("Traffic profile: created %u (%s)", id, created.imported ? "legacy setup imported" : "fresh defaults");
        } else routes = trafficprofilestore::Open(root, id);
        g_cachedId = id; g_cachedRoutes = routes.wstring(); g_reported = false;
        return g_cachedRoutes;
    } catch (const std::exception& error) {
        if (!g_reported) {
            g_reported = true;
            logger::Important("Traffic profile: %s; traffic remains paused", error.what());
            notify::Show("Traffic setup unavailable. Check FVO_NPCPilots.esm and this playthrough's TrafficProfiles folder.");
        }
        return {};
    }
}
std::wstring File(const wchar_t* name) {
    const auto routes = Routes();
    return routes.empty() ? std::wstring() : (std::filesystem::path(routes).parent_path() / name).wstring();
}
}
extern "C" __declspec(dllexport) int FVO_TrafficProfileDir(wchar_t* out, int size) {
    if (!out || size < 1) return 0;
    out[0] = 0;
    const auto routes = trafficprofile::Routes();
    if (routes.empty() || routes.size() >= std::size_t(size)) return 0;
    std::copy(routes.begin(), routes.end(), out); out[routes.size()] = 0;
    return 1;
}
