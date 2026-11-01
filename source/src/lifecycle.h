#pragma once
#include "lifecycle_requests.h"
#include <cstdint>
#include <memory>
namespace lifecycle {
using Reference=std::shared_ptr<void>;
bool InService();
bool InCover(); // running from the loading-screen step (lifecycle_cover.h): the engine's own flushes do not run here
void InstallCover();
std::size_t Outstanding(const char* operation); // requests of that kind not yet finished
Reference Keep(std::uintptr_t ref,unsigned expectedId=0); // owning handle lookup, checks pointer and optional form ID
std::optional<std::uint64_t> Poll(const std::string& key,unsigned id,const std::function<Work()>& factory);
bool Pending(unsigned id=0,const char* operation=nullptr);
bool Post(const std::string& key,unsigned id,const std::function<Work()>& factory); // false: the table is full and it was dropped
void Reset();
void Close(); // the game's load routine: what is queued never runs, nothing new until Open
void Open();
void Cancel(unsigned id);
void Forget(const std::string& key);
// Fire-and-observe operations use Poll on every update until completion. No synchronous waits. The one private
// lock acquisition and drain is the loading-screen cover (lifecycle_cover.h): non-blocking, main thread, the
// service's own lock, traffic's commands only, because the native queue does not drain behind the screen.
// A full table is a wait for requests (Poll answers nothing yet, Pending true) and a drop for posts (Post false).
}
