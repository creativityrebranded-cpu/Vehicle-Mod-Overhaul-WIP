#pragma once
#include "observations.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace logger
{
	// Opens Documents\My Games\Starfield\SFSE\Logs\SeamlessPlanets.log ; the previous one is kept as SeamlessPlanets.prev.log.
	void Init();
	// No lock, no timestamp: for the crash handler (the lock may be held by the crashing thread).
	void WriteUnlocked(const char* fmt, ...);
#ifdef FVO_RELEASE
	// Release DLLs (cmake -DFVO_RELEASE=ON, see CMakeLists.txt): no logging. The file holds only the Important lines and a crash report.
	// Templates, not a macro: arguments are still evaluated, so a call with a side effect behaves as in a development build.
	template<class... A> inline void Write(const char*, const A&...) {}
	template<class... A> inline void Observe(const char*, std::uint32_t, std::uint64_t, const char*, const A&...) {}
	template<class... A> inline void ObserveReference(const char*, std::uint32_t, std::uint64_t, const char*, const A&...) {}
	inline void Service() {}
	inline bool Verbose() { return false; }  // the default setting; a [log] verbose=1 line is ignored
	template<class... A> inline void Important(const char* fmt, const A&... args) { WriteUnlocked(fmt, args...); }
#else
	void Write(const char* fmt, ...);
	// Lines kept in release DLLs too: the packager reads the version from the DLL. One-off startup lines only.
	template<class... A> inline void Important(const char* fmt, const A&... args) { Write(fmt, args...); }
	// Correlated diagnostic points: changes + heartbeat, independent of verbose logging. No game-state mutation.
	inline void ObserveFields(const char* channel,const char* identity,std::uint32_t vehicle,std::uint64_t state,const char* fmt,va_list args){
		const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		const auto key=observations::Key(channel,vehicle)^(std::uint64_t(observations::epoch.load())*0xD6E8FEB86659FD93ull);
		const auto decision=observations::gate.Sample(static_cast<std::uint64_t>(now),key,state);
		if(!decision.emit)return;
		char fields[1200]{};std::vsnprintf(fields,sizeof fields,fmt,args);
		Write("OBS system=%s %s=%08X state=%llX coalesced=%u contention=%u %s",channel,identity,vehicle,
			static_cast<unsigned long long>(state),decision.coalesced,decision.contended,fields);
	}
	inline void Observe(const char* channel,std::uint32_t vehicle,std::uint64_t state,const char* fmt,...){
		va_list args;va_start(args,fmt);ObserveFields(channel,"vehicle",vehicle,state,fmt,args);va_end(args);
	}
	inline void ObserveReference(const char* channel,std::uint32_t ref,std::uint64_t state,const char* fmt,...){
		va_list args;va_start(args,fmt);ObserveFields(channel,"reference",ref,state,fmt,args);va_end(args);
	}
	// Main-thread service: bounded flushes during gameplay; startup and crash output
	// remain immediate. No worker thread, dropped lines or changed verbosity.
	void Service();

	// Chatty lines (per-second NPC state, the 2 s pilot position line, HUD notices, traffic per-NPC lines) only when
	// Logs\FlyingVehicleOverhaul\settings.ini has [log] verbose=1 (re-read every 5 s). Decisions and faults are always logged.
	bool Verbose();
#endif
}
