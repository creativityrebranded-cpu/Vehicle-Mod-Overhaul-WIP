#pragma once

// The context engine (v1 phase 4): decides from what is going on in the game which NPC gets into which vehicle, how it behaves, and when it gets out.
// It replaces the test keys (board / exit / escort-pursuit toggle); it only calls the public npcvehicles API. Main thread.
// Rules (about every 0.5 s; the actor scan 1 s):
//   fight (any NPC within 80 m of the player is in combat): that NPC with a free vehicle within 40 m, not yet in a job -> BoardNpc (it runs when in combat).
//     A follower of the player boards as Escort, anyone else as Pursuit (hostile, fires the vehicle weapon).
//     A follower with no free vehicle near but a vehicle an NPC drives within 40 m with a free seat -> boards it as a passenger.
//   calm (the player and the driver not in combat) and the vehicle nearly still for 60 s -> everyone gets out (UnloadVehicle).
// Master switch: Logs\SeamlessVehicle\seamless.ini [context] enabled=0 (re-read every 5 s).
namespace context
{
	void Tick();
	void Reset();
}
