#include "features.h"

#include "log.h"

#include <Windows.h>
#include <ShlObj.h>

#include <atomic>
#include <mutex>
#include <string>

namespace features
{
	namespace
	{
		constexpr int kCount = int(Id::Count);
		constexpr ULONGLONG kReloadMs = 5000;

		const Info kInfo[kCount] = {
			{ Id::NpcVehicles, "NPCs use vehicles", "NPCs board, drive and fight in vehicles, and use them to travel.", L"context", L"enabled", true },
			{ Id::CombatAi, "Full vehicle combat AI", "Dogfights, tactics, squads, morale and personalities. Off: simpler attack runs.", L"combat", L"ai", true },
			{ Id::Rams, "Vehicle rams", "Combat vehicles may ram and sideswipe.", L"combat", L"ram", true },
			{ Id::HoverOrDrive, "Hover or drive", "Vehicles choose to fly or drive on the ground from the terrain, the trip and the fight. Off: they hover, and drive only to get out of tight spots.", L"nav", L"travel", true },
			{ Id::VehicleDamage, "Vehicle damage and wrecks", "Vehicles take damage from shots and rams, hurt their driver, and wreck at 0 health.", L"hits", L"enabled", true },
			{ Id::HealthBars, "Vehicle health bars", "A vehicle health bar under the enemy's bar, and health lines in the status card.", L"hud", L"healthBars", true },
			{ Id::StatusCard, "Vehicle status card", "The card that shows what your vehicle is doing.", L"hud", L"card", true },
			{ Id::Witnesses, "Crash witnesses", "Bystanders flee or cower near a vehicle crash.", L"witness", L"enabled", true },
			{ Id::Placement, "Parked vehicles (Carfield / Vehicles.esm)", "Places those mods' vehicles at parking spots. Takes effect after loading a save.", L"placement", L"enabled", true },
			{ Id::DebugLog, "Detailed log", "Writes much more to the mod's log, for bug reports.", L"log", L"verbose", false },
		};

		std::atomic<bool> g_on[kCount];
		std::atomic<unsigned> g_gen{ 0 };
		std::mutex g_lock;  // the ini file
		ULONGLONG g_readAt = 0;
		bool g_loaded = false;

		const std::wstring& IniPath()
		{
			static std::wstring path;
			if (path.empty()) {
				PWSTR docs = nullptr;
				if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
					path = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessVehicle\\seamless.ini";
					CoTaskMemFree(docs);
				}
			}
			return path;
		}

		void Reload()
		{
			std::lock_guard lk(g_lock);
			const auto& ini = IniPath();
			for (const auto& f : kInfo) {
				const bool v = ini.empty() ? f.defaultOn : GetPrivateProfileIntW(f.section, f.key, f.defaultOn ? 1 : 0, ini.c_str()) != 0;
				if (!g_loaded || g_on[int(f.id)].load() != v) {
					if (g_loaded) logger::Write("Features: %s -> %s (seamless.ini)", f.label, v ? "on" : "off");
					g_on[int(f.id)].store(v);
					g_gen.fetch_add(1);
				}
			}
			g_loaded = true;
		}
	}

	const Info& InfoOf(Id id) { return kInfo[int(id)]; }

	bool On(Id id)
	{
		if (!g_loaded) {
			// Before the first Tick (a hook at load): the defaults, the ini read once.
			static std::once_flag once;
			std::call_once(once, [] { Reload(); });
		}
		return g_on[int(id)].load(std::memory_order_relaxed);
	}

	void Set(Id id, bool on)
	{
		const auto& f = kInfo[int(id)];
		{
			std::lock_guard lk(g_lock);
			const auto& ini = IniPath();
			if (!ini.empty()) WritePrivateProfileStringW(f.section, f.key, on ? L"1" : L"0", ini.c_str());
		}
		if (g_on[int(id)].exchange(on) != on) g_gen.fetch_add(1);
		logger::Write("Features: %s -> %s (settings menu)", f.label, on ? "on" : "off");
	}

	void ResetAll()
	{
		for (const auto& f : kInfo) Set(f.id, f.defaultOn);
	}

	unsigned Generation() { return g_gen.load(); }

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		if (g_loaded && now - g_readAt < kReloadMs) return;
		g_readAt = now;
		Reload();
	}
}
