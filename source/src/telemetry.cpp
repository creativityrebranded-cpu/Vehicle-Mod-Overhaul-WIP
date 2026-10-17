#include "telemetry.h"
#include "plugininfo.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "anchor.h"

#include <Windows.h>
#include <ShlObj.h>
#include <Psapi.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "Psapi.lib")

namespace telemetry
{
	namespace
	{
		std::mutex              g_lock;
		std::condition_variable g_wake;
		std::vector<std::string> g_pending;
		std::FILE*              g_file = nullptr;
		LARGE_INTEGER           g_freq{}, g_start{};
		std::atomic_bool        g_ready = false;

		void Writer()
		{
			std::vector<std::string> batch;
			for (;;) {
				{
					std::unique_lock lk(g_lock);
					g_wake.wait_for(lk, std::chrono::milliseconds(250), [] { return g_pending.size() > 256; });
					batch.swap(g_pending);
				}
				for (const auto& line : batch) {
					std::fputs(line.c_str(), g_file);
				}
				if (!batch.empty()) {
					std::fflush(g_file);
				}
				batch.clear();
			}
		}

		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }

		template <class T>
		T At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<const T*>(p + o); }

		// Grid entry -> cell, same validity test as the engine (FUN_140589e60).
		std::uintptr_t GridCell(std::uintptr_t entry)
		{
			if (!entry) {
				return 0;
			}
			const auto state = At<std::uint32_t>(entry, off::kGridEntryState);
			if ((((state & 0x70000000u) + 0xD0000000u) & 0xEFFFFFFFu) != 0) {
				return 0;
			}
			return At<std::uintptr_t>(entry, off::kGridEntryCell);
		}
	}

	std::uint64_t NowUs()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return static_cast<std::uint64_t>((now.QuadPart - g_start.QuadPart) * 1000000 / g_freq.QuadPart);
	}

	void Init()
	{
		QueryPerformanceFrequency(&g_freq);
		QueryPerformanceCounter(&g_start);

		PWSTR docs = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
			return;
		}
		std::wstring dir = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessPlanets";
		CoTaskMemFree(docs);
		SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

		SYSTEMTIME t;
		GetLocalTime(&t);
		wchar_t name[96];
		swprintf_s(name, L"\\%stelemetry-%04u%02u%02u-%02u%02u%02u.jsonl", plugininfo::TelemetryPrefix(), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		g_file = _wfopen((dir + name).c_str(), L"w");
		if (!g_file) {
			logger::Write("Telemetry: could not open file");
			return;
		}
		std::thread(Writer).detach();
		g_ready = true;
	}

	void Record(const char* type, const char* fieldsFmt, ...)
	{
		if (!g_ready) {
			return;
		}
		char fields[768];
		va_list args;
		va_start(args, fieldsFmt);
		std::vsnprintf(fields, sizeof(fields), fieldsFmt, args);
		va_end(args);

		char line[1024];
		const double ms = NowUs() / 1000.0;
		FILETIME wall{};
		GetSystemTimePreciseAsFileTime(&wall);
		const std::uint64_t fileTime = (std::uint64_t(wall.dwHighDateTime) << 32) | wall.dwLowDateTime;
		const auto utcMs = (fileTime - 116444736000000000ull) / 10000ull;
		const auto threadId = GetCurrentThreadId();
		if (fields[0]) {
			std::snprintf(line, sizeof(line), "{\"t\":%.3f,\"utc_ms\":%llu,\"thread\":%lu,\"type\":\"%s\",%s}\n", ms,
				static_cast<unsigned long long>(utcMs), threadId, type, fields);
		}
		else {
			std::snprintf(line, sizeof(line), "{\"t\":%.3f,\"utc_ms\":%llu,\"thread\":%lu,\"type\":\"%s\"}\n", ms,
				static_cast<unsigned long long>(utcMs), threadId, type);
		}
		{
			std::lock_guard lk(g_lock);
			g_pending.emplace_back(line);
			if (g_pending.size() > 256) g_wake.notify_one();
		}
	}

	void Frame()
	{
		static std::uint64_t lastFrameUs = 0;
		static std::uint64_t lastSampleUs = 0;
		static std::uint64_t lastResourceUs = 0;
		static IO_COUNTERS previousIo{};
		static bool ioBaseline = false;
		static game::Vec3 lastSamplePos{};
		static std::unordered_map<std::uintptr_t, std::uint64_t> pendingSince;  // cell -> first seen not attached
		static std::uint8_t previousManagerState = 0xFF;
		static std::uint64_t managerStateSince = 0;
		static std::uint64_t gridWaitSince = 0;

		const auto now = NowUs();
		const double frameMs = lastFrameUs ? (now - lastFrameUs) / 1000.0 : 0.0;
		lastFrameUs = now;

		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto tes = Ptr(addr::kTESPtr);
		const auto mgr = Ptr(addr::kPlanetManagerPtr);
		const std::uint8_t managerState = mgr ? At<std::uint8_t>(mgr, off::kMgrState) : std::uint8_t{ 0 };
		if (managerState != previousManagerState) {
			Record("manager_state", "\"from\":%u,\"to\":%u,\"previous_ms\":%.1f", previousManagerState, managerState,
				managerStateSince ? (now - managerStateSince) / 1000.0 : 0.0);
			previousManagerState = managerState;
			managerStateSince = now;
		}
		if (!player || !tes) {
			return;
		}
		if (now - lastResourceUs >= 2000000) {
			lastResourceUs = now;
			PROCESS_MEMORY_COUNTERS_EX memory{};
			memory.cb = sizeof(memory);
			IO_COUNTERS io{};
			const bool memoryOk = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != 0;
			const bool ioOk = GetProcessIoCounters(GetCurrentProcess(), &io) != 0;
			if (memoryOk && ioOk) {
				if (!ioBaseline) { previousIo = io; ioBaseline = true; }
				size_t telemetryQueue = 0;
				{ std::lock_guard lk(g_lock); telemetryQueue = g_pending.size(); }
				Record("process_resources", "\"working_mb\":%.1f,\"private_mb\":%.1f,\"page_faults\":%lu,\"read_mb\":%.2f,\"write_mb\":%.2f,\"read_ops\":%llu,\"write_ops\":%llu,\"telemetry_queue\":%zu",
					memory.WorkingSetSize / 1048576.0, memory.PrivateUsage / 1048576.0, memory.PageFaultCount,
					(io.ReadTransferCount - previousIo.ReadTransferCount) / 1048576.0,
					(io.WriteTransferCount - previousIo.WriteTransferCount) / 1048576.0,
					static_cast<unsigned long long>(io.ReadOperationCount - previousIo.ReadOperationCount),
					static_cast<unsigned long long>(io.WriteOperationCount - previousIo.WriteOperationCount), telemetryQueue);
				previousIo = io;
			}
		}
		// Follow the anchor: the player outdoors, or their ship while they are inside it.
		const auto anchor = anchor::Current();
		const auto pos = At<game::Vec3>(anchor.ref ? anchor.ref : player, off::kRefPosition);
		const bool interior = !anchor.ref;

		// The outer 5x5 ring is a native soft-load tier (uGridsToDemand is 3).
		// Measure attachment latency only after a cell enters the 3x3 demand tier.
		std::uint32_t loaded = 0, attached = 0, demandLoaded = 0, demandAttached = 0;
		bool demandSplit = false;
		std::uint16_t demandSeenMask = 0, demandReadyMask = 0;
		if (!interior) {
			std::unordered_set<std::uintptr_t> inGrid;
			const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
			const auto count = At<std::uint32_t>(tes, off::kTESGridCount);
			constexpr std::uintptr_t kGridsToDemand = 0x05E8A668;
			demandSplit = count == 25 && *reinterpret_cast<const std::uint32_t*>(game::Base() + kGridsToDemand) == 3;
			const int gx = At<std::int32_t>(tes, off::kTESGridCentre);
			const int gy = At<std::int32_t>(tes, off::kTESGridCentre + 4);
			for (std::uint32_t i = 0; entries && i < count; ++i) {
				const auto cell = GridCell(entries[i]);
				if (!cell) {
					continue;
				}
				inGrid.insert(cell);
				++loaded;
				const bool isAttached = At<std::uint8_t>(cell, off::kCellState) == 10;
				const auto ext = At<std::uintptr_t>(cell, off::kCellExterior);
				const bool inDemand = !demandSplit || (ext && std::abs(At<std::int32_t>(ext, 0) - gx) <= 1 &&
					std::abs(At<std::int32_t>(ext, 4) - gy) <= 1);
				if (inDemand) {
					if (demandSplit) {
						const unsigned bit = static_cast<unsigned>((At<std::int32_t>(ext, 4) - gy + 1) * 3 + (At<std::int32_t>(ext, 0) - gx + 1));
						demandSeenMask |= static_cast<std::uint16_t>(1u << bit);
						if (isAttached) demandReadyMask |= static_cast<std::uint16_t>(1u << bit);
					}
					else { ++demandLoaded; if (isAttached) ++demandAttached; }
				}
				if (isAttached) {
					++attached;
					if (const auto it = pendingSince.find(cell); it != pendingSince.end()) {
						std::int32_t cx = 0, cy = 0;
						if (ext) {
							cx = At<std::int32_t>(ext, 0);
							cy = At<std::int32_t>(ext, 4);
						}
						Record("cell_ready", "\"form\":\"%08X\",\"cx\":%d,\"cy\":%d,\"ms\":%.1f",
							At<std::uint32_t>(cell, off::kFormId), cx, cy, (now - it->second) / 1000.0);
						pendingSince.erase(it);
					}
				}
				else if (!inDemand) {
					pendingSince.erase(cell);  // state 6 is expected for the soft outer ring
				}
				else if (!pendingSince.count(cell)) {
					pendingSince.emplace(cell, now);
				}
				else if (now - pendingSince[cell] > 5000000) {
					// Stuck: log what the cell looks like (rate-limited per cell) to see what it is waiting on.
					static std::unordered_map<std::uintptr_t, std::uint64_t> lastStuckLog;
					auto& lastLog = lastStuckLog[cell];
					if (now - lastLog > 10000000) {
						lastLog = now;
						std::int32_t cx = 0, cy = 0;
						float wx = 0, wy = 0;
						if (ext) {
							cx = At<std::int32_t>(ext, 0);
							cy = At<std::int32_t>(ext, 4);
							wx = At<float>(ext, 0x24);
							wy = At<float>(ext, 0x28);
						}
						Record("cell_stuck", "\"form\":\"%08X\",\"cx\":%d,\"cy\":%d,\"corner\":[%.0f,%.0f],\"state\":%u,\"flags\":\"%08X\",\"refs\":%u,\"s\":%.1f,\"entry_state\":\"%08X\"",
							At<std::uint32_t>(cell, off::kFormId), cx, cy, wx, wy, At<std::uint8_t>(cell, off::kCellState), At<std::uint32_t>(cell, off::kCellFlags),
							At<std::uint32_t>(cell, 0x80), (now - pendingSince[cell]) / 1e6, At<std::uint32_t>(entries[i], off::kGridEntryState));
					}
				}
			}
			// A cell may leave the moving 5x5 grid and return much later. Start a new interval on re-entry;
			// otherwise a 30 s+ "cell_ready" figure can include time when it was not requested at all.
			std::erase_if(pendingSince, [&](const auto& item) { return !inGrid.contains(item.first); });
			if (demandSplit) {
				demandLoaded = static_cast<std::uint32_t>(std::popcount(demandSeenMask));
				demandAttached = static_cast<std::uint32_t>(std::popcount(demandReadyMask));
			}
		}
		else pendingSince.clear();
		const auto demandExpected = demandSplit ? 9u : At<std::uint32_t>(tes, off::kTESGridCount);
		if (!interior && managerState == 2 && demandAttached < demandExpected) {
			if (!gridWaitSince) {
				gridWaitSince = now;
				Record("demand_wait_start", "\"demand_loaded\":%u,\"demand_attached\":%u,\"loaded\":%u,\"attached\":%u", demandLoaded, demandAttached, loaded, attached);
			}
		}
		else if (gridWaitSince) {
			Record("demand_wait_end", "\"ms\":%.1f,\"demand_loaded\":%u,\"demand_attached\":%u,\"loaded\":%u,\"attached\":%u,\"interior\":%d",
				(now - gridWaitSince) / 1000.0, demandLoaded, demandAttached, loaded, attached, interior ? 1 : 0);
			gridWaitSince = 0;
		}

		if (frameMs > 50.0) {
			Record("hitch", "\"frame_ms\":%.1f,\"x\":%.1f,\"y\":%.1f,\"loaded\":%u,\"attached\":%u,\"demand_attached\":%u", frameMs, pos.x, pos.y, loaded, attached, demandAttached);
		}

		if (now - lastSampleUs >= 250000) {
			const double sampleSeconds = lastSampleUs ? (now - lastSampleUs) / 1e6 : 0.0;
			const double sampleDistance = std::hypot(pos.x - lastSamplePos.x, pos.y - lastSamplePos.y);
			const double speed = sampleSeconds > 0.0 && sampleDistance < 500.0 ? sampleDistance / sampleSeconds : 0.0;
			lastSamplePos = pos;
			lastSampleUs = now;
			const auto state = mgr ? At<std::uint8_t>(mgr, off::kMgrState) : 0;
			const auto gx = mgr ? At<std::int32_t>(mgr, off::kMgrGridCentre) : 0;
			const auto gy = mgr ? At<std::int32_t>(mgr, off::kMgrGridCentre + 4) : 0;
			// tes_cell/tes_grid: what the engine's loaded 5x5 cell grid is centred on (does it follow the ship?).
			const std::int32_t anchorCx = static_cast<std::int32_t>(std::floor(pos.x / off::kCellSize));
			const std::int32_t anchorCy = static_cast<std::int32_t>(std::floor(pos.y / off::kCellSize));
			Record("sample", "\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"frame_ms\":%.2f,\"speed_mps\":%.1f,\"interior\":%d,\"in_ship\":%d,\"mgr\":%u,\"grid\":[%d,%d],"
				"\"tes_cell\":[%d,%d],\"tes_grid\":[%d,%d],\"anchor_cell\":[%d,%d],\"loaded\":%u,\"attached\":%u,\"demand_loaded\":%u,\"demand_attached\":%u",
				pos.x, pos.y, pos.z, frameMs, speed, interior ? 1 : 0, anchor.inShip ? 1 : 0, state, gx, gy,
				At<std::int32_t>(tes, off::kTESCurrentCell), At<std::int32_t>(tes, off::kTESCurrentCell + 4),
				At<std::int32_t>(tes, off::kTESGridCentre), At<std::int32_t>(tes, off::kTESGridCentre + 4),
				anchorCx, anchorCy, loaded, attached, demandLoaded, demandAttached);
		}
	}
}
