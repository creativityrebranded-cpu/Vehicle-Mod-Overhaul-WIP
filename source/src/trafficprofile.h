#pragma once
#include <string>
#include <mutex>
namespace trafficprofile {
std::recursive_mutex& Mutex();
using Guard = std::lock_guard<std::recursive_mutex>;
void Allow(bool allowed);
void SaveReady(bool ready);
void BeginLoad();
// The rail retains control while a save briefly pauses its physical writes.
bool FleetRailAllowed();
bool FleetRailWritesAllowed();
void BeforeSave();
void AfterSave();
void Report(); // deliver queued error notifications on the main frame thread
std::wstring Routes();
std::wstring PermanentDir();
bool RefreshPermanent(std::string& why);
std::wstring File(const wchar_t* name);
#ifdef FVO_TRAFFICPROFILE_TEST
namespace testing {
void Configure(const std::wstring& storage, const std::wstring& defaults, float* saved, bool established);
}
#endif
}
