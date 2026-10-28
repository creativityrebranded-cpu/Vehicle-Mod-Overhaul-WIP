#include "features.h"

#include "log.h"
#include "fvopaths.h"
#include "featureinstaller.h"

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
			{ Id::HoverOrDrive, "Hover or drive", "Vehicles choose to fly or drive on the ground from the terrain, the trip and the fight. Off: they hover, and drive only to get out of tight spots.", L"nav", L"travel", true },
			{ Id::VehicleDamage, "Vehicle damage and wrecks", "Vehicles take damage from shots and crashes, hurt their driver, and wreck at 0 health.", L"hits", L"enabled", true },
			{ Id::HealthBars, "Vehicle health bars", "A vehicle health bar under the enemy's bar, and health lines in the status card.", L"hud", L"healthBars", true },
			{ Id::StatusCard, "Vehicle status card", "The card that shows what your vehicle is doing.", L"hud", L"card", true },
			{ Id::Witnesses, "Crash witnesses", "Bystanders flee or cower near a vehicle crash.", L"witness", L"enabled", true },
			{ Id::Placement, "Removed vehicle randomizer", "Compatibility key only; vehicle types remain as supplied by their plugins.", L"placement", L"enabled", false },
			{ Id::FreeRoll, "Free roll (hover)", "Your hovering vehicle can roll all the way round and turns about its own axes. Off: the roll stops at 46 degrees and levels by itself.", L"flight", L"freeRoll", false },
			{ Id::FlightAssist, "Flight assist", "Free roll levels near upright. With Physical VTOL, bounded thrusters and torque stabilize drift and spin. Off: manual VTOL retains uncommanded momentum.", L"flight", L"assist", true },
			{ Id::PhysicalVtol, "Physical VTOL (experimental)", "Player hover uses thruster forces and torque with native mass and inertia. Flight assist stabilizes it; off preserves uncommanded spin and drift. No wing aerodynamics yet.", L"flight", L"physicalVtol", false },
			{ Id::PhysicalAi, "Physical VTOL for AI pilots (experimental)", "Autopilot trips, summons and NPC-driven vehicles fly with thruster forces and torque on native mass and inertia: turns bank, speed builds and coasts. A vehicle whose speed the game keeps wiping falls back to the standard flight.", L"flight", L"physicalAi", false },
			{ Id::PlacementMode, "Placement mode", "The scanner's action key edits vehicle spots instead of targeting. Tap: save a parking spot where you aim. Hold: delete the saved spot you aim at. Double tap: make it this area's summon point, where your vehicle appears when summoned from far away. Spots take effect after the placements are rebuilt.", L"placement", L"recording", false },
			{ Id::NativeGround, "Ground planner and native driving (experimental)", "Vehicles plan their routes on the ground first: around water, cliffs, steep slopes and buildings, on roads where there are any, flying only where driving cannot go. Vehicles with a driver use the game's own ground driving. Off: the old hover-or-drive rules.", L"nav", L"groundPlanner", false },
			{ Id::DebugLog, "Detailed log", "Writes much more to the mod's log, for bug reports.", L"log", L"verbose", false },
			{ Id::Traffic, "Everyday traffic", "Calm NPCs take parked vehicles near you on round trips to nearby settlements and outposts, then park them again. No vehicles are created.", L"context", L"traffic", true },
			{ Id::RouteMarkers, "Show traffic routes", "Probes in the sky along every route in the FVO_Traffic files of the world you are in, larger ones at exits and landing places. The routes themselves are only defined in the files.", L"traffic", L"markers", false },
		};

		std::atomic<bool> g_on[kCount];
		std::atomic<unsigned> g_gen{ 0 };
		std::mutex g_lock;  // the ini file
		ULONGLONG g_readAt = 0;
		std::atomic<bool> g_loaded{false};
        bool g_haveStamp=false;
        std::uint64_t g_stamp=0,g_size=0;

		const std::wstring& IniPath()
		{
			static std::wstring path;
			if (path.empty()) {
				PWSTR docs = nullptr;
				if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
					path = fvopaths::Settings();
					CoTaskMemFree(docs);
				}
			}
			return path;
		}

		void Reload()
		{
			std::lock_guard lk(g_lock);
			const auto& ini = IniPath();
            static bool installerChecked=false;
            if(!installerChecked){
                installerChecked=true;
                wchar_t exe[32768]{};const auto length=GetModuleFileNameW(nullptr,exe,DWORD(std::size(exe)));
                if(length&&length<std::size(exe)){
                    std::wstring directory(exe,length);const auto slash=directory.find_last_of(L"\\/");
                    if(slash!=std::wstring::npos){
                        directory.resize(slash);directory+=L"\\Data\\SFSE\\Plugins\\FlyingVehicleOverhaul.features";
                        const auto result=featureinstaller::Apply(directory,ini,kInfo);
                        if(result.status==featureinstaller::Status::Applied)
                            logger::Write("Installer features: applied %u changed choices; unchanged choices retain gameplay settings",result.changed);
                        else if(result.status==featureinstaller::Status::Invalid||result.status==featureinstaller::Status::Failed)
                            logger::Write("Installer features: configuration import failed (%d); existing settings retained where possible",int(result.status));
                    }
                }
            }
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            const bool exists=!ini.empty()&&GetFileAttributesExW(ini.c_str(),GetFileExInfoStandard,&attributes);
            const auto error=exists?ERROR_SUCCESS:GetLastError();
            const bool known=exists||ini.empty()||error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND;
            const std::uint64_t stamp=exists?(std::uint64_t(attributes.ftLastWriteTime.dwHighDateTime)<<32)|attributes.ftLastWriteTime.dwLowDateTime:0;
            const std::uint64_t size=exists?(std::uint64_t(attributes.nFileSizeHigh)<<32)|attributes.nFileSizeLow:0;
            if(g_loaded&&g_haveStamp&&known&&stamp==g_stamp&&size==g_size)return;
			for (const auto& f : kInfo) {
				const bool v = Available(f.id) && (ini.empty() ? f.defaultOn : GetPrivateProfileIntW(f.section, f.key, f.defaultOn ? 1 : 0, ini.c_str()) != 0);
				if (!g_loaded || g_on[int(f.id)].load() != v) {
					if (g_loaded) logger::Write("Features: %s -> %s (settings.ini)", f.label, v ? "on" : "off");
					g_on[int(f.id)].store(v);
					g_gen.fetch_add(1);
				}
			}
			g_loaded = true;
            g_haveStamp=known;g_stamp=stamp;g_size=size;
		}
	}

	const Info& InfoOf(Id id) { return kInfo[int(id)]; }

	bool On(Id id)
	{
		if (!Available(id)) return false;
		if (!g_loaded) {
			// Before the first Tick (a hook at load): the defaults, the ini read once.
			static std::once_flag once;
			std::call_once(once, [] { Reload(); });
		}
		return g_on[int(id)].load(std::memory_order_relaxed);
	}

	void Set(Id id, bool on)
	{
		on = on && Available(id);
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
