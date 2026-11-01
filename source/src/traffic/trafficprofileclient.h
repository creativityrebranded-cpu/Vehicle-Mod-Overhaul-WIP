#pragma once
#include <Windows.h>
#include <string>
namespace trafficprofileclient {
class Guard {
    void (*lock_)(int) = nullptr;
    bool core_ = false;
public:
    Guard() {
        const auto core = GetModuleHandleW(L"FlyingVehicleOverhaul.dll");
        core_ = core != nullptr;
        if (core) lock_ = reinterpret_cast<void (*)(int)>(GetProcAddress(core, "FVO_TrafficFilesLock"));
        if (lock_) lock_(1);
    }
    ~Guard() { if (lock_) lock_(0); }
    // Prior v13 cores have no file-lock export; retain their existing behavior.
    explicit operator bool() const { return core_; }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};
inline std::wstring Routes() {
    const auto core = GetModuleHandleW(L"FlyingVehicleOverhaul.dll");
    const auto get = core ? reinterpret_cast<int (*)(wchar_t*, int)>(GetProcAddress(core, "FVO_TrafficProfileDir")) : nullptr;
    wchar_t path[32768]{};
    return get && get(path, 32768) ? std::wstring(path) : std::wstring();
}
inline std::wstring File(const wchar_t* name) {
    auto path = Routes();
    if (path.empty()) return {};
    path.resize(path.find_last_of(L"\\/"));
    return path + L"\\" + name;
}
}
