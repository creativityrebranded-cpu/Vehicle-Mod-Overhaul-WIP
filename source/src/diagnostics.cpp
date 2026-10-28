#include "diagnostics.h"
#include "hangwatch.h"

#include "addresses.h"
#include "game.h"
#include "log.h"

#include <Windows.h>

#include <cmath>
#include <thread>

namespace diagnostics
{
	namespace
	{
		const char* StateName(std::uint8_t s)
		{
			switch (s) {
			case 2: return "idle";
			case 3: return "shifting";
			case 4: return "shift-finished";
			default: return "other";
			}
		}

		void Run()
		{
			const auto limits = game::ReadLimits();
			logger::Write("Limits: border %s at %.0f m, physics half-extent %.0f m, terrain shift after %u cells",
				limits.borderEnabled ? "on" : "off", limits.borderMetres, limits.physicsMetres, limits.terrainShiftCells);

			game::PlanetState last{};
			int tick = 0;
			for (;;) {
				Sleep(500);
				const auto s = game::ReadPlanetState();
				hangwatch::Check(s.haveManager && s.managerState != 2);
				if (!s.havePlayer) {
					continue;
				}

				// Log every terrain shift as it happens.
				if (s.haveManager && (s.gridCentreX != last.gridCentreX || s.gridCentreY != last.gridCentreY)) {
					logger::Write("Terrain grid re-centred to cell (%d, %d), shift (%d, %d) cells",
						s.gridCentreX, s.gridCentreY, s.shiftDeltaX, s.shiftDeltaY);
				}

				// Every 5 s: where the player is relative to the wall and the physics box.
				if (++tick % 10 == 0) {
					const float edge = std::fmax(std::fabs(s.playerPos.x), std::fabs(s.playerPos.y));
					logger::Write("Player (%.1f, %.1f, %.1f) m | %.0f m to wall | %.0f m to physics edge | planet manager %s",
						s.playerPos.x, s.playerPos.y, s.playerPos.z,
						limits.borderMetres - edge, limits.physicsMetres - edge,
						s.haveManager ? StateName(s.managerState) : "not found");
				}
				last = s;
			}
		}
	}

	void Start()
	{
		std::thread(Run).detach();
	}
}
