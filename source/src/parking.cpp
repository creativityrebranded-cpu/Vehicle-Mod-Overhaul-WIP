#include "parking.h"

#include <cmath>

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "telemetry.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

namespace parking
{
	namespace
	{
		struct Spot
		{
			bool valid = false;
			std::uint32_t planet = 0;
			double lat = 0, lon = 0;
		};
		std::mutex g_lock;
		Spot g_spot;

		std::wstring FilePath()
		{
			PWSTR docs = nullptr;
			std::wstring path;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
				path = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessPlanets";
				SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
				path += L"\\parked-ships.txt";
			}
			CoTaskMemFree(docs);
			return path;
		}

		// save name -> "planetHex lat lon"
		std::map<std::string, std::string> ReadAll()
		{
			std::map<std::string, std::string> all;
			std::ifstream in(FilePath());
			std::string line;
			while (std::getline(in, line)) {
				const auto tab = line.find('\t');
				if (tab != std::string::npos) {
					all[line.substr(0, tab)] = line.substr(tab + 1);
				}
			}
			return all;
		}

		void WriteAll(const std::map<std::string, std::string>& all)
		{
			const auto path = FilePath();
			if (path.empty()) {
				return;
			}
			const auto temporary = path + L".tmp";
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			if (!out) {
				logger::Write("Parking: could not open temporary state file");
				return;
			}
			for (const auto& [name, value] : all) {
				out << name << '\t' << value << '\n';
			}
			out.close();
			if (!out || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				logger::Write("Parking: could not publish state file (error %lu)", GetLastError());
			}
		}
	}

	void Park(std::uintptr_t mgr, float x, float y, float z)
	{
		double out[2] = {};
		const float p[3] = { x, y, z };
		reinterpret_cast<void* (*)(std::uintptr_t, double*, const float*)>(game::Base() + addr::kWorldToLatLong)(mgr, out, p);
		std::lock_guard lk(g_lock);
		g_spot = { true, *reinterpret_cast<std::uint32_t*>(mgr + off::kMgrPlanetId), out[1], out[0] };
		logger::Write("Parking: ship left at lat %.6f, long %.6f (planet %08X)", g_spot.lat, g_spot.lon, g_spot.planet);
		telemetry::Record("ship_parked", "\"planet\":\"%08X\",\"lat\":%.6f,\"lon\":%.6f", g_spot.planet, g_spot.lat, g_spot.lon);
	}

	void Track(std::uintptr_t mgr, float x, float y, float z)
	{
		double out[2] = {};
		const float p[3] = { x, y, z };
		reinterpret_cast<void* (*)(std::uintptr_t, double*, const float*)>(game::Base() + addr::kWorldToLatLong)(mgr, out, p);
		const auto planet = *reinterpret_cast<std::uint32_t*>(mgr + off::kMgrPlanetId);
		if (!std::isfinite(out[0]) || !std::isfinite(out[1]) || !planet) {
			return;
		}
		{
			std::lock_guard lk(g_lock);
			// ~20 m on a 1500 km planet is ~1.3e-5 rad; compare loosely so a parked ship does not rewrite every 5 s.
			if (g_spot.valid && g_spot.planet == planet && std::fabs(g_spot.lat - out[1]) < 1.3e-5 && std::fabs(g_spot.lon - out[0]) < 1.3e-5) {
				return;
			}
			g_spot = { true, planet, out[1], out[0] };
		}
		logger::Write("Parking: ship seen at lat %.6f, long %.6f (planet %08X)", out[1], out[0], planet);
	}

	void Clear()
	{
		std::lock_guard lk(g_lock);
		g_spot = {};
	}

	bool Get(std::uint32_t& planet, double& lat, double& lon)
	{
		// The observed ship or this save's own record is the only valid navigation target.
		const auto mgr = *reinterpret_cast<std::uintptr_t*>(game::Base() + addr::kPlanetManagerPtr);
		const auto current = mgr ? *reinterpret_cast<std::uint32_t*>(mgr + off::kMgrPlanetId) : 0;
		if (!current) return false;
		{
			std::lock_guard lk(g_lock);
			if (g_spot.valid && g_spot.planet == current) {
				planet = g_spot.planet;
				lat = g_spot.lat;
				lon = g_spot.lon;
				return true;
			}
		}
		return false;
	}

	void OnSave(const char* saveName)
	{
		if (!saveName || !*saveName) {
			return;
		}
		Spot s;
		{
			std::lock_guard lk(g_lock);
			s = g_spot;
		}
		auto all = ReadAll();
		if (s.valid) {
			char buf[96];
			std::snprintf(buf, sizeof(buf), "%08X %.9f %.9f", s.planet, s.lat, s.lon);
			all[saveName] = buf;
		}
		else {
			all.erase(saveName);
		}
		WriteAll(all);
	}

	void OnLoad(const char* saveName)
	{
		Spot s;
		if (saveName && *saveName) {
			const auto all = ReadAll();
			const auto it = all.find(saveName);
			if (it != all.end()) {
				std::istringstream in(it->second);
				in >> std::hex >> s.planet >> std::dec >> s.lat >> s.lon;
				s.valid = !in.fail();
			}
		}
		std::lock_guard lk(g_lock);
		g_spot = s;
		if (s.valid) {
			logger::Write("Parking: this save's ship spot lat %.6f, long %.6f (planet %08X)", s.lat, s.lon, s.planet);
		}
		else {
			logger::Write("Parking: no ship spot for this save; awaiting observed ship");
		}
	}
}
