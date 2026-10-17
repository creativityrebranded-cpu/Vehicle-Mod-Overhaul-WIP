#include "shipvehicle.h"

#include "addresses.h"
#include "controlmap.h"
#include "game.h"
#include "log.h"
#include "notify.h"
#include "shipmodel.h"
#include "vehicleflight.h"

#include <Windows.h>

#include <cstdint>
#include <initializer_list>

namespace shipvehicle
{
	namespace
	{
		bool g_want = false;
		bool g_keyDown = false;
		int g_toggle = 0;
		std::uintptr_t g_vehicle = 0;
		std::uintptr_t g_loggedPilotShip = 0;
		ULONGLONG g_lastPilotCheck = 0;

		bool Focused()
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}

		void ChooseKey()
		{
			if (g_toggle) return;
			for (const int vk : std::initializer_list<int>{ 'V', 'B', 'J', VK_F8 }) {
				if (controlmap::KeyFreeIn(vk, "Vehicle")) {
					g_toggle = vk;
					logger::Write("Ship vehicle preview: visual toggle %s", controlmap::KeyName(vk).c_str());
					return;
				}
			}
		}
	}

	void Tick()
	{
		ChooseKey();
		const auto player = *reinterpret_cast<std::uintptr_t*>(game::Base() + addr::kPlayerCharacterPtr);
		// The requested seat launch must reuse the exterior that was captured before boarding. Record whether
		// that prerequisite survived the interior cell switch; do not leave the pilot seat or move the real ship.
		const ULONGLONG now = GetTickCount64();
		if (now - g_lastPilotCheck >= 500) {
			g_lastPilotCheck = now;
			std::uintptr_t pilotShip = 0;
			if (player) {
				const auto ship = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, char, std::uintptr_t, std::uintptr_t)>(
					game::Base() + addr::kShipOfRef)(player, 1, 0, 0);
				if (ship) {
					float piloting = 0.0f;
					auto subject = ship;
					reinterpret_cast<char (*)(std::uintptr_t*, std::uintptr_t, std::uintptr_t, float*)>(game::Base() + addr::kCondGetActorInShipPilotSeat)(
						&subject, player, 0, &piloting);
					if (piloting >= 0.5f) pilotShip = ship;
				}
			}
			if (pilotShip != g_loggedPilotShip) {
				g_loggedPilotShip = pilotShip;
				if (pilotShip) {
					logger::Write("Ship vehicle: pilot seated in %08X; exterior hull cache %s; seat flight not enabled",
						*reinterpret_cast<std::uint32_t*>(pilotShip + 0x28), shipmodel::Ready() ? "ready" : "missing");
				}
			}
		}
		const auto vehicle = player ? reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(game::Base() + addr::kPlayerVehicle)(player) : 0;
		if (!vehicle || vehicle != g_vehicle) {
			g_want = false;
			g_vehicle = vehicle;
		}
		const bool down = vehicle && g_toggle && Focused() && (GetAsyncKeyState(g_toggle) & 0x8000);
		if (down && !g_keyDown) {
			g_want = !g_want;
			notify::Log(g_want ? "Ship hull preview on vehicle (visual only)" : "Vehicle look restored");
			logger::Write("Ship vehicle preview: %s", g_want ? "on" : "off");
		}
		g_keyDown = down;
		shipmodel::ShowOnVehicle(g_want && vehicleflight::Active() ? vehicle : 0);
	}

	void Reset()
	{
		g_want = false;
		g_keyDown = false;
		g_vehicle = 0;
		g_loggedPilotShip = 0;
		g_lastPilotCheck = 0;
		shipmodel::Reset();
	}
}
