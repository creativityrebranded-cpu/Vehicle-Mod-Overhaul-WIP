#pragma once

// Read-only probe (2026-10-27): does a vehicle reference already own an engine
// MovementControllerComponent? The REV-8's base form comes from
// EnterableControllableTemplate, which lists BGSPathingData_Component, and the
// engine creates a controller for every reference whose base has it.
// Every 5 s it looks up the controller of the player's vehicle and the nearest
// others with the engine's own non-actor lookup (14068a800) and logs what it
// finds: the component, its data, the movement type (MOVT) and the goal state.
// It never submits, cancels or changes anything.
namespace native_movement_probe
{
	void Tick();  // main thread, once per frame
}
