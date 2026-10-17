// World re-centring on planets: the operation Bethesda left unfinished (BSWorldOriginShiftEvent has working
// subscribers but is never sent). See the Starfield Engine Reference note, sections "World origin shifting",
// "Loaded cells and iterating objects", "Sending events" and "Physics world access".
//
// Order (one frame, main thread):
//   1. Havok: shift all bodies and the broadphase (hknpWorld::shiftWorld).
//   2. Objects: every loaded reference's stored position and 3D (world transforms, bounds).
//   3. Cells: world corner of each loaded exterior cell; re-key the worldspace coordinate -> cell map and
//      each cell's own coordinates; TES grid centre and current cell.
//   4. Planet manager: terrain grid centre, and the planet-wide origin coordinate moved the other way,
//      so every world cell still maps to the same place on the planet.
//   5. Send BSWorldOriginShiftEvent: nav grid, audio and camera shift themselves.

#include "recentre.h"
#include "navgen.h"
#include "hangwatch.h"

#include "api_core.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "notify.h"
#include "content.h"
#include "flight.h"
#include "handoff.h"
#include "landing.h"
#include "sphere.h"
#include "prefetch.h"
#include "telemetry.h"
#include "watchpoint.h"
#include "trace.h"
#include "terrainguard.h"
#include "hook.h"
#include "rebase.h"
#include "parking.h"

#include <mutex>
#include <shared_mutex>
#include <intrin.h>

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <unordered_map>
#include <climits>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <unordered_set>
#include <vector>
#include <map>
#include <string>

// Bethesda's own terrain shift with the world-origin delta (+0x1EC; see ShiftTerrainViaEngine). Kept on, per the
// user: the goal is to complete the path Bethesda started. The first re-centres that actually ran (0.11.0)
// crashed within a second of it; the crash logger pinpoints what is still missing.
// 0.51.4: the live landing follow stays off. The first time it ran (2026-10-15 03:45, New Atlantis, hovering) the
// terrain page buffers of the old landing were freed while blocks were still being built: 300+ access violations in
// the block builder on many threads within 5 ms, then the game died. Saves already land at the player ("save as
// landing"), which is what the follow was for.
constexpr bool kLiveLandingFollow = false;
// 0.55.0: the landing lat/long records (+0xB0 origin, +0x98 landing) follow every shift (the safe half of the engine's
// landing move; see FollowLandingRecords). The terrain window is never rebuilt live.
constexpr bool kLandingRecordsFollow = true;
// 0.52.0: no live world shifting at all (the re-anchor rebuilds the world through the engine instead).
constexpr bool kLiveRecentre = true;  // 0.52.1: back on (restructured shift: data now, derived parts staggered)
bool kEngineTerrainShift = true;  // runtime: F8 = on, Shift+F8 = off (phase 1 A/B test)

namespace recentre
{
	static std::atomic_uint64_t g_nativeSaveLoadSerial{ 0 };
	std::uint64_t NativeSaveLoadSerial() { return g_nativeSaveLoadSerial.load(std::memory_order_acquire); }
	namespace
	{
		using game::Base;

		template <class T>
		T& At(std::uintptr_t p, std::uint32_t offset)
		{
			return *reinterpret_cast<T*>(p + offset);
		}

		void SubVec(std::uintptr_t p, float dx, float dy)
		{
			auto* v = reinterpret_cast<float*>(p);
			v[0] -= dx;
			v[1] -= dy;
		}

		void ShiftTESGridUpdateHistory(std::uintptr_t tes, std::int32_t cx, std::int32_t cy)
		{
			// FUN_140576530 generates the next row/column of cell requests from the difference between
			// TES+0xC4/+0xC8 and this cached previous cell. Leaving the cache in the old frame produced
			// requests 36-92 cells away immediately after a re-centre (2026-10-16 live trace).
			const auto oldX = At<std::int32_t>(tes, off::kTESCurrentCell);
			const auto oldY = At<std::int32_t>(tes, off::kTESCurrentCell + 4);
			const auto cachedX = At<std::int32_t>(tes, off::kTESGridUpdatePreviousCell);
			const auto cachedY = At<std::int32_t>(tes, off::kTESGridUpdatePreviousCell + 4);
			// The engine uses INT_MAX as an unset cell. Only translate an initialized, nearby cache.
			if (oldX != INT_MAX && oldY != INT_MAX &&
				std::abs(std::int64_t(cachedX) - oldX) <= 16 &&
				std::abs(std::int64_t(cachedY) - oldY) <= 16) {
				At<std::int32_t>(tes, off::kTESGridUpdatePreviousCell) = cachedX - cx;
				At<std::int32_t>(tes, off::kTESGridUpdatePreviousCell + 4) = cachedY - cy;
				logger::Write("Re-centre: native grid-update history (%d, %d) -> (%d, %d) with current (%d, %d) -> (%d, %d)",
					cachedX, cachedY, cachedX - cx, cachedY - cy, oldX, oldY, oldX - cx, oldY - cy);
			} else {
				logger::Write("Re-centre: native grid-update history not translated: cached (%d, %d), current (%d, %d)",
					cachedX, cachedY, oldX, oldY);
			}
		}

		std::uintptr_t Global(std::uint32_t rva)
		{
			return *reinterpret_cast<std::uintptr_t*>(Base() + rva);
		}

		void MarkSyncBoundsDirty();  // body->actor sync box (below)

		// Saves are written as a landing at the player (BeginLandingSave); false = the original landing's frame (0.39-0.46).
		constexpr bool kSaveAsLanding = true;

		template <class Fn>
		Fn EngineFn(std::uint32_t rva)
		{
			return reinterpret_cast<Fn>(Base() + rva);
		}

		void Lock(std::uintptr_t lock) { EngineFn<void (*)(std::uintptr_t)>(addr::kRWLockWrite)(lock); }
		void Unlock(std::uintptr_t lock) { EngineFn<void (*)(std::uintptr_t)>(addr::kRWLockWriteRelease)(lock); }
		// FUN_1422CC3D0 takes this eight-byte BSReadWriteLock by incrementing the
		// writer count (0x1000), then setting the writer bit (0x400000).
		// Only acquire an entirely idle lock here: waiting while holding the planet
		// manager lock can deadlock a cell-load job that needs that manager.
		bool TryLockIdle(std::uintptr_t lock)
		{
			auto* state = reinterpret_cast<volatile LONG*>(lock + 4);
			if (InterlockedCompareExchange(state, 0x401000, 0) != 0) return false;
			InterlockedExchange(reinterpret_cast<volatile LONG*>(lock), static_cast<LONG>(GetCurrentThreadId()));
			return true;
		}


		// The TES grid centre / current cell rewrite, with the navmesh module's not-yet-started entries translated into
		// the same frame. The engine's navmesh update task reads the centre and walks the loaded-cell array under the
		// shared TES lock, and the engine itself rewrites that array under the exclusive lock (FUN_14058AC20,
		// FUN_141435C30, FUN_141895460), so the rewrite takes it exclusively. Only short, non-waiting work is done while
		// held, so a reader that needs this thread cannot deadlock it. The caller has already established that no
		// update task is queued or running (navgen::InFlight), which is the only producer that appends entries.
		void ShiftTESGrid(std::uintptr_t tes, std::int32_t cx, std::int32_t cy)
		{
			const auto lock = tes + off::kTESGridLock;
			Lock(lock);
			ShiftTESGridUpdateHistory(tes, cx, cy);
			At<std::int32_t>(tes, off::kTESGridCentre) -= cx;
			At<std::int32_t>(tes, off::kTESGridCentre + 4) -= cy;
			At<std::int32_t>(tes, off::kTESCurrentCell) -= cx;
			At<std::int32_t>(tes, off::kTESCurrentCell + 4) -= cy;
			const auto moved = navgen::TranslatePending(cx, cy);
			Unlock(lock);
			if (moved) {
				logger::Write("Re-centre: %u pending navmesh entries translated by (%d, %d)", moved, -cx, -cy);
			}
		}

		// ---- 1. Havok -------------------------------------------------------------------------------

		bool ShiftPhysics(float dx, float dy)
		{
			const auto worldSlot = Global(addr::kExteriorWorldPtr);
			const auto bhkWorld = worldSlot ? *reinterpret_cast<std::uintptr_t*>(worldSlot) : 0;
			const auto hknp = bhkWorld ? At<std::uintptr_t>(bhkWorld, off::kBhkHknpWorld) : 0;
			if (!hknp) {
				logger::Write("Re-centre: no exterior physics world");
				return false;
			}
			const auto iface = hknp + off::kHknpShiftIface;
			auto** vtbl = *reinterpret_cast<void***>(iface);
			if (reinterpret_cast<std::uintptr_t>(vtbl[76]) != Base() + addr::kHavokShiftWorld) {
				logger::Write("Re-centre: physics world vtable does not match hknpWorld::shiftWorld");
				return false;
			}
			alignas(16) float shift[4] = { -dx, -dy, 0.0f, 0.0f };  // bodies move by +shift
			Lock(hknp + off::kHknpLock);
			reinterpret_cast<void (*)(std::uintptr_t, const float*)>(vtbl[76])(iface, shift);
			Unlock(hknp + off::kHknpLock);
			return true;
		}

		// ---- 2. Objects -----------------------------------------------------------------------------

		// Class name from the MSVC RTTI behind the vtable, or "?" (SEH-guarded: no C++ objects in here).
		const char* RttiNameOf(std::uintptr_t obj)
		{
			__try {
				const auto vtbl = *reinterpret_cast<std::uintptr_t*>(obj);
				const auto col = *reinterpret_cast<std::uintptr_t*>(vtbl - 8);
				const auto td = Base() + *reinterpret_cast<std::uint32_t*>(col + 0x0C);
				return reinterpret_cast<const char*>(td + 0x10);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return "?";
			}
		}

		void ShiftNode(std::uintptr_t node, float dx, float dy, bool root, int depth)
		{
			if (!node || depth > 64) {
				return;
			}
			if (root) {
				SubVec(node + off::kNodeLocalT, dx, dy);  // roots hang under world-space containers
			}
			SubVec(node + off::kNodeWorldT, dx, dy);
			SubVec(node + off::kNodePrevWorldT, dx, dy);
			SubVec(node + off::kNodeBoundCentre, dx, dy);

			auto** vtbl = *reinterpret_cast<void***>(node);
			const auto asNode = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(vtbl[off::kVtblIsNode])(node);
			if (!asNode) {
				return;
			}
			const auto children = At<std::uintptr_t*>(asNode, off::kNodeChildren);
			const auto count = At<std::uint16_t>(asNode, off::kNodeChildCount);
			for (std::uint16_t i = 0; children && i < count; ++i) {
				ShiftNode(children[i], dx, dy, false, depth + 1);
			}
		}

		struct ShiftContext
		{
			float dx, dy;
			std::unordered_set<std::uintptr_t> done;
			std::uint32_t refs = 0;
			std::unordered_set<std::uintptr_t> roots;  // 3D roots moved (objects and scene geometry)
			bool dataOnly = false;     // positions only (saves): no 3D, no engine update
			bool haveCentre = false;   // engine 3D update only near this point (after the shift)
			float cx = 0, cy = 0;
			// 0.53.1: objects beyond kEngineUpdateRadius get their engine 3D update after the shift, a batch per frame.
			// Without it they stayed drawn at the old place while their collision had moved: invisible solid objects
			// (2026-10-15: the rover hit one at 100 m/s 3.3 km from the shift point; the player stood on one 9 m up).
			std::vector<std::pair<std::uintptr_t, std::uint32_t>> far3D;  // (reference, form id)
		};

		std::mutex g_far3DLock;
		std::vector<std::pair<std::uintptr_t, std::uint32_t>> g_far3D;
		void EngineUpdate3D(std::uintptr_t node);

		// At the sync point (game-update thread), every frame: up to kFar3DPerFrame of the far objects get the engine 3D
		// update. The reference is re-read (form id must match: it may have unloaded meanwhile).
		void ServiceFar3D()
		{
			constexpr std::size_t kFar3DPerFrame = 400;
			std::vector<std::pair<std::uintptr_t, std::uint32_t>> batch;
			{
				std::lock_guard lk(g_far3DLock);
				if (g_far3D.empty()) {
					return;
				}
				const auto n = std::min(kFar3DPerFrame, g_far3D.size());
				batch.assign(g_far3D.end() - n, g_far3D.end());
				g_far3D.resize(g_far3D.size() - n);
			}
			unsigned done = 0;
			for (const auto& [ref, formId] : batch) {
				if (!ref || game::SafeRead<std::uint32_t>(ref + off::kFormId).value_or(0) != formId) {
					continue;
				}
				const auto loaded = At<std::uintptr_t>(ref, off::kRefLoadedData);
				const auto node = loaded ? At<std::uintptr_t>(loaded, off::kLoadedData3D) : 0;
				if (node) {
					EngineUpdate3D(node);
					++done;
				}
			}
			static ULONGLONG lastLog = 0;
			std::size_t left;
			{
				std::lock_guard lk(g_far3DLock);
				left = g_far3D.size();
			}
			if (left == 0 || GetTickCount64() - lastLog > 2000) {
				lastLog = GetTickCount64();
				logger::Write("Re-centre: far 3D updates: %u done this frame, %zu left", done, left);
			}
		}

		// Objects farther than this from the player get their 3D moved but not the engine update (renderer push): they
		// are beyond draw distance. The update was most of the 45 ms object step (0.38.5 telemetry, ~21k objects).
		constexpr float kEngineUpdateRadius = 2500.0f;

		// The engine's own 3D update after a move, as the move routine (FUN_142062f50) does it: the root's
		// UpdateDownwardPass (vtbl +0x248) with a zeroed NiUpdateData stamped with the current update frame
		// (+0x44), then the parent's upward pass (vtbl +0x2E8) unless the data's bit 9 was set. Writing the transform
		// alone left static geometry drawn at the old place (0.34.1: F11, which runs this path, brought objects back).
		void EngineUpdate3D(std::uintptr_t node)
		{
			if (!node) {
				return;
			}
			alignas(16) std::uint8_t data[0x60] = {};
			*reinterpret_cast<std::uint32_t*>(data + 0x44) = *reinterpret_cast<std::uint32_t*>(Base() + addr::kUpdateFrameId);
			auto** vtbl = *reinterpret_cast<void***>(node);
			reinterpret_cast<void (*)(std::uintptr_t, void*)>(vtbl[0x248 / 8])(node, data);
			const auto parent = At<std::uintptr_t>(node, off::kNodeParent);
			if (parent && ((*reinterpret_cast<std::uint32_t*>(data + 0x38) >> 9) & 1) == 0) {
				auto** pv = *reinterpret_cast<void***>(parent);
				reinterpret_cast<void (*)(std::uintptr_t, void*)>(pv[0x2E8 / 8])(parent, data);
			}
		}

		void ShiftRef(ShiftContext& ctx, std::uintptr_t ref)
		{
			if (!ref || !ctx.done.insert(ref).second) {
				return;
			}
			SubVec(ref + off::kRefPosition, ctx.dx, ctx.dy);
			if (ctx.dataOnly) {
				++ctx.refs;
				return;
			}
			if (const auto loaded = At<std::uintptr_t>(ref, off::kRefLoadedData)) {
				const auto node = At<std::uintptr_t>(loaded, off::kLoadedData3D);
				if (node && ctx.roots.insert(node).second) {
					ShiftNode(node, ctx.dx, ctx.dy, true, 0);
					const auto p = At<game::Vec3>(ref, off::kRefPosition);
					if (!ctx.haveCentre || std::hypot(p.x - ctx.cx, p.y - ctx.cy) < kEngineUpdateRadius) {
						EngineUpdate3D(node);
					}
					else {
						ctx.far3D.emplace_back(ref, At<std::uint32_t>(ref, off::kFormId));
					}
				}
			}
			++ctx.refs;
			if ((ctx.refs & 255u) == 0) hangwatch::ShiftRefs(ctx.refs);
		}

		// Callback object for ForEachRefInCell: vtable slot 2 gets NiPointer<TESObjectREFR>*.
		struct RefCallback
		{
			void* const* vtbl;
			ShiftContext* ctx;

			static void Noop(RefCallback*) {}
			static int Invoke(RefCallback* self, std::uintptr_t* refPtr)
			{
				ShiftRef(*self->ctx, refPtr ? *refPtr : 0);
				return 1;  // continue
			}
			inline static void* const kVtbl[4] = { reinterpret_cast<void*>(&Noop), reinterpret_cast<void*>(&Noop),
				reinterpret_cast<void*>(&Invoke), reinterpret_cast<void*>(&Noop) };
		};

		void ShiftCellRefs(ShiftContext& ctx, std::uintptr_t cell)
		{
			if (!cell) {
				return;
			}
			RefCallback cb{ RefCallback::kVtbl, &ctx };
			EngineFn<int (*)(std::uintptr_t, RefCallback*, char)>(addr::kForEachRefInCell)(cell, &cb, 0);
		}

		// ---- 2b. Scene geometry without an object ------------------------------------------------------
		// Cities keep their building packs (precombined/instanced meshes: BSWeakReferenceNode trees on cell corners,
		// ~40k nodes in Akila) and cell geometry (reflection probes, decals) beside the object 3D, placed by local
		// translate under identity containers. They moved with nothing, so the city rendered 2 km away (0.34.2 scene
		// dump). Under the scene root, the branches that hold object 3D or building packs are walked through their
		// identity containers; every positioned node found there that was not moved yet is moved as a root.
		bool IsIdentityContainer(std::uintptr_t n)
		{
			const auto l = At<game::Vec3>(n, off::kNodeLocalT), w = At<game::Vec3>(n, off::kNodeWorldT);
			return std::fabs(l.x) < 0.01f && std::fabs(l.y) < 0.01f && std::fabs(w.x) < 0.01f && std::fabs(w.y) < 0.01f;
		}

		std::uintptr_t AsNode(std::uintptr_t n)
		{
			auto** vtbl = *reinterpret_cast<void***>(n);
			return reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(vtbl[off::kVtblIsNode])(n);
		}

		bool HasClassBelow(std::uintptr_t n, const char* cls, int depth)
		{
			if (!n || depth > 5) {
				return false;
			}
			if (std::strcmp(RttiNameOf(n), cls) == 0) {
				return true;
			}
			const auto asNode = AsNode(n);
			const auto children = asNode ? At<std::uintptr_t*>(asNode, off::kNodeChildren) : nullptr;
			const auto cc = asNode ? At<std::uint16_t>(asNode, off::kNodeChildCount) : 0;
			for (std::uint16_t i = 0; children && i < cc; ++i) {
				if (HasClassBelow(children[i], cls, depth + 1)) {
					return true;
				}
			}
			return false;
		}

		bool HoldsAny(std::uintptr_t n, const std::unordered_set<std::uintptr_t>& roots, int depth)
		{
			if (!n || depth > 8) {
				return false;
			}
			if (roots.count(n)) {
				return true;
			}
			const auto asNode = AsNode(n);
			const auto children = asNode ? At<std::uintptr_t*>(asNode, off::kNodeChildren) : nullptr;
			const auto cc = asNode ? At<std::uint16_t>(asNode, off::kNodeChildCount) : 0;
			for (std::uint16_t i = 0; children && i < cc; ++i) {
				if (HoldsAny(children[i], roots, depth + 1)) {
					return true;
				}
			}
			return false;
		}

		void ShiftGeometryUnder(ShiftContext& ctx, std::uintptr_t n, int depth, std::uint32_t& moved)
		{
			if (!n || depth > 32 || ctx.roots.count(n)) {
				return;  // object 3D already moved
			}
			if (!IsIdentityContainer(n) && !HoldsAny(n, ctx.roots, 0)) {  // a positioned node holding object 3D: go inside instead
				ctx.roots.insert(n);
				ShiftNode(n, ctx.dx, ctx.dy, true, 0);
				EngineUpdate3D(n);
				++moved;
				return;
			}
			const auto asNode = AsNode(n);
			const auto children = asNode ? At<std::uintptr_t*>(asNode, off::kNodeChildren) : nullptr;
			const auto cc = asNode ? At<std::uint16_t>(asNode, off::kNodeChildCount) : 0;
			for (std::uint16_t i = 0; children && i < cc; ++i) {
				ShiftGeometryUnder(ctx, children[i], depth + 1, moved);
			}
		}

		void ShiftSceneGeometry(ShiftContext& ctx)
		{
			std::unordered_set<std::uintptr_t> tops;
			for (const auto r : ctx.roots) {
				auto top = r;
				for (int d = 0; d < 32; ++d) {
					const auto parent = At<std::uintptr_t>(top, off::kNodeParent);
					if (!parent) {
						break;
					}
					top = parent;
				}
				if (top != r) {
					tops.insert(top);
				}
			}
			std::uint32_t moved = 0, branches = 0;
			for (const auto top : tops) {
				const auto asNode = AsNode(top);
				const auto children = asNode ? At<std::uintptr_t*>(asNode, off::kNodeChildren) : nullptr;
				const auto cc = asNode ? At<std::uint16_t>(asNode, off::kNodeChildCount) : 0;
				const std::unordered_set<std::uintptr_t> objectRoots(ctx.roots);
				for (std::uint16_t i = 0; children && i < cc; ++i) {
					const auto c = children[i];
					// Only the branch with the object 3D (cells) and the branch with the building packs; not the sky,
					// lights, camera or sub-scenes.
					const bool identity = c && IsIdentityContainer(c);
					const bool holds = identity && HoldsAny(c, objectRoots, 0);
					const bool packs = identity && !holds && HasClassBelow(c, ".?AVBSWeakReferenceNode@@", 0);
					// Scene census (0.51.6, log only): every top-level branch, moved or not, with where its contents sit,
					// to find the distant terrain / LOD left in the old frame (floating terrain after a shift).
					if (c) {
						const auto cn = AsNode(c);
						const auto gcs = cn ? At<std::uintptr_t*>(cn, off::kNodeChildren) : nullptr;
						const auto gcc = cn ? At<std::uint16_t>(cn, off::kNodeChildCount) : std::uint16_t(0);
						std::string kinds;
						double farthest = 0;
						unsigned sampled = 0;
						for (std::uint16_t k = 0; gcs && k < gcc && sampled < 64; ++k) {
							if (!gcs[k]) continue;
							++sampled;
							const float* w = reinterpret_cast<const float*>(gcs[k] + off::kNodeWorldT);
							farthest = std::max(farthest, std::hypot(double(w[0]), double(w[1])));
							if (k < 3) {
								if (!kinds.empty()) kinds += ", ";
								kinds += RttiNameOf(gcs[k]);
							}
						}
						const float* cw = reinterpret_cast<const float*>(c + off::kNodeWorldT);
						logger::Write("Re-centre: scene branch %u of %p: %s, %u children, world (%.0f, %.0f), children up to %.0f m from origin; %s; first: %s",
							unsigned(i), reinterpret_cast<void*>(top), RttiNameOf(c), unsigned(gcc), cw[0], cw[1], farthest,
							holds ? "MOVED (holds objects)" : (packs ? "MOVED (building packs)" : (identity ? "not moved (no objects)" : "not moved (positioned node)")),
							kinds.empty() ? "-" : kinds.c_str());
					}
					if (!c || !identity || !(holds || packs)) {
						continue;
					}
					++branches;
					const auto before = moved;
					ShiftGeometryUnder(ctx, c, 0, moved);
					logger::Write("Re-centre:   branch %u of %p (%s, %u children): %u geometry roots moved", unsigned(i), reinterpret_cast<void*>(top),
						RttiNameOf(c), unsigned(At<std::uint16_t>(AsNode(c) ? AsNode(c) : c, off::kNodeChildCount)), moved - before);
				}
			}
			logger::Write("Re-centre: moved %u scene geometry roots without an object (building packs, probes, decals) in %u branches", moved, branches);
		}

		// ---- 3. Cells -------------------------------------------------------------------------------

		// TES grid entries wrap the cell; same validity test as the engine's own grid loop (FUN_140589e60).
		std::uintptr_t GridCell(std::uintptr_t entry);

		// Re-attach of the loaded grid after a shift (0.43.0): every attached grid cell is detached and attached again
		// through the engine's own pair (FUN_140B03360(cell, 1, 0); FUN_140B02C80(cell, *(TES+0x88)); FUN_140AF1560(cell, 1),
		// as the TES progressive detach/attach and FUN_1405B17B0/F0 do). Whatever the engine derives from a cell when it
		// attaches is rebuilt from the shifted data by the engine itself instead of being patched one system at a time
		// (sync box, collision registration, grouped geometry, navmesh links; 0.39-0.42 found these one by one). Runs on
		// the game-update thread at the sync point, inside Run, before the player is placed.
		void ReattachGrid(std::uintptr_t tes)
		{
			const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
			const auto count = std::min<std::uint32_t>(At<std::uint32_t>(tes, off::kTESGridCount), 64);
			const auto arg = At<std::uintptr_t>(tes, 0x88);
			std::uintptr_t cells[64];
			std::uint32_t n = 0;
			for (std::uint32_t i = 0; entries && i < count; ++i) {
				const auto cell = GridCell(entries[i]);
				if (cell && At<std::uint8_t>(cell, off::kCellState) == 10) {
					cells[n++] = cell;
				}
			}
			const auto t0 = telemetry::NowUs();
			for (std::uint32_t i = 0; i < n; ++i) {
				EngineFn<void (*)(std::uintptr_t, char, char)>(addr::kCellDetach)(cells[i], 1, 0);
			}
			const auto t1 = telemetry::NowUs();
			unsigned attached = 0;
			for (std::uint32_t i = 0; i < n; ++i) {
				const auto st = At<std::uint8_t>(cells[i], off::kCellState);
				if (st == 6 || st == 7) {
					EngineFn<void (*)(std::uintptr_t, std::uintptr_t)>(addr::kCellAttach)(cells[i], arg);
					EngineFn<void (*)(std::uintptr_t, char)>(addr::kCellAttachFinish)(cells[i], 1);
					++attached;
				}
			}
			const auto t2 = telemetry::NowUs();
			logger::Write("Re-centre: re-attached %u of %u grid cells (detach %.1f ms, attach %.1f ms)", attached, n, (t1 - t0) / 1000.0, (t2 - t1) / 1000.0);
			telemetry::Record("reattach", "\"cells\":%u,\"attached\":%u,\"us_detach\":%llu,\"us_attach\":%llu", n, attached,
				static_cast<unsigned long long>(t1 - t0), static_cast<unsigned long long>(t2 - t1));
		}

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

		std::uint32_t PackCoord(std::int32_t x, std::int32_t y)
		{
			return (static_cast<std::uint32_t>(static_cast<std::uint16_t>(x)) << 16) | static_cast<std::uint16_t>(y);
		}

		// Rebuild the worldspace's coordinate -> cell form id table with every key moved by (-cx, -cy).
		void RekeyCellMap(std::uintptr_t worldspace, std::int32_t cx, std::int32_t cy)
		{
			const auto table = worldspace + off::kWsCellMap + 8;
			auto* entries = At<std::uint8_t*>(table, off::kTblEntries);
			const auto capacity = At<std::uint64_t>(table, off::kTblCapacity);
			if (!entries || capacity == 0) {
				return;
			}
			struct Pair { std::uint32_t key, value; };
			std::vector<Pair> pairs;
			for (std::uint64_t i = 0; i < capacity; ++i) {
				auto* e = entries + i * 16;
				if (*reinterpret_cast<std::int32_t*>(e + 8) != -1) {
					pairs.push_back({ *reinterpret_cast<std::uint32_t*>(e), *reinterpret_cast<std::uint32_t*>(e + 4) });
				}
			}
			// Clear exactly as the table's constructor does.
			for (std::uint64_t i = 0; i < capacity; ++i) {
				*reinterpret_cast<std::uint64_t*>(entries + i * 16 + 8) = ~0ull;
			}
			At<std::uint64_t>(table, off::kTblFree) = capacity;
			At<std::uint64_t>(table, off::kTblLastFree) = capacity;

			const auto insert = EngineFn<char (*)(std::uintptr_t, std::uint64_t*, const std::uint32_t*)>(addr::kMapInsertKey);
			for (const auto& p : pairs) {
				const auto x = static_cast<std::int16_t>(p.key >> 16);
				const auto y = static_cast<std::int16_t>(p.key & 0xFFFF);
				const std::uint32_t key = PackCoord(x - cx, y - cy);
				std::uint64_t index = capacity;
				insert(table, &index, &key);
				auto* tableEntries = At<std::uint8_t*>(table, off::kTblEntries);  // unchanged: same count
				// The engine's insert (FUN_140310090) only links a slot and returns its index; the caller writes the key
				// and the value (0.44.1: the key was never written, so every position -> cell lookup after a shift found
				// nothing: ground rays, Cell watch "map handle 00000000", FUN_140BBD970's other callers).
				*reinterpret_cast<std::uint32_t*>(tableEntries + index * 16 + 0) = key;
				*reinterpret_cast<std::uint32_t*>(tableEntries + index * 16 + 4) = p.value;
			}
			// Self-test with the engine's own lookup (FUN_1403101F0): every re-keyed coordinate must find its value.
			unsigned bad = 0;
			for (const auto& p : pairs) {
				const auto x = static_cast<std::int16_t>(p.key >> 16);
				const auto y = static_cast<std::int16_t>(p.key & 0xFFFF);
				const std::uint32_t key = PackCoord(x - cx, y - cy);
				const auto idx = EngineFn<std::uint64_t (*)(std::uintptr_t, const std::uint32_t*)>(addr::kMapFindKey)(table, &key);
				const auto* tableEntries = At<std::uint8_t*>(table, off::kTblEntries);
				if (idx >= capacity || *reinterpret_cast<const std::uint32_t*>(tableEntries + idx * 16 + 4) != p.value) {
					++bad;
				}
			}
			logger::Write("Re-centre: re-keyed %zu worldspace cell entries (%u not found by the engine's lookup)", pairs.size(), bad);
		}

		void ShiftCell(std::uintptr_t cell, std::int32_t cx, std::int32_t cy, float dx, float dy)
		{
			if (!cell || (At<std::uint8_t>(cell, off::kCellFlags) & 1)) {
				return;  // interiors have no grid position
			}
			if (const auto ext = At<std::uintptr_t>(cell, off::kCellExterior)) {
				At<std::int32_t>(ext, 0x00) -= cx;
				At<std::int32_t>(ext, 0x04) -= cy;
				SubVec(ext + 0x24, dx, dy);
			}
		}

		// ---- 4. Planet manager ----------------------------------------------------------------------

		void ShiftPlanetManager(std::uintptr_t mgr, std::int32_t cx, std::int32_t cy, float dx, float dy)
		{
			At<std::int32_t>(mgr, off::kMgrGridCentre) -= cx;
			At<std::int32_t>(mgr, off::kMgrGridCentre + 4) -= cy;
			At<std::int32_t>(mgr, off::kMgrNewCentre) -= cx;
			At<std::int32_t>(mgr, off::kMgrNewCentre + 4) -= cy;
			SubVec(mgr + off::kMgrPlayerPos, dx, dy);

			// The world cell that becomes (0, 0) maps to the planet coordinate (origin + (cx, cy)).
			const auto planetId = At<std::uint32_t>(mgr, off::kMgrPlanetId);
			const auto grid = EngineFn<std::uintptr_t (*)(std::uintptr_t, std::uint32_t, char)>(addr::kGetPlanetGrid)(0, planetId, 1);
			if (!grid) {
				logger::Write("Re-centre: planet grid not found; terrain mapping not updated");
				return;
			}
			const std::int32_t offset[2] = { cx, cy };
			const auto offsetCoord = EngineFn<void (*)(std::uintptr_t, std::uintptr_t, const std::int32_t*)>(addr::kOffsetPlanetCoord);
			offsetCoord(grid, mgr + off::kMgrOriginCoord, offset);
			if (!kEngineTerrainShift) {
				offsetCoord(grid, mgr + off::kMgrOriginCoord2, offset);
			}
		}

		// The manager's own lock (mgr+0x24 owner thread, +0x28 state), as BGSPlanet::Manager uses it.
		struct ManagerLock
		{
			std::uintptr_t mgr = 0;
			std::uint8_t   owned = 0;
		};

		bool TryLockManager(std::uintptr_t mgr, ManagerLock& h)
		{
			EngineFn<ManagerLock* (*)(ManagerLock*, std::uintptr_t)>(addr::kPlanetManagerTryLock)(&h, mgr);
			return h.owned != 0;
		}

		// Same release sequence the engine inlines after FUN_14078f480 (e.g. in FUN_1417ca630).
		void UnlockManager(ManagerLock& h)
		{
			if (!h.owned) {
				return;
			}
			auto* word = reinterpret_cast<volatile LONG*>(h.mgr + 0x28);
			if ((static_cast<ULONG>(*word) & 0xFFC00000u) == 0x400000u) {
				At<DWORD>(h.mgr, 0x24) = 0;
				const auto before = static_cast<ULONG>(InterlockedExchangeAdd(word, -0x401000));
				if (before & 0xFFFu) {
					WakeByAddressAll(const_cast<LONG*>(word));
				}
				else if ((before & 0x3FF000u) > 0x1000u) {
					WakeByAddressSingle(const_cast<LONG*>(word));
				}
			}
			else {
				InterlockedExchangeAdd(word, -0x400000);
			}
			h.owned = 0;
		}

		void CallModules(std::uintptr_t mgr, std::uint32_t slotOffset, ManagerLock* h)
		{
			const auto count = At<std::uint32_t>(mgr, off::kMgrModuleCount);
			const auto modules = At<std::uintptr_t*>(mgr, off::kMgrModules);
			for (std::uint32_t i = 0; modules && i < count; ++i) {
				if (const auto m = modules[i]) {
					auto** vtbl = *reinterpret_cast<void***>(m);
					reinterpret_cast<void (*)(std::uintptr_t, ManagerLock*)>(vtbl[slotOffset / 8])(m, h);
				}
			}
		}

		// Runs the planet manager's own shift (what ShiftToTask does on the IO thread), synchronously, with
		// the world-origin delta that vanilla never sets (+0x1EC). Landscape then re-centres its grid offset on
		// the world origin and moves every terrain block's cell coordinates by the delta; Overlays resets.
		// No slide (+0x1A0 = 0): the terrain window stays where it is, only its world placement changes.
		// Requires the world origin to land on the terrain centre, i.e. (cx, cy) = the old grid centre.
		void ShiftTerrainViaEngine(std::uintptr_t mgr, ManagerLock& h, std::int32_t cx, std::int32_t cy)
		{
			InterlockedExchange8(reinterpret_cast<volatile char*>(mgr + off::kMgrState), 3);
			CallModules(mgr, 0x60, &h);  // begin
			At<std::int32_t>(mgr, off::kMgrShiftDelta) = 0;
			At<std::int32_t>(mgr, off::kMgrShiftDelta + 4) = 0;
			At<std::int32_t>(mgr, off::kMgrNewCentre) = At<std::int32_t>(mgr, off::kMgrGridCentre);
			At<std::int32_t>(mgr, off::kMgrNewCentre + 4) = At<std::int32_t>(mgr, off::kMgrGridCentre + 4);
			std::memcpy(reinterpret_cast<void*>(mgr + off::kMgrLatLong2), reinterpret_cast<const void*>(mgr + off::kMgrLatLong), 0x18);
			std::memcpy(reinterpret_cast<void*>(mgr + off::kMgrOriginCoord2), reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), 0x2C);
			At<std::int32_t>(mgr, off::kMgrOriginDelta) = cx;
			At<std::int32_t>(mgr, off::kMgrOriginDelta + 4) = cy;
			At<std::int32_t>(mgr, off::kMgrOriginDelta + 8) = 0;
			InterlockedExchange8(reinterpret_cast<volatile char*>(mgr + off::kMgrShifting), 1);
			EngineFn<char (*)(std::uintptr_t, ManagerLock*)>(addr::kPlanetManagerShiftModules)(mgr, &h);  // state -> 4
			EngineFn<void (*)(std::uintptr_t, ManagerLock*)>(addr::kPlanetManagerShiftCommit)(mgr, &h);   // state -> 2, deltas cleared
		}

		// The worldspace's persistent cell (permanent references: the landed ship, markers, quest objects).
		// Found at runtime by scanning the worldspace's pointer fields for an exterior cell of this worldspace
		// with the persistent flag; the offset is cached and re-validated every time.
		std::uintptr_t PersistentCell(std::uintptr_t ws)
		{
			static std::uint32_t cached = 0;
			const auto valid = [ws](std::uintptr_t c) {
				if (!c || (c & 7)) {
					return false;
				}
				const auto w = game::SafeRead<std::uintptr_t>(c + off::kCellWorldspace);
				const auto f = game::SafeRead<std::uint32_t>(c + off::kCellFormFlags);
				const auto i = game::SafeRead<std::uint8_t>(c + off::kCellFlags);
				return w && f && i && *w == ws && (*f & 0x400u) && !(*i & 1);
			};
			if (cached) {
				if (const auto c = game::SafeRead<std::uintptr_t>(ws + cached); c && valid(*c)) {
					return *c;
				}
			}
			for (std::uint32_t o = 0x80; o < 0x300; o += 8) {
				if (const auto c = game::SafeRead<std::uintptr_t>(ws + o); c && valid(*c)) {
					if (cached != o) {
						cached = o;
						logger::Write("Re-centre: worldspace persistent cell found at +0x%X", o);
						telemetry::Record("persistent_cell", "\"offset\":\"0x%X\"", o);
					}
					return *c;
				}
			}
			return 0;
		}

		// The Landscape module's multi-file terrain grid (hand-built terrain, Data\\Terrain\\*): its offset at +0x70
		// is the world cell of grid index 0, i.e. terrain centre - half the grid (FUN_140782070 on landing). The
		// engine's origin-delta branch sets -half (centre 0); set it explicitly so shifts that do not end on
		// the terrain centre (shifting back to the original origin) stay consistent.
		void FixTerrainGridOffset(std::uintptr_t mgr)
		{
			const auto count = At<std::uint32_t>(mgr, off::kMgrModuleCount);
			const auto modules = At<std::uintptr_t*>(mgr, off::kMgrModules);
			const auto landscapeVtbl = game::Base() + addr::kLandscapeVtbl;
			for (std::uint32_t i = 0; modules && i < count; ++i) {
				const auto m = modules[i];
				if (!m || At<std::uintptr_t>(m, 0) != landscapeVtbl) {
					continue;
				}
				const auto grid = At<std::uintptr_t>(m, 0x20);
				const auto master = grid ? At<std::uintptr_t>(grid, 0x18) : 0;
				if (!master) {
					return;  // no hand-built terrain in this worldspace
				}
				const auto w = At<std::int32_t>(master, 0x28), h = At<std::int32_t>(master, 0x2C);
				const auto ox = At<std::int32_t>(mgr, off::kMgrGridCentre) - w / 2;
				const auto oy = At<std::int32_t>(mgr, off::kMgrGridCentre + 4) - h / 2;
				telemetry::Record("terrain_grid_offset", "\"before\":[%d,%d],\"after\":[%d,%d],\"size\":[%d,%d]",
					At<std::int32_t>(grid, 0x70), At<std::int32_t>(grid, 0x74), ox, oy, w, h);
				At<std::int32_t>(grid, 0x70) = ox;
				At<std::int32_t>(grid, 0x74) = oy;
				return;
			}
		}

		// Total shift applied to each worldspace (cells), so hand-built worldspaces can return to their
		// original frame before any hand-built cell loads from the game files.
		struct Shift { std::int32_t x = 0, y = 0; };
		std::unordered_map<std::uintptr_t, Shift> g_totalShift;

		// The planet manager's origin coordinate (+0xC8, 0x2C bytes) right after our last shift of a worldspace.
		// The game never changes it during normal play, so a different value means the manager restarted
		// (save load, death reload, fast travel, door transition): the world is back in its original frame.
		struct OriginSnapshot { std::uint8_t bytes[0x2C]; std::uint32_t planet; };
		std::unordered_map<std::uintptr_t, OriginSnapshot> g_origin;

		// ---- 3b. Worldspace planet origin ------------------------------------------------------------
		// The loaded-cell database finds a planet cell by the worldspace's planet origin (ws+0x310, the packed planet
		// cell id of grid (0, 0)) offset by the requested grid coordinates (FUN_140422600 -> FUN_140424C50). Without
		// moving that origin, every cell requested after a shift resolved to the planet cell at the OLD coordinates,
		// i.e. cells near the original landing spot (0.33.2: CELL NOT READY at cell borders, Akila content misplaced).

		// Per worldspace: true while the stored landing (mgr+0x98) matches the shifted frame (every re-centre since the
		// landing last changed followed it).
		std::unordered_map<std::uintptr_t, bool> g_landingFollows;
		// The value we last wrote per worldspace, so a frame reset only undoes our own change.
		std::unordered_map<std::uintptr_t, std::uint64_t> g_wsOriginWritten;

		// Recent shifts (cells), newest last: a cell whose load was requested before some of them finishes with coordinates
		// from before them; FixStaleCells moves it by exactly that sum.
		struct ShiftRec { std::int32_t cx, cy; };
		std::vector<ShiftRec> g_history;
		std::atomic<std::uint64_t> g_frameVersion{ 0 };
		std::mutex g_historyLock;  // also read by the LoadCell run hook on loader threads
		// A native LoadCell task calls FUN_14041F120 inside FUN_14041F8F0. Hold the shared side for that
		// entire task; a shift takes the exclusive side only when no task is in flight. Queued tasks can
		// then enter in the new frame and resolve their stored planet key through the existing ledger hooks.
		std::shared_mutex g_cellLoadFrameLock;
		// The request, task run, and build entry may be reached independently. All three must see
		// one published origin. Nested entries on the same worker share its existing read lock;
		// calls made by the shift itself are already inside the exclusive transaction.
		thread_local unsigned t_frameReadDepth = 0;
		thread_local bool t_frameWriteOwner = false;
		struct FrameReadScope
		{
			std::shared_lock<std::shared_mutex> lock;
			FrameReadScope()
			{
				if (t_frameReadDepth++ == 0 && !t_frameWriteOwner) {
					lock = std::shared_lock<std::shared_mutex>(g_cellLoadFrameLock);
				}
			}
			~FrameReadScope() { --t_frameReadDepth; }
		};
		struct FrameWriteOwner
		{
			FrameWriteOwner() { t_frameWriteOwner = true; }
			~FrameWriteOwner() { t_frameWriteOwner = false; }
		};
		// The last shift in metres and until when old-frame player writes are corrected (stale-write fix below).
		std::atomic<unsigned long long> g_staleUntil{ 0 };
		std::atomic<float> g_shiftX{ 0 }, g_shiftY{ 0 };
		std::atomic<unsigned long long> g_shiftAt{ 0 };  // when the last shift ran (trace window, runaway guard)

		// Set by every re-centre, so the jump watch does not count our own move of the player as a jump.
		bool g_justShifted = false;
		game::Vec3 g_playerAfterShift{};  // where the shift put the player (the jump watch compares the next frame with it)

		// Packed planet cell id of grid (x, y) in the worldspace's current frame; ~0 when not a planet worldspace.
		std::uint64_t PlanetCellId(std::uintptr_t ws, std::int32_t x, std::int32_t y)
		{
			std::uint64_t out = ~0ull;
			const std::int32_t xy[2] = { x, y };
			EngineFn<std::uint64_t* (*)(std::uint64_t*, std::uintptr_t, const std::int32_t*)>(addr::kPlanetCellKey)(&out, ws, xy);
			return out;
		}

		// The new origin for a shift by (cx, cy), or ~0 when the worldspace has none of its own or the encoding does not
		// round-trip (then a shift would load the wrong cells, so the caller refuses).
		enum class OriginPlan { None, Ok, Bad };
		OriginPlan PlanPlanetOrigin(std::uintptr_t ws, std::int32_t cx, std::int32_t cy, std::uint64_t& moved)
		{
			const auto origin = At<std::uint64_t>(ws, off::kWsPlanetOrigin);
			if (origin == ~0ull) {
				return OriginPlan::None;
			}
			const auto same = PlanetCellId(ws, 0, 0);
			moved = PlanetCellId(ws, cx, cy);
			if (same != origin || moved == ~0ull) {
				logger::Write("Re-centre: worldspace planet origin %016llX does not round-trip (got %016llX, moved %016llX)",
					static_cast<unsigned long long>(origin), static_cast<unsigned long long>(same), static_cast<unsigned long long>(moved));
				return OriginPlan::Bad;
			}
			return OriginPlan::Ok;
		}

		// The loaded-cell database key of grid (x, y) (three words: coordinate or planet id, type tag, worldspace).
		struct CellKey { std::uint32_t w[3]; };
		CellKey LoadKey(std::uintptr_t ws, std::int32_t x, std::int32_t y)
		{
			CellKey k{};
			EngineFn<std::uint32_t* (*)(std::uint32_t*, std::uint32_t, std::int16_t, std::int16_t)>(addr::kCellLoadKey)(k.w, At<std::uint32_t>(ws, 0x28),
				static_cast<std::int16_t>(x), static_cast<std::int16_t>(y));
			return k;
		}

		// ---- 5. Event -------------------------------------------------------------------------------

		struct OriginShiftEvent
		{
			std::uint32_t worldspaceId;
			float dx, dy, dz;
		};

		// Callback object for EventNotify: vtable slot 2 gets each subscriber.
		struct SinkCallback
		{
			void* const* vtbl;
			const OriginShiftEvent* evt;
			std::uintptr_t source;

			static void Noop(SinkCallback*) {}
			static int Invoke(SinkCallback* self, std::uintptr_t sink)
			{
				auto** sinkVtbl = *reinterpret_cast<void***>(sink);
				using ProcessEvent = int (*)(std::uintptr_t, const OriginShiftEvent*, std::uintptr_t);
				return reinterpret_cast<ProcessEvent>(sinkVtbl[1])(sink, self->evt, self->source);
			}
			inline static void* const kVtbl[4] = { reinterpret_cast<void*>(&Noop), reinterpret_cast<void*>(&Noop),
				reinterpret_cast<void*>(&Invoke), reinterpret_cast<void*>(&Noop) };
		};

		void SendOriginShift(std::uint32_t worldspaceId, float dx, float dy)
		{
			const auto source = EngineFn<std::uintptr_t (*)()>(addr::kOriginShiftEventSource)();
			const OriginShiftEvent evt{ worldspaceId, dx, dy, 0.0f };
			SinkCallback cb{ SinkCallback::kVtbl, &evt, source };
			EngineFn<void (*)(std::uintptr_t, SinkCallback*)>(addr::kEventNotify)(source, &cb);
		}
	}

	namespace
	{
		// Hand-built cells come from the game's data files: their form IDs do not start with 0xFF. Cells the
		// planet generates on the fly get runtime IDs (0xFF......) and are registered in the same map as the
		// player explores, so the raw entry count is NOT a hand-built test (it grows on every planet).


		// Conditions must hold continuously this long before a re-centre (no loading, no teleport).
		constexpr ULONGLONG kSettleMs = 5000;
		bool g_runUrgent = false;  // the current Run is an urgent zone re-centre (relaxed cell check)
		// Automatic re-centre while moving (walking, running, driving): below this speed the prefetch does not lead the
		// loaded grid, so the cells only change when the player crosses one; every loaded cell attached for
		// kRecentreSettleMs means nothing is queued under the old coordinates. (Was: under 3 m/s for 5 s, which never
		// happened while walking, so the physics never followed.)
		constexpr double kRecentreMaxSpeed = 20.0;    // m/s
		constexpr ULONGLONG kRecentreSettleMs = 1000;
		constexpr ULONGLONG kRecentreSpacingMs = 10000;  // between non-urgent re-centres (0.55.0 move schedule)

		// Calls fn(x, y) for every hand-built cell (non-runtime form ID) in the worldspace's cell map.
		template <class Fn>
		std::uint32_t ForEachAuthoredCell(std::uintptr_t worldspace, Fn&& fn)
		{
			const auto table = worldspace + off::kWsCellMap + 8;
			const auto* entries = At<std::uint8_t*>(table, off::kTblEntries);
			const auto capacity = At<std::uint64_t>(table, off::kTblCapacity);
			std::uint32_t n = 0;
			for (std::uint64_t i = 0; entries && i < capacity; ++i) {
				const auto* e = entries + i * 16;
				if (*reinterpret_cast<const std::int32_t*>(e + 8) == -1) {
					continue;  // empty slot
				}
				const auto formId = *reinterpret_cast<const std::uint32_t*>(e + 4);
				if ((formId >> 24) == 0xFF) {
					continue;  // generated at runtime
				}
				const auto key = *reinterpret_cast<const std::uint32_t*>(e);
				fn(static_cast<std::int32_t>(static_cast<std::int16_t>(key >> 16)), static_cast<std::int32_t>(static_cast<std::int16_t>(key & 0xFFFF)));
				++n;
			}
			return n;
		}

		std::uint32_t AuthoredCells(std::uintptr_t worldspace)
		{
			return ForEachAuthoredCell(worldspace, [](std::int32_t, std::int32_t) {});
		}

		// The player's cell and every loaded grid cell are valid and fully attached.
		bool WorldIsSettled(std::uintptr_t tes, std::uintptr_t playerCell)
		{
			if (At<std::uint8_t>(playerCell, off::kCellState) != 10) {
				return false;
			}
			const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
			const auto count = At<std::uint32_t>(tes, off::kTESGridCount);
			if (!entries || count == 0) {
				return false;
			}
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto cell = GridCell(entries[i]);
				if (!cell || At<std::uint8_t>(cell, off::kCellState) != 10) {
					return false;
				}
			}
			return true;
		}

		// Logs why a re-centre was refused, at most once every 5 seconds.
		bool Refuse(const char* reason)
		{
			static ULONGLONG last = 0;
			const ULONGLONG now = GetTickCount64();
			if (now - last > 5000) {
				last = now;
				logger::Write("Re-centre refused: %s", reason);
				telemetry::Record("recentre_refused", "\"reason\":\"%s\"", reason);
			}
			return false;
		}
	}

	bool InShipInterior(std::uintptr_t ref)
	{
		const auto cell = ref ? At<std::uintptr_t>(ref, off::kRefParentCell) : 0;
		if (!cell || !(At<std::uint8_t>(cell, off::kCellFlags) & 1)) {
			return false;
		}
		const auto data = At<std::uintptr_t>(cell, off::kCellExterior);
		return data && At<std::uint8_t>(data, off::kCellInteriorKind) == 1;
	}

	std::uintptr_t ShipOf(std::uintptr_t ref)
	{
		return ref ? EngineFn<std::uintptr_t (*)(std::uintptr_t, char, std::uintptr_t, std::uintptr_t)>(addr::kShipOfRef)(ref, 1, 0, 0) : 0;
	}

	Anchor CurrentAnchor()
	{
		const auto player = Global(addr::kPlayerCharacterPtr);
		const auto playerCell = player ? At<std::uintptr_t>(player, off::kRefParentCell) : 0;
		if (!playerCell) {
			return {};
		}
		if (!(At<std::uint8_t>(playerCell, off::kCellFlags) & 1)) {
			return { player, playerCell, false };
		}
		// Inside a landed ship: the engine streams the planet around the ship (FUN_1417df210), so it is the anchor.
		if (InShipInterior(player)) {
			const auto ship = ShipOf(player);
			const auto shipCell = ship ? At<std::uintptr_t>(ship, off::kRefParentCell) : 0;
			if (shipCell && !(At<std::uint8_t>(shipCell, off::kCellFlags) & 1) && At<std::uintptr_t>(shipCell, off::kCellWorldspace)) {
				return { ship, shipCell, true };
			}
		}
		return {};
	}

	bool WorldSettled()
	{
		const auto tes = Global(addr::kTESPtr);
		const auto mgr = Global(addr::kPlanetManagerPtr);
		const auto a = CurrentAnchor();
		if (!tes || !mgr || !a.ref || At<std::uint8_t>(mgr, off::kMgrState) != 2) {
			return false;
		}
		return WorldIsSettled(tes, a.cell);
	}

	bool AuthoredWorldspace()
	{
		const auto a = CurrentAnchor();
		const auto ws = a.cell ? At<std::uintptr_t>(a.cell, off::kCellWorldspace) : 0;
		return !ws || AuthoredCells(ws) > 0;
	}

	std::int32_t NearestAuthoredCell()
	{
		const auto a = CurrentAnchor();
		const auto ws = a.cell ? At<std::uintptr_t>(a.cell, off::kCellWorldspace) : 0;
		if (!ws) {
			return INT32_MAX;
		}
		const auto pos = At<game::Vec3>(a.ref, off::kRefPosition);
		const auto px = static_cast<std::int32_t>(std::floor(pos.x / off::kCellSize));
		const auto py = static_cast<std::int32_t>(std::floor(pos.y / off::kCellSize));
		// The cell map's keys are in the current frame (re-keyed on every shift).
		std::int32_t best = INT32_MAX;
		ForEachAuthoredCell(ws, [&](std::int32_t x, std::int32_t y) {
			best = std::min(best, std::max(std::abs(x - px), std::abs(y - py)));
		});
		return best;
	}

	namespace
	{
		// Collects a cell's references (ForEachRefInCell callback, vtable slot 2).
		struct CollectRefs
		{
			void* const* vtbl;
			std::vector<std::uintptr_t>* out;

			static void Noop(CollectRefs*) {}
			static int Invoke(CollectRefs* self, std::uintptr_t* refPtr)
			{
				if (refPtr && *refPtr) {
					self->out->push_back(*refPtr);
				}
				return 1;
			}
			inline static void* const kVtbl[4] = { reinterpret_cast<void*>(&Noop), reinterpret_cast<void*>(&Noop),
				reinterpret_cast<void*>(&Invoke), reinterpret_cast<void*>(&Noop) };
		};

		// The cell's coordinate in the current frame: its key in the worldspace cell map (re-keyed on every shift).
		bool CurrentKey(std::uintptr_t worldspace, std::uint32_t formId, std::int32_t& x, std::int32_t& y)
		{
			const auto table = worldspace + off::kWsCellMap + 8;
			const auto* entries = At<std::uint8_t*>(table, off::kTblEntries);
			const auto capacity = At<std::uint64_t>(table, off::kTblCapacity);
			for (std::uint64_t i = 0; entries && i < capacity; ++i) {
				const auto* e = entries + i * 16;
				if (*reinterpret_cast<const std::int32_t*>(e + 8) == -1 || *reinterpret_cast<const std::uint32_t*>(e + 4) != formId) {
					continue;
				}
				const auto key = *reinterpret_cast<const std::uint32_t*>(e);
				x = static_cast<std::int16_t>(key >> 16);
				y = static_cast<std::int16_t>(key & 0xFFFF);
				return true;
			}
			return false;
		}

		// Loaded-cell window: grid cells this far (cells) from the TES grid centre cannot be in the loaded grid,
		// so a cell that says it is there still carries coordinates from before a shift.
		constexpr std::int32_t kStaleCells = 8;
	}

	void FixStaleCells()
	{
		// Cells whose load was requested before a shift finish with the coordinates of the old frame (shifts now run while
		// cells load, so the re-centre is not starved while driving, 0.36.2). Each TES grid slot implies its cell's
		// coordinates (slot i of an n x n grid around the centre: x = cx - n/2 + i / n, y = cy - n/2 + i % n, as every
		// cell watch shows); a cell off by exactly the sum of the most recent shifts is moved into place with its objects.
		// Anything else is only logged. (The old version matched ws+0x90 values against form ids, but they are handles,
		// so it never fixed anything.)
		static ULONGLONG last = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last < 100) {
			return;
		}
		std::vector<ShiftRec> history;
		{
			std::lock_guard<std::mutex> lock(g_historyLock);
			history = g_history;
		}
		if (history.empty()) {
			return;
		}
		last = now;
		const auto tes = Global(addr::kTESPtr);
		const auto a = CurrentAnchor();
		if (!tes || !a.ref || !a.cell) {
			return;
		}
		const auto worldspace = At<std::uintptr_t>(a.cell, off::kCellWorldspace);
		if (!worldspace) {
			return;
		}
		const auto cx = At<std::int32_t>(tes, off::kTESGridCentre);
		const auto cy = At<std::int32_t>(tes, off::kTESGridCentre + 4);
		const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
		const auto count = At<std::uint32_t>(tes, off::kTESGridCount);
		const auto n = static_cast<std::uint32_t>(std::lround(std::sqrt(double(count))));
		if (!entries || n == 0 || n * n != count) {
			return;
		}
		const std::int32_t half = std::int32_t(n / 2);
		for (std::uint32_t i = 0; i < count; ++i) {
			const auto cell = GridCell(entries[i]);
			if (!cell || (At<std::uint8_t>(cell, off::kCellFlags) & 1) || At<std::uintptr_t>(cell, off::kCellWorldspace) != worldspace) {
				continue;
			}
			const auto ext = At<std::uintptr_t>(cell, off::kCellExterior);
			if (!ext) {
				continue;
			}
			const auto state = At<std::uint8_t>(cell, off::kCellState);
			if (state < 6) {
				continue;  // still being built; its coordinates may not be set yet
			}
			const std::int32_t sx = At<std::int32_t>(ext, 0x00), sy = At<std::int32_t>(ext, 0x04);
			const std::int32_t ex = cx - half + std::int32_t(i / n), ey = cy - half + std::int32_t(i % n);
			if (sx == ex && sy == ey) {
				continue;
			}
			const std::int32_t dcx = sx - ex, dcy = sy - ey;
			std::int32_t sumx = 0, sumy = 0;
			bool matches = false;
			for (auto it = history.rbegin(); it != history.rend(); ++it) {
				sumx += it->cx;
				sumy += it->cy;
				if (sumx == dcx && sumy == dcy) {
					matches = true;
					break;
				}
			}
			const auto formId = At<std::uint32_t>(cell, 0x28);
			if (!matches) {
				static std::uint32_t lastOdd = 0;
				if (lastOdd != formId) {
					lastOdd = formId;
					logger::Write("Re-centre: grid slot %u cell %08X stored (%d, %d), slot says (%d, %d): not a recorded shift, left alone", i, formId, sx, sy,
						ex, ey);
				}
				continue;
			}
			const float dx = dcx * off::kCellSize, dy = dcy * off::kCellSize;
			const bool attached = state == 10;

			// Coordinates first, so references moved below land in this same cell.
			ShiftCell(cell, dcx, dcy, dx, dy);
			std::vector<std::uintptr_t> refs;
			CollectRefs cb{ CollectRefs::kVtbl, &refs };
			EngineFn<int (*)(std::uintptr_t, CollectRefs*, char)>(addr::kForEachRefInCell)(cell, &cb, 0);
			if (!attached) {
				// Not attached yet: no physics bodies; moving the position and 3D (with the engine update) is enough.
				ShiftContext ctx{ dx, dy };
				for (const auto r : refs) {
					ShiftRef(ctx, r);
				}
			}
			else {
				// Already attached: move through the engine's move routine so 3D and physics follow.
				const auto move = EngineFn<void (*)(std::uintptr_t, std::uint64_t*, std::uintptr_t, std::uintptr_t, float*, float*)>(addr::kMoveReference);
				for (const auto r : refs) {
					std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
					auto p = At<game::Vec3>(r, off::kRefPosition);
					float pos[3] = { p.x - dx, p.y - dy, p.z };
					auto rr = At<game::Vec3>(r, off::kRefRotation);
					float rot[3] = { rr.x, rr.y, rr.z };
					move(r, &handle, cell, worldspace, pos, rot);
				}
			}
			logger::Write("Re-centre: repaired cell %08X loaded across a shift: (%d, %d) -> (%d, %d), %zu objects (%s)", formId, sx, sy, ex, ey, refs.size(),
				attached ? "attached" : "loading");
			telemetry::Record("cell_fixed", "\"form\":\"%08X\",\"from\":[%d,%d],\"to\":[%d,%d],\"refs\":%zu,\"attached\":%d",
				formId, sx, sy, ex, ey, refs.size(), attached ? 1 : 0);
		}
	}

	// Puts a worldspace's bookkeeping back in its original frame without moving loaded cells (they are being
	// unloaded or replaced by the reload): cell map keys back, persistent references back (they stay in
	// memory across transitions), shift record cleared.
	void ResetFrame(std::uintptr_t worldspace, const char* why, bool movePersistent)
	{
		// When the last re-centre moved the stored landing with the world (followLanding), the engine's state is a plain
		// landing at the player's spot: saves match it and a reload or transition needs nothing undone. Only our own
		// bookkeeping is cleared (0.48.1; moving persistent references back here would put them in a frame nothing uses).
		if (const auto f = g_landingFollows.find(worldspace); f != g_landingFollows.end() && f->second) {
			const auto t = g_totalShift.find(worldspace);
			const int tx = t != g_totalShift.end() ? t->second.x : 0, ty = t != g_totalShift.end() ? t->second.y : 0;
			g_totalShift.erase(worldspace);
			g_origin.erase(worldspace);
			g_wsOriginWritten.erase(worldspace);
			g_landingFollows.erase(worldspace);
			g_shiftAt.store(0);
			g_staleUntil.store(0);
			{
				std::lock_guard<std::mutex> lock(g_historyLock);
				g_history.clear();
			}
			logger::Write("Re-centre: frame reset (%s): the landing followed the player (total shift %d, %d); nothing to undo", why, tx, ty);
			telemetry::Record("frame_reset", "\"why\":\"%s\",\"tx\":%d,\"ty\":%d,\"landing_followed\":1", why, tx, ty);
			return;
		}
		const auto it = g_totalShift.find(worldspace);
		if (it == g_totalShift.end() || (it->second.x == 0 && it->second.y == 0)) {
			g_totalShift.erase(worldspace);
			g_origin.erase(worldspace);
			return;
		}
		const auto tx = it->second.x, ty = it->second.y;
		Lock(worldspace + off::kWsCellMapLock);
		RekeyCellMap(worldspace, -tx, -ty);
		Unlock(worldspace + off::kWsCellMapLock);
		if (const auto w = g_wsOriginWritten.find(worldspace); w != g_wsOriginWritten.end()) {
			std::uint64_t back = ~0ull;
			// Only when the value is still ours: a reload re-derives it in the original frame by itself.
			if (At<std::uint64_t>(worldspace, off::kWsPlanetOrigin) == w->second && PlanPlanetOrigin(worldspace, -tx, -ty, back) == OriginPlan::Ok) {
				At<std::uint64_t>(worldspace, off::kWsPlanetOrigin) = back;
			}
			g_wsOriginWritten.erase(w);
		}
		std::uint32_t moved = 0;
		if (movePersistent) {
			if (const auto persistent = PersistentCell(worldspace)) {
				ShiftContext ctx{ -tx * off::kCellSize, -ty * off::kCellSize };
				ShiftCellRefs(ctx, persistent);
				moved = ctx.refs;
			}
		}
		g_totalShift.erase(worldspace);
		g_origin.erase(worldspace);
		g_shiftAt.store(0);       // a load/transition places the player: no runaway guard
		trace::Burst(15000, "frame reset (load or transition)");
		ClearRequest();
		g_staleUntil.store(0);
		{
			std::lock_guard<std::mutex> lock(g_historyLock);
			g_history.clear();
		}
		logger::Write("Re-centre: frame reset (%s): undid total shift (%d, %d), %u persistent objects moved back", why, tx, ty, moved);
		telemetry::Record("frame_reset", "\"why\":\"%s\",\"tx\":%d,\"ty\":%d,\"persistent\":%u", why, tx, ty, moved);
	}

	void ResetAllFrames(const char* why, bool movePersistent)
	{
		std::vector<std::uintptr_t> worldspaces;
		for (const auto& [ws, shift] : g_totalShift) {
			worldspaces.push_back(ws);
		}
		for (const auto ws : worldspaces) {
			ResetFrame(ws, why, movePersistent);
		}
	}

	bool WatchFrame()
	{
		if (g_totalShift.empty()) {
			return false;
		}
		const auto mgr = Global(addr::kPlanetManagerPtr);
		const auto tes = Global(addr::kTESPtr);
		const auto a = CurrentAnchor();
		const auto ws = a.cell ? At<std::uintptr_t>(a.cell, off::kCellWorldspace) : 0;
		bool reset = false;
		std::vector<std::uintptr_t> shifted;
		for (const auto& [w, shift] : g_totalShift) {
			shifted.push_back(w);
		}
		for (const auto w : shifted) {
			const char* why = nullptr;
			if (!a.ref && mgr && At<std::uint8_t>(mgr, off::kMgrState) == 2 && tes && At<std::uintptr_t>(tes, 0x188) == w) {
				// The ship/interior anchor can disappear briefly while the exterior grid remains live.
				// ResetFrame does not move loaded cells, so undoing the shift here splits their coordinates
				// from the cell map and the player/ship on the next exterior transition.
				continue;
			}
			if (w != ws) {
				why = a.ref ? "left the worldspace" : "went indoors or into space";
			}
			else if (mgr && At<std::uint8_t>(mgr, off::kMgrState) < 2) {
				// The manager is tearing down / reloading the planet (state 0/1): reset now, before the reload
				// reaches the cell map (resetting after it restarted crashed mid-load).
				why = "planet manager unloading (load or transition)";
			}
			else if (mgr) {
				const auto it = g_origin.find(w);
				if (it != g_origin.end() && (std::memcmp(it->second.bytes, reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), sizeof(it->second.bytes)) != 0 ||
						it->second.planet != At<std::uint32_t>(mgr, off::kMgrPlanetId))) {
					why = "planet manager restarted (load or transition)";
				}
			}
			if (why) {
				ResetFrame(w, why, true);
				reset = true;
			}
		}
		return reset;
	}

	bool ShiftBackToOriginal(const char* why)
	{
		const auto a = CurrentAnchor();
		const auto ws = a.cell ? At<std::uintptr_t>(a.cell, off::kCellWorldspace) : 0;
		const auto it = ws ? g_totalShift.find(ws) : g_totalShift.end();
		if (it == g_totalShift.end() || (it->second.x == 0 && it->second.y == 0)) {
			return true;
		}
		const auto tx = it->second.x, ty = it->second.y;
		if (Run(-tx, -ty)) {
			logger::Write("Re-centre: shifted back to the original frame (%s)", why);
			g_totalShift.erase(ws);
			g_origin.erase(ws);
			return true;
		}
		logger::Write("Re-centre: could NOT shift back to the original frame (%s); total shift (%d, %d) remains", why, tx, ty);
		telemetry::Record("shift_back_failed", "\"why\":\"%s\",\"tx\":%d,\"ty\":%d", why, tx, ty);
		return false;
	}

	void TotalShift(std::int32_t& x, std::int32_t& y)
	{
		const auto a = CurrentAnchor();
		const auto ws = a.cell ? At<std::uintptr_t>(a.cell, off::kCellWorldspace) : 0;
		const auto it = ws ? g_totalShift.find(ws) : g_totalShift.end();
		x = it != g_totalShift.end() ? it->second.x : 0;
		y = it != g_totalShift.end() ? it->second.y : 0;
	}

	void SetEngineTerrainShift(bool on) { kEngineTerrainShift = on; }

	// Match the load path's landing origin before changing the live world. The player is used only outdoors;
	// an interior ship position is not a position on the planet. Keep the target close to the terrain window:
	// the earlier zone-centre shift moved it several kilometres away and terrain jobs crashed.
	struct LandingTarget
	{
		std::int32_t cx = 0, cy = 0;
		std::uint8_t latlong[16]{};
	};

	// Why the last landing target was refused (logged at most every 5 s by the caller).
	const char* g_landingRefusal = "";

	bool PlanLandingTarget(std::uintptr_t mgr, LandingTarget& target)
	{
		g_landingRefusal = "";
		const auto player = Global(addr::kPlayerCharacterPtr);
		const auto a = CurrentAnchor();
		if (!mgr || !player || a.ref != player || !a.cell ||
			(At<std::uint8_t>(a.cell, off::kCellFlags) & 1) || At<std::uint8_t>(mgr, off::kMgrState) != 2) {
			g_landingRefusal = "not on foot outdoors with the planet manager idle";
			return false;
		}
		const auto ws = At<std::uintptr_t>(a.cell, off::kCellWorldspace);
		if (!ws || At<std::uint64_t>(ws, off::kWsPlanetOrigin) == ~0ull) {
			g_landingRefusal = "not a planet worldspace";
			return false;
		}
		const auto pos = At<game::Vec3>(player, off::kRefPosition);
		alignas(16) std::uint8_t ll[32] = {};
		const float fp[3] = { pos.x, pos.y, pos.z };
		EngineFn<void* (*)(std::uintptr_t, void*, const float*)>(addr::kWorldToLatLong)(mgr, ll, fp);
		const auto grid = EngineFn<std::uintptr_t (*)(void*, std::uint32_t, int)>(addr::kPlanetGridFor)
			(ll, At<std::uint32_t>(mgr, off::kMgrPlanetId), 1);
		if (!grid) {
			g_landingRefusal = "no planet grid for the player's lat/long";
			return false;
		}
		alignas(16) std::uint8_t aligned[0x60] = {}, moved[0x60] = {};
		EngineFn<void* (*)(std::uintptr_t, void*, void*)>(addr::kAlignedOrigin)(grid, aligned, ll);
		// The same cube face as the live origin (mgr+0xC8 byte 0): within a face the frames differ by a pure translation.
		// 0.48.1 compared grid object pointers, which may differ for the same face.
		if (aligned[0] != At<std::uint8_t>(mgr, off::kMgrOriginCoord)) {
			g_landingRefusal = "the player is on another cube face (needs a landing, not a shift)";
			return false;
		}
		std::int32_t shift[2] = {};
		EngineFn<void* (*)(std::uintptr_t, std::int32_t*, const void*, const void*)>(addr::kCoordOffset)
			(grid, shift, reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), aligned);
		std::memcpy(moved, reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), 0x2C);
		EngineFn<void (*)(std::uintptr_t, std::uintptr_t, const std::int32_t*)>(addr::kOffsetPlanetCoord)
			(grid, reinterpret_cast<std::uintptr_t>(moved), shift);
		std::int32_t residual[2] = {};
		EngineFn<void* (*)(std::uintptr_t, std::int32_t*, const void*, const void*)>(addr::kCoordOffset)
			(grid, residual, moved, aligned);
		const float nx = pos.x - shift[0] * off::kCellSize;
		const float ny = pos.y - shift[1] * off::kCellSize;
		static char why[160];
		if (residual[0] || residual[1]) {
			std::snprintf(why, sizeof(why), "coordinate check failed (residual %d, %d)", residual[0], residual[1]);
			g_landingRefusal = why;
			return false;
		}
		if (std::fabs(nx) > 5000.0f || std::fabs(ny) > 5000.0f) {
			std::snprintf(why, sizeof(why), "player would be %.0f, %.0f m from the new origin", nx, ny);
			g_landingRefusal = why;
			return false;
		}
		// The target must stay inside the terrain window (100 cells wide, centred on mgr grid centre): 32 cells keeps it
		// well inside (0.48.1 allowed 16, which block-aligned origins can exceed).
		const int wx = shift[0] - At<std::int32_t>(mgr, off::kMgrGridCentre), wy = shift[1] - At<std::int32_t>(mgr, off::kMgrGridCentre + 4);
		if (std::abs(wx) > 32 || std::abs(wy) > 32) {
			std::snprintf(why, sizeof(why), "aligned origin %d, %d cells from the terrain window centre (limit 32)", wx, wy);
			g_landingRefusal = why;
			return false;
		}
		target.cx = shift[0];
		target.cy = shift[1];
		std::memcpy(target.latlong, ll, sizeof(target.latlong));
		return true;
	}

	namespace
	{
		// The original landing of this load (mgr+0x98 before the records first followed a shift): the landing city
		// guard (landing.cpp) still asks where the player really landed.
		struct OriginalLanding { std::uint32_t planet = 0; double ll[2] = {}; double origin[2] = {}; bool set = false; } g_originalLanding;

		// Lat/long of an origin coordinate record, exactly as the landing setup derives mgr+0xB0 from mgr+0xC8.
		bool OriginLatLong(std::uintptr_t mgr, const void* origin, double out[2])
		{
			const auto pg = At<std::uintptr_t>(mgr, 0x120);
			const auto grid = pg ? At<std::uintptr_t>(pg, 0x58) : 0;
			if (!grid) {
				return false;
			}
			alignas(16) std::uint8_t cube[0x20] = {};
			EngineFn<void* (*)(std::uintptr_t, void*, const void*)>(addr::kOriginToCube)(grid, cube, origin);
			alignas(16) double ll[4] = {};
			EngineFn<double* (*)(double*, const void*)>(addr::kCubeToLatLong)(ll, cube);
			if (!std::isfinite(ll[0]) || !std::isfinite(ll[1])) {
				return false;
			}
			out[0] = ll[0];
			out[1] = ll[1];
			return true;
		}

		// The safe half of Bethesda's landing move (FUN_14078b030 lines writing +0x98/+0xB0), after our shift moved
		// the origin +0xC8: the origin's lat/long record +0xB0 and the landing lat/long +0x98 follow it, so the
		// engine's lat/long -> world conversions and landing-centred checks use the same frame as the terrain. No
		// terrain-window rebuild and no module re-init (those crashed live / switch navmesh off). Self-check first:
		// before the shift, +0xB0 must equal the lat/long computed from +0xC8 the same way, or nothing is written.
		void FollowLandingRecords(std::uintptr_t mgr, const double before[2], bool beforeOk, bool firstOfFrame)
		{
			const double stored[2] = { At<double>(mgr, 0xB0), At<double>(mgr, 0xB8) };
			if (!beforeOk || std::fabs(before[0] - stored[0]) > 1e-6 || std::fabs(before[1] - stored[1]) > 1e-6) {
				logger::Write("Re-centre: landing records NOT followed: self-check failed (computed %.8f, %.8f vs stored +0xB0 %.8f, %.8f)",
					before[0], before[1], stored[0], stored[1]);
				return;
			}
			double now[2];
			if (!OriginLatLong(mgr, reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), now)) {
				logger::Write("Re-centre: landing records NOT followed: no lat/long for the new origin");
				return;
			}
			const auto planet = At<std::uint32_t>(mgr, off::kMgrPlanetId);
			if (firstOfFrame || !g_originalLanding.set || g_originalLanding.planet != planet) {
				std::memcpy(g_originalLanding.ll, reinterpret_cast<const void*>(mgr + 0x98), 16);
				std::memcpy(g_originalLanding.origin, stored, 16);
				g_originalLanding.planet = planet;
				g_originalLanding.set = true;
			}
			const double oldLanding[2] = { At<double>(mgr, 0x98), At<double>(mgr, 0xA0) };
			At<double>(mgr, 0xB0) = now[0];
			At<double>(mgr, 0xB8) = now[1];
			At<double>(mgr, 0x98) = now[0];
			At<double>(mgr, 0xA0) = now[1];
			logger::Write("Re-centre: landing records follow: origin lat/long %.8f, %.8f -> %.8f, %.8f; landing %.8f, %.8f -> origin",
				stored[0], stored[1], now[0], now[1], oldLanding[0], oldLanding[1]);
			telemetry::Record("landing_records", "\"lat\":%.8f,\"lon\":%.8f", now[0], now[1]);
		}
	}

	bool OriginalLandingLatLong(double out[2])
	{
		const auto mgr = Global(addr::kPlanetManagerPtr);
		if (!mgr) {
			return false;
		}
		std::int32_t tx = 0, ty = 0;
		TotalShift(tx, ty);
		if ((tx || ty) && g_originalLanding.set && g_originalLanding.planet == At<std::uint32_t>(mgr, off::kMgrPlanetId)) {
			std::memcpy(out, g_originalLanding.ll, 16);
		}
		else {
			std::memcpy(out, reinterpret_cast<const void*>(mgr + 0x98), 16);
		}
		return true;
	}

	bool Run(std::int32_t cx, std::int32_t cy, bool followLanding)
	{
		if (cx == 0 && cy == 0) {
			return false;
		}
		const auto tes = Global(addr::kTESPtr);
		const auto player = Global(addr::kPlayerCharacterPtr);
		const auto mgr = Global(addr::kPlanetManagerPtr);
		if (!tes || !player || !mgr) {
			return Refuse("game objects not available");
		}
		const auto anchor = CurrentAnchor();
		if (!anchor.ref) {
			return Refuse("indoors");
		}
		if (At<std::uint8_t>(mgr, off::kMgrState) != 2 || At<std::uint8_t>(mgr, off::kMgrShifting) != 0) {
			return Refuse("planet manager busy");
		}
		// Never while a save is being written: the save's anchoring plan and the fields it serializes (position, grid,
		// ws+0x310) must come from one frame (R17).
		if (rebase::Saving()) {
			return Refuse("a save is being written");
		}
		const auto worldspace = At<std::uintptr_t>(anchor.cell, off::kCellWorldspace);
		if (!worldspace) {
			return Refuse("no worldspace");
		}
		// Navmesh generation work in flight holds grid-frame coordinates in copies this plugin cannot reach, and its
		// completion marks whichever cell then sits at those coordinates as generated (NAVMESH_SNAPSHOT_AUDIT_20261017).
		// A normal shift waits for it to drain. An urgent shift (the player is at the edge of the grid) cannot wait; it
		// goes ahead, and the in-flight entries are reported.
		if (const auto inFlight = navgen::InFlight()) {
			if (!g_runUrgent) {
				return Refuse("navmesh generation in flight");
			}
			logger::Write("Re-centre: urgent shift with %u navmesh entries in flight (their completion will mark old coordinates)", inFlight);
		}
		// Never move the frame while a native LoadCell job is building/attaching a cell. Try only: waiting on
		// the game thread could deadlock a worker needing that thread. Pending jobs are safe during an urgent
		// shift because LoadCellRunDetour and LoadCellBuildDetour rebase their planet-keyed coordinates later.
		std::unique_lock<std::shared_mutex> loadFrameLock(g_cellLoadFrameLock, std::try_to_lock);
		if (!loadFrameLock.owns_lock()) return Refuse("cell load inside native build");
		FrameWriteOwner frameWriteOwner;
		if (!g_runUrgent && (PendingCellLoads() != 0 || LiveCellLoadJobs() != 0)) {
			return Refuse("cell loads pending");
		}
		if (g_runUrgent) {
			if (At<std::uint8_t>(anchor.cell, off::kCellState) != 10) {
				return Refuse("player's cell not attached");
			}
		}
		else {
			if (!WorldIsSettled(tes, anchor.cell)) {
				return Refuse("cells still loading");
			}
		}
		const float dx = cx * off::kCellSize;
		const float dy = cy * off::kCellSize;

		// Take the cell map lock before the manager lock and before moving any state.
		// A queued loader can hold this lock while reaching for the manager. Waiting
		// for it after moving physics and objects left the game half shifted (01:31 hang).
		const auto mapLock = worldspace + off::kWsCellMapLock;
		if (!TryLockIdle(mapLock)) return Refuse("worldspace cell map busy");
		struct ReleaseMap { std::uintptr_t lock; ~ReleaseMap() { Unlock(lock); } } releaseMap{ mapLock };
		// Hold the planet manager's lock for the whole re-centre so no terrain shift starts in the middle.
		ManagerLock mgrLock;
		if (!TryLockManager(mgr, mgrLock)) {
			return Refuse("planet manager locked");
		}
		struct Release { ManagerLock& h; ~Release() { UnlockManager(h); } } release{ mgrLock };
		LandingTarget landing;
		if (followLanding && (!PlanLandingTarget(mgr, landing) || landing.cx != cx || landing.cy != cy)) {
			return Refuse("landing target changed before the shift");
		}

		std::uint64_t newOrigin = ~0ull;
		const auto originPlan = PlanPlanetOrigin(worldspace, cx, cy, newOrigin);
		if (originPlan == OriginPlan::Bad) {
			return Refuse("planet origin encoding not understood");
		}
		// Cell keys that must not change meaning: sample grid coordinates in the NEW frame, keyed now at their OLD names.
		const auto gx0 = At<std::int32_t>(tes, off::kTESGridCentre) - cx, gy0 = At<std::int32_t>(tes, off::kTESGridCentre + 4) - cy;
		const std::int32_t sample[4][2] = { { gx0, gy0 }, { gx0 + 3, gy0 }, { gx0, gy0 - 3 }, { gx0 + 12, gy0 + 12 } };
		CellKey before[4];
		for (int i = 0; i < 4; ++i) {
			before[i] = LoadKey(worldspace, sample[i][0] + cx, sample[i][1] + cy);
		}

		// 0.34.1/0.35.0/0.35.3: within a frame something put the player back at the old coordinates. The write watch on
		// the player position is armed permanently (arming every thread takes ~0.3 s, too late when armed here).
		watchpoint::ResetReports();
		logger::Write("Re-centre: shifting on thread %lu, player x %.1f", watchpoint::ThreadId(), At<float>(player, off::kRefPosition));
		hangwatch::BeginShift();
		struct ShiftWatch { ~ShiftWatch() { hangwatch::EndShift(); } } shiftWatch;
		hangwatch::ShiftStage("before_callbacks", 0, 0);
		coreapi::FireBeforeShift(cx, cy, tes);  // module plugins: nothing has moved yet

		const auto t0 = telemetry::NowUs();
		// 1. Physics first: if it fails, nothing else has changed.
		hangwatch::ShiftStage("physics", 0, 0);
		if (!ShiftPhysics(dx, dy)) {
			return Refuse("physics world not shiftable");
		}

		const auto t1 = telemetry::NowUs();
		// 2 + 3. Objects and cells.
		hangwatch::ShiftStage("objects", 0, 0);
		ShiftContext ctx{ dx, dy };
		{
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			ctx.haveCentre = true;
			ctx.cx = pp.x - dx;
			ctx.cy = pp.y - dy;
		}
		const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
		const auto count = At<std::uint32_t>(tes, off::kTESGridCount);
		unsigned cellsDone = 0;
		for (std::uint32_t i = 0; entries && i < count; ++i) {
			const auto cell = GridCell(entries[i]);
			ShiftCellRefs(ctx, cell);
			ShiftCell(cell, cx, cy, dx, dy);
			hangwatch::ShiftProgress(++cellsDone, ctx.refs);
		}
		const auto persistent = PersistentCell(worldspace);
		ShiftCellRefs(ctx, persistent);  // permanent references (landed ship, markers) live here, not in the grid
		ShiftRef(ctx, anchor.ref);  // the player outdoors, or their ship; in case it is not listed in a loaded cell
		hangwatch::ShiftStage("scene_geometry", cellsDone, ctx.refs);
		ShiftSceneGeometry(ctx);
		// Inside the ship the player stands in the ship's interior cell, which has its own coordinates: not shifted.

		const auto t2 = telemetry::NowUs();
		hangwatch::ShiftStage("cell_map", cellsDone, ctx.refs);
		RekeyCellMap(worldspace, cx, cy);
		if (originPlan == OriginPlan::Ok) {
			At<std::uint64_t>(worldspace, off::kWsPlanetOrigin) = newOrigin;
			g_wsOriginWritten[worldspace] = newOrigin;
		}
		int keysKept = 0;
		for (int i = 0; i < 4; ++i) {
			const auto after = LoadKey(worldspace, sample[i][0], sample[i][1]);
			if (std::memcmp(after.w, before[i].w, sizeof(after.w)) == 0) {
				++keysKept;
				continue;
			}
			logger::Write("Re-centre: CELL KEY CHANGED at new (%d, %d): before %08X %08X %08X, after %08X %08X %08X", sample[i][0], sample[i][1],
				before[i].w[0], before[i].w[1], before[i].w[2], after.w[0], after.w[1], after.w[2]);
		}
		logger::Write("Re-centre: planet origin %s; %d/4 sampled cell keys name the same cells after the shift",
			originPlan == OriginPlan::Ok ? "moved" : "none (not a planet worldspace)", keysKept);
		telemetry::Record("cell_keys", "\"origin\":%d,\"kept\":%d", originPlan == OriginPlan::Ok ? 1 : 0, keysKept);

		ShiftTESGrid(tes, cx, cy);

		const auto t3 = telemetry::NowUs();
		// 4. Planet manager / terrain.
		hangwatch::ShiftStage("planet_manager", cellsDone, ctx.refs);
		double llBefore[2] = {};
		const bool llBeforeOk = !followLanding && OriginLatLong(mgr, reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), llBefore);
		ShiftPlanetManager(mgr, cx, cy, dx, dy);
		if (kEngineTerrainShift) {
			ShiftTerrainViaEngine(mgr, mgrLock, cx, cy);
			FixTerrainGridOffset(mgr);
		}
		if (!followLanding) {
			g_landingFollows[worldspace] = false;  // a fallback shift: the stored landing no longer matches the frame
			if (kLandingRecordsFollow) {
				const auto tot = g_totalShift.find(worldspace);
				const bool first = tot == g_totalShift.end() || (tot->second.x == 0 && tot->second.y == 0);
				FollowLandingRecords(mgr, llBefore, llBeforeOk, first);  // after the engine's shift commit (state 2), under our lock
			}
		}
		if (followLanding) {
			const auto grid = EngineFn<std::uintptr_t (*)(void*, std::uint32_t, int)>(addr::kPlanetGridFor)
				(landing.latlong, At<std::uint32_t>(mgr, off::kMgrPlanetId), 1);
			alignas(16) std::uint8_t aligned[0x60] = {};
			std::int32_t residual[2] = { INT32_MAX, INT32_MAX };
			if (grid) {
				EngineFn<void* (*)(std::uintptr_t, void*, void*)>(addr::kAlignedOrigin)(grid, aligned, landing.latlong);
				EngineFn<void* (*)(std::uintptr_t, std::int32_t*, const void*, const void*)>(addr::kCoordOffset)
					(grid, residual, reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), aligned);
			}
			if (residual[0] == 0 && residual[1] == 0) {
				std::memcpy(reinterpret_cast<void*>(mgr + 0x98), landing.latlong, sizeof(landing.latlong));
				g_landingFollows[worldspace] = true;
				double ll[2]{};
				std::memcpy(ll, landing.latlong, sizeof(ll));
				logger::Write("Re-centre: live landing follows player (lat/long %.6f, %.6f; aligned origin now at world zero)", ll[0], ll[1]);
				telemetry::Record("live_landing", "\"lat\":%.8f,\"lon\":%.8f,\"shift\":[%d,%d]", ll[0], ll[1], cx, cy);
			}
			else {
				g_landingFollows[worldspace] = false;
				logger::Write("Re-centre: landing origin disagrees after shift by (%d, %d) cells; stored landing left unchanged, save fallback retained",
					residual[0], residual[1]);
			}
		}
		auto& total = g_totalShift[worldspace];
		total.x += cx;
		total.y += cy;
		auto& snap = g_origin[worldspace];
		std::memcpy(snap.bytes, reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), sizeof(snap.bytes));
		snap.planet = At<std::uint32_t>(mgr, off::kMgrPlanetId);
		const auto t4 = telemetry::NowUs();

		// 5. Let the engine's own subscribers follow.
		hangwatch::ShiftStage("origin_event", cellsDone, ctx.refs);
		SendOriginShift(At<std::uint32_t>(worldspace, 0x28), dx, dy);

		const auto t5 = telemetry::NowUs();
		logger::Write("Re-centred by (%d, %d) cells: %u objects moved, total shift (%d, %d)%s%s", cx, cy, ctx.refs, total.x, total.y,
			anchor.inShip ? " (anchor: player's ship)" : "", persistent ? "" : " (persistent cell not found)");
		telemetry::Record("recentre",
			"\"engine_terrain\":%d,\"cx\":%d,\"cy\":%d,\"objects\":%u,\"cells_authored\":%llu,\"us_physics\":%llu,\"us_objects\":%llu,\"us_cellmap\":%llu,\"us_terrain\":%llu,\"us_event\":%llu,\"us_total\":%llu",
			kEngineTerrainShift ? 1 : 0, cx, cy, ctx.refs, static_cast<unsigned long long>(AuthoredCells(worldspace)),
			static_cast<unsigned long long>(t1 - t0), static_cast<unsigned long long>(t2 - t1), static_cast<unsigned long long>(t3 - t2),
			static_cast<unsigned long long>(t4 - t3), static_cast<unsigned long long>(t5 - t4), static_cast<unsigned long long>(t5 - t0));
		readout::OnRecentre(cx, cy, ctx.refs);
		// The player's character controller and its physics proxy keep their own position (hknpWorld::shiftWorld does
		// not move them; the movement update reads the controller back onto the player). The actor position setter
		// with warp (FUN_14193B410, as a teleport finishes) sets the controller (vtbl +0x240), zeroes its velocity and
		// re-syncs the proxy, synchronously. (0.38.1 used the move routine, whose teleport landed 0.4-1.2 s later.)
		// Rebuild the grid's terrain collision (and re-attach) once the shift has settled: detach, clear, build, attach
		hangwatch::ShiftStage("post_shift", cellsDone, ctx.refs);
		// (terrainguard::ServicePending). 0.43.1: right after a shift no ray found ground anywhere in the cells that
		// existed before it (here, old frame, double shift), while cells loaded after it had ground.
		terrainguard::MarkGridPending(tes, 500);
		MarkSyncBoundsDirty();  // the sync box must follow the shifted collision (rebuilt in this frame's visit)
		g_shiftAt.store(0);  // our own warp below must not meet the runaway guard of an earlier shift
		if (anchor.ref == player && !anchor.inShip) {
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			alignas(16) float pos[4] = { pp.x, pp.y, pp.z, 0.0f };
			EngineFn<void (*)(std::uintptr_t, float*, char)>(addr::kActorSetPosition)(player, pos, 1);
		}
		g_shiftX.store(dx);
		g_shiftY.store(dy);
		g_shiftAt.store(GetTickCount64());
		prefetch::ResetProbes();
		trace::Burst(5000, "re-centre");
		g_staleUntil.store(GetTickCount64() + 4000);
		{
			std::lock_guard<std::mutex> lock(g_historyLock);
			g_history.push_back({ cx, cy });
			g_frameVersion.fetch_add(1);
			if (g_history.size() > 16) {
				g_history.erase(g_history.begin());
			}
		}
		g_justShifted = true;
		g_playerAfterShift = At<game::Vec3>(player, off::kRefPosition);
		if (!ctx.far3D.empty()) {
			std::lock_guard lk(g_far3DLock);
			g_far3D.insert(g_far3D.end(), ctx.far3D.begin(), ctx.far3D.end());
			logger::Write("Re-centre: %zu objects beyond %.0f m get their 3D update over the next frames", ctx.far3D.size(), kEngineUpdateRadius);
		}
		hangwatch::ShiftStage("after_callbacks", cellsDone, ctx.refs);
		coreapi::FireShift(cx, cy, tes);  // module plugins: the shift is complete
		return true;
	}

	// ---- Shifts run inside the game update ---------------------------------------------------------------------
	// The SFSE frame task runs on whichever worker is free, alongside the game update. A shift made there could land
	// while the engine was syncing physics bodies to actors (FUN_140D50650 from the loaded-cells visit): the sync then
	// wrote a position read before the shift and the player was thrown back (0.34-0.37: intermittent snap-backs, once
	// to 74 km away). The frame task now only asks; the shift runs at the start of the engine's per-frame loaded-cells
	// visit (FUN_1405781C0), in the update sequence itself, before that sync.
	namespace
	{
		struct Deferred
		{
			std::atomic<int> state{ 0 };  // 0 idle, 1 asked, 3 running, 2 done
			std::int32_t cx = 0, cy = 0;
			bool urgent = false;
			bool followLanding = false;
			bool result = false;
			ULONGLONG since = 0;
		} g_def;
		bool g_syncHooked = false;

		using Visit_t = void (*)(std::uintptr_t, char);
		Visit_t g_visit = nullptr;

		// Saves: SFSE never sends PreSaveGame/PostSaveGame here (no "Saving" line in any session), so the shift back to
		// the original frame never ran (0.38.0: a save written at total shift (-47, 38)). Save requests set flags on the
		// save manager (+0x50: 1 autosave, 8/0x10 other saves; FUN_14184FF90); the request is seen here, inside the
		// game update, and the world shifted back before the save manager writes; forward again once the writer ran and
		// the flags are clear.
		std::atomic<int> g_saveState{ 0 };  // 0 none, 1 shifted back for a pending save, 2 written
		std::atomic<bool> g_saveBackAsked{ false };  // SaveGame started in a shifted frame and waits for the shift back
		ULONGLONG g_saveSince = 0;
		std::uint32_t g_lastSaveFlags = 0;
		void SaveWatch()
		{
			const auto slm = Global(addr::kSaveLoadManagerPtr);
			if (!slm) {
				return;
			}
			const auto flags = At<std::uint32_t>(slm, 0x50);
			if (flags != g_lastSaveFlags) {
				logger::Write("Save watch: save manager flags %08X -> %08X", g_lastSaveFlags, flags);
				g_lastSaveFlags = flags;
			}
			return;  // the save itself is handled in SaveGame (SaveFrame); the flags are only logged
			const bool pending = (flags & (0x1 | 0x8 | 0x10)) != 0 || g_saveBackAsked.load();
			const ULONGLONG now = GetTickCount64();
			if (g_saveState.load() == 0 && pending) {
				std::int32_t tx = 0, ty = 0;
				TotalShift(tx, ty);
				if (tx != 0 || ty != 0) {
					BeforeSave();
					g_saveState.store(1);
					g_saveSince = now;
				}
			}
			else if (g_saveState.load() == 2 && !pending) {
				AfterSave();
				g_saveState.store(0);
			}
			else if (g_saveState.load() != 0 && now - g_saveSince > 15000) {
				logger::Write("Save watch: no save written within 15 s of the request; shifting forward again");
				AfterSave();
				g_saveState.store(0);
			}
		}

		std::atomic<unsigned long> g_syncThread{ 0 };  // thread of the last per-frame loaded-cells visit

		// ---- Movement trace (game update, per frame) ------------------------------------------------------------
		// Every 0.5 s: position, horizontal/vertical speed, the character controller's own position and its gap to the
		// player, height above the ground probe, parent cell and its state, distance to the physics edge, total shift;
		// a FLOATING flag when the player stays > 2 m above the ground without falling for 1 s. For 3 s after a shift the
		// same line is written every frame. Also to telemetry ("move").
		bool ReadControllerPos(std::uintptr_t player, float out[4])
		{
			__try {
				const auto proc = *reinterpret_cast<std::uintptr_t*>(player + 0x228);
				const auto mid = proc ? *reinterpret_cast<std::uintptr_t*>(proc + 8) : 0;
				const auto ctrl = mid ? *reinterpret_cast<std::uintptr_t*>(mid + 0x458) : 0;
				if (!ctrl) {
					return false;
				}
				auto** vtbl = *reinterpret_cast<void***>(ctrl);
				reinterpret_cast<void (*)(std::uintptr_t, float*, char)>(vtbl[0x248 / 8])(ctrl, out, 1);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// ---- Body->actor sync bounds ---------------------------------------------------------------------------
		// Exterior cells share one sync object (FUN_140AF1250: *(DAT_1461DF6E8 + 8)). Each frame the loaded-cells visit
		// (FUN_140578350 -> FUN_140D50650) syncs physics bodies onto their references, and FUN_140D53F10 clamps every
		// result into the object's box (+0x40 min, +0x50 max). The box is the union of the collision of the loaded cells
		// (FUN_140D527A0 / FUN_140D52B20), rebuilt only when the visited cell changes or the dirty byte +0xC8 is set,
		// which only a cell attach does (FUN_140B02C80). Nothing rebuilds it after a re-centre (the bodies move, the box
		// does not: the clamp pulls the player kilometres back, the 0.39.x runaway) or when terrain collision arrives
		// after its cell attached (far from the landing the box stops short of the new ground: below its floor the
		// player is held up, the floating / jump you can't land from). We set the dirty byte after every shift and
		// four times a second, so the engine rebuilds the box from the current collision in the same visit.
		constexpr std::uintptr_t kSyncBoundsMin = 0x40, kSyncBoundsMax = 0x50, kSyncCell = 0x60, kSyncDirty = 0xC8;

		std::uintptr_t ExteriorSync()
		{
			const auto slot = Global(addr::kExteriorWorldPtr);
			return slot ? At<std::uintptr_t>(slot, 8) : 0;
		}

		void MarkSyncBoundsDirty()
		{
			if (const auto sync = ExteriorSync()) {
				At<std::uint8_t>(sync, kSyncDirty) = 1;
			}
		}

		// Box of the sync object; false when there is none.
		bool SyncBounds(float mn[4], float mx[4])
		{
			const auto sync = ExteriorSync();
			if (!sync) {
				return false;
			}
			std::memcpy(mn, reinterpret_cast<const void*>(sync + kSyncBoundsMin), 16);
			std::memcpy(mx, reinterpret_cast<const void*>(sync + kSyncBoundsMax), 16);
			return true;
		}

		std::string BoxText(const game::Vec3& p)
		{
			float mn[4], mx[4];
			if (!SyncBounds(mn, mx)) {
				return "n/a";
			}
			char text[160];
			const bool inside = p.x >= mn[0] && p.x <= mx[0] && p.y >= mn[1] && p.y <= mx[1] && p.z >= mn[2] && p.z <= mx[2];
			std::snprintf(text, sizeof(text), "(%.0f, %.0f, %.0f)-(%.0f, %.0f, %.0f)%s", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], inside ? "" : " PLAYER OUTSIDE");
			return text;
		}

		// ---- Detail trace: snapshot and change events (trace file) ---------------------------------------------
		struct SlotState { std::uintptr_t cell = 0; std::uint32_t id = 0; std::uint8_t state = 0xFF; std::int32_t x = 0, y = 0; };
		SlotState g_slots[64];
		struct MgrState { std::uint8_t state = 0xFF, shifting = 0xFF; std::int32_t gx = 0, gy = 0, nx = 0, ny = 0, dx = 0, dy = 0; } g_mgrLast;
		std::int32_t g_tesLast[4] = { INT_MIN, INT_MIN, INT_MIN, INT_MIN };

		void TraceSnapshot()
		{
			static ULONGLONG last = 0;
			const ULONGLONG now = GetTickCount64();
			const auto tes = Global(addr::kTESPtr);
			const auto mgr = Global(addr::kPlanetManagerPtr);
			const auto player = Global(addr::kPlayerCharacterPtr);
			if (!tes || !player) {
				return;
			}
			// Changes, every frame.
			if (mgr) {
				MgrState m{ At<std::uint8_t>(mgr, off::kMgrState), At<std::uint8_t>(mgr, off::kMgrShifting), At<std::int32_t>(mgr, off::kMgrGridCentre),
					At<std::int32_t>(mgr, off::kMgrGridCentre + 4), At<std::int32_t>(mgr, off::kMgrNewCentre), At<std::int32_t>(mgr, off::kMgrNewCentre + 4),
					At<std::int32_t>(mgr, off::kMgrShiftDelta), At<std::int32_t>(mgr, off::kMgrShiftDelta + 4) };
				if (std::memcmp(&m, &g_mgrLast, sizeof(m)) != 0) {
					trace::Line("planet", "manager state %u -> %u, shifting %u -> %u, grid centre (%d, %d) -> (%d, %d), new centre (%d, %d), delta (%d, %d)",
						g_mgrLast.state, m.state, g_mgrLast.shifting, m.shifting, g_mgrLast.gx, g_mgrLast.gy, m.gx, m.gy, m.nx, m.ny, m.dx, m.dy);
					g_mgrLast = m;
				}
			}
			const std::int32_t t4[4] = { At<std::int32_t>(tes, off::kTESGridCentre), At<std::int32_t>(tes, off::kTESGridCentre + 4),
				At<std::int32_t>(tes, off::kTESCurrentCell), At<std::int32_t>(tes, off::kTESCurrentCell + 4) };
			if (std::memcmp(t4, g_tesLast, sizeof(t4)) != 0) {
				trace::Line("tes", "grid centre (%d, %d) -> (%d, %d), current cell (%d, %d) -> (%d, %d)", g_tesLast[0], g_tesLast[1], t4[0], t4[1], g_tesLast[2],
					g_tesLast[3], t4[2], t4[3]);
				std::memcpy(g_tesLast, t4, sizeof(t4));
			}
			const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
			const auto count = std::min<std::uint32_t>(At<std::uint32_t>(tes, off::kTESGridCount), 64);
			for (std::uint32_t i = 0; entries && i < count; ++i) {
				const auto cell = entries[i] ? At<std::uintptr_t>(entries[i], off::kGridEntryCell) : 0;
				SlotState s{};
				if (cell) {
					const auto ext = At<std::uintptr_t>(cell, off::kCellExterior);
					s = { cell, At<std::uint32_t>(cell, 0x28), At<std::uint8_t>(cell, off::kCellState), ext ? At<std::int32_t>(ext, 0) : 0, ext ? At<std::int32_t>(ext, 4) : 0 };
				}
				auto& o = g_slots[i];
				if (o.cell != s.cell || o.state != s.state || o.x != s.x || o.y != s.y) {
					trace::Line("grid", "slot %u: cell %08X (%d, %d) state %u -> cell %08X (%d, %d) state %u", i, o.id, o.x, o.y, unsigned(o.state), s.id, s.x, s.y,
						unsigned(s.state));
					o = s;
				}
			}
			{
				static float lastMin[4] = {}, lastMax[4] = {};
				float mn[4], mx[4];
				if (SyncBounds(mn, mx) && (std::memcmp(mn, lastMin, 16) != 0 || std::memcmp(mx, lastMax, 16) != 0)) {
					trace::Line("bounds", "sync box (%.1f, %.1f, %.1f)-(%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f)-(%.1f, %.1f, %.1f), cell %p", lastMin[0], lastMin[1],
						lastMin[2], lastMax[0], lastMax[1], lastMax[2], mn[0], mn[1], mn[2], mx[0], mx[1], mx[2],
						reinterpret_cast<void*>(At<std::uintptr_t>(ExteriorSync(), kSyncCell)));
					std::memcpy(lastMin, mn, 16);
					std::memcpy(lastMax, mx, 16);
				}
			}
			// Snapshot: 10 per second, every frame while detailed.
			if (!trace::Detailed() && now - last < 100) {
				return;
			}
			last = now;
			const auto p = At<game::Vec3>(player, off::kRefPosition);
			const auto r = At<game::Vec3>(player, off::kRefRotation);
			alignas(16) float c[4] = {};
			const bool haveCtrl = ReadControllerPos(player, c);
			float ground = 0;
			unsigned age = 0;
			const bool haveGround = prefetch::LatestGround(prefetch::kSlotTrace, ground, age) && age <= 30 && ground > -1e30f;
			const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
			const auto ext = cell ? At<std::uintptr_t>(cell, off::kCellExterior) : 0;
			const auto ws = cell ? At<std::uintptr_t>(cell, off::kCellWorldspace) : 0;
			std::int32_t tx = 0, ty = 0;
			TotalShift(tx, ty);
			const auto slm = Global(addr::kSaveLoadManagerPtr);
			const auto act = terrainguard::LastActivityMs();
			const auto a = CurrentAnchor();
			trace::Line("snap",
				"pos (%.2f, %.2f, %.2f) rot (%.2f, %.2f, %.2f) ctrl %s(%.2f, %.2f, %.2f) above %.2f%s | cell %08X (%d, %d) st %u ws+0x310 %016llX | "
				"tes (%d, %d) cur (%d, %d) | mgr st %u sh %u grid (%d, %d) | shift (%d, %d) | loads q%d live%d | terrain %lld ms ago | save flags %08X | "
				"request %s | anchor %s | sync box %s",
				p.x, p.y, p.z, r.x, r.y, r.z, haveCtrl ? "" : "n/a ", c[0], c[1], c[2], haveGround ? p.z - ground : -9999.0f, haveGround ? "" : "?",
				cell ? At<std::uint32_t>(cell, 0x28) : 0u, ext ? At<std::int32_t>(ext, 0) : 0, ext ? At<std::int32_t>(ext, 4) : 0,
				cell ? unsigned(At<std::uint8_t>(cell, off::kCellState)) : 0u, ws ? static_cast<unsigned long long>(At<std::uint64_t>(ws, off::kWsPlanetOrigin)) : 0ull,
				g_tesLast[0], g_tesLast[1], g_tesLast[2], g_tesLast[3], unsigned(g_mgrLast.state), unsigned(g_mgrLast.shifting), g_mgrLast.gx, g_mgrLast.gy, tx, ty,
				PendingCellLoads(), LiveCellLoadJobs(), act ? static_cast<long long>(now - act) : -1ll, slm ? At<std::uint32_t>(slm, 0x50) : 0u,
				Pending() ? "pending" : "none",
				a.ref == player ? "player" : (a.inShip ? "ship" : "other"), BoxText(p).c_str());
		}

		// Ground diagnostic (0.42.1): after a shift the normal ground probe found nothing under the player for minutes
		// while the player was held at a height. Once a second, long rays (from 1 km above, 3 km long): under the player
		// with the normal layers and with every layer, and with every layer at the player's pre-shift spot (old frame,
		// position + last shift) and at the doubly shifted spot (position - last shift). Collision that is missing hits
		// nowhere; collision left in the old frame hits at +shift; collision moved twice hits at -shift.
		void GroundDiag(const game::Vec3& p, ULONGLONG now)
		{
			static ULONGLONG last = 0;
			if (now - last < 1000) {
				return;
			}
			last = now;
			const float sx = g_shiftX.load(), sy = g_shiftY.load();
			const char* names[prefetch::kSlotDiagCount] = { "here", "here all layers", "old frame", "double shift" };
			float res[prefetch::kSlotDiagCount];
			unsigned ages[prefetch::kSlotDiagCount];
			char text[512];
			int n = std::snprintf(text, sizeof(text), "pos (%.1f, %.1f, %.1f), last shift (%.0f, %.0f):", p.x, p.y, p.z, sx, sy);
			for (int i = 0; i < prefetch::kSlotDiagCount; ++i) {
				const bool have = prefetch::LatestGround(prefetch::kSlotDiag0 + i, res[i], ages[i]);
				n += std::snprintf(text + n, sizeof(text) - n, " %s %s", names[i], !have ? "-" : res[i] > -1e30f ? "" : "MISS");
				if (have && res[i] > -1e30f) {
					n += std::snprintf(text + n, sizeof(text) - n, "%.1f", res[i]);
				}
				n += std::snprintf(text + n, sizeof(text) - n, ";");
			}
			trace::Line("ground", "%s", text);
			const auto tes = Global(addr::kTESPtr);
			if (g_shiftAt.load() && now - g_shiftAt.load() < 120000) {
				logger::Write("Ground diag: %s TES+0x188 %p TES+0xE8 %p", text, reinterpret_cast<void*>(tes ? At<std::uintptr_t>(tes, 0x188) : 0),
					reinterpret_cast<void*>(tes ? At<std::uintptr_t>(tes, 0xE8) : 0));
			}
			prefetch::RequestGround(prefetch::kSlotDiag0 + 0, p.x, p.y, p.z, 1000.0f, 3000.0f);
			prefetch::RequestGroundMask(prefetch::kSlotDiag0 + 1, p.x, p.y, p.z, 1000.0f, 3000.0f, 0xFFFFFFFFu);
			prefetch::RequestGroundMask(prefetch::kSlotDiag0 + 2, p.x + sx, p.y + sy, p.z, 1000.0f, 3000.0f, 0xFFFFFFFFu);
			prefetch::RequestGroundMask(prefetch::kSlotDiag0 + 3, p.x - sx, p.y - sy, p.z, 1000.0f, 3000.0f, 0xFFFFFFFFu);
		}

		void MoveTrace()
		{
			static ULONGLONG lastSample = 0, lastFrame = 0, floatingSince = 0;
			static game::Vec3 lastPos{};
			const ULONGLONG now = GetTickCount64();
			const auto player = Global(addr::kPlayerCharacterPtr);
			const auto tes = Global(addr::kTESPtr);
			if (!player || !tes || At<std::uintptr_t>(tes, off::kTESInteriorCell) != 0) {
				return;
			}
			const bool afterShift = g_shiftAt.load() && now - g_shiftAt.load() < 3000;
			if (!afterShift && now - lastSample < 500) {
				return;
			}
			const double dt = lastFrame ? double(now - lastFrame) / 1000.0 : 0.0;
			lastFrame = now;
			const auto p = At<game::Vec3>(player, off::kRefPosition);
			const double hs = dt > 0 ? std::hypot(p.x - lastPos.x, p.y - lastPos.y) / dt : 0.0;
			const double vs = dt > 0 ? (p.z - lastPos.z) / dt : 0.0;
			lastPos = p;
			alignas(16) float c[4] = {};
			const bool haveCtrl = ReadControllerPos(player, c);
			const double gap = haveCtrl ? std::hypot(c[0] - p.x, c[1] - p.y) : -1.0;
			float ground = 0;
			unsigned age = 0;
			const bool gotGround = prefetch::LatestGround(prefetch::kSlotTrace, ground, age);
			const bool haveGround = gotGround && age <= 30 && ground > -1e30f;
			prefetch::RequestGround(prefetch::kSlotTrace, p.x, p.y, p.z, 3.0f, 300.0f);
			GroundDiag(p, now);
			// Movement diagnosis: a slow no-clip traverse cannot be attributed to terrain contact. Record the
			// command reaching PlayerControls alongside the resulting position and the ray's actual state.
			const auto controls = Global(addr::kPlayerControlsPtr);
			const float inputX = controls ? At<float>(controls, off::kPlayerControlsData) : 0.0f;
			const float inputY = controls ? At<float>(controls, off::kPlayerControlsData + 4) : 0.0f;
			const float inputZ = controls ? At<float>(controls, off::kPlayerControlsData + 8) : 0.0f;
			const bool noClip = *reinterpret_cast<std::uint8_t*>(Base() + addr::kNoClipFlag) != 0;
			const auto playerVehicle = EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(player);
			unsigned keys = 0;
			const int vk[7] = { 'W', 'S', 'A', 'D', VK_SPACE, VK_CONTROL, VK_SHIFT };
			for (unsigned i = 0; i < 7; ++i) {
				if (GetAsyncKeyState(vk[i]) & 0x8000) keys |= 1u << i;
			}
			DWORD foregroundPid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
			const char* probeState = !gotGround ? "pending" : age > 30 ? "stale" : ground <= -1e30f ? "miss" : "hit";
			const double above = haveGround ? p.z - ground : -9999.0;
			bool floating = false;
			if (haveGround && above > 2.0 && std::fabs(vs) < 0.5) {
				if (!floatingSince) floatingSince = now;
				floating = now - floatingSince >= 1000;
			}
			else {
				floatingSince = 0;
			}
			const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
			const auto ext = cell ? At<std::uintptr_t>(cell, off::kCellExterior) : 0;
			const double reach = std::max(std::fabs(p.x), std::fabs(p.y));
			std::int32_t tx = 0, ty = 0;
			TotalShift(tx, ty);
			if (!afterShift) {
				lastSample = now;
			}
			logger::Write("Move%s: pos (%.1f, %.1f, %.1f) speed %.1f m/s vz %.1f; controller %s(%.1f, %.1f, %.1f) gap %.1f m; above ground %.1f m%s; cell %08X (%d, %d) state %u; "
				"%.0f m to physics edge; shift (%d, %d); noclip %d vehicle %p input (%.2f, %.2f, %.2f) keys %02X focus %d probe %s age %u%s",
				afterShift ? " [after shift]" : "", p.x, p.y, p.z, hs, vs, haveCtrl ? "" : "n/a ", c[0], c[1], c[2], gap, above, haveGround ? "" : " (no probe yet)",
				cell ? At<std::uint32_t>(cell, 0x28) : 0u, ext ? At<std::int32_t>(ext, 0) : 0, ext ? At<std::int32_t>(ext, 4) : 0,
				cell ? unsigned(At<std::uint8_t>(cell, off::kCellState)) : 0u, double(handoff::PhysicsHalfExtent()) - reach, tx, ty,
				noClip ? 1 : 0, reinterpret_cast<void*>(playerVehicle), inputX, inputY, inputZ, keys, foregroundPid == GetCurrentProcessId() ? 1 : 0, probeState, gotGround ? age : 0u,
				floating ? "  <-- FLOATING" : "");
			telemetry::Record("move", "\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"hs\":%.1f,\"vz\":%.1f,\"gap\":%.1f,\"above\":%.1f,\"cellstate\":%u,\"float\":%d,\"shift\":%d",
				p.x, p.y, p.z, hs, vs, gap, above, cell ? unsigned(At<std::uint8_t>(cell, off::kCellState)) : 0u, floating ? 1 : 0, afterShift ? 1 : 0);
		}

		// After a load far from the landing origin the player's cell can be attached without ground collision (it loaded
		// before the terrain source reached it; 0.43.1: the player fell to -4.7 km). Hold the player where they are
		// (warp to the held point zeroes the controller's velocity) until the cell's collision is rebuilt, 30 s at most.
		void HoldPlayerForGround()
		{
			static bool holding = false;
			static float held[4] = {};
			static ULONGLONG since = 0;
			const auto player = Global(addr::kPlayerCharacterPtr);
			const auto cell = player ? At<std::uintptr_t>(player, off::kRefParentCell) : 0;
			// 0.53.1: never while driving. The warp fought the rover every frame (2026-10-15 14:42:47: the setter alternated
			// between the vehicle +0xBA900E and this hold +0x193B442 dozens of times a second): the player could not move.
			const bool inVehicle = player && EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(player) != 0;
			const bool hold = cell && !inVehicle && terrainguard::HoldPlayer(cell);
			if (hold && !holding) {
				const auto p = At<game::Vec3>(player, off::kRefPosition);
				held[0] = p.x; held[1] = p.y; held[2] = p.z; held[3] = 0;
				since = GetTickCount64();
				logger::Write("Terrain collision: holding the player at (%.1f, %.1f, %.1f) until the ground under them is built", p.x, p.y, p.z);
				notify::Show("Waiting for the ground to load...");
			}
			if (!hold && holding) {
				logger::Write("Terrain collision: player released after %.1f s", (GetTickCount64() - since) / 1000.0);
			}
			holding = hold;
			if (holding) {
				alignas(16) float pos[4] = { held[0], held[1], held[2], 0.0f };
				EngineFn<void (*)(std::uintptr_t, float*, char)>(addr::kActorSetPosition)(player, pos, 1);
			}
		}

		// 0.53.2 safety net: a player left under the terrain (2026-10-15: the snap out of the rover placed them 3-4 m below
		// the real collision; afterwards "no ground", invisible pushing, stuck). On foot outdoors, every 250 ms: no floor
		// within 60 m below the feet, but a surface within 12 m above, for a full second = buried: lift onto that surface.
		void RescueBuriedPlayer()
		{
			static ULONGLONG last = 0;
			static int buriedTicks = 0;
			const ULONGLONG now = GetTickCount64();
			if (now - last < 250) {
				return;
			}
			last = now;
			const auto player = Global(addr::kPlayerCharacterPtr);
			const auto cell = player ? At<std::uintptr_t>(player, off::kRefParentCell) : 0;
			if (!cell || (At<std::uint8_t>(cell, off::kCellFlags) & 1) ||
				EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(player) != 0) {
				buriedTicks = 0;
				return;
			}
			const auto p = At<game::Vec3>(player, off::kRefPosition);
			float below = 0, above = 0;
			unsigned ab = 99, aa = 99;
			const bool haveBelow = prefetch::LatestGround(prefetch::kSlotRescue0, below, ab) && ab <= 3;
			const bool haveAbove = prefetch::LatestGround(prefetch::kSlotRescue0 + 1, above, aa) && aa <= 3;
			const bool floorBelow = haveBelow && below > -1e30f && below != -500.0f;
			const bool surfaceAbove = haveAbove && above > -1e30f && above != -500.0f && above > p.z + 0.5f && above < p.z + 12.0f;
			if (haveBelow && haveAbove && !floorBelow && surfaceAbove) {
				if (++buriedTicks >= 4) {
					alignas(16) float pos[4] = { p.x, p.y, above + 0.5f, 0.0f };
					EngineFn<void (*)(std::uintptr_t, float*, char)>(addr::kActorSetPosition)(player, pos, 1);
					logger::Write("Rescue: the player was under the ground at (%.1f, %.1f, %.1f) (no floor below, surface %.1f m above); lifted onto it",
						p.x, p.y, p.z, above - p.z);
					telemetry::Record("rescue_buried", "\"pos\":[%.1f,%.1f,%.1f],\"surface\":%.1f", p.x, p.y, p.z, above);
					notify::Show("Lifted out of the ground");
					buriedTicks = 0;
				}
			}
			else {
				buriedTicks = 0;
			}
			prefetch::RequestGround(prefetch::kSlotRescue0, p.x, p.y, p.z + 0.3f, 0.0f, 60.0f);
			prefetch::RequestGround(prefetch::kSlotRescue0 + 1, p.x, p.y, p.z + 12.0f, 0.0f, 11.5f);
		}

		void VisitDetour(std::uintptr_t a, char b)
		{
			static thread_local bool inside = false;  // the move routine inside Run may come back here
			// Only the per-frame call (from FUN_1417C8B50) is the sync point. The same function is also called from the
			// grid relocation (FUN_140666C00, also used by the load grid rebuild FUN_1418290D0), FUN_14059BA60 and
			// FUN_141844B30 (save/load manager): a shift there would land in the middle of a grid rebuild or a load.
			const bool perFrame = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - Base() == addr::kLoadedCellsVisitPerFrameReturn;
			if (inside || !perFrame) {
				g_visit(a, b);
				return;
			}
			inside = true;
			g_syncThread.store(GetCurrentThreadId());
			trace::NextFrame();
			TraceSnapshot();
			MoveTrace();
			SaveWatch();
			int asked = 1;
			if (g_def.state.compare_exchange_strong(asked, 3)) {  // 3: running
				g_runUrgent = g_def.urgent;
				g_def.result = Run(g_def.cx, g_def.cy, g_def.followLanding);
				g_runUrgent = false;
				g_def.state.store(2);
			}
			terrainguard::ServicePending(Global(addr::kTESPtr));
			ServiceFar3D();
			navgen::Service();
			flight::SyncTick(Global(addr::kTESPtr));
			coreapi::FireSync(Global(addr::kTESPtr));
			HoldPlayerForGround();
			RescueBuriedPlayer();
			{
				static ULONGLONG lastDirty = 0;
				const ULONGLONG now = GetTickCount64();
				if (now - lastDirty >= 250) {
					lastDirty = now;
					MarkSyncBoundsDirty();
				}
			}
			inside = false;
			g_visit(a, b);
		}

		// -1 still pending, 0 refused, 1 shifted.
		int SyncedRun(std::int32_t cx, std::int32_t cy, bool urgent, bool followLanding = false)
		{
			const ULONGLONG now = GetTickCount64();
			const int st = g_def.state.load();
			if (st == 2) {
				g_def.state.store(0);
				return g_def.result ? 1 : 0;
			}
			if (st == 3) {
				return -1;  // running inside the game update right now
			}
			if (st == 1) {
				if (now - g_def.since > 2000) {
					// The sync point was not reached (loading screen, menus): give the request up; it is asked again.
					int expected = 1;
					if (g_def.state.compare_exchange_strong(expected, 0)) {
						logger::Write("Re-centre: the game update's loaded-cells visit did not run for 2 s; shift (%d, %d) not made", g_def.cx, g_def.cy);
						return 0;
					}
				}
				return -1;
			}
			if (!g_syncHooked) {
				g_runUrgent = urgent;
				const bool r = Run(cx, cy, followLanding);
				g_runUrgent = false;
				return r ? 1 : 0;
			}
			g_def.cx = cx;
			g_def.cy = cy;
			g_def.urgent = urgent;
			g_def.followLanding = followLanding;
			g_def.since = now;
			g_def.state.store(1);
			return -1;
		}
	}

	// Saves are written in the original frame. A save holds every changed reference position and the runtime cells
	// with their grid coordinates, all in the live frame; written from a shifted frame, the load hung on the loading
	// screen (0.37.1-0.37.3: planet manager never started), whatever the stored ws+0x310 was. Shifting back for the
	// save gives exactly the frame the proven anchoring (0.20) wrote. The save runs at a frame boundary (the game
	// update is not running), so these two shifts run here directly, with the relaxed (urgent) checks.
	namespace
	{
		std::int32_t g_saveTx = 0, g_saveTy = 0;
		bool g_saveShifted = false;
	}

	void BeforeSave()
	{
		std::int32_t tx = 0, ty = 0;
		TotalShift(tx, ty);
		g_saveShifted = false;
		if (tx == 0 && ty == 0) {
			return;
		}
		g_runUrgent = true;
		const bool ok = ShiftBackToOriginal("saving in the original frame");
		g_runUrgent = false;
		if (ok) {
			g_saveTx = tx;
			g_saveTy = ty;
			g_saveShifted = true;
		}
		else {
			logger::Write("Re-centre: could not shift back for the save; this save is written in the shifted frame (%d, %d)", tx, ty);
		}
	}

	void OnSaveWrite()
	{
		std::int32_t tx = 0, ty = 0;
		TotalShift(tx, ty);
		if (g_saveState.load() == 1) {
			g_saveState.store(2);
		}
		(void)tx;
		(void)ty;  // 0.48.2: no warning here; SaveGameDetour logs which frame each save is written in
	}

	void AfterSave()
	{
		if (!g_saveShifted) {
			return;
		}
		g_saveShifted = false;
		g_runUrgent = true;
		const bool ok = Run(g_saveTx, g_saveTy);
		g_runUrgent = false;
		if (ok) {
			logger::Write("Re-centre: after the save, the world shifted forward again by (%d, %d)", g_saveTx, g_saveTy);
			return;
		}
		// Refused (0.38.4: "planet manager locked" right after the save, the player stayed 6.6 km out, past the physics
		// edge): an urgent re-centre onto the player's terrain window instead, retried by the normal machinery.
		logger::Write("Re-centre: after the save the forward shift (%d, %d) was refused; an urgent re-centre is requested instead", g_saveTx, g_saveTy);
		RequestCentre(g_saveTx, g_saveTy, true);
	}

	// ---- Stale player positions after a shift ------------------------------------------------------------------
	// The player's character controller and its physics proxy are not moved by hknpWorld::shiftWorld. Until the
	// teleport queued by the move routine lands (0.4-1.2 s later), the movement update (FUN_141B02B70, controller
	// position) and the body->actor sync (FUN_140D500B0, possibly clamped) write the player's old-frame position back
	// through FUN_140B4E410 (0.38.1: x -223 -> -1599 inside the same visit, the grid followed and the frame reset).
	// For 4 s after a shift, a player position near "current + shift" (the old frame) is moved by the shift into the
	// new frame before it is written. Everything else passes through unchanged.
	namespace
	{
		// FUN_140D500B0 sends each actor-owned Havok body through FUN_140D53F10 before
		// writing the result to the actor. A driver drop can originate in either the
		// body's input position or this placement function's navmesh/bounds result.
		// Capture both at that boundary, including on-foot drops, before changing the
		// engine's placement policy.
		using PlaceBody_t = void (*)(std::uintptr_t, std::uintptr_t, const float*, float*, const void*);
		PlaceBody_t g_placeBody = nullptr;
		std::atomic<ULONGLONG> g_lastPlaceDiag{ 0 };

		void PlaceBodyDetour(std::uintptr_t sync, std::uintptr_t actor, const float* bodyPos, float* placedPos, const void* bodyBounds)
		{
			g_placeBody(sync, actor, bodyPos, placedPos, bodyBounds);
			const auto player = Global(addr::kPlayerCharacterPtr);
			if (!player || actor != player || !bodyPos || !placedPos) return;
			const auto refPos = At<game::Vec3>(player, off::kRefPosition);
			const auto vehicle = EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(player);
			const float delta = std::fabs(placedPos[2] - refPos.z);
			const float altered = std::fabs(placedPos[2] - bodyPos[2]);
			if (delta < 10.0f && altered < 10.0f) return;
			const auto now = GetTickCount64();
			auto last = g_lastPlaceDiag.load();
			if (now - last < 250 || !g_lastPlaceDiag.compare_exchange_strong(last, now)) return;
			float mn[4] = {}, mx[4] = {};
			const bool bounds = SyncBounds(mn, mx);
			const auto vp = vehicle ? At<game::Vec3>(vehicle, off::kRefPosition) : game::Vec3{};
			logger::Write("Body placement: actor (%.2f, %.2f, %.2f), Havok input (%.2f, %.2f, %.2f), placed (%.2f, %.2f, %.2f), vehicle %s (%.2f, %.2f, %.2f), sync %p bounds z %.2f..%.2f%s",
				refPos.x, refPos.y, refPos.z, bodyPos[0], bodyPos[1], bodyPos[2], placedPos[0], placedPos[1], placedPos[2],
				vehicle ? "seated" : "on foot", vp.x, vp.y, vp.z, reinterpret_cast<void*>(sync),
				bounds ? mn[2] : 0.0f, bounds ? mx[2] : 0.0f, bounds ? "" : " (bounds unavailable)");
		}

		using SetPos_t = void (*)(std::uintptr_t, float*);
		SetPos_t g_setPos = nullptr;
		std::atomic<unsigned> g_staleFixed{ 0 };

		// Tracing of big player position writes, and the runaway guard (0.39.0: after a 4 km shift the body->actor sync
		// moved the player 3.4 km further every frame, to 19 km out). Shared by both position writers.
		std::atomic<unsigned> g_bigWrites{ 0 };
		constexpr ULONGLONG kGuardMs = 5000;
		constexpr double kGuardJump = 300.0;

		// Returns true when the write must be blocked (keep the current position).
		bool TraceWrite(const char* path, std::uintptr_t ref, const float* in, void* caller)
		{
			if (!ref || !in || ref != Global(addr::kPlayerCharacterPtr)) {
				return false;
			}
			const auto cur = At<game::Vec3>(ref, off::kRefPosition);
			const double jump = std::hypot(in[0] - cur.x, in[1] - cur.y);
			if (jump >= 1.0 || std::fabs(in[2] - cur.z) >= 1.0) {
				trace::Line("write", "%s: (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f) d %.2f m, caller Starfield.exe+0x%llX", path, cur.x, cur.y, cur.z, in[0], in[1], in[2],
					jump, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(caller) - Base()));
			}
			// 0.53.3 diagnosis: a write that drops the driver more than 20 m (the snap out of the rover at cell crossings).
			// Logs the call stack and the cell state once a second, so the engine path behind it can be fixed properly.
			if (in[2] < cur.z - 20.0f) {
				const auto vehicle = EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(ref);
				static ULONGLONG lastStack = 0;
				if (vehicle && GetTickCount64() - lastStack > 1000) {
					lastStack = GetTickCount64();
					void* frames[20] = {};
					const auto n = RtlCaptureStackBackTrace(0, 20, frames, nullptr);
					char stack[700] = {};
					std::size_t used = 0;
					for (USHORT i = 0; i < n && used + 40 < sizeof(stack); ++i) {
						const auto a = reinterpret_cast<std::uintptr_t>(frames[i]);
						HMODULE mod = nullptr;
						char name[64] = "?";
						if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a), &mod) && mod) {
							char full[MAX_PATH] = {};
							GetModuleFileNameA(mod, full, MAX_PATH);
							const char* b = std::strrchr(full, 92);
							std::snprintf(name, sizeof(name), "%s", b ? b + 1 : full);
						}
						used += std::snprintf(stack + used, sizeof(stack) - used, "%s%s+0x%llX", i ? " < " : "", name,
							static_cast<unsigned long long>(a - reinterpret_cast<std::uintptr_t>(mod)));
					}
					const auto tes = Global(addr::kTESPtr);
					const auto pc = At<std::uintptr_t>(ref, off::kRefParentCell), vc = At<std::uintptr_t>(vehicle, off::kRefParentCell);
					const auto pce = pc ? At<std::uintptr_t>(pc, off::kCellExterior) : 0, vce = vc ? At<std::uintptr_t>(vc, off::kCellExterior) : 0;
					const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
					logger::Write("Drop: driver write z %.1f -> %.1f at (%.1f, %.1f); vehicle at (%.1f, %.1f, %.1f); player cell (%d, %d) st %u, vehicle cell (%d, %d) st %u, by position (%d, %d); TES grid centre (%d, %d) current (%d, %d); stack %s",
						cur.z, in[2], in[0], in[1], vp.x, vp.y, vp.z, pce ? At<std::int32_t>(pce, 0) : 0, pce ? At<std::int32_t>(pce, 4) : 0, pc ? unsigned(At<std::uint8_t>(pc, off::kCellState)) : 0u,
						vce ? At<std::int32_t>(vce, 0) : 0, vce ? At<std::int32_t>(vce, 4) : 0, vc ? unsigned(At<std::uint8_t>(vc, off::kCellState)) : 0u,
						int(std::floor(in[0] / 100.0f)), int(std::floor(in[1] / 100.0f)),
						tes ? At<std::int32_t>(tes, off::kTESGridCentre) : 0, tes ? At<std::int32_t>(tes, off::kTESGridCentre + 4) : 0,
						tes ? At<std::int32_t>(tes, off::kTESCurrentCell) : 0, tes ? At<std::int32_t>(tes, off::kTESCurrentCell + 4) : 0, stack);
					navgen::LogState("drop");
				}
			}
			if (jump < 50.0) {
				return false;
			}
			const ULONGLONG now = GetTickCount64();
			const ULONGLONG since = g_shiftAt.load() ? now - g_shiftAt.load() : ~0ull;
			const bool block = since < kGuardMs && jump > kGuardJump;
			const auto n = g_bigWrites.fetch_add(1) + 1;
			if (n <= 200 || n % 50 == 0) {
				char where[96];
				std::snprintf(where, sizeof(where), "Starfield.exe+0x%llX",
					static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(caller) - Base()));
				logger::Write("Trace: %s write moves the player %.0f m: (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f), caller %s, %.1f s after a shift%s", path, jump, cur.x,
					cur.y, cur.z, in[0], in[1], in[2], where, since == ~0ull ? -1.0 : double(since) / 1000.0, block ? " -> BLOCKED (runaway guard)" : "");
			}
			return block;
		}

		void SetPosDetour(std::uintptr_t ref, float* pos)
		{
			if (TraceWrite("setter", ref, pos, _ReturnAddress()) && g_setPos) {
				// Runaway guard: keep the current position (0.39.1 also re-warped the controller here, which fed the
				// oscillation) and have the sync box rebuilt.
				MarkSyncBoundsDirty();
				return;
			}
			if (ref && pos && GetTickCount64() < g_staleUntil.load() && ref == Global(addr::kPlayerCharacterPtr)) {
				const float sx = g_shiftX.load(), sy = g_shiftY.load();
				const auto cur = At<game::Vec3>(ref, off::kRefPosition);
				const double toOld = std::hypot(pos[0] - (cur.x + sx), pos[1] - (cur.y + sy));
				const double toCur = std::hypot(pos[0] - cur.x, pos[1] - cur.y);
				const double len = std::hypot(sx, sy);
				if (toOld < 800.0 && toCur > 0.6 * len) {
					float fixed[3] = { pos[0] - sx, pos[1] - sy, pos[2] };
					const auto n = g_staleFixed.fetch_add(1) + 1;
					if (n <= 30) {
						logger::Write("Re-centre: stale player position (%.1f, %.1f) from the old frame written after a shift; moved to (%.1f, %.1f) (%u so far)",
							pos[0], pos[1], fixed[0], fixed[1], n);
					}
					g_setPos(ref, fixed);
					return;
				}
			}
			g_setPos(ref, pos);
		}
	}

	namespace
	{
		// The actor position update FUN_141B031F0 (event path via FUN_141953F60) writes +0x8C directly, bypassing
		// FUN_140B4E410: after the 0.38.4 warp it still wrote the old-frame position once (x 200 -> -2599, +0x1B033A8).
		using ActorPos_t = void (*)(std::uintptr_t, float*);
		ActorPos_t g_actorPos = nullptr;

		bool CorrectStale(std::uintptr_t ref, const float* in, float* out)
		{
			if (!ref || !in || GetTickCount64() >= g_staleUntil.load() || ref != Global(addr::kPlayerCharacterPtr)) {
				return false;
			}
			const float sx = g_shiftX.load(), sy = g_shiftY.load();
			const auto cur = At<game::Vec3>(ref, off::kRefPosition);
			const double toOld = std::hypot(in[0] - (cur.x + sx), in[1] - (cur.y + sy));
			const double toCur = std::hypot(in[0] - cur.x, in[1] - cur.y);
			if (!(toOld < 800.0 && toCur > 0.6 * std::hypot(sx, sy))) {
				return false;
			}
			out[0] = in[0] - sx;
			out[1] = in[1] - sy;
			out[2] = in[2];
			return true;
		}

		void ActorPosDetour(std::uintptr_t actor, float* pos)
		{
			if (TraceWrite("actor update", actor, pos, _ReturnAddress())) {
				return;  // runaway guard: keep the current position
			}
			float fixed[4];
			if (CorrectStale(actor, pos, fixed)) {
				const auto n = g_staleFixed.fetch_add(1) + 1;
				if (n <= 30) {
					logger::Write("Re-centre: stale player position (%.1f, %.1f) from the old frame (actor update) moved to (%.1f, %.1f) (%u so far)",
						pos[0], pos[1], fixed[0], fixed[1], n);
				}
				g_actorPos(actor, fixed);
				return;
			}
			g_actorPos(actor, pos);
		}
	}

	bool InstallStaleWriteFix()
	{
		static const std::uint8_t kPlacePrologue[16] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 };
		void* placeOriginal = nullptr;
		if (hook::Install(Base() + 0x00D53F10, kPlacePrologue, sizeof(kPlacePrologue), reinterpret_cast<void*>(&PlaceBodyDetour), &placeOriginal)) {
			g_placeBody = reinterpret_cast<PlaceBody_t>(placeOriginal);
		}
		else {
			logger::Write("Re-centre: body placement diagnostic hook failed");
		}
		static const std::uint8_t kActorPrologue[14] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC };
		void* actorOriginal = nullptr;
		if (hook::Install(Base() + addr::kActorPositionUpdate, kActorPrologue, sizeof(kActorPrologue), reinterpret_cast<void*>(&ActorPosDetour), &actorOriginal)) {
			g_actorPos = reinterpret_cast<ActorPos_t>(actorOriginal);
		}
		else {
			logger::Write("Re-centre: actor position update hook failed");
		}
		static const std::uint8_t kPrologue[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
		void* original = nullptr;
		if (!hook::Install(Base() + addr::kRefSetPosition, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&SetPosDetour), &original)) {
			logger::Write("Re-centre: position setter hook failed (stale player writes after shifts not corrected)");
			return false;
		}
		g_setPos = reinterpret_cast<SetPos_t>(original);
		logger::Write("Re-centre: stale player positions after a shift are moved into the new frame");
		return true;
	}

	// ---- Every save goes through BGSSaveLoadGame::SaveGame (FUN_141814220) --------------------------------------
	// The save-flag watch only sees requests that set save manager flags; the save itself starts in SaveGame (from the
	// save task FUN_141821B70), for autosaves and saves from the menu alike. At its entry the world is shifted back to
	// the original frame (if not already); after it, the next game update (sync point) shifts forward again.
	namespace
	{
		using SaveGame_t = void (*)(std::int64_t*, std::uint64_t, std::int64_t, std::uint64_t);
		SaveGame_t g_saveGame = nullptr;

		// What a save writes is put into the original frame for the length of SaveGame and back right after, in the same
		// call on the game-update thread (0.38.4/0.38.5 logs: SaveGame runs on that thread), so no frame is drawn in
		// between: reference positions (grid cells, persistent cell, player/anchor), cell coordinates and corners, the
		// worldspace cell map, ws+0x310 and the TES grid. Nothing else moves: no physics, 3D, terrain or planet manager.
		// (0.38.x shifted the whole world back and forward: the terrain rebuilt twice and disappeared for a while, the
		// forward shift could be refused, and the player sat past the physics edge.)
		bool SaveFrame(std::int32_t cx, std::int32_t cy)
		{
			const auto tes = Global(addr::kTESPtr);
			const auto a = CurrentAnchor();
			const auto ws = a.cell ? At<std::uintptr_t>(a.cell, off::kCellWorldspace) : 0;
			if (!tes || !a.ref || !ws) {
				return false;
			}
			const float dx = cx * off::kCellSize, dy = cy * off::kCellSize;
			ShiftContext ctx{ dx, dy };
			ctx.dataOnly = true;
			const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
			const auto count = At<std::uint32_t>(tes, off::kTESGridCount);
			for (std::uint32_t i = 0; entries && i < count; ++i) {
				const auto cell = GridCell(entries[i]);
				ShiftCellRefs(ctx, cell);
				ShiftCell(cell, cx, cy, dx, dy);
			}
			ShiftCellRefs(ctx, PersistentCell(ws));
			ShiftRef(ctx, a.ref);
			Lock(ws + off::kWsCellMapLock);
			RekeyCellMap(ws, cx, cy);
			Unlock(ws + off::kWsCellMapLock);
			std::uint64_t origin = ~0ull;
			if (PlanPlanetOrigin(ws, cx, cy, origin) == OriginPlan::Ok) {
				At<std::uint64_t>(ws, off::kWsPlanetOrigin) = origin;
				g_wsOriginWritten[ws] = origin;
			}
			ShiftTESGrid(tes, cx, cy);
			auto& total = g_totalShift[ws];
			total.x += cx;
			total.y += cy;
			logger::Write("Save: save data moved by (%d, %d) cells (%u objects); total shift now (%d, %d)", cx, cy, ctx.refs, total.x, total.y);
			return true;
		}

		// ---- Save as a landing (0.47.0) ---------------------------------------------------------------------------
		// A save is written as if the player had just landed where they stand: the landing lat/long (mgr+0x98) is the
		// player's own, the planet coordinate of grid (0, 0) (mgr+0xC8) is the block-aligned origin the engine derives
		// from that lat/long (FUN_14096EFB0, as the load's planet setup does), and the whole world is written in that
		// frame (references, cells, cell map, ws+0x310, TES grid: SaveFrame, a data-only shift). On load the game sees an
		// ordinary landing at the player's spot: the terrain starts around the player, who is near the origin and inside
		// the landing area. Everything is put back right after SaveGame, on the same thread. The 0.37-0.38 attempt
		// rewrote only the player's location record and left the rest of the save in the old frame (loads hung).
		struct LandingSave
		{
			bool active = false;
			std::int32_t cx = 0, cy = 0;          // live frame -> landing frame, in cells
			std::uint8_t oldLatLong[16]{};
			std::uint8_t oldOrigin[0x2C]{};
		};

		// The live manager already describes a loadable landing when its stored landing coordinates resolve to the
		// current origin and the player is within the load's local terrain area. In this state SaveGame needs no frame
		// conversion at all, regardless of how many earlier world-origin shifts have accumulated.
		bool LiveLandingMatches()
		{
			const auto mgr = Global(addr::kPlanetManagerPtr);
			const auto player = Global(addr::kPlayerCharacterPtr);
			const auto a = CurrentAnchor();
			if (!mgr || !player || a.ref != player || !a.cell ||
				(At<std::uint8_t>(a.cell, off::kCellFlags) & 1) || At<std::uint8_t>(mgr, off::kMgrState) != 2) {
				return false;
			}
			const auto pos = At<game::Vec3>(player, off::kRefPosition);
			if (std::fabs(pos.x) > 5000.0f || std::fabs(pos.y) > 5000.0f) {
				return false;
			}
			const auto grid = EngineFn<std::uintptr_t (*)(void*, std::uint32_t, int)>(addr::kPlanetGridFor)
				(reinterpret_cast<void*>(mgr + 0x98), At<std::uint32_t>(mgr, off::kMgrPlanetId), 1);
			if (!grid) {
				return false;
			}
			alignas(16) std::uint8_t aligned[0x60] = {};
			EngineFn<void* (*)(std::uintptr_t, void*, void*)>(addr::kAlignedOrigin)
				(grid, aligned, reinterpret_cast<void*>(mgr + 0x98));
			std::int32_t delta[2] = {};
			EngineFn<void* (*)(std::uintptr_t, std::int32_t*, const void*, const void*)>(addr::kCoordOffset)
				(grid, delta, reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), aligned);
			return delta[0] == 0 && delta[1] == 0;
		}

		bool BeginLandingSave(LandingSave& ls)
		{
			const auto mgr = Global(addr::kPlanetManagerPtr);
			const auto a = CurrentAnchor();
			if (!mgr || !a.ref || !a.cell || At<std::uint8_t>(mgr, off::kMgrState) != 2 || (At<std::uint8_t>(a.cell, off::kCellFlags) & 1)) {
				return false;  // not outdoors on a settled planet: the save is written as before
			}
			const auto ws = At<std::uintptr_t>(a.cell, off::kCellWorldspace);
			if (!ws || At<std::uint64_t>(ws, off::kWsPlanetOrigin) == ~0ull) {
				return false;
			}
			const auto pos = At<game::Vec3>(a.ref, off::kRefPosition);
			alignas(16) std::uint8_t latlong[32] = {};
			const float fp[3] = { pos.x, pos.y, pos.z };
			EngineFn<void* (*)(std::uintptr_t, void*, const float*)>(addr::kWorldToLatLong)(mgr, latlong, fp);
			const auto planet = At<std::uint32_t>(mgr, off::kMgrPlanetId);
			const auto grid = EngineFn<std::uintptr_t (*)(void*, std::uint32_t, int)>(addr::kPlanetGridFor)(latlong, planet, 1);
			if (!grid) {
				logger::Write("Save as landing: no planet grid for the player's lat/long; written in the original frame");
				return false;
			}
			alignas(16) std::uint8_t aligned[0x60] = {};
			EngineFn<void* (*)(std::uintptr_t, void*, void*)>(addr::kAlignedOrigin)(grid, aligned, latlong);
			std::int32_t shift[2] = {};
			EngineFn<void* (*)(std::uintptr_t, std::int32_t*, const void*, const void*)>(addr::kCoordOffset)(grid, shift, reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), aligned);
			// 0.57.1 save structure: the engine snaps a landing to a block-aligned origin, which can leave the player up to
			// ~48 cells from it (18:03: 41, -22), i.e. past the play-area half-width (39 cells) and 2-9 cells from the edge of
			// the 100-cell terrain source the load builds around that origin. Landing points around the player are tried;
			// the one whose aligned origin is nearest the player (same cube face) becomes the save's landing. The player's
			// own position is kept only if no neighbouring landing gives a measurably closer aligned origin.
			{
				const auto offsetOf = [&](const std::int32_t sh[2]) {
					const float x = pos.x - sh[0] * off::kCellSize, y = pos.y - sh[1] * off::kCellSize;
					return std::max(std::fabs(x), std::fabs(y)) / off::kCellSize;
				};
				const float own = offsetOf(shift);
				float best = own;
				int bestI = 0, bestJ = 0;
				alignas(16) std::uint8_t bestLL[32] = {}, bestAligned[0x60] = {};
				std::int32_t bestShift[2] = { shift[0], shift[1] };
				// Search across neighbouring aligned blocks. A ten-cell improvement rule previously
				// kept a 44-cell save when a 36-cell landing was available (Mars, 20:30:10).
				constexpr int kSteps[] = { -120, -100, -80, -60, -40, -20, 0, 20, 40, 60, 80, 100, 120 };
				for (const int i : kSteps) {
					for (const int j : kSteps) {
						if (i == 0 && j == 0) {
							continue;
						}
						alignas(16) std::uint8_t ll2[32] = {}, al2[0x60] = {};
						const float cp[3] = { pos.x + i * off::kCellSize, pos.y + j * off::kCellSize, pos.z };
						EngineFn<void* (*)(std::uintptr_t, void*, const float*)>(addr::kWorldToLatLong)(mgr, ll2, cp);
						EngineFn<void* (*)(std::uintptr_t, void*, void*)>(addr::kAlignedOrigin)(grid, al2, ll2);
						if (al2[0] != At<std::uint8_t>(mgr, off::kMgrOriginCoord)) {
							continue;  // another cube face
						}
						std::int32_t sh2[2] = {};
						EngineFn<void* (*)(std::uintptr_t, std::int32_t*, const void*, const void*)>(addr::kCoordOffset)(grid, sh2, reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), al2);
						const float o = offsetOf(sh2);
						if (o < best - 0.5f) {
							best = o;
							bestI = i;
							bestJ = j;
							std::memcpy(bestLL, ll2, sizeof(bestLL));
							std::memcpy(bestAligned, al2, sizeof(bestAligned));
							bestShift[0] = sh2[0];
							bestShift[1] = sh2[1];
						}
					}
				}
				if ((bestI || bestJ) && best < own - 0.5f) {
					logger::Write("Save as landing: landing moved %d, %d cells from the player so the saved origin is %.0f cells from the player (was %.0f)", bestI, bestJ,
						best, own);
					std::memcpy(latlong, bestLL, sizeof(bestLL));
					std::memcpy(aligned, bestAligned, sizeof(bestAligned));
					shift[0] = bestShift[0];
					shift[1] = bestShift[1];
				}
				if (best > 30.0f) {
					logger::Write("Save as landing: closest sampled aligned origin is %.0f cells from the player (native play area may be smaller); checked %u landing candidates",
						best, static_cast<unsigned>(std::size(kSteps) * std::size(kSteps)));
				}
			}
			// Check: the live origin offset by the shift must be the aligned origin, and the player must be near it.
			alignas(16) std::uint8_t moved[0x60] = {};
			std::memcpy(moved, reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), 0x2C);
			EngineFn<void (*)(std::uintptr_t, std::uintptr_t, const std::int32_t*)>(addr::kOffsetPlanetCoord)(grid, reinterpret_cast<std::uintptr_t>(moved), shift);
			std::int32_t residual[2] = {};
			EngineFn<void* (*)(std::uintptr_t, std::int32_t*, const void*, const void*)>(addr::kCoordOffset)(grid, residual, moved, aligned);
			const float nx = pos.x - shift[0] * off::kCellSize, ny = pos.y - shift[1] * off::kCellSize;
			if (residual[0] != 0 || residual[1] != 0 || std::fabs(nx) > 5000.0f || std::fabs(ny) > 5000.0f) {
				logger::Write("Save as landing: frame check failed (shift %d, %d; residual %d, %d; player would be at %.0f, %.0f); written in the original frame",
					shift[0], shift[1], residual[0], residual[1], nx, ny);
				return false;
			}
			std::uint64_t probe = 0;
			if (PlanPlanetOrigin(ws, shift[0], shift[1], probe) != OriginPlan::Ok) {
				logger::Write("Save as landing: no planet origin for the landing frame (shift %d, %d); written in the original frame", shift[0], shift[1]);
				return false;
			}
			if ((shift[0] != 0 || shift[1] != 0) && !SaveFrame(shift[0], shift[1])) {
				logger::Write("Save as landing: data shift failed; written in the original frame");
				return false;
			}
			ls.active = true;
			ls.cx = shift[0];
			ls.cy = shift[1];
			std::memcpy(ls.oldLatLong, reinterpret_cast<const void*>(mgr + 0x98), 16);
			std::memcpy(ls.oldOrigin, reinterpret_cast<const void*>(mgr + off::kMgrOriginCoord), 0x2C);
			std::memcpy(reinterpret_cast<void*>(mgr + 0x98), latlong, 16);
			std::memcpy(reinterpret_cast<void*>(mgr + off::kMgrOriginCoord), aligned, 0x2C);
			double ll[2];
			std::memcpy(ll, latlong, 16);
			logger::Write("Save as landing: landing at lat/long %.6f, %.6f; world written %d, %d cells from the live frame; player at (%.1f, %.1f, %.1f) in the save",
				ll[0], ll[1], shift[0], shift[1], nx, ny, pos.z);
			telemetry::Record("save_landing", "\"shift\":[%d,%d],\"pos\":[%.1f,%.1f,%.1f]", shift[0], shift[1], nx, ny, pos.z);
			return true;
		}

		void EndLandingSave(const LandingSave& ls)
		{
			if (!ls.active) {
				return;
			}
			const auto mgr = Global(addr::kPlanetManagerPtr);
			if (mgr) {
				std::memcpy(reinterpret_cast<void*>(mgr + 0x98), ls.oldLatLong, 16);
				std::memcpy(reinterpret_cast<void*>(mgr + off::kMgrOriginCoord), ls.oldOrigin, 0x2C);
			}
			if (ls.cx != 0 || ls.cy != 0) {
				SaveFrame(-ls.cx, -ls.cy);
			}
		}

		void SaveGameDetour(std::int64_t* a, std::uint64_t b, std::int64_t c, std::uint64_t d)
		{
			std::int32_t tx = 0, ty = 0;
			TotalShift(tx, ty);
			trace::Burst(3000, "save");
			flight::BeforeSave();
			coreapi::FireBeforeSave();
			if (LiveLandingMatches()) {
				logger::Write("Save: live landing matches the current origin; SaveGame writes the world unchanged (total shift %d, %d)", tx, ty);
				g_saveGame(a, b, c, d);
				parking::OnSave(reinterpret_cast<const char*>(d));
				flight::AfterSave();
				coreapi::FireAfterSave();
				return;
			}
			LandingSave ls;
			bool shifted = false;
			if (!(kSaveAsLanding && BeginLandingSave(ls))) {
				shifted = (tx != 0 || ty != 0) && SaveFrame(-tx, -ty);  // fallback: the original landing's frame
			}
			// 0.55.0: the landing records follow shifts; a save written in the original frame gets the original ones.
			std::uint8_t followed[0x28] = {};
			const auto mgrS = Global(addr::kPlanetManagerPtr);
			const bool swapRecords = shifted && mgrS && g_originalLanding.set && g_originalLanding.planet == At<std::uint32_t>(mgrS, off::kMgrPlanetId);
			if (swapRecords) {
				std::memcpy(followed, reinterpret_cast<const void*>(mgrS + 0x98), sizeof(followed));
				std::memcpy(reinterpret_cast<void*>(mgrS + 0x98), g_originalLanding.ll, 16);
				std::memcpy(reinterpret_cast<void*>(mgrS + 0xB0), g_originalLanding.origin, 16);
			}
			g_saveGame(a, b, c, d);
			if (swapRecords) {
				std::memcpy(reinterpret_cast<void*>(mgrS + 0x98), followed, sizeof(followed));
			}
			if (ls.active) {
				EndLandingSave(ls);
			}
			else if (shifted) {
				SaveFrame(tx, ty);
			}
			flight::AfterSave();
			coreapi::FireAfterSave();
			parking::OnSave(reinterpret_cast<const char*>(d));
			logger::Write("Save: SaveGame finished on thread %lu%s", GetCurrentThreadId(),
				ls.active ? " (written as a landing at the player)" : (shifted ? " (written in the original frame)" : ""));
			return;
		}

	}

	bool InstallSaveHook()
	{
		static const std::uint8_t kPrologue[15] = { 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10 };
		void* original = nullptr;
		if (!hook::Install(Base() + addr::kSaveGame, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&SaveGameDetour), &original)) {
			logger::Write("Save: SaveGame hook failed (only flagged saves are shifted back)");
			return false;
		}
		g_saveGame = reinterpret_cast<SaveGame_t>(original);
		logger::Write("Save: matching live landings are written unchanged; legacy frame conversion remains as a fallback (SaveGame hooked)");
		return true;
	}

	// ---- Load hang diagnostics (R20) ---------------------------------------------------------------------------
	// Loading a save made away from the original landing leaves the planet manager in state 0 (no terrain). The
	// research points at FUN_14096FB40 returning an empty region when the saved packed origin does not belong to the
	// selected planet grid (grid +0x54). Logged: every planet manager setup (arguments, state before/after, decoded
	// origin) and every decode made inside it (grid, grid +0x54, packed id, output).
	namespace
	{
		using PmInit_t = void* (*)(std::uintptr_t, int*, std::uint32_t, std::uint64_t*);
		using Decode_t = std::uint8_t* (*)(std::uintptr_t, std::uint8_t*, std::uint64_t);
		PmInit_t g_pmInit = nullptr;
		Decode_t g_decode = nullptr;
		thread_local bool t_inPmInit = false;

		std::uint8_t* DecodeDetour(std::uintptr_t grid, std::uint8_t* out, std::uint64_t packed)
		{
			auto* r = g_decode(grid, out, packed);
			if (t_inPmInit) {
				bool empty = true;
				for (int i = 0; i < 0x2C; ++i) {
					empty = empty && out[i] == 0;
				}
				logger::Write("Load diag: decode packed %016llX with grid %p (grid+0x54 %08X, packed planet word %08X): face %u, %s",
					static_cast<unsigned long long>(packed), reinterpret_cast<void*>(grid), grid ? *reinterpret_cast<std::uint32_t*>(grid + 0x54) : 0u,
					static_cast<unsigned>(packed >> 41), unsigned(out[0]), empty ? "EMPTY OUTPUT" : "decoded");
			}
			return r;
		}

		void* PmInitDetour(std::uintptr_t mgr, int* desc, std::uint32_t flag, std::uint64_t* origin)
		{
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - Base();
			if (caller == 0x78AFB8) {
				g_nativeSaveLoadSerial.fetch_add(1, std::memory_order_release);
			}
			// The origin is the landing's latitude/longitude (two doubles, FUN_14078AF30 copies 16 bytes from the save).
			double lat = 0, lon = 0;
			if (origin) {
				std::memcpy(&lat, origin, 8);
				std::memcpy(&lon, origin + 1, 8);
			}
			logger::Write("Load diag: planet manager setup: state %u, flag %u, landing lat/long %s %.6f, %.6f, desc %p, called from Starfield.exe+0x%llX",
				unsigned(At<std::uint8_t>(mgr, off::kMgrState)), flag, origin ? "given" : "none", lat, lon, reinterpret_cast<void*>(desc),
				static_cast<unsigned long long>(caller));
			hangwatch::NoteLoadThread();
			t_inPmInit = true;
			auto* r = g_pmInit(mgr, desc, flag, origin);
			t_inPmInit = false;
			const auto* c = reinterpret_cast<const std::uint8_t*>(mgr + off::kMgrOriginCoord);
			logger::Write("Load diag: planet manager setup done: state %u, mgr+0xC8 face %u, %08X %08X %08X %08X", unsigned(At<std::uint8_t>(mgr, off::kMgrState)),
				unsigned(c[0]), *reinterpret_cast<const std::uint32_t*>(c + 4), *reinterpret_cast<const std::uint32_t*>(c + 8),
				*reinterpret_cast<const std::uint32_t*>(c + 0x14), *reinterpret_cast<const std::uint32_t*>(c + 0x24));
			return r;
		}
	}

	bool InstallLoadDiagnostics()
	{
		static const std::uint8_t kInit[14] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
		static const std::uint8_t kDecode[16] = { 0x40, 0x53, 0x55, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x58, 0xC6, 0x02, 0x00, 0x45, 0x33, 0xFF };
		void* a = nullptr;
		void* b = nullptr;
		const bool okA = hook::Install(Base() + addr::kPlanetManagerInit, kInit, sizeof(kInit), reinterpret_cast<void*>(&PmInitDetour), &a);
		const bool okB = hook::Install(Base() + addr::kPlanetCoordDecode, kDecode, sizeof(kDecode), reinterpret_cast<void*>(&DecodeDetour), &b);
		g_pmInit = reinterpret_cast<PmInit_t>(a);
		g_decode = reinterpret_cast<Decode_t>(b);
		logger::Write("Load diag: planet manager setup %s, origin decode %s", okA ? "hooked" : "NOT hooked", okB ? "hooked" : "NOT hooked");
		return okA && okB;
	}

	bool InstallSyncPoint()
	{
		static const std::uint8_t kPrologue[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18 };
		void* original = nullptr;
		if (!hook::Install(Base() + addr::kLoadedCellsVisit, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&VisitDetour), &original)) {
			logger::Write("Re-centre: sync point hook failed; shifts run from the frame task");
			return false;
		}
		g_visit = reinterpret_cast<Visit_t>(original);
		g_syncHooked = true;
		logger::Write("Re-centre: shifts run inside the game update (loaded-cells visit)");
		return true;
	}

	namespace
	{
		struct Request
		{
			bool active = false;
			std::int32_t cx = 0, cy = 0;
			bool urgent = false;
			ULONGLONG since = 0;
		} g_request;
		std::atomic<bool> g_pending{ false };
	}

	bool Pending() { return g_pending.load(); }

	void ClearRequest()
	{
		if (g_request.active) {
			logger::Write("Re-centre: waiting request (%d, %d cells) dropped by a frame reset", g_request.cx, g_request.cy);
		}
		g_request.active = false;
		g_pending = false;
	}

	namespace
	{
		// Terrain guard: the terrain block generator (FUN_14098a330, worker jobs) crashed when a shift landed while it was
		// still rebuilding blocks from the previous move (0.32.0 +0x98ACDB, 0.33.0 +0x98EB3D). No shift until the terrain
		// window has not moved for kTerrainSettleMs and the last shift is kShiftSpacingMs old.
		// 0.55.0 planned moves: a 2 s still terrain window (8 s was never reached while moving) and 10 s between moves;
		// the 750 ms job-quiet rule and the terrain guard stay.
		constexpr ULONGLONG kTerrainSettleMs = 2000;
		constexpr ULONGLONG kShiftSpacingMs = 10000;
		ULONGLONG g_lastTerrainMove = 0;
		std::int32_t g_lastGridX = 0x7FFFFFFF, g_lastGridY = 0x7FFFFFFF;
		ULONGLONG g_lastShift = 0;

		void WatchTerrain(std::uintptr_t mgr, ULONGLONG now)
		{
			if (!mgr) {
				return;
			}
			const auto gx = At<std::int32_t>(mgr, off::kMgrGridCentre), gy = At<std::int32_t>(mgr, off::kMgrGridCentre + 4);
			if (gx != g_lastGridX || gy != g_lastGridY) {
				g_lastGridX = gx;
				g_lastGridY = gy;
				g_lastTerrainMove = now;
			}
		}

		// Terrain building must be quiet: a shift while terrain block / page stitch jobs run makes them drop their
		// blocks (0.35.5: 6,000 blocks skipped by the guard, holes the player fell through). The guard hooks see every
		// one of those jobs, so their last call time is the engine's own activity signal.
		constexpr ULONGLONG kTerrainQuietMs = 750;  // 1.5 s was never reached while driving (0.36.0: no re-centre at 50-100 m/s)

		bool TerrainSettled(ULONGLONG now, ULONGLONG* waitMs = nullptr, bool urgent = false)
		{
			const ULONGLONG a = g_lastTerrainMove ? now - g_lastTerrainMove : ~0ull;
			const ULONGLONG b = g_lastShift ? now - g_lastShift : ~0ull;
			const auto act = terrainguard::LastActivityMs();
			const ULONGLONG c = act ? now - act : ~0ull;
			if (waitMs) {
				const ULONGLONG wa = a < kTerrainSettleMs ? kTerrainSettleMs - a : 0, wb = b < kShiftSpacingMs ? kShiftSpacingMs - b : 0,
				                wc = c < kTerrainQuietMs ? kTerrainQuietMs - c : 0;
				*waitMs = std::max(std::max(wa, wb), wc);
			}
			// Urgent (physics deadline): neither the still terrain window (moves every 2-4 s when driving fast) nor the
			// quiet moment is waited for; the terrain guard covers the jobs.
			return (urgent || a >= kTerrainSettleMs) && b >= kShiftSpacingMs && (urgent || c >= kTerrainQuietMs);
		}
	}

	namespace
	{
		std::atomic<unsigned long long> g_lastRunMs{ 0 };
	}
	unsigned long long LastRunMs() { return g_lastRunMs.load(); }

	int PendingCellLoads()
	{
		// BSResource2::DB<LoadedCellDB>: queue heads at (db+0x40)+0xC8 -> array of 0x40-byte queues, count at db+0x12C;
		// a non-empty head is pending work (FUN_1427d4900 takes and re-inserts the lists). Read only.
		const auto db = Global(addr::kLoadedCellDBPtr);
		if (!db) {
			return 0;
		}
		const auto heads = At<std::uintptr_t>(db, 0x108);
		const auto count = At<std::uint32_t>(db, 0x12C);
		if (!heads || count == 0 || count > 64) {
			return 0;
		}
		int pending = 0;
		const auto now = GetTickCount64();
		static std::atomic<std::uintptr_t> lastHead[64]{};
		static std::atomic<ULONGLONG> headSince[64]{}, queueSince[64]{};
		static std::atomic<bool> dumped[64]{};
		for (std::uint32_t i = 0; i < count; ++i) {
			const auto head = At<std::uintptr_t>(heads, i * 0x40);
			if (head != 0 && head != 1) {
				++pending;
				if (!queueSince[i].load()) queueSince[i].store(now);
				if (lastHead[i].exchange(head) != head) {
					headSince[i].store(now);
					dumped[i].store(false);
				}
				const auto active = terrainguard::LastActivityMs();
				const auto headAt = headSince[i].load(), queueAt = queueSince[i].load();
				if (headAt && now >= headAt && now - headAt > 10000 &&
					active && now - active > 3000 && !dumped[i].exchange(true)) {
					const auto q = heads + i * 0x40;
					std::uint64_t words[8]{};
					for (unsigned j = 0; j < 8; ++j) {
						const auto v = game::SafeRead<std::uint64_t>(q + j * 8);
						words[j] = v ? *v : 0;
					}
					logger::Write("Stuck load: queue %u head %p unchanged %.1f s, nonempty %.1f s, terrain idle %.1f s; raw %016llX %016llX %016llX %016llX %016llX %016llX %016llX %016llX",
						i, reinterpret_cast<void*>(head), double(now - headAt) / 1000.0, double(queueAt && now >= queueAt ? now - queueAt : 0) / 1000.0,
						double(now - active) / 1000.0, static_cast<unsigned long long>(words[0]), static_cast<unsigned long long>(words[1]),
						static_cast<unsigned long long>(words[2]), static_cast<unsigned long long>(words[3]),
						static_cast<unsigned long long>(words[4]), static_cast<unsigned long long>(words[5]),
						static_cast<unsigned long long>(words[6]), static_cast<unsigned long long>(words[7]));
					telemetry::Record("loader_head_stuck", "\"queue\":%u,\"head\":\"%p\",\"head_ms\":%llu,\"nonempty_ms\":%llu,\"terrain_idle_ms\":%llu",
						i, reinterpret_cast<void*>(head), static_cast<unsigned long long>(now - headAt),
						static_cast<unsigned long long>(queueAt && now >= queueAt ? now - queueAt : 0), static_cast<unsigned long long>(now - active));
				}
			}
			else {
				lastHead[i].store(0);
				headSince[i].store(0);
				queueSince[i].store(0);
				dumped[i].store(false);
			}
		}
		// LiveCellLoadJobs() is not added: 1-2 such nodes stay alive for minutes even standing still (0.36.1/0.36.2), which
		// starved every re-centre. A load that finishes across a shift is repaired by FixStaleCells, and one that arrives
		// without a worldspace by the cell guard.
		return pending;
	}

	int LiveCellLoadJobs()
	{
		// Queues empty is not enough: a LoadCell job already taken by a worker (waiting on IO) finished 1.6 s after a
		// shift with the old coordinates and crashed resolving its worldspace (+0x41EFD6, 0.35.4). The task graph counts
		// live nodes per (priority * 7 + type) at stats+0x508 (+1 in FUN_140420410 when a LoadCell node is made, -1 in
		// FUN_142423940 when it is done); type 6 = LoadCell. Read only, guarded.
		const auto stats = Global(addr::kTaskNodeStatsPtr);
		if (!stats) {
			return 0;
		}
		const auto arr = game::SafeRead<std::uintptr_t>(stats + 0x508);
		if (!arr || !*arr) {
			return 0;
		}
		int live = 0;
		// Priorities 0..5: the grid update creates LoadCell nodes with priority 0 or 5 (FUN_140578B80 -> FUN_140575C10,
		// `-c & 5`); 0.35.5 summed 0..4 and missed the grid's own loads (worker R16).
		for (int p = 0; p < 6; ++p) {
			const auto v = game::SafeRead<std::int32_t>(*arr + std::uintptr_t(p * 7 + 6) * 4);
			if (v && *v > 0 && *v < 100000) {
				live += *v;
			}
		}
		return live;
	}

	void RequestCentre(std::int32_t cx, std::int32_t cy, bool urgent)
	{
		// 0.52.0: live re-centres off. The world is rebuilt by the engine instead (re-anchor: quicksave as a landing at
		// the player + quickload, reanchor.cpp); nothing in the live world is moved. Kept in the code for comparison.
		if (!kLiveRecentre) {
			static bool told = false;
			if (!told && (cx != 0 || cy != 0)) {
				told = true;
				logger::Write("Re-centre: live re-centres are off (0.52.0); request (%d, %d cells)%s ignored", cx, cy, urgent ? " [urgent]" : "");
			}
			g_request.active = false;
			g_pending = false;
			return;
		}
		if (cx == 0 && cy == 0) {
			g_request.active = false;
			g_pending = false;
			return;
		}
		const bool changed = !g_request.active || g_request.cx != cx || g_request.cy != cy || (urgent && !g_request.urgent);
		g_request.cx = cx;
		g_request.cy = cy;
		g_request.urgent = g_request.urgent || urgent;
		if (!g_request.active) {
			g_request.since = GetTickCount64();
		}
		g_request.active = true;
		g_pending = true;
		if (changed) {
			logger::Write("Re-centre: requested onto the zone centre (%d, %d cells)%s", cx, cy, urgent ? " [urgent: physics edge ahead]" : "");
		}
	}

	void Tick()
	{
		static ULONGLONG settledSince = 0;
		static ULONGLONG lastWaitLog = 0;
		static game::Vec3 lastPos{};
		static LARGE_INTEGER lastQpc{};
		const ULONGLONG now = GetTickCount64();

		const auto s = game::ReadPlanetState();
		const auto tes = Global(addr::kTESPtr);
		const auto playerCell = s.havePlayer ? At<std::uintptr_t>(Global(addr::kPlayerCharacterPtr), off::kRefParentCell) : 0;
		LARGE_INTEGER qpc, freq;
		QueryPerformanceCounter(&qpc);
		QueryPerformanceFrequency(&freq);
		const double dt = lastQpc.QuadPart ? double(qpc.QuadPart - lastQpc.QuadPart) / double(freq.QuadPart) : 0.0;
		lastQpc = qpc;
		const double moved = std::hypot(s.playerPos.x - lastPos.x, s.playerPos.y - lastPos.y);
		const bool jumped = moved > 200.0;
		static double speed = 0.0;  // smoothed over ~0.5 s (positions update in steps, raw per-frame speed spiked to 300 m/s)
		if (dt > 1e-4 && !jumped) {
			speed += (moved / dt - speed) * std::min(1.0, dt / 0.5);
		}
		lastPos = s.playerPos;
		{
			static ULONGLONG lastHealth = 0;
			if (now - lastHealth >= 500) {
				lastHealth = now;
				const auto terrainAt = terrainguard::LastActivityMs();
				telemetry::Record("stream_health", "\"speed_mps\":%.1f,\"mgr\":%u,\"shifting\":%u,\"cell_queues\":%d,\"cell_jobs\":%d,\"collision_pending\":%u,\"terrain_idle_ms\":%lld,\"shift_pending\":%d,\"urgent\":%d",
					speed, s.haveManager ? unsigned(s.managerState) : 0u,
					s.haveManager ? unsigned(At<std::uint8_t>(Global(addr::kPlanetManagerPtr), off::kMgrShifting)) : 0u,
					PendingCellLoads(), LiveCellLoadJobs(), terrainguard::PendingCount(),
					terrainAt ? static_cast<long long>(now - terrainAt) : -1ll, g_request.active ? 1 : 0, g_request.urgent ? 1 : 0);
			}
		}

		WatchTerrain(Global(addr::kPlanetManagerPtr), now);
		g_pending = g_request.active;
		// Only settle the grid when the player is actually slow enough to make a normal shift. Suppressing lead for the
		// entire request while driving starved forward loading for minutes (21:09-21:15), even though the speed gate
		// could not allow the shift. Urgent shifts retain forward loading until their own safety gate runs.
		const bool suppressLead = g_request.active && !g_request.urgent && speed < kRecentreMaxSpeed;
		prefetch::SuppressLead(suppressLead);
		if (!g_request.active) {
			settledSince = 0;
			return;
		}
		// Safe moment: every loaded cell attached (nothing queued under the old coordinates: an F8 switch during cell
		// loads crashed a cell-load job); normally also below 20 m/s (the prefetch is not leading the grid) for a moment.
		const int loads = PendingCellLoads();
		const bool common = s.haveManager && s.managerState == 2 && tes && playerCell && At<std::uintptr_t>(tes, off::kTESInteriorCell) == 0 && !jumped;
		// Normal: every loaded cell attached and the loader idle. Urgent (physics edge ahead): while driving something is
		// always loading, and waiting for an idle loader kept the player outside the physics box for 40 s (0.36.2); the
		// player's own cell must be attached, cells loading across the shift are repaired by FixStaleCells.
		const bool cellsReady = common && (g_request.urgent ? At<std::uint8_t>(playerCell, off::kCellState) == 10
		                                                    : (loads == 0 && WorldIsSettled(tes, playerCell)));
		const bool calm = cellsReady && (g_request.urgent || speed < kRecentreMaxSpeed) && TerrainSettled(now, nullptr, g_request.urgent);
		const auto recordGate = [&](const char* reason) {
			static ULONGLONG lastGateRecord = 0;
			if (now - lastGateRecord < 1000) return;
			lastGateRecord = now;
			const auto terrainAt = terrainguard::LastActivityMs();
			telemetry::Record("shift_gate", "\"reason\":\"%s\",\"age_ms\":%llu,\"urgent\":%d,\"speed_mps\":%.1f,\"cells_ready\":%d,\"cell_queues\":%d,\"cell_jobs\":%d,\"collision_pending\":%u,\"terrain_idle_ms\":%lld,\"lead_suppressed\":%d",
				reason, static_cast<unsigned long long>(now - g_request.since), g_request.urgent ? 1 : 0, speed,
				cellsReady ? 1 : 0, loads, LiveCellLoadJobs(), terrainguard::PendingCount(),
				terrainAt ? static_cast<long long>(now - terrainAt) : -1ll, suppressLead ? 1 : 0);
		};
		if (!calm) {
			recordGate("not_calm");
			settledSince = 0;
			if (now - lastWaitLog > 5000) {
				lastWaitLog = now;
				const auto act = terrainguard::LastActivityMs();
				logger::Write("Re-centre: waiting (%d, %d cells%s): speed %.1f m/s, cells %s, %d cell loads busy, terrain jobs %.1f s ago", g_request.cx,
					g_request.cy, g_request.urgent ? ", urgent" : "", speed, cellsReady ? "attached" : "loading", loads,
					act ? double(now - act) / 1000.0 : -1.0);
			}
			return;
		}
		if (settledSince == 0) {
			settledSince = now;
		}
		if (!g_request.urgent && now - settledSince < kRecentreSettleMs) {
			recordGate("settle_pause");
			return;
		}
		// Never right after the content post-steps (overlay / texture / navmesh rebuild hooks): a shift next to them left
		// new cells never ready (0.32.1).
		if (!g_request.urgent && content::LastPostSteps() && now - content::LastPostSteps() < 20000) {
			recordGate("content_poststeps");
			return;
		}
		// Re-centres are spaced out (a handoff right after one would otherwise shift the world again within seconds).
		static ULONGLONG lastRun = 0;
		if (!g_request.urgent && lastRun && now - lastRun < kRecentreSpacingMs) {
			recordGate("shift_spacing");
			return;
		}
		recordGate("ready_to_run");
		settledSince = 0;
		// Prefer the block-aligned origin the load path derives from the player's latitude/longitude. This makes the
		// live landing and frame agree. Keep the terrain-window target as a fallback when that target is unavailable or
		// too far from the current terrain window; the earlier multi-kilometre zone-centre shift crashed terrain jobs.
		std::int32_t cx = 0, cy = 0;
		bool followLanding = false;
		if (const auto mgr = Global(addr::kPlanetManagerPtr)) {
			cx = At<std::int32_t>(mgr, off::kMgrGridCentre);
			cy = At<std::int32_t>(mgr, off::kMgrGridCentre + 4);
			LandingTarget landing;
			const bool planned = kEngineTerrainShift && PlanLandingTarget(mgr, landing);
			if (planned && kLiveLandingFollow) {
				cx = landing.cx;
				cy = landing.cy;
				followLanding = true;
			}
			else {
				static ULONGLONG lastRefusalLog = 0;
				if (now - lastRefusalLog > 5000) {
					lastRefusalLog = now;
					logger::Write("Re-centre: landing target %s (%s); shifting onto the terrain window centre instead", planned ? "not followed" : "refused",
						planned ? "live landing follow off since 0.51.4: it freed terrain pages mid-build and crashed; saves land at the player" :
						(kEngineTerrainShift ? g_landingRefusal : "engine terrain shift off"));
				}
				// On another cube face the frames differ by a rotation: an urgent re-centre becomes a landing there.
				// On another cube face the frames differ by a rotation: an urgent re-centre becomes a landing there.
				if (!planned && g_request.urgent && std::strstr(g_landingRefusal, "cube face")) {
					landing::RequestLandHere("crossing onto another cube face");
				}
			}
		}
		if (cx == 0 && cy == 0) {
			logger::Write("Re-centre: zone (%d, %d cells) requested, but the world is already centred on the player's terrain window; nothing to shift",
				g_request.cx, g_request.cy);
			g_request = Request{};
			g_pending = false;
			return;
		}
		static ULONGLONG lastTargetLog = 0;
		if (now - lastTargetLog > 2000) {
			lastTargetLog = now;
			logger::Write("Re-centre: zone (%d, %d cells) requested; shifting onto %s (%d, %d)", g_request.cx, g_request.cy,
				followLanding ? "the player's aligned landing origin" : "the terrain window centre (fallback)", cx, cy);
		}
		const bool urgent = g_request.urgent;
		const ULONGLONG since = g_request.since;
		const int r = SyncedRun(cx, cy, urgent, followLanding);
		if (r < 0) {
			return;  // runs inside the game update; the answer comes next frame
		}
		const bool ran = r == 1;
		if (ran) {
			lastRun = now;
			g_lastShift = now;
			g_lastRunMs = now;
			g_request = Request{};
			g_pending = false;
			logger::Write("Re-centre: world centred on the zone (%d, %d cells)%s, %.1f s after the request", cx, cy, urgent ? " [urgent]" : "",
				double(now - since) / 1000.0);
			notify::Log("World centred on the zone");
		}
	}

	// ---- Measured re-centre test (F10) ----------------------------------------------------------------------
	// The planet position (latitude/longitude through the game's own world -> planet conversion, and height) is what a
	// re-centre must never change; the world position is only a view. Snapshot anchors, shift, re-measure 1 s later:
	//   object moved by exactly the shift; still converts to the same lat/long; its 3D still with it;
	//   cell corner and coordinates moved by the shift; the player's lat/long unchanged.
	// Any failure: roll the shift back and log which category failed. Anchors are matched again by form id after
	// the shift (no stored pointers are read later).
	namespace
	{
		struct RefAnchor
		{
			std::uint32_t formId = 0;
			char type = 0;
			bool authoredCell = false;
			float px = 0, py = 0, pz = 0;
			double lat = 0, lon = 0;
			bool hasNode = false;
			float nox = 0, noy = 0, noz = 0;  // 3D world position minus the reference position
			float box = 0, boy = 0, boz = 0;  // 3D world bound centre (culling) minus the reference position
		};
		struct CellAnchor
		{
			std::uint32_t formId = 0;
			bool authored = false;
			std::int32_t x = 0, y = 0;
			float cx = 0, cy = 0;
			double lat = 0, lon = 0;
		};
		struct FrameCheck
		{
			int stage = 0;  // 0 idle, 1 shifted (waiting to measure)
			ULONGLONG since = 0;
			std::int32_t cx = 0, cy = 0;
			double playerLat = 0, playerLon = 0;
			std::unordered_map<std::uint32_t, RefAnchor> refs;
			std::unordered_map<std::uint32_t, CellAnchor> cells;
			bool keyDown = false;
		} g_fc;

		void PlanetLatLong(std::uintptr_t mgr, float x, float y, float z, double& lat, double& lon)
		{
			double out[2] = {};
			const float p[3] = { x, y, z };
			EngineFn<void* (*)(std::uintptr_t, double*, const float*)>(addr::kWorldToLatLong)(mgr, out, p);
			lon = out[0];
			lat = out[1];
		}

		std::uintptr_t RefNode(std::uintptr_t ref)
		{
			const auto loaded = At<std::uintptr_t>(ref, 0xB8);
			return loaded ? At<std::uintptr_t>(loaded, 0x08) : 0;
		}

		// Every loaded grid cell and the worldspace's persistent cell, with their references (up to a cap).
		void CollectWorld(std::uintptr_t mgr, std::unordered_map<std::uint32_t, RefAnchor>& refs, std::unordered_map<std::uint32_t, CellAnchor>& cells,
			const std::unordered_map<std::uint32_t, RefAnchor>* only = nullptr)
		{
			const auto tes = Global(addr::kTESPtr);
			const auto a = CurrentAnchor();
			if (!tes || !a.cell) {
				return;
			}
			const auto ws = At<std::uintptr_t>(a.cell, off::kCellWorldspace);
			std::vector<std::uintptr_t> cellList;
			const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
			const auto count = At<std::uint32_t>(tes, off::kTESGridCount);
			for (std::uint32_t i = 0; entries && i < count; ++i) {
				if (const auto c = GridCell(entries[i])) {
					cellList.push_back(c);
				}
			}
			const auto persistent = ws ? PersistentCell(ws) : 0;
			if (persistent) {
				cellList.push_back(persistent);
			}
			constexpr std::size_t kMaxRefs = 3000;
			for (const auto cell : cellList) {
				const auto cellId = At<std::uint32_t>(cell, 0x28);
				const bool authored = (cellId >> 24) != 0xFF;
				if (cell != persistent && At<std::uintptr_t>(cell, off::kCellExterior)) {
					const auto ext = At<std::uintptr_t>(cell, off::kCellExterior);
					CellAnchor ca;
					ca.formId = cellId;
					ca.authored = authored;
					ca.x = At<std::int32_t>(ext, 0x00);
					ca.y = At<std::int32_t>(ext, 0x04);
					ca.cx = At<float>(ext, 0x24);
					ca.cy = At<float>(ext, 0x28);
					PlanetLatLong(mgr, ca.cx, ca.cy, 0.0f, ca.lat, ca.lon);
					cells[cellId] = ca;
				}
				std::vector<std::uintptr_t> list;
				CollectRefs cb{ CollectRefs::kVtbl, &list };
				EngineFn<int (*)(std::uintptr_t, CollectRefs*, char)>(addr::kForEachRefInCell)(cell, &cb, 0);
				for (const auto r : list) {
					if (!only && refs.size() >= kMaxRefs) {
						break;
					}
					const auto formId = At<std::uint32_t>(r, 0x28);
					if (formId == 0x14 || (only && !only->count(formId))) {
						continue;  // the player has its own check (it may walk during the measuring second)
					}
					RefAnchor ra;
					ra.formId = formId;
					const auto base = At<std::uintptr_t>(r, 0x98);
					ra.type = base ? At<char>(base, 0x2E) : 0;
					ra.authoredCell = authored;
					const auto p = At<game::Vec3>(r, off::kRefPosition);
					ra.px = p.x;
					ra.py = p.y;
					ra.pz = p.z;
					PlanetLatLong(mgr, p.x, p.y, p.z, ra.lat, ra.lon);
					if (const auto node = RefNode(r)) {
						ra.hasNode = true;
						ra.nox = At<float>(node, 0xB0) - p.x;
						ra.noy = At<float>(node, 0xB4) - p.y;
						ra.noz = At<float>(node, 0xB8) - p.z;
						ra.box = At<float>(node, 0x100) - p.x;
						ra.boy = At<float>(node, 0x104) - p.y;
						ra.boz = At<float>(node, 0x108) - p.z;
					}
					refs[ra.formId] = ra;
				}
			}
		}

		// ---- Scene walk: every 3D node under the containers that hold the loaded objects --------------------------
		// The object check above follows each reference's own 3D. This walk also covers what hangs beside them (combined
		// meshes, terrain, LOD, anything without a reference) and the hidden flag, to explain "places invisible".
		struct NodeSnap { float x, y, z, bx, by, bz; bool hidden; };
		struct SceneSnap
		{
			std::unordered_map<std::uintptr_t, NodeSnap> nodes;  // nodes outside any reference's 3D
			std::unordered_map<std::uintptr_t, NodeSnap> roots;  // reference 3D roots
			std::size_t containers = 0;
		};

		// Class name from the MSVC RTTI behind the vtable, or "?" (SEH-guarded: no C++ objects in here).
		const char* RttiName(std::uintptr_t obj)
		{
			__try {
				const auto vtbl = *reinterpret_cast<std::uintptr_t*>(obj);
				const auto col = *reinterpret_cast<std::uintptr_t*>(vtbl - 8);
				const auto td = game::Base() + *reinterpret_cast<std::uint32_t*>(col + 0x0C);
				return reinterpret_cast<const char*>(td + 0x10);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return "?";
			}
		}

		NodeSnap Snap(std::uintptr_t n)
		{
			return { At<float>(n, 0xB0), At<float>(n, 0xB4), At<float>(n, 0xB8), At<float>(n, 0x100), At<float>(n, 0x104), At<float>(n, 0x108),
				(At<std::uint64_t>(n, 0x118) & 1) != 0 };
		}

		void CollectScene(SceneSnap& out)
		{
			const auto tes = Global(addr::kTESPtr);
			const auto a = CurrentAnchor();
			if (!tes || !a.cell) {
				return;
			}
			std::vector<std::uintptr_t> cellList;
			const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
			const auto count = At<std::uint32_t>(tes, off::kTESGridCount);
			for (std::uint32_t i = 0; entries && i < count; ++i) {
				if (const auto c = GridCell(entries[i])) {
					cellList.push_back(c);
				}
			}
			if (const auto ws = At<std::uintptr_t>(a.cell, off::kCellWorldspace)) {
				if (const auto persistent = PersistentCell(ws)) {
					cellList.push_back(persistent);
				}
			}
			std::unordered_set<std::uintptr_t> tops;
			for (const auto cell : cellList) {
				std::vector<std::uintptr_t> list;
				CollectRefs cb{ CollectRefs::kVtbl, &list };
				EngineFn<int (*)(std::uintptr_t, CollectRefs*, char)>(addr::kForEachRefInCell)(cell, &cb, 0);
				for (const auto r : list) {
					const auto node = RefNode(r);
					if (!node) {
						continue;
					}
					out.roots[node] = Snap(node);
					auto top = node;
					for (int d = 0; d < 32; ++d) {
						const auto parent = At<std::uintptr_t>(top, off::kNodeParent);
						if (!parent) {
							break;
						}
						top = parent;
					}
					if (top != node) {
						tops.insert(top);
					}
				}
			}
			out.containers = tops.size();
			constexpr std::size_t kMaxNodes = 400000;
			std::vector<std::pair<std::uintptr_t, int>> stack;
			for (const auto t : tops) {
				stack.push_back({ t, 0 });
			}
			while (!stack.empty() && out.nodes.size() < kMaxNodes) {
				const auto [n, depth] = stack.back();
				stack.pop_back();
				if (!n || out.roots.count(n) || out.nodes.count(n)) {
					continue;  // a reference's own 3D is measured by the object check
				}
				out.nodes[n] = Snap(n);
				if (depth > 64) {
					continue;
				}
				auto** vtbl = *reinterpret_cast<void***>(n);
				const auto asNode = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(vtbl[off::kVtblIsNode])(n);
				if (!asNode) {
					continue;
				}
				const auto children = At<std::uintptr_t*>(asNode, off::kNodeChildren);
				const auto cc = At<std::uint16_t>(asNode, off::kNodeChildCount);
				for (std::uint16_t i = 0; children && i < cc; ++i) {
					stack.push_back({ children[i], depth + 1 });
				}
			}
		}

		SceneSnap g_scene;

		// Compares the scene walk before and after a shift by (dx, dy) m and logs what did not follow.
		void ReportScene(const SceneSnap& before, float dx, float dy)
		{
			SceneSnap after;
			CollectScene(after);
			struct Tally { int moved = 0, stayed = 0, other = 0, hiddenNow = 0; };
			std::map<std::string, Tally> byClass;
			int moved = 0, stayed = 0, other = 0, matched = 0, hiddenBefore = 0, hiddenAfter = 0, boundStayed = 0;
			const auto classify = [&](float bx, float by, float ax, float ay) {
				const double toMoved = std::hypot(ax - (bx - dx), ay - (by - dy));
				const double toStayed = std::hypot(ax - bx, ay - by);
				return toMoved < 0.5 ? 0 : toStayed < 0.5 ? 1 : 2;
			};
			for (const auto& [n, b] : before.nodes) {
				const auto it = after.nodes.find(n);
				if (it == after.nodes.end()) {
					continue;
				}
				const auto& a = it->second;
				++matched;
				const int c = classify(b.x, b.y, a.x, a.y);
				auto& t = byClass[RttiName(n)];
				if (c == 0) { ++moved; ++t.moved; }
				else if (c == 1) { ++stayed; ++t.stayed; }
				else { ++other; ++t.other; }
				if (c == 0 && classify(b.bx, b.by, a.bx, a.by) == 1) {
					++boundStayed;
				}
				hiddenBefore += b.hidden ? 1 : 0;
				hiddenAfter += a.hidden ? 1 : 0;
				t.hiddenNow += a.hidden ? 1 : 0;
			}
			int rootsMatched = 0, rootsHiddenBefore = 0, rootsHiddenAfter = 0;
			for (const auto& [n, b] : before.roots) {
				const auto it = after.roots.find(n);
				if (it == after.roots.end()) {
					continue;
				}
				++rootsMatched;
				rootsHiddenBefore += b.hidden ? 1 : 0;
				rootsHiddenAfter += it->second.hidden ? 1 : 0;
			}
			logger::Write("Frame check: scene walk: %zu containers; %d object roots (hidden %d -> %d); %d other nodes matched of %zu: moved %d, "
				"LEFT BEHIND %d, elsewhere %d, bound left behind %d, hidden %d -> %d",
				after.containers, rootsMatched, rootsHiddenBefore, rootsHiddenAfter, matched, before.nodes.size(), moved, stayed, other, boundStayed,
				hiddenBefore, hiddenAfter);
			for (const auto& [name, t] : byClass) {
				if (t.stayed || t.other || t.hiddenNow) {
					logger::Write("Frame check:   class %s: moved %d, left behind %d, elsewhere %d, hidden %d", name.c_str(), t.moved, t.stayed, t.other, t.hiddenNow);
				}
			}
			telemetry::Record("scene_walk", "\"containers\":%zu,\"roots\":%d,\"nodes\":%d,\"moved\":%d,\"left\":%d,\"other\":%d,\"hidden_before\":%d,\"hidden_after\":%d",
				after.containers, rootsMatched, matched, moved, stayed, other, hiddenBefore, hiddenAfter);
		}

		// Player jump watch: after a shift, a jump of the player not made by us (0.34.1: dragged 1.7 km back to the old
		// coordinates at 774 m/s) is logged with the terrain manager and grid state that came with it.
		void WatchPlayerJump(ULONGLONG now)
		{
			static game::Vec3 last{};
			static bool have = false;
			const auto player = Global(addr::kPlayerCharacterPtr);
			const auto mgr = Global(addr::kPlanetManagerPtr);
			const auto tes = Global(addr::kTESPtr);
			if (!player || !mgr || !tes) {
				have = false;
				return;
			}
			const auto p = At<game::Vec3>(player, off::kRefPosition);
			if (g_justShifted) {
				g_justShifted = false;
				last = g_playerAfterShift;  // so a revert in the first frame after the shift is seen too
				have = true;
			}
			if (have && g_lastShift && now - g_lastShift < 30000) {
				const double jump = std::hypot(p.x - last.x, p.y - last.y);
				if (jump > 50.0) {
					logger::Write("Jump watch: player jumped %.0f m in one frame, (%.1f, %.1f) -> (%.1f, %.1f), %.1f s after the shift; terrain manager state %u, "
						"shifting %u, grid centre (%d, %d), new centre (%d, %d), last delta (%d, %d); TES grid centre (%d, %d)", jump, last.x, last.y, p.x, p.y,
						double(now - g_lastShift) / 1000.0, unsigned(At<std::uint8_t>(mgr, off::kMgrState)), unsigned(At<std::uint8_t>(mgr, off::kMgrShifting)),
						At<std::int32_t>(mgr, off::kMgrGridCentre), At<std::int32_t>(mgr, off::kMgrGridCentre + 4), At<std::int32_t>(mgr, off::kMgrNewCentre),
						At<std::int32_t>(mgr, off::kMgrNewCentre + 4), At<std::int32_t>(mgr, off::kMgrShiftDelta), At<std::int32_t>(mgr, off::kMgrShiftDelta + 4),
						At<std::int32_t>(tes, off::kTESGridCentre), At<std::int32_t>(tes, off::kTESGridCentre + 4));
					telemetry::Record("player_jump", "\"m\":%.0f,\"after_s\":%.1f", jump, double(now - g_lastShift) / 1000.0);
				}
			}
			last = p;
			have = true;
		}

		// Reads 8 bytes or returns false (SEH-guarded: no C++ objects in here).
		bool SafeRead(std::uintptr_t a, std::uintptr_t& out)
		{
			__try {
				out = *reinterpret_cast<const std::uintptr_t*>(a);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Structure of the scene around the loaded objects, to find where the geometry without a reference hangs
		// (combined/instanced meshes left behind by the re-centre, 0.34.1) and which cell field points at it.
		struct Counts { int nodes = 0, refs = 0; };
		Counts CountTree(std::uintptr_t n, const std::unordered_map<std::uintptr_t, NodeSnap>& roots, int depth)
		{
			Counts c;
			if (!n || depth > 64) {
				return c;
			}
			if (roots.count(n)) {
				c.refs = 1;
				return c;
			}
			c.nodes = 1;
			auto** vtbl = *reinterpret_cast<void***>(n);
			const auto asNode = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(vtbl[off::kVtblIsNode])(n);
			if (!asNode) {
				return c;
			}
			const auto children = At<std::uintptr_t*>(asNode, off::kNodeChildren);
			const auto cc = At<std::uint16_t>(asNode, off::kNodeChildCount);
			for (std::uint16_t i = 0; children && i < cc; ++i) {
				const auto k = CountTree(children[i], roots, depth + 1);
				c.nodes += k.nodes;
				c.refs += k.refs;
			}
			return c;
		}

		void DumpTree(std::uintptr_t n, const std::unordered_map<std::uintptr_t, NodeSnap>& roots, int depth, int& lines,
			std::unordered_map<std::uintptr_t, int>& depthOf)
		{
			if (!n || depth > 5 || lines > 220 || roots.count(n)) {
				return;
			}
			depthOf[n] = depth;
			auto** vtbl = *reinterpret_cast<void***>(n);
			const auto asNode = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(vtbl[off::kVtblIsNode])(n);
			const auto children = asNode ? At<std::uintptr_t*>(asNode, off::kNodeChildren) : nullptr;
			const auto cc = asNode ? At<std::uint16_t>(asNode, off::kNodeChildCount) : 0;
			const auto t = CountTree(n, roots, 0);
			if (depth <= 2 || t.nodes >= 30 || t.refs > 0) {
				int directRefs = 0;
				for (std::uint16_t i = 0; children && i < cc; ++i) {
					directRefs += roots.count(children[i]) ? 1 : 0;
				}
				logger::Write("Scene: %*s%s %p children %u (object roots %d) subtree: %d nodes, %d object roots; local (%.1f, %.1f, %.1f) world (%.1f, %.1f, %.1f) flags %llX",
					depth * 2, "", RttiName(n), reinterpret_cast<void*>(n), unsigned(cc), directRefs, t.nodes, t.refs, At<float>(n, 0x70), At<float>(n, 0x74),
					At<float>(n, 0x78), At<float>(n, 0xB0), At<float>(n, 0xB4), At<float>(n, 0xB8), static_cast<unsigned long long>(At<std::uint64_t>(n, 0x118)));
				++lines;
			}
			if (depth <= 1 || t.nodes >= 30) {
				for (std::uint16_t i = 0; children && i < cc; ++i) {
					DumpTree(children[i], roots, depth + 1, lines, depthOf);
				}
			}
		}

		void DumpSceneStructure(const SceneSnap& snap)
		{
			std::unordered_set<std::uintptr_t> tops;
			for (const auto& [node, s] : snap.roots) {
				auto top = node;
				for (int d = 0; d < 32; ++d) {
					const auto parent = At<std::uintptr_t>(top, off::kNodeParent);
					if (!parent) {
						break;
					}
					top = parent;
				}
				if (top != node) {
					tops.insert(top);
				}
			}
			int lines = 0;
			std::unordered_map<std::uintptr_t, int> depthOf;
			for (const auto t : tops) {
				DumpTree(t, snap.roots, 0, lines, depthOf);
			}
			// Which cell fields (directly or one pointer away) point at a node of the tree.
			const auto tes = Global(addr::kTESPtr);
			const auto entries = tes ? At<std::uintptr_t*>(tes, off::kTESGridCells) : nullptr;
			const auto count = tes ? At<std::uint32_t>(tes, off::kTESGridCount) : 0;
			int shown = 0;
			for (std::uint32_t i = 0; entries && i < count && shown < 6; ++i) {
				const auto cell = GridCell(entries[i]);
				if (!cell) {
					continue;
				}
				++shown;
				for (std::uint32_t o = 0; o < 0x200; o += 8) {
					std::uintptr_t v = 0;
					if (!SafeRead(cell + o, v) || !v) {
						continue;
					}
					if (const auto it = depthOf.find(v); it != depthOf.end()) {
						logger::Write("Scene: cell %08X +0x%X -> %s %p (depth %d)", At<std::uint32_t>(cell, 0x28), o, RttiName(v), reinterpret_cast<void*>(v), it->second);
						continue;
					}
					if (v < 0x10000 || (v >> 47) != 0) {
						continue;
					}
					for (std::uint32_t o2 = 0; o2 < 0x80; o2 += 8) {
						std::uintptr_t w = 0;
						if (SafeRead(v + o2, w) && w) {
							if (const auto it = depthOf.find(w); it != depthOf.end()) {
								logger::Write("Scene: cell %08X +0x%X -> +0x%X -> %s %p (depth %d)", At<std::uint32_t>(cell, 0x28), o, o2, RttiName(w),
									reinterpret_cast<void*>(w), it->second);
							}
						}
					}
				}
			}
		}

		void MeasureAndDecide()
		{
			const auto mgr = Global(addr::kPlanetManagerPtr);
			const auto player = Global(addr::kPlayerCharacterPtr);
			const double radius = handoff::PlanetRadius();
			if (!mgr || !player || radius <= 0) {
				logger::Write("Frame check: cannot measure (no planet); rolling back once the terrain has settled");
				g_fc.stage = 2;
				return;
			}
			std::unordered_map<std::uint32_t, RefAnchor> refs;
			std::unordered_map<std::uint32_t, CellAnchor> cells;
			CollectWorld(mgr, refs, cells, &g_fc.refs);
			const float dx = g_fc.cx * off::kCellSize, dy = g_fc.cy * off::kCellSize;
			// 2 m: loose physics objects settle or roll on their own during the measuring second (0.36.0: 0.64 m, a false
			// failure that rolled the world back 3.6 km while driving). A wrong shift is off by whole cells (100 m).
			constexpr double kPosTol = 2.0, kLatTol = 2.0, kNodeTol = 0.5;

			struct Bad { std::uint32_t id; char type; bool authored; double posErr, latErr, nodeErr, boundErr; };
			std::vector<Bad> badPos, badLat, badNode, badBound;
			double worstBound = 0;
			int matched = 0, matchedAuthored = 0;
			double worstPos = 0, worstLat = 0, worstNode = 0;
			for (const auto& [id, before] : g_fc.refs) {
				const auto it = refs.find(id);
				if (it == refs.end()) {
					continue;
				}
				const auto& after = it->second;
				++matched;
				matchedAuthored += before.authoredCell ? 1 : 0;
				const double posErr = std::hypot(after.px - (before.px - dx), after.py - (before.py - dy));
				const double latErr = sphere::Angle(before.lat, before.lon, after.lat, after.lon) * radius;
				double nodeErr = 0, boundErr = 0;
				if (before.hasNode && after.hasNode) {
					boundErr = std::sqrt(double(after.box - before.box) * (after.box - before.box) + double(after.boy - before.boy) * (after.boy - before.boy) +
						double(after.boz - before.boz) * (after.boz - before.boz));
					nodeErr = std::sqrt(double(after.nox - before.nox) * (after.nox - before.nox) + double(after.noy - before.noy) * (after.noy - before.noy) +
						double(after.noz - before.noz) * (after.noz - before.noz));
				}
				worstPos = std::max(worstPos, posErr);
				worstLat = std::max(worstLat, latErr);
				worstNode = std::max(worstNode, nodeErr);
				worstBound = std::max(worstBound, boundErr);
				const Bad b{ id, before.type, before.authoredCell, posErr, latErr, nodeErr, boundErr };
				if (boundErr > kNodeTol) badBound.push_back(b);
				if (posErr > kPosTol) badPos.push_back(b);
				if (latErr > kLatTol) badLat.push_back(b);
				if (nodeErr > kNodeTol) badNode.push_back(b);
			}
			int cellsMatched = 0, badCells = 0;
			double worstCorner = 0, worstCellLat = 0;
			for (const auto& [id, before] : g_fc.cells) {
				const auto it = cells.find(id);
				if (it == cells.end()) {
					continue;
				}
				const auto& after = it->second;
				++cellsMatched;
				const double cornerErr = std::hypot(after.cx - (before.cx - dx), after.cy - (before.cy - dy));
				const bool keyOk = after.x == before.x - g_fc.cx && after.y == before.y - g_fc.cy;
				const double latErr = sphere::Angle(before.lat, before.lon, after.lat, after.lon) * radius;
				worstCorner = std::max(worstCorner, cornerErr);
				worstCellLat = std::max(worstCellLat, latErr);
				if (cornerErr > kPosTol || !keyOk || latErr > kLatTol) {
					++badCells;
					if (badCells <= 10) {
						logger::Write("Frame check:   cell %08X%s coords (%d, %d) -> (%d, %d) [expected (%d, %d)], corner error %.2f m, lat/long error %.2f m", id,
							before.authored ? " (hand-built)" : "", before.x, before.y, after.x, after.y, before.x - g_fc.cx, before.y - g_fc.cy, cornerErr, latErr);
					}
				}
			}
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			double plat, plon;
			PlanetLatLong(mgr, pp.x, pp.y, pp.z, plat, plon);
			const double playerErr = sphere::Angle(g_fc.playerLat, g_fc.playerLon, plat, plon) * radius;

			logger::Write("Frame check: shift (%d, %d) cells. Objects: %d matched (%d in hand-built cells) of %zu; moved wrong %zu (worst %.2f m); "
				"planet position changed %zu (worst %.2f m); 3D left behind %zu (worst %.2f m). Cells: %d matched, %d wrong (worst corner %.2f m, "
				"lat/long %.2f m). Player planet position changed %.2f m.",
				g_fc.cx, g_fc.cy, matched, matchedAuthored, g_fc.refs.size(), badPos.size(), worstPos, badLat.size(), worstLat, badNode.size(), worstNode,
				cellsMatched, badCells, worstCorner, worstCellLat, playerErr);
			telemetry::Record("frame_check", "\"cx\":%d,\"cy\":%d,\"matched\":%d,\"bad_pos\":%zu,\"bad_lat\":%zu,\"bad_node\":%zu,\"cells\":%d,\"bad_cells\":%d,\"player_m\":%.2f",
				g_fc.cx, g_fc.cy, matched, badPos.size(), badLat.size(), badNode.size(), cellsMatched, badCells, playerErr);
			const auto dump = [](const char* what, std::vector<Bad>& v, double Bad::*key) {
				std::sort(v.begin(), v.end(), [key](const Bad& a, const Bad& b) { return a.*key > b.*key; });
				for (std::size_t i = 0; i < v.size() && i < 12; ++i) {
					logger::Write("Frame check:   %s: %08X type '%c'(%d)%s pos %.2f m, lat/long %.2f m, 3D %.2f m", what, v[i].id, v[i].type > 31 ? v[i].type : '?',
						int(v[i].type), v[i].authored ? " hand-built cell" : "", v[i].posErr, v[i].latErr, v[i].nodeErr);
				}
			};
			dump("moved wrong", badPos, &Bad::posErr);
			dump("planet position changed", badLat, &Bad::latErr);
			dump("3D left behind", badNode, &Bad::nodeErr);
			dump("culling bound left behind", badBound, &Bad::boundErr);
			logger::Write("Frame check: culling bounds: %zu left behind (worst %.2f m)", badBound.size(), worstBound);
			ReportScene(g_scene, dx, dy);
			g_scene = SceneSnap{};

			const bool pass = badPos.empty() && badLat.empty() && badNode.empty() && badBound.empty() && badCells == 0 && playerErr <= kLatTol;
			if (pass) {
				logger::Write("Frame check: PASSED: the shift kept every measured planet position; kept.");
				notify::Show("Frame check PASSED (%d objects, %d cells)", matched, cellsMatched);
			}
			else {
				// Rolling back right away put a second shift inside the terrain rebuild and crashed the terrain generator
				// (0.34.1, +0x98ACDB): roll back once the terrain has settled.
				g_fc.stage = 2;
				logger::Write("Frame check: FAILED: rolling back once the terrain has settled");
				notify::Show("Frame check FAILED (see log): %zu moved wrong, %zu planet-position, %zu 3D, %d cells, player %.1f m; rolling back soon", badPos.size(),
					badLat.size(), badNode.size(), badCells, playerErr);
			}
		}
	}

	namespace
	{
		// F11 test: re-place every object within kRefreshRange of the player through the engine's own move routine, at
		// the position it already has. The re-centre writes 3D transforms directly; if the renderer keeps its own copy
		// of static objects, invisible places reappear after this (renderer stale), otherwise they stay invisible.
		constexpr float kRefreshRange = 400.0f;
		bool g_refreshKeyDown = false;

		void RefreshNearby()
		{
			const auto tes = Global(addr::kTESPtr);
			const auto player = Global(addr::kPlayerCharacterPtr);
			const auto a = CurrentAnchor();
			if (!tes || !player || a.ref != player) {
				notify::Show("3D refresh: go outside on a planet first");
				return;
			}
			const auto worldspace = At<std::uintptr_t>(a.cell, off::kCellWorldspace);
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			const auto move = EngineFn<void (*)(std::uintptr_t, std::uint64_t*, std::uintptr_t, std::uintptr_t, float*, float*)>(addr::kMoveReference);
			const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
			const auto count = At<std::uint32_t>(tes, off::kTESGridCount);
			int moved = 0, withNode = 0;
			for (std::uint32_t i = 0; entries && i < count; ++i) {
				const auto cell = GridCell(entries[i]);
				if (!cell || At<std::uint8_t>(cell, off::kCellState) != 10) {
					continue;
				}
				std::vector<std::uintptr_t> refs;
				CollectRefs cb{ CollectRefs::kVtbl, &refs };
				EngineFn<int (*)(std::uintptr_t, CollectRefs*, char)>(addr::kForEachRefInCell)(cell, &cb, 0);
				for (const auto r : refs) {
					if (r == player) {
						continue;
					}
					const auto p = At<game::Vec3>(r, off::kRefPosition);
					if (std::hypot(p.x - pp.x, p.y - pp.y) > kRefreshRange) {
						continue;
					}
					std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
					float pos[3] = { p.x, p.y, p.z };
					const auto rr = At<game::Vec3>(r, off::kRefRotation);
					float rot[3] = { rr.x, rr.y, rr.z };
					withNode += RefNode(r) ? 1 : 0;
					move(r, &handle, cell, worldspace, pos, rot);
					++moved;
				}
			}
			std::int32_t tx = 0, ty = 0;
			TotalShift(tx, ty);
			logger::Write("3D refresh: re-placed %d objects (%d with 3D) within %.0f m through the engine move routine; total shift (%d, %d)", moved, withNode,
				kRefreshRange, tx, ty);
			telemetry::Record("refresh_3d", "\"objects\":%d,\"with_3d\":%d,\"tx\":%d,\"ty\":%d", moved, withNode, tx, ty);
			notify::Show("3D refresh: %d objects re-placed. Did the invisible places come back?", moved);
		}
	}

	void FrameCheckTick()
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		const bool f11 = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
		if (f11 && !g_refreshKeyDown) {
			RefreshNearby();
		}
		g_refreshKeyDown = f11;
		const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
		const bool pressed = down && !g_fc.keyDown;
		g_fc.keyDown = down;
		const ULONGLONG now = GetTickCount64();
		WatchTerrain(Global(addr::kPlanetManagerPtr), now);
		WatchPlayerJump(now);
		if (const auto pl = Global(addr::kPlayerCharacterPtr)) {
			const auto* px = reinterpret_cast<const float*>(pl + off::kRefPosition);
			if (!watchpoint::Watching(px)) {
				watchpoint::Arm(px, 300.0f, 0xFFFFFFFFu, "player x");
			}
		}
		if (g_fc.stage == 1) {
			if (now - g_fc.since >= 1000) {
				g_fc.stage = 0;
				MeasureAndDecide();
				g_fc.refs.clear();
				g_fc.cells.clear();
			}
			return;
		}
		if (g_fc.stage == 3) {
			const int r = SyncedRun(g_fc.cx, g_fc.cy, false);
			if (r < 0) {
				return;
			}
			if (r == 0) {
				notify::Show("Frame check: the shift was refused (see log)");
				g_fc.refs.clear();
				g_fc.cells.clear();
				g_fc.stage = 0;
				return;
			}
			g_lastShift = now;
			g_fc.stage = 1;
			g_fc.since = now;
			notify::Show("Frame check: shifted, measuring in 1 s...");
			return;
		}
		if (g_fc.stage == 2) {
			const auto a = CurrentAnchor();
			const auto tes = Global(addr::kTESPtr);
			if (TerrainSettled(now) && tes && a.ref && WorldIsSettled(tes, a.cell) && PendingCellLoads() == 0) {
				const int r = SyncedRun(-g_fc.cx, -g_fc.cy, false);
				if (r < 0) {
					return;
				}
				const bool back = r == 1;
				if (back) {
					g_lastShift = now;
					g_fc.stage = 0;
				}
				logger::Write("Frame check: roll back %s", back ? "done" : "refused, retrying");
				notify::Show("Frame check: %s", back ? "rolled back" : "roll back waiting");
			}
			return;
		}
		if (!pressed) {
			return;
		}
		const auto mgr = Global(addr::kPlanetManagerPtr);
		const auto tes = Global(addr::kTESPtr);
		const auto player = Global(addr::kPlayerCharacterPtr);
		const auto a = CurrentAnchor();
		if (!mgr || !tes || !player || a.ref != player || handoff::PlanetRadius() <= 0) {
			notify::Show("Frame check: go outside on a planet (zones measured) first");
			return;
		}
		if (!WorldIsSettled(tes, a.cell) || PendingCellLoads() != 0) {
			notify::Show("Frame check: cells still loading, try again in a moment");
			return;
		}
		ULONGLONG waitMs = 0;
		if (!TerrainSettled(now, &waitMs)) {
			notify::Show("Frame check: terrain still settling from the last move, try again in %.0f s", double(waitMs) / 1000.0 + 0.5);
			return;
		}
		std::int32_t cx = At<std::int32_t>(mgr, off::kMgrGridCentre), cy = At<std::int32_t>(mgr, off::kMgrGridCentre + 4);
		if (cx == 0 && cy == 0) {
			const auto p = At<game::Vec3>(player, off::kRefPosition);
			cx = std::int32_t(std::lround(p.x / off::kCellSize));
			cy = std::int32_t(std::lround(p.y / off::kCellSize));
		}
		if (cx == 0 && cy == 0) {
			notify::Show("Frame check: already centred here (walk 300 m first)");
			return;
		}
		g_fc.refs.clear();
		g_fc.cells.clear();
		CollectWorld(mgr, g_fc.refs, g_fc.cells);
		g_scene = SceneSnap{};
		CollectScene(g_scene);
		logger::Write("Frame check: scene walk snapshot: %zu object roots, %zu other nodes under %zu containers", g_scene.roots.size(), g_scene.nodes.size(),
			g_scene.containers);
		DumpSceneStructure(g_scene);
		const auto p = At<game::Vec3>(player, off::kRefPosition);
		PlanetLatLong(mgr, p.x, p.y, p.z, g_fc.playerLat, g_fc.playerLon);
		logger::Write("Frame check: snapshot of %zu objects and %zu cells; shifting by (%d, %d) cells", g_fc.refs.size(), g_fc.cells.size(), cx, cy);
		g_fc.cx = cx;
		g_fc.cy = cy;
		g_fc.stage = 3;  // waiting for the shift inside the game update
		g_fc.since = now;
		SyncedRun(cx, cy, false);
	}

	// ---- Cell watch (data collection for "CELL NOT READY" after shifts) -----------------------------------------
	namespace
	{
		// The worldspace cell map: entry key (x << 16 | y as int16), form id at +4, next at +8 (-1 = empty slot).
		std::uint32_t MapLookup(std::uintptr_t worldspace, std::int32_t x, std::int32_t y)
		{
			const auto table = worldspace + off::kWsCellMap + 8;
			const auto* entries = At<std::uint8_t*>(table, off::kTblEntries);
			const auto capacity = At<std::uint64_t>(table, off::kTblCapacity);
			const std::uint32_t key = (std::uint32_t(std::uint16_t(x)) << 16) | std::uint16_t(y);
			for (std::uint64_t i = 0; entries && i < capacity; ++i) {
				const auto* e = entries + i * 16;
				if (*reinterpret_cast<const std::int32_t*>(e + 8) == -1) {
					continue;
				}
				if (*reinterpret_cast<const std::uint32_t*>(e) == key) {
					return *reinterpret_cast<const std::uint32_t*>(e + 4);
				}
			}
			return 0;
		}
	}

	void CellWatchTick()
	{
		static ULONGLONG lastLog = 0, notReadySince = 0;
		const ULONGLONG now = GetTickCount64();
		const auto tes = Global(addr::kTESPtr);
		const auto player = Global(addr::kPlayerCharacterPtr);
		if (!tes || !player) {
			return;
		}
		const auto pcell = At<std::uintptr_t>(player, off::kRefParentCell);
		const auto ws = pcell ? At<std::uintptr_t>(pcell, off::kCellWorldspace) : 0;
		if (!pcell || !ws || At<std::uintptr_t>(tes, off::kTESInteriorCell) != 0) {
			notReadySince = 0;
			return;
		}
		const bool playerCellReady = At<std::uint8_t>(pcell, off::kCellState) == 10;
		if (playerCellReady) {
			notReadySince = 0;
		}
		else if (notReadySince == 0) {
			notReadySince = now;
		}
		const bool afterShift = g_lastShift && now - g_lastShift < 60000;
		const bool stuck = notReadySince && now - notReadySince > 1000;
		if (!(afterShift || stuck) || now - lastLog < 2000) {
			return;
		}
		lastLog = now;
		const auto p = At<game::Vec3>(player, off::kRefPosition);
		const std::int32_t px = std::int32_t(std::floor(p.x / off::kCellSize)), py = std::int32_t(std::floor(p.y / off::kCellSize));
		const auto pext = At<std::uintptr_t>(pcell, off::kCellExterior);
		const std::uint32_t mapId = MapLookup(ws, px, py);
		const auto gcx = At<std::int32_t>(tes, off::kTESGridCentre), gcy = At<std::int32_t>(tes, off::kTESGridCentre + 4);
		const auto tcx = At<std::int32_t>(tes, off::kTESCurrentCell), tcy = At<std::int32_t>(tes, off::kTESCurrentCell + 4);
		logger::Write("Cell watch%s: player (%.1f, %.1f) cell by position (%d, %d) -> map handle %08X; parent cell %08X at %p coords (%d, %d) state %u; "
			"TES grid centre (%d, %d), current cell (%d, %d); %u cell-load queues busy%s",
			afterShift ? " [after shift]" : "", p.x, p.y, px, py, mapId, At<std::uint32_t>(pcell, 0x28), reinterpret_cast<void*>(pcell), pext ? At<std::int32_t>(pext, 0) : 0,
			pext ? At<std::int32_t>(pext, 4) : 0, unsigned(At<std::uint8_t>(pcell, off::kCellState)), gcx, gcy, tcx, tcy, unsigned(PendingCellLoads()),
			stuck ? " [player cell NOT attached]" : "");
		const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
		const auto count = At<std::uint32_t>(tes, off::kTESGridCount);
		int shown = 0;
		for (std::uint32_t i = 0; entries && i < count; ++i) {
			const auto entry = entries[i];
			if (!entry) {
				logger::Write("Cell watch:   slot %u: empty", i);
				continue;
			}
			const auto estate = At<std::uint32_t>(entry, off::kGridEntryState);
			const auto cell = At<std::uintptr_t>(entry, off::kGridEntryCell);
			if (!cell) {
				logger::Write("Cell watch:   slot %u: entry state %08X, no cell", i, estate);
				continue;
			}
			const auto ext = At<std::uintptr_t>(cell, off::kCellExterior);
			const std::int32_t ex = ext ? At<std::int32_t>(ext, 0) : 0x7FFFFFFF, ey = ext ? At<std::int32_t>(ext, 4) : 0x7FFFFFFF;
			const auto id = At<std::uint32_t>(cell, 0x28);
			const auto cstate = unsigned(At<std::uint8_t>(cell, off::kCellState));
			const bool odd = cstate != 10 || std::abs(ex - gcx) > 2 || std::abs(ey - gcy) > 2;  // the map holds handles: no key check
			if (odd || stuck || shown < 0) {
				const auto k = LoadKey(ws, ex, ey);
				logger::Write("Cell watch:   slot %u: cell %08X at %p stored (%d, %d) load key %08X %08X %08X state %u, entry state %08X%s", i, id,
					reinterpret_cast<void*>(cell), ex, ey, k.w[0], k.w[1], k.w[2], cstate, estate, odd ? "  <--" : "");
				++shown;
			}
		}
		if (shown == 0) {
			logger::Write("Cell watch:   all %u grid cells attached, keyed and within the grid around (%d, %d)", count, gcx, gcy);
		}
	}
	// ---- LoadCell jobs requested before a shift -------------------------------------------------------------
	// A LoadCell task keeps the grid coordinates it was requested with (+0xD8/+0xDC) and passes them on to the terrain
	// generation of the cell (FUN_14041F120 -> FUN_14041EFA0 -> FUN_140990850: heights, collision and placement from
	// x*100, y*100; worker R19). A task requested before a shift therefore built its terrain in the old frame (gaps and
	// misplaced ground after shifts while cells load). Its database key is absolute: for planet cells the packed planet
	// cell id. Before the task runs, its coordinates are checked against the key in the current frame and, if they name
	// another cell, moved by the sum of the most recent shifts that makes them match.
	namespace
	{
		using LoadCellRequest_t = int (*)(std::uintptr_t, int, std::uintptr_t, std::int16_t, std::int16_t,
			std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
		LoadCellRequest_t g_loadCellRequest = nullptr;

		int LoadCellRequestDetour(std::uintptr_t db, int priority, std::uintptr_t ws, std::int16_t x, std::int16_t y,
			std::uintptr_t result, std::uintptr_t state, std::uintptr_t dependency, std::uintptr_t key)
		{
			FrameReadScope frameRead;
			// The native grid producer can finish an old-frame request list just after a shift. Correct the coordinates
			// here, before FUN_140420410 derives the planet key; the build-time key check cannot repair a bad key.
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - Base();
			const auto tes = Global(addr::kTESPtr);
			const auto mgr = Global(addr::kPlanetManagerPtr);
			const auto gridWs = tes ? At<std::uintptr_t>(tes, 0x188) : 0;
			if (ws && ws == gridWs && mgr && At<std::uint8_t>(mgr, off::kMgrState) == 2) {
				const int gx = At<std::int32_t>(tes, off::kTESGridCentre);
				const int gy = At<std::int32_t>(tes, off::kTESGridCentre + 4);
				const bool distantRequest = std::abs(int(x) - gx) > 8 || std::abs(int(y) - gy) > 8;
				const auto shiftedAt = g_shiftAt.load();
				const auto now = GetTickCount64();
				if (distantRequest && caller == 0x014361B1 && priority == 5 && shiftedAt && now >= shiftedAt && now - shiftedAt < 1000) {
					ShiftRec shift{};
					{
						std::lock_guard<std::mutex> lock(g_historyLock);
						if (!g_history.empty()) shift = g_history.back();
					}
					int matches = 0, chosenX = 0, chosenY = 0, chosenMultiple = 0;
					if (std::abs(shift.cx) + std::abs(shift.cy) >= 4) {
						for (int multiple : { -2, -1, 1, 2 }) {
							const int nx = int(x) - multiple * shift.cx;
							const int ny = int(y) - multiple * shift.cy;
							if (std::abs(nx - gx) <= 8 && std::abs(ny - gy) <= 8) {
								++matches;
								chosenX = nx;
								chosenY = ny;
								chosenMultiple = multiple;
							}
						}
					}
					if (matches == 1) {
						void* stack[3]{};
						const auto depth = CaptureStackBackTrace(1, 3, stack, nullptr);
						const auto parent = depth > 1 ? reinterpret_cast<std::uintptr_t>(stack[1]) - Base() : 0;
						logger::Write("Re-centre: native cell request rebased before key (%d, %d) -> (%d, %d), multiple %d of last shift (%d, %d), parent +%08llX",
							int(x), int(y), chosenX, chosenY, chosenMultiple, shift.cx, shift.cy, static_cast<unsigned long long>(parent));
						telemetry::Record("cell_request_rebased", "\"old_x\":%d,\"old_y\":%d,\"x\":%d,\"y\":%d,\"multiple\":%d,\"parent\":\"%08llX\"",
							int(x), int(y), chosenX, chosenY, chosenMultiple, static_cast<unsigned long long>(parent));
						x = static_cast<std::int16_t>(chosenX);
						y = static_cast<std::int16_t>(chosenY);
					}
				}
				if (distantRequest || (shiftedAt && now >= shiftedAt && now - shiftedAt < 5000)) {
					trace::Line("cellreq", "caller +%08llX priority %d request (%d, %d) grid (%d, %d) far %d origin %016llX",
						static_cast<unsigned long long>(caller), priority, int(x), int(y), gx, gy, distantRequest ? 1 : 0,
						static_cast<unsigned long long>(At<std::uint64_t>(ws, off::kWsPlanetOrigin)));
				}
			}
			return g_loadCellRequest(db, priority, ws, x, y, result, state, dependency, key);
		}

		using LoadCellRun_t = std::uint64_t (*)(std::uintptr_t);
		using LoadCellBuild_t = void (*)(const std::uint32_t*, std::uintptr_t, std::int32_t*,
			std::uintptr_t, std::uintptr_t, std::uintptr_t);
		LoadCellBuild_t g_loadCellBuild = nullptr;

		void LoadCellBuildDetour(const std::uint32_t* key, std::uintptr_t ws, std::int32_t* coords,
			std::uintptr_t result, std::uintptr_t state, std::uintptr_t flags)
		{
			FrameReadScope frameRead;
			// This is the last common point before FUN_14041EFA0 constructs terrain and collision.
			// The DB's packed planet key survives shifts, so it serves as the job's frame ledger.
			const auto version = g_frameVersion.load();
			if (key && coords && ws && key[1] == *reinterpret_cast<std::uint32_t*>(Base() + addr::kPlanetKeyTag) &&
				At<std::uint64_t>(ws, off::kWsPlanetOrigin) != ~0ull) {
				const std::uint64_t wanted = (std::uint64_t(key[2]) << 32) | key[0];
				if (PlanetCellId(ws, coords[0], coords[1]) != wanted) {
					std::vector<ShiftRec> history;
					{
						std::lock_guard<std::mutex> lock(g_historyLock);
						history = g_history;
					}
					int sx = 0, sy = 0;
					for (auto it = history.rbegin(); it != history.rend(); ++it) {
						sx += it->cx;
						sy += it->cy;
						if (PlanetCellId(ws, coords[0] - sx, coords[1] - sy) == wanted) {
							logger::Write("Re-centre: cell build rebased before terrain (%d, %d) -> (%d, %d), frame %llu",
								coords[0], coords[1], coords[0] - sx, coords[1] - sy, static_cast<unsigned long long>(version));
							coords[0] -= sx;
							coords[1] -= sy;
							break;
						}
					}
				}
			}
			g_loadCellBuild(key, ws, coords, result, state, flags);
			if (g_frameVersion.load() != version) {
				telemetry::Record("cell_build_crossed_shift", "\"x\":%d,\"y\":%d,\"from\":%llu,\"to\":%llu",
					coords ? coords[0] : 0, coords ? coords[1] : 0,
					static_cast<unsigned long long>(version), static_cast<unsigned long long>(g_frameVersion.load()));
			}
		}
		LoadCellRun_t g_loadCellRun = nullptr;
		std::atomic<unsigned> g_loadCellFixed{ 0 };

		std::uint64_t LoadCellRunDetour(std::uintptr_t task)
		{
			FrameReadScope frameRead;
			const auto startedUs = telemetry::NowUs();
			bool movedFrame = false;
			const auto entry = task ? *reinterpret_cast<std::uintptr_t*>(task + 0xC0) : 0;
			const auto ws = task ? *reinterpret_cast<std::uintptr_t*>(task + 0xD0) : 0;
			if (entry && ws && *reinterpret_cast<std::uint32_t*>(entry + 0x0C) == *reinterpret_cast<std::uint32_t*>(Base() + addr::kPlanetKeyTag)) {
				const std::uint64_t key = (std::uint64_t(*reinterpret_cast<std::uint32_t*>(entry + 0x10)) << 32) | *reinterpret_cast<std::uint32_t*>(entry + 0x08);
				auto& x = *reinterpret_cast<std::int32_t*>(task + 0xD8);
				auto& y = *reinterpret_cast<std::int32_t*>(task + 0xDC);
				trace::Line("cellload", "task %p run: request (%d, %d), planet key %016llX", reinterpret_cast<void*>(task), x, y, static_cast<unsigned long long>(key));
				if (At<std::uint64_t>(ws, off::kWsPlanetOrigin) != ~0ull && PlanetCellId(ws, x, y) != key) {
					std::vector<ShiftRec> history;
					{
						std::lock_guard<std::mutex> lock(g_historyLock);
						history = g_history;
					}
					std::int32_t sx = 0, sy = 0;
					bool fixed = false;
					for (auto it = history.rbegin(); it != history.rend(); ++it) {
						sx += it->cx;
						sy += it->cy;
						if (PlanetCellId(ws, x - sx, y - sy) == key) {
							const auto n = g_loadCellFixed.fetch_add(1) + 1;
							if (n <= 20 || n % 100 == 0) {
								logger::Write("Re-centre: cell load requested before a shift: coordinates (%d, %d) -> (%d, %d) (%u so far)", x, y, x - sx, y - sy, n);
							}
							x -= sx;
							y -= sy;
							movedFrame = true;
							fixed = true;
							break;
						}
					}
					if (!fixed) {
						static std::atomic<unsigned> odd{ 0 };
						if (odd.fetch_add(1) < 10) {
							logger::Write("Re-centre: cell load (%d, %d) does not match its planet key %016llX in this frame or any recent one", x, y,
								static_cast<unsigned long long>(key));
						}
					}
				}
			}
			const auto result = g_loadCellRun(task);
			const auto elapsedUs = telemetry::NowUs() - startedUs;
			const auto coords = task ? reinterpret_cast<const std::int32_t*>(task + 0xD8) : nullptr;
			telemetry::Record("cell_job", "\"task\":\"%p\",\"x\":%d,\"y\":%d,\"frame_fixed\":%d,\"worldspace\":%d,\"us\":%llu",
				reinterpret_cast<void*>(task), coords ? coords[0] : 0, coords ? coords[1] : 0, movedFrame ? 1 : 0, ws ? 1 : 0,
				static_cast<unsigned long long>(elapsedUs));
			return result;
		}
	}

	bool InstallLoadCellFix()
	{
		static const std::uint8_t kPrologue[16] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89, 0xC0, 0x00, 0x00, 0x00 };
		void* original = nullptr;
		if (!hook::Install(Base() + addr::kLoadCellRun, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&LoadCellRunDetour), &original)) {
			logger::Write("Re-centre: LoadCell run hook failed (loads across a shift keep old coordinates)");
			return false;
		}
		g_loadCellRun = reinterpret_cast<LoadCellRun_t>(original);
		logger::Write("Re-centre: LoadCell jobs requested before a shift are moved into the new frame");
		return true;
	}

	bool InstallLoadCellRequestTrace()
	{
		static const std::uint8_t kPrologue[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
			0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20 };
		void* original = nullptr;
		if (!hook::Install(Base() + addr::kLoadCellRequest, kPrologue, sizeof(kPrologue),
			reinterpret_cast<void*>(&LoadCellRequestDetour), &original)) {
			logger::Write("Re-centre: cell request producer trace unavailable");
			return false;
		}
		g_loadCellRequest = reinterpret_cast<LoadCellRequest_t>(original);
		logger::Write("Re-centre: cell request producers traced around shifts and outside the active grid");
		return true;
	}

	bool InstallLoadCellBuildFix()
	{
		static const std::uint8_t kPrologue[18] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
			0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x49, 0x8B, 0xF1 };
		void* original = nullptr;
		if (!hook::Install(Base() + addr::kLoadCellBuild, kPrologue, sizeof(kPrologue),
			reinterpret_cast<void*>(&LoadCellBuildDetour), &original)) {
			logger::Write("Re-centre: cell build frame correction unavailable");
			return false;
		}
		g_loadCellBuild = reinterpret_cast<LoadCellBuild_t>(original);
		logger::Write("Re-centre: cell build coordinates checked against stable planet keys before terrain generation");
		return true;
	}

	std::uint64_t PlanetCellIdAt(std::uintptr_t worldspace, std::int32_t x, std::int32_t y) { return PlanetCellId(worldspace, x, y); }

}
