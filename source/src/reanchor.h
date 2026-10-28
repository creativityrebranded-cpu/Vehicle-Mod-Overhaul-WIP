#pragma once

// SeamlessReanchor (module plugin; was core 0.52.0): rebuild the world around the player with the game's own quicksave + quickload.
// The quicksave is written as a landing at the player (SaveGameDetour, "save as landing"), so the quickload builds
// a fresh, engine-made world whose landing is the player's spot. Nothing in the live world is moved by us.
// First stage: a key, with the loading screen, to measure how long it takes and how exactly the player comes back.
// Later: a frame held on screen instead of the loading screen, and an automatic trigger at a still moment.
namespace reanchor
{
	void Tick();  // main thread, every frame
	void LoadReset(); // discard distance baseline when the engine starts a new load
}
