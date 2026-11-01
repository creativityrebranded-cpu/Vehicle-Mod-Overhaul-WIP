#pragma once
#include "observations.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace logger
{
	// Opens Documents\My Games\Starfield\SFSE\Logs\SeamlessPlanets.log ; the previous one is kept as SeamlessPlanets.prev.log.
	void Init();
	void Write(const char* fmt, ...);
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

	// No lock, no timestamp: for the crash handler (the lock may be held by the crashing thread).
	void WriteUnlocked(const char* fmt, ...);
}
