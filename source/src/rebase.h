#pragma once

// Saves anchored to the player's zone (the game's own landing mechanism). A save stores the player's world position
// and the planet landing lat/long (mgr+0x98); loading replays the landing (MoveTo) there and places the player at
// that position, so loading far from the landing left the player outside everything that was built (no collision,
// nothing moving, falling through).
//
// The location record writer FUN_1418286f0 is wrapped: after it writes, the landing lat/long in the new bytes is
// replaced by the player's own lat/long and the player position by the same point in that landing's frame (cell
// offset from the aligned origin FUN_14096efb0, the same alignment MoveTo uses). Live game state is never touched.
// Loading then lands the player at their zone natively. The retired distance-based ship hide is restored when
// an older save carries its parked-ship record. The flight model may still deliberately hide the real ship.
namespace rebase
{
	// True from the pre-save message until the post-save message (and inside the record writer).
	bool Saving();
	bool Install();

	// Save-frame bookkeeping. Saving no longer hides the home ship.
	void PreSave();
	// Whether a save made now would be anchored to the player's zone (outdoors on a planet, frame check passes).
	bool CanAnchor();
	// Clear the save-frame bookkeeping flag.
	void PostSave();
	void ResetAfterLoad();  // the flight visual state does not survive a save load
	void Tick();  // periodically restores a ship hidden by the retired distance rule
	// Park the home ship out of sight now (flight start), marking its spot on the planet map.
	void HideShipNow(const char* why);

	// The home ship is hidden by us (globe markers skip it).
	bool ShipHidden();
}
