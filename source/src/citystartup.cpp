#include "citystartup.h"
#include "fvopaths.h"
#include "log.h"
#include <Windows.h>
#include <filesystem>

namespace citystartup {
namespace { Result g_snapshot; }
const Result& Snapshot(){return g_snapshot;}
void Run(){
    const auto started=GetTickCount64();wchar_t exe[32768]{};
    const auto n=GetModuleFileNameW(nullptr,exe,DWORD(std::size(exe)));
    if(!n||n>=std::size(exe)){logger::Write("City build: executable path unavailable");return;}
    const auto target=std::filesystem::path(exe).parent_path()/L"Data"/L"FVO_CityTraffic.esm";
    try {g_snapshot=RebuildAt(target.wstring(),fvopaths::Dir());}
    catch(const std::exception& error){logger::Write("City build: failed safely: %s",error.what());return;}
    if(!g_snapshot.error.empty())logger::Write("City build: %s (verified existing identities: %zu)",g_snapshot.error.c_str(),g_snapshot.active.size());
    else logger::Write("City build: %s; %zu parked, %zu recorded, %zu removed, %d pool vehicles (%llu ms)",g_snapshot.changed?"rebuilt":"up to date",g_snapshot.parked,g_snapshot.recorded,g_snapshot.removed,citybuild::kPool,GetTickCount64()-started);
}
}
