#pragma once
#include <cstdint>

// The traffic route editor on the local surface map (2026-10-28, the user: "write the full design ... then start
// building"; design: notes/traffic-route-editor-20261028/DESIGN.md). F7 while the local map is open shows every traffic
// route of this world over the map's own 3D view and edits the editor's routes: drop, drag, height, reverse, one / two
// way, speed, vehicles, an all-day or local-time schedule, landing and spawn spots, a one-vehicle test. Saves to
// FVO_Traffic\Editor_<world>.ini, which the traffic addon re-reads within ~2 s. Elsewhere F7 stays the HUD editor.
// Engine evidence and limits: notes/traffic-route-editor-20261028/ENGINE_RESEARCH.md.
namespace routeedit
{
	// The local map's update and the star map menu's message slot. Main thread, once.
	bool Install();
	// hoverui's MovieImpl::Advance detour, for every movie (only the star map's is used), on that movie's thread.
	void Advance(std::uintptr_t movie);
	// hoverui's MovieImpl destructor detour. Before the game's destructor: stop using the movie. After it: forget the
	// editor's values (never release them). 2026-10-29 crash: they were wiped BEFORE the destructor, which then walked
	// its list of external values through the wiped links (read of 0x18, Starfield.exe+0x318A140 under +0x3198DD4).
	void MovieClosing(std::uintptr_t movie);
	void MovieGone(std::uintptr_t movie);
	// The local map is on screen (its camera updated within the last `withinMs`; 300 ms: now).
	bool MapOpen(unsigned withinMs = 300);
	// The planet's local time of day in hours (0 .. 24): TES +0x140 (the current planet's data) +0xEF8, as the Papyrus
	// GetLocalTime native FUN_14200FDD0 reads it. false when there is no planet (no fallback to another clock).
	bool LocalHours(double& hours);
	// Main thread, every update: answers the editor's "what is under the cursor" asks (new points and spots go 2 m above it).
	void GroundTick();
}
