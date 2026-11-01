#include "traffic/traffic_api.h"
#include "traffic/trafficprofileclient.h"
#include "sfse/PluginAPI.h"
#include <cstdio>

namespace {
SFSEMessagingInterface::EventCallback listener = nullptr;
bool Register(PluginHandle, const char*, SFSEMessagingInterface::EventCallback fn) { listener = fn; return true; }
bool Dispatch(PluginHandle, std::uint32_t, void*, std::uint32_t, const char*) { return true; }
SFSEMessagingInterface messaging{SFSEMessagingInterface::kInterfaceVersion, &Register, &Dispatch};
void* Query(std::uint32_t id) { return id == kInterface_Messaging ? &messaging : nullptr; }
PluginHandle Handle() { return 1; }
const PluginInfo* Info(const char*) { return nullptr; }
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    HMODULE core = LoadLibraryW(argv[1]);
    if (!core) { std::printf("FAIL core load %lu\n", GetLastError()); return 1; }
    {
        trafficprofileclient::Guard files;
        if (!files) { std::printf("FAIL file guard fallback\n"); return 1; }
    }
    const auto query = reinterpret_cast<FvoTrafficApi_t>(GetProcAddress(core, "FVO_TrafficApi"));
    FvoTrafficHooks hooks{};
    const auto api = query ? query(13, &hooks) : nullptr;
    const auto readonly = query ? query(4, nullptr) : nullptr;
    if (!api || api->version != 13 || !readonly || readonly->version != 4) {
        std::printf("FAIL published API 13 / read-only 4 compatibility\n"); return 1;
    }
    HMODULE addon = LoadLibraryW(argv[2]);
    if (!addon) { std::printf("FAIL addon load %lu\n", GetLastError()); return 1; }
    const auto load = reinterpret_cast<_SFSEPlugin_Load>(GetProcAddress(addon, "SFSEPlugin_Load"));
    SFSEInterface sfse{0, 0x01100F40, SFSEInterface::kInterfaceVersion, &Query, &Handle, &Info};
    if (!load || !load(&sfse) || !listener) { std::printf("FAIL addon registration\n"); return 1; }
    SFSEMessagingInterface::Message message{"SFSE", SFSEMessagingInterface::kMessage_PostDataLoad, 0, nullptr};
    listener(&message); // attaches real old/new addon callbacks to real old/new core; no game callbacks are run
    std::printf("PASS mixed DLL load, addon attachment, API 13, read-only 4, %s file-lock export\n",
                GetProcAddress(core, "FVO_TrafficFilesLock") ? "present" : "legacy absent");
    return 0;
}
