#pragma once
#include <Windows.h>
#include <ShlObj.h>
#include <atomic>
#include <cstdint>
#include <string>

// Flying Vehicle Overhaul (FVO) files (0.73.0, the user: every name says FVO). Settings, HUD layout, bindings and the
// placement recordings live in Documents\My Games\Starfield\SFSE\Logs\FlyingVehicleOverhaul\; the feature switches are
// settings.ini there. Until 0.72.0 the folder was Logs\SeamlessVehicle\ and the switches seamless.ini: the first call moves
// that folder (and renames the file) once, so nobody loses their settings. If the move fails the old folder stays in use.
namespace fvopaths
{
	inline const std::wstring& Dir()
	{
		static const std::wstring dir = [] {
			PWSTR docs = nullptr;
			std::wstring logs;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) logs = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs";
			CoTaskMemFree(docs);
			if (logs.empty()) return std::wstring();
			const std::wstring now = logs + L"\\FlyingVehicleOverhaul", old = logs + L"\\SeamlessVehicle";
			const auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
			std::wstring use = now;
			if (!exists(now) && exists(old) && !MoveFileExW(old.c_str(), now.c_str(), 0)) use = old;  // in use: keep the old one
			SHCreateDirectoryExW(nullptr, use.c_str(), nullptr);
			const std::wstring ini = use + L"\\settings.ini", oldIni = use + L"\\seamless.ini";
			if (!exists(ini) && exists(oldIni)) MoveFileExW(oldIni.c_str(), ini.c_str(), 0);
			return use;
		}();
		return dir;
	}
	inline std::wstring File(const wchar_t* name) { return Dir().empty() ? std::wstring() : Dir() + L"\\" + name; }
	inline const std::wstring& Settings()
	{
		static const std::wstring ini = File(L"settings.ini");
		return ini;
	}

	// Has a file changed since this watcher last looked? Its write time and size, checked at most every 5 s; true on the first call
	// (2026-10-29 audit: modules re-read their ini keys every 5 s whether or not anything had changed, each read opening and parsing
	// the file in a OneDrive folder). Readers keep the same 5 s pickup of hand edits and settings-menu changes. Any thread.
	struct Watch
	{
		std::atomic<ULONGLONG> at{ 0 };
		std::atomic<std::uint64_t> seen{ ~0ull };
		bool Changed(const std::wstring& path)
		{
			const ULONGLONG now = GetTickCount64();
			ULONGLONG prev = at.load();
			if (prev && now - prev < 5000) return false;
			if (!at.compare_exchange_strong(prev, now)) return false;
			WIN32_FILE_ATTRIBUTE_DATA a{};
			std::uint64_t stamp = 0;
			if (!path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
				stamp = ((std::uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime) ^ (std::uint64_t(a.nFileSizeLow) << 1) ^ 1;
			return seen.exchange(stamp) != stamp;
		}
	};
}
