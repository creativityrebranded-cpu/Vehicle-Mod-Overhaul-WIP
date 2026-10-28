#pragma once

// Ship landing probe (research, 2026-10-20; notes/NPC_SHIP_LANDING_AND_HIJACK_20261020.md, section "Probe").
// Logs NPC ships every 0.25 s once they land / take off near you (the landing message hooks find them).
// One test key, . : the nearest flying NPC ship (not the player's, player not aboard) gets two fly-to points
// (PathingRequestOpenSpace, no landing messages): 400 m ahead, then after 4 s 200 m to the right of that. Not wired into the build: the integration owner adds shipprobe.cpp to CMakeLists and calls Install/Tick/Reset.
namespace shipprobe
{
	bool Install();  // once, at plugin load (hooks the landing message handlers; logging only)
	void Tick();     // main thread, every frame
	void Reset();    // a save load
}
