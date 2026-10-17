#include "flighttune.h"

#include "log.h"
#include "notify.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace flighttune
{
	namespace
	{
		struct Entry
		{
			const char* name;    // key in flight.ini
			const char* label;   // shown in game
			double fm::Params::* field;
			double step;         // additive step, or the factor when mult
			bool mult;
			double lo, hi;
		};

		// The values worth tuning by feel. Defaults are the model's own (fm::Params).
		const Entry kEntries[] = {
			{ "min_clearance", "Terrain assist: minimum height above ground (m)", &fm::Params::minClearance, 5.0, false, 0.0, 300.0 },
			{ "assist_lift_max", "Terrain assist: strongest lift (m/s)", &fm::Params::assistLiftMax, 5.0, false, 5.0, 150.0 },
			{ "floor_gain", "Terrain assist: how hard it pushes up per metre too low (1/s)", &fm::Params::floorGain, 1.25, true, 0.05, 10.0 },
			{ "brake_decel", "Terrain assist: hardest braking (m/s^2)", &fm::Params::brakeDecel, 10.0, false, 0.0, 300.0 },
			{ "brake_margin", "Terrain assist: gap kept when stopping (m)", &fm::Params::brakeMargin, 5.0, false, 0.0, 200.0 },
			{ "top_speed", "Top speed (m/s)", &fm::Params::vMax, 10.0, false, 10.0, 600.0 },
			{ "accel", "Speed change rate (m/s^2)", &fm::Params::accel, 5.0, false, 1.0, 300.0 },
			{ "throttle_rate", "W/S throttle rate (per s)", &fm::Params::throttleRate, 0.1, false, 0.05, 5.0 },
			{ "lift_speed", "Space/Ctrl up/down speed (m/s)", &fm::Params::liftSpeed, 5.0, false, 1.0, 200.0 },
			{ "strafe_speed", "A/D slide speed (m/s)", &fm::Params::strafeSpeed, 5.0, false, 1.0, 200.0 },
			{ "yaw_per_count", "Mouse turn strength (yaw per mouse speed)", &fm::Params::yawPerCount, 1.2, true, 0.0001, 0.05 },
			{ "pitch_per_count", "Mouse pitch strength (pitch per mouse speed)", &fm::Params::pitchPerCount, 1.2, true, 0.0001, 0.05 },
			{ "yaw_rate_max", "Fastest turn (rad/s)", &fm::Params::easyYawRateMax, 0.1, false, 0.1, 6.0 },
			{ "mouse_on", "Mouse speed to start turning (counts/s)", &fm::Params::mouseOn, 1.25, true, 1.0, 2000.0 },
			{ "mouse_off", "Mouse speed below which the turn stops (counts/s)", &fm::Params::mouseOff, 1.25, true, 0.5, 2000.0 },
			{ "level_rate", "Nose levelling when the mouse is still (1/s)", &fm::Params::levelRate, 0.2, false, 0.0, 10.0 },
			{ "bank_per_yaw", "Lean into turns (looks only)", &fm::Params::bankPerYawRate, 0.1, false, 0.0, 3.0 },
			{ "boost_multiplier", "Rover hover: boost strength multiplier", &fm::Params::boostMultiplier, 1.2, true, 0.05, 20.0 },
			{ "hover_gain", "Rover hover: how hard it holds the height (1/s)", &fm::Params::hoverGain, 1.2, true, 0.05, 20.0 },
			{ "hover_max_up", "Rover hover: fastest rise (m/s)", &fm::Params::hoverMaxUp, 2.0, false, 0.5, 100.0 },
			{ "hover_max_down", "Rover hover: fastest sink (m/s)", &fm::Params::hoverMaxDown, 1.0, false, 0.5, 100.0 },
			{ "level_gain", "Rover hover: how fast it is turned back upright (1/s)", &fm::Params::levelGain, 0.5, false, 0.0, 30.0 },
			{ "hold_rate", "Rover hover: Space/Ctrl hold height change (m/s)", &fm::Params::holdRate, 1.0, false, 0.5, 50.0 },
			{ "brake_height_margin", "Terrain assist: height margin over what is ahead (m)", &fm::Params::brakeHeightMargin, 1.0, false, 0.0, 100.0 },
			{ "clearance", "Hull / rover half-height used by the assist (m)", &fm::Params::clearance, 0.5, false, 0.0, 30.0 },
		};
		constexpr int kCount = int(sizeof(kEntries) / sizeof(kEntries[0]));

		std::mutex g_lock;
		double g_value[kCount];
		bool g_set[kCount] = {};  // tuned (saved) vs default
		int g_sel = 0;
		bool g_keys[256] = {};

		std::wstring FilePath()
		{
			PWSTR docs = nullptr;
			std::wstring path;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
				path = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessPlanets";
				SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
				path += L"\\flight.ini";
			}
			CoTaskMemFree(docs);
			return path;
		}

		void Save()
		{
			FILE* f = nullptr;
			if (_wfopen_s(&f, FilePath().c_str(), L"w") != 0 || !f) {
				return;
			}
			std::fprintf(f, "; SeamlessPlanets flight tuning (Numpad / * select, + - change, 0 default). Only tuned values are listed.\n");
			for (int i = 0; i < kCount; ++i) {
				if (g_set[i]) {
					std::fprintf(f, "%s=%.6g\n", kEntries[i].name, g_value[i]);
				}
			}
			std::fclose(f);
		}

		bool Pressed(int vk)
		{
			const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
			const bool was = g_keys[vk & 0xFF];
			g_keys[vk & 0xFF] = down;
			return down && !was;
		}

		void Show(int i, double v, bool tuned)
		{
			notify::Show("Tune %d/%d: %s = %.4g%s", i + 1, kCount, kEntries[i].label, v, tuned ? "" : " (default)");
		}
	}

	void Load()
	{
		FILE* f = nullptr;
		if (_wfopen_s(&f, FilePath().c_str(), L"r") != 0 || !f) {
			return;
		}
		char line[160];
		int n = 0;
		std::lock_guard lk(g_lock);
		while (std::fgets(line, sizeof(line), f)) {
			char key[64];
			double v = 0;
			if (line[0] == ';' || std::sscanf(line, "%63[^=]=%lf", key, &v) != 2) {
				continue;
			}
			for (int i = 0; i < kCount; ++i) {
				if (std::strcmp(key, kEntries[i].name) == 0) {
					g_value[i] = std::clamp(v, kEntries[i].lo, kEntries[i].hi);
					g_set[i] = true;
					++n;
				}
			}
		}
		std::fclose(f);
		logger::Write("Flight tuning loaded: %d tuned values", n);
	}

	void Apply(fm::Params& p)
	{
		std::lock_guard lk(g_lock);
		for (int i = 0; i < kCount; ++i) {
			if (g_set[i]) {
				p.*(kEntries[i].field) = g_value[i];
			}
		}
	}

	void Tick(fm::Params& p)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		if (pid != GetCurrentProcessId()) {
			return;
		}
		const bool prev = Pressed(VK_DIVIDE), next = Pressed(VK_MULTIPLY), up = Pressed(VK_ADD), down = Pressed(VK_SUBTRACT);
		const bool reset = Pressed(VK_NUMPAD0), show = Pressed(VK_NUMPAD7);
		if (!(prev || next || up || down || reset || show)) {
			return;
		}
		std::lock_guard lk(g_lock);
		if (prev) g_sel = (g_sel + kCount - 1) % kCount;
		if (next) g_sel = (g_sel + 1) % kCount;
		const Entry& e = kEntries[g_sel];
		double& cur = p.*(e.field);
		if (up || down) {
			double v = cur;
			if (e.mult) v = up ? v * e.step : v / e.step;
			else v = up ? v + e.step : v - e.step;
			v = std::clamp(v, e.lo, e.hi);
			cur = v;
			g_value[g_sel] = v;
			g_set[g_sel] = true;
			Save();
			logger::Write("Flight tuning: %s = %.6g", e.name, v);
		}
		if (reset) {
			cur = fm::Params{}.*(e.field);
			g_set[g_sel] = false;
			Save();
			logger::Write("Flight tuning: %s back to default %.6g", e.name, cur);
		}
		Show(g_sel, cur, g_set[g_sel]);
	}
}
