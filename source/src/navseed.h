#pragma once

// The nav map seeded from the game's own collision objects (2026-10-20, the user: "can the game rather provide this global information
// cheaply to the planners ... read the actual collision data and positions associated with them"). Every loaded reference within kRadius
// of the player that has a physics body in its 3D (the game's own body-id path, as vehicleworld finds vehicle bodies) is written into the
// nav map as a solid box: its base object's bounds (TESBoundObject +0x48 / +0x54) at the reference's position and heading, scaled by its
// live bound sphere. A full pass every ~2 s, a few hundred references a frame. The planner then knows walls, buildings, rocks and bridge
// decks before any probe has touched them; probes still add what the boxes miss. Actors and vehicles are not objects (vehicles have
// their own predictions); boxes larger than kMaxHalf are left to the probes (a box that big is a poor fit).
namespace navseed
{
	void Tick();   // main thread, every frame (after world::Publish)
	void Reset();  // a save load
}
