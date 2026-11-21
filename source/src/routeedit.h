#pragma once
#include <cstdint>

// F7 Traffic Manager in native photo mode; the surface map is a read-only
// traffic overview independent of F7. Existing route formats and save workflow
// are preserved. Photo input: hold RMB / controller L3 to use the native camera.
// Engine evidence: notes/traffic-photomode-20261104/HANDOFF.md.
namespace routeedit
{
	// The local map's update and the star map menu's message slot. Main thread, once.
	bool Install();
	// hoverui's MovieImpl::Advance detour, for every movie (only the star map and photo movies are used), on that movie's thread.
	void Advance(std::uintptr_t movie);
	// hoverui's MovieImpl destructor detour. Before the game's destructor: stop using the movie. After it: forget the
	// editor's values (never release them). 2026-10-29 crash: they were wiped BEFORE the destructor, which then walked
	// its list of external values through the wiped links (read of 0x18, Starfield.exe+0x318A140 under +0x3198DD4).
	void MovieClosing(std::uintptr_t movie);
	void MovieGone(std::uintptr_t movie);
	// The local map is on screen (its camera updated within the last `withinMs`; 300 ms: now).
	bool MapOpen(unsigned withinMs = 300);
    bool PhotoOpen();
	// Main thread: F7 in play enters photo mode with the Traffic Manager open; F7 with it open leaves photo mode.
	void PhotoKeyTick();
	// The planet's local time of day in hours (0 .. 24): TES +0x140 (the current planet's data) +0xEF8, as the Papyrus
	// GetLocalTime native FUN_14200FDD0 reads it. false when there is no planet (no fallback to another clock).
	bool LocalHours(double& hours);
	// Main thread, every update: answers the editor's "what is under the cursor" asks (new points and spots go 2 m above it).
	void GroundTick();
	// Main thread, every update (THREAD-1): the F7 route items' reference scan (the cell iterator and every reference read), answered for the draw thread.
	void ItemsMainTick();
}
