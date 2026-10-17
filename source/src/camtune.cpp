#include "camtune.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "notify.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace camtune
{
	namespace
	{
		constexpr float kDefaultDistance = 12.0f;  // zoom value held in third person while flying
		constexpr float kDefaultHeight = 0.0f;     // fOverShoulderPosZ while flying (0 = leave the game's value)

		struct Values
		{
			float distance = kDefaultDistance;
			float height = kDefaultHeight;
			Eye eye{};
		};
		std::mutex g_lock;
		Values g_values;

		struct Saved
		{
			bool active = false;
			float minZoom = 0, maxZoom = 0, posZ = 0;
		} g_saved;

		bool g_keys[256] = {};

		float& Setting(std::uint32_t rva) { return *reinterpret_cast<float*>(game::Base() + rva); }

		std::wstring FilePath()
		{
			PWSTR docs = nullptr;
			std::wstring path;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
				path = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessPlanets";
				SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
				path += L"\\camera.ini";
			}
			CoTaskMemFree(docs);
			return path;
		}

		void Save()
		{
			Values v;
			{
				std::lock_guard lk(g_lock);
				v = g_values;
			}
			FILE* f = nullptr;
			if (_wfopen_s(&f, FilePath().c_str(), L"w") == 0 && f) {
				std::fprintf(f, "distance=%.3f\nheight=%.3f\neye_forward=%.3f\neye_right=%.3f\neye_up=%.3f\n", v.distance, v.height, v.eye.forward,
					v.eye.right, v.eye.up);
				std::fclose(f);
			}
		}

		void ApplyThirdPerson()
		{
			if (!g_saved.active) {
				return;
			}
			Values v;
			{
				std::lock_guard lk(g_lock);
				v = g_values;
			}
			Setting(addr::kMaxCurrentZoom) = v.distance;
			Setting(addr::kMinCurrentZoom) = v.distance;  // held: the third-person view sits at this zoom
			Setting(addr::kOverShoulderPosZ) = v.height != 0.0f ? v.height : g_saved.posZ;
		}

		bool Pressed(int vk)
		{
			const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
			const bool was = g_keys[vk & 0xFF];
			g_keys[vk & 0xFF] = down;
			return down && !was;
		}

		void Show()
		{
			Values v;
			{
				std::lock_guard lk(g_lock);
				v = g_values;
			}
			notify::Show("Camera: 3rd-person distance %.1f, height %.1f | eye fwd %.2f right %.2f up %.2f", v.distance, v.height, v.eye.forward,
				v.eye.right, v.eye.up);
		}
	}

	void Load()
	{
		FILE* f = nullptr;
		if (_wfopen_s(&f, FilePath().c_str(), L"r") != 0 || !f) {
			return;
		}
		Values v;
		char line[128];
		while (std::fgets(line, sizeof(line), f)) {
			float x = 0;
			if (std::sscanf(line, "distance=%f", &x) == 1) v.distance = x;
			else if (std::sscanf(line, "height=%f", &x) == 1) v.height = x;
			else if (std::sscanf(line, "eye_forward=%f", &x) == 1) v.eye.forward = x;
			else if (std::sscanf(line, "eye_right=%f", &x) == 1) v.eye.right = x;
			else if (std::sscanf(line, "eye_up=%f", &x) == 1) v.eye.up = x;
		}
		std::fclose(f);
		std::lock_guard lk(g_lock);
		g_values = v;
		logger::Write("Camera tuning loaded: distance %.1f, height %.1f, eye (%.2f, %.2f, %.2f)", v.distance, v.height, v.eye.forward, v.eye.right, v.eye.up);
	}

	void Begin()
	{
		if (g_saved.active) {
			return;
		}
		g_saved = { true, Setting(addr::kMinCurrentZoom), Setting(addr::kMaxCurrentZoom), Setting(addr::kOverShoulderPosZ) };
		ApplyThirdPerson();
		logger::Write("Camera: flight values on (game zoom %.1f..%.1f, height %.1f kept)", g_saved.minZoom, g_saved.maxZoom, g_saved.posZ);
	}

	void End()
	{
		if (!g_saved.active) {
			return;
		}
		Setting(addr::kMinCurrentZoom) = g_saved.minZoom;
		Setting(addr::kMaxCurrentZoom) = g_saved.maxZoom;
		Setting(addr::kOverShoulderPosZ) = g_saved.posZ;
		g_saved = {};
	}

	void Tick()
	{
		if (!g_saved.active) {
			return;
		}
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		if (pid != GetCurrentProcessId()) {
			return;
		}
		bool changed = false;
		{
			std::lock_guard lk(g_lock);
			auto& v = g_values;
			if (Pressed(VK_PRIOR)) { v.distance = std::min(500.0f, v.distance * 1.25f); changed = true; }
			if (Pressed(VK_NEXT)) { v.distance = std::max(0.5f, v.distance / 1.25f); changed = true; }
			if (Pressed(VK_HOME)) { v.height += 0.5f; changed = true; }
			if (Pressed(VK_END)) { v.height -= 0.5f; changed = true; }
			if (Pressed(VK_NUMPAD8)) { v.eye.forward += 0.1f; changed = true; }
			if (Pressed(VK_NUMPAD2)) { v.eye.forward -= 0.1f; changed = true; }
			if (Pressed(VK_NUMPAD6)) { v.eye.right += 0.1f; changed = true; }
			if (Pressed(VK_NUMPAD4)) { v.eye.right -= 0.1f; changed = true; }
			if (Pressed(VK_NUMPAD9)) { v.eye.up += 0.1f; changed = true; }
			if (Pressed(VK_NUMPAD3)) { v.eye.up -= 0.1f; changed = true; }
			if (Pressed(VK_NUMPAD5)) { v = Values{}; changed = true; }
		}
		if (changed) {
			ApplyThirdPerson();
			Save();
			Show();
		}
	}

	Eye EyeOffset()
	{
		std::lock_guard lk(g_lock);
		return g_values.eye;
	}
}
