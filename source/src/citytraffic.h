#pragma once
#include <cstdint>

// City ring traffic, stage 3 (2026-10-24, the user): the dedicated vehicles and pilots of FVO_CityTraffic.esm
// (scripts/make_traffic_esm.py). Vehicle k is that plugin's local 0x800 + k, its pilot 0x820 + k. Here: every pilot is seated
// in its own vehicle whenever both are loaded, and the traffic vehicles are kept out of every other system's hands (fight
// boarding, everyday traffic, the placement patch). The clock schedule (ringschedule.h) moves them in stage 4.
namespace citytraffic
{
	void Tick();                                 // main thread, every frame (works every 2 s)
	void Reset();                                // a save load: the plugin's ids are looked up again
	bool IsTrafficVehicle(std::uint32_t formId); // one of the traffic vehicles (false when the plugin is not loaded)
	bool IsPoolVehicle(std::uint32_t formId); // pool identities stay stable; excluded from base/row swaps
	int VehicleIndex(std::uint32_t formId);      // its k, or -1
}
