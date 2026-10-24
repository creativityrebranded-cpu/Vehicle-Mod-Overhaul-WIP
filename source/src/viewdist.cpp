#include "viewdist.h"
#include "game.h"
#include "log.h"
#include "fvopaths.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace viewdist
{
	namespace
	{
		// The settings' float values (the addresses the game's rebuild FUN_140573d40 reads: 145e86f30 actors, 145e86f70 objects) and that rebuild itself
		// (copies the settings into its per-category table).
		constexpr std::uint32_t kObjectsValue = 0x05E86F70, kActorsValue = 0x05E86F30;
		constexpr std::uint32_t kRebuild = 0x00573D40;      // FUN_140573d40

		// On by default (2026-10-18, the user: "make the mod add this"): vehicles fade out at 3x the game's distance. settings.ini [view] objectFade=1 restores the game's own.
		constexpr float kDefaultObjects = 3.0f;
		ULONGLONG g_readAt = 0, g_checkAt = 0;
		float g_wantObjects = kDefaultObjects, g_wantActors = 1.0f;
		bool g_applied = false;

		float ReadFloat(const std::wstring& ini, const wchar_t* key, float def)
		{
			wchar_t buf[32] = {};
			GetPrivateProfileStringW(L"view", key, L"", buf, 32, ini.c_str());
			if (!buf[0]) return def;
			const float v = static_cast<float>(_wtof(buf));
			return std::isfinite(v) && v >= 0.0f && v <= 1000.0f ? v : def;
		}

		float* Value(std::uint32_t rva) { return reinterpret_cast<float*>(game::Base() + rva); }

		// 1 = changed and rebuilt, 0 = already as wanted, -1 = fault. (No C++ objects here: __try needs a plain frame.)
		int Apply(float wantObjects, float wantActors)
		{
			__try {
				float* objects = Value(kObjectsValue);
				float* actors = Value(kActorsValue);
				if (*objects == wantObjects && *actors == wantActors) return 0;  // (also re-asserts after the game or a load put the defaults back)
				*objects = wantObjects;
				*actors = wantActors;
				reinterpret_cast<void (*)()>(game::Base() + kRebuild)();
				return 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
		}
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		if (!g_readAt || now - g_readAt >= 5000) {
			g_readAt = now;
			PWSTR docs = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
				const std::wstring ini = fvopaths::Settings();
				CoTaskMemFree(docs);
				const float o = ReadFloat(ini, L"objectFade", kDefaultObjects), a = ReadFloat(ini, L"actorFade", 1.0f);
				if (o != g_wantObjects || a != g_wantActors) {
					logger::Write("View distance: objects x%.2f, actors x%.2f (settings.ini [view])", o, a);
					g_wantObjects = o;
					g_wantActors = a;
					g_applied = false;
				}
			}
		}
		if (g_wantObjects == 1.0f && g_wantActors == 1.0f && !g_applied) return;  // the game's own distances asked: nothing touched
		if (now - g_checkAt < 1000) return;
		g_checkAt = now;
		const int r = Apply(g_wantObjects, g_wantActors);
		if (r == 1) {
			g_applied = true;
			logger::Write("View distance: applied (objects x%.2f, actors x%.2f)", g_wantObjects, g_wantActors);
		}
		else if (r < 0) {
			logger::Write("View distance: could not write the setting (disabled)");
			g_wantObjects = g_wantActors = 1.0f;
			g_applied = false;
		}
	}

	void Reset() { g_checkAt = 0; }
}
