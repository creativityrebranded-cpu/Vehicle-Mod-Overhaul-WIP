#pragma once
// Atomic Vehicle + Driver Lifecycle transitions (2026-11-23):
// Centralizes the state transitions that previously spanned 4-6 separate files so a player-entered vehicle
// can never remain keyframed/birth-held and an unseated traffic driver can never ghost into an old cell.

#include "loadedarea.h"
#include "log.h"
#include "simtier.h"
#include "trafficpool.h"
#include "vehiclediag.h"

#include <cstdint>

namespace vehiclelifecycle {

// Transition: ANY -> PlayerOwned.
// Atomically clears BirthHeld, notifies trafficpool, queues physics-safe EnsureDynamicFor, and audits the invariant.
inline void OnPlayerEnteredVehicle(std::uint32_t vehicleId, bool periodicAuditOnly = false) {
	if (!vehicleId) return;
	if (vehiclediag::BirthHeld(vehicleId)) {
		trafficpool::NotePlayerTook(vehicleId, "player_entered");
		vehiclediag::ReleaseBirthToPlayer(vehicleId);
		archproof::State().lifeToPlayer.fetch_add(1, std::memory_order_relaxed);
		if (vehiclediag::BirthHeld(vehicleId)) {
			// Invariant violation: player-entered vehicle is STILL birth-held after ReleaseBirthToPlayer.
			archproof::State().violKeyframedPlayer.fetch_add(1, std::memory_order_relaxed);
			logger::Write("OBS system=arch.invariant event=player_still_birth_held vehicle=%08X", vehicleId);
		}
		return;
	}
	if (!periodicAuditOnly) {
		archproof::State().lifeEnsureDyn.fetch_add(1, std::memory_order_relaxed);
	}
	vehiclediag::EnsureDynamicFor(vehicleId);
}

// Transition: SeatedTrafficDriver -> Unseated.
// Atomically refiles the unseated actor's Havok collision into its current cell so it never ghosts at an old cell.
inline void OnDriverUnseated(std::uint32_t actorId) {
	if (!actorId) return;
	loadedarea::RefileUnseatedActor(actorId);
	archproof::State().lifeUnseatRefile.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace vehiclelifecycle
