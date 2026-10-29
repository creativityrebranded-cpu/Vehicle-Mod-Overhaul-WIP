#pragma once
// Cold-start traffic pool (2026-10-28, the user: traffic that is already driving when you arrive, "without risk"). The traffic
// addon names spawn spots (on the ground, checked flat and open offline) for the player's world; core creates one REV-8 with a
// citizen driver per spot, ONCE per save (the ids are kept in trafficpool.ini and reused on later visits), seats the driver
// with the proven instant seat (sit state checked), then asks the addon for a trip and starts it with the driver already
// aboard: climb to a free slot of the route, follow it, land at a parking row. A pair that does not load or will not sit is
// disabled. Exports FVO_PoolSet (fvo_addon_api.h).
namespace trafficpool
{
	void Tick();   // main thread, every frame
	void Reset();  // a save load
}
