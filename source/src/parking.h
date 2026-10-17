#pragma once

#include <cstdint>

// Where the player left their ship, as planet lat/long. Once the ship is hidden (saves are anchored to the player's
// zone, so the hidden ship's own position no longer means anything), the planet map's "My Ship" marker shows this
// spot. Kept per save (SFSE has no plugin co-save here): a small file next to the plugin's logs maps save names to
// parked spots, written on every save and read on load.
namespace parking
{
	// The ship was hidden while standing at this world position on the current planet (main thread).
	void Park(std::uintptr_t mgr, float x, float y, float z);
	void Clear();
	// 0.57.2: the ship is here (3D loaded in the player's world): record its spot when it moved more than 20 m. Also kept
	// as the planet's last known ship spot, used when a save has no record of its own.
	void Track(std::uintptr_t mgr, float x, float y, float z);

	bool Get(std::uint32_t& planet, double& lat, double& lon);

	// SFSE PreSaveGame / PreLoadGame (save name from the message).
	void OnSave(const char* saveName);
	void OnLoad(const char* saveName);
}
