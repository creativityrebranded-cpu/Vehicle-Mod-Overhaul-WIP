#pragma once
#include <Windows.h>
#include <string>
namespace trafficprofileclient {
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
