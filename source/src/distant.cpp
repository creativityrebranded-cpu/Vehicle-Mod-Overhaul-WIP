#include "distant.h"

#include "addresses.h"
#include "core_api.h"
#include "game.h"
#include "log.h"
#include "modscene.h"
#include "anchor.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace distant
{
	namespace
	{
		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }

		std::uintptr_t AsNode(std::uintptr_t n)
		{
			__try {
				auto** vtbl = *reinterpret_cast<void***>(n);
				return reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(vtbl[off::kVtblIsNode])(n);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		struct Census
		{
			std::map<std::string, unsigned> kinds;
			unsigned nodes = 0, farNodes = 0;
			double farthest = 0;
		};

		void Walk(std::uintptr_t n, const game::Vec3& player, Census& c, int depth)
		{
			if (!n || depth > 6 || c.nodes > 20000) return;
			++c.nodes;
			++c.kinds[modscene::RttiName(n)];
			const float* w = reinterpret_cast<const float*>(n + off::kNodeWorldT);
			const double d = std::hypot(double(w[0]) - player.x, double(w[1]) - player.y);
			if (d > 2000.0) ++c.farNodes;
			c.farthest = std::max(c.farthest, d);
			const auto node = AsNode(n);
			if (!node) return;
			const auto ch = At<std::uintptr_t*>(node, off::kNodeChildren);
			const auto cc = At<std::uint16_t>(node, off::kNodeChildCount);
			for (std::uint16_t i = 0; ch && i < cc; ++i) Walk(ch[i], player, c, depth + 1);
		}
	}

	namespace
	{
		// 0.2: the census of 0.1 showed nothing farther than 3.5 km under the player's scene root, while `tdl` hides the
		// floating pieces, so the distant LOD hangs under another scene root. Find every BSSceneNode the game keeps a global
		// pointer to (scan of the exe's writable sections for pointers to objects with the scene root's vtable), once.
		bool g_scanned = false;
		std::vector<std::uintptr_t> g_roots;  // other scene roots found by the scan (not the player's)

		// 0.3: per other root, a sample of node positions before a shift, compared after it: did the shift move them?
		struct NodeSnap { std::uintptr_t node; float wx, wy, lx, ly; };
		std::vector<std::vector<NodeSnap>> g_before;

		void Sample(std::uintptr_t n, std::vector<NodeSnap>& out, int depth)
		{
			if (!n || depth > 6 || out.size() >= 300) return;
			const float* w = reinterpret_cast<const float*>(n + off::kNodeWorldT);
			const float* l = reinterpret_cast<const float*>(n + off::kNodeLocalT);
			if (depth > 0) out.push_back({ n, w[0], w[1], l[0], l[1] });
			const auto node = AsNode(n);
			if (!node) return;
			const auto ch = At<std::uintptr_t*>(node, off::kNodeChildren);
			const auto cc = At<std::uint16_t>(node, off::kNodeChildCount);
			for (std::uint16_t i = 0; ch && i < cc; ++i) Sample(ch[i], out, depth + 1);
		}

		std::uintptr_t ReadQ(std::uintptr_t a)
		{
			__try {
				return *reinterpret_cast<const std::uintptr_t*>(a);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		void ScanRoots(std::uintptr_t knownRoot, const game::Vec3& player)
		{
			const auto vt = ReadQ(knownRoot);
			const auto base = game::Base();
			const auto dos = base;
			const auto nt = base + *reinterpret_cast<const std::uint32_t*>(dos + 0x3C);
			const auto sections = *reinterpret_cast<const std::uint16_t*>(nt + 6);
			const auto optSize = *reinterpret_cast<const std::uint16_t*>(nt + 0x14);
			const auto sec = nt + 0x18 + optSize;
			std::vector<std::uintptr_t> found;
			for (unsigned i = 0; i < sections; ++i) {
				const auto h = sec + i * 40;
				const auto chars = *reinterpret_cast<const std::uint32_t*>(h + 36);
				if (!(chars & 0x80000000u)) continue;  // writable sections only
				const auto va = base + *reinterpret_cast<const std::uint32_t*>(h + 12);
				const auto size = *reinterpret_cast<const std::uint32_t*>(h + 8);
				for (std::uintptr_t a = va; a + 8 <= va + size; a += 8) {
					const auto q = *reinterpret_cast<const std::uintptr_t*>(a);
					if (q < 0x10000 || q > 0x00007FFFFFFFFFFFull || (q & 0xF)) continue;
					if (ReadQ(q) == vt && std::find(found.begin(), found.end(), q) == found.end()) {
						found.push_back(q);
						if (found.size() > 32) break;
					}
				}
			}
			logger::Write("Distant: %zu scene roots (BSSceneNode) held by globals%s", found.size(), found.empty() ? "" : "; per root:");
			g_roots.clear();
			for (const auto r : found) {
				if (r != knownRoot) g_roots.push_back(r);
				Census c;
				Walk(r, player, c, 0);
				logger::Write("Distant:   root %p%s: %u nodes (6 levels), %u more than 2 km from the player, farthest %.0f m", reinterpret_cast<void*>(r),
					r == knownRoot ? " (the player's)" : "", c.nodes, c.farNodes, c.farthest);
			}
		}
	}

	void BeforeShift(std::int32_t, std::int32_t, std::uintptr_t)
	{
		const auto root = modscene::SceneRoot();
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		if (!root || !player) return;
		// 0.4: the global scan found no other scene root (and raised access violations), so it is off. Instead: every
		// node under the scene root whose class names LOD or distant objects (BGSDistantObjectBlock, BSLOD*, ...),
		// sampled before the shift and compared after it.
		g_roots.clear();
		g_before.clear();
		std::vector<std::uintptr_t> lod;
		for (const char* cls : { "DistantObject", "LOD", "Clipmap", "Terrain" }) {
			std::vector<std::uintptr_t> found;
			modscene::FindClass(root, cls, found, 400);
			for (const auto n : found) {
				if (std::find(lod.begin(), lod.end(), n) == lod.end()) lod.push_back(n);
			}
		}
		std::map<std::string, unsigned> kinds;
		for (const auto n : lod) ++kinds[modscene::RttiName(n)];
		std::string list;
		for (const auto& [k, v] : kinds) list += (list.empty() ? "" : ", ") + k + " x" + std::to_string(v);
		logger::Write("Distant: before the shift: %zu LOD / distant / terrain nodes under the scene root%s%s", lod.size(), list.empty() ? "" : ": ", list.c_str());
		if (!lod.empty()) {
			g_roots.push_back(root);
			g_before.emplace_back();
			for (const auto n : lod) {
				const float* w = reinterpret_cast<const float*>(n + off::kNodeWorldT);
				const float* l = reinterpret_cast<const float*>(n + off::kNodeLocalT);
				g_before.back().push_back({ n, w[0], w[1], l[0], l[1] });
			}
		}
		(void)g_scanned;
	}

	namespace
	{
		// The console's ToggleDistantLOD (`tdl`, handler at Starfield.exe+0xEB91F0) only flips a dword flag at
		// +0x1E0 of the object at Starfield.exe+0x5FD9C20: new = (old byte == 0). Option 1 of the floating-terrain fix:
		// the same toggle right after a shift (distant LOD hidden), and back to the original once the world has
		// settled, so the LOD shows again from the new frame. The log says whether the pieces came back right.
		constexpr std::uint32_t kLodToggleOwner = 0x05FD9C20, kLodToggleFlag = 0x1E0;
		constexpr ULONGLONG kMinHideMs = 1000, kMaxHideMs = 8000;
		bool g_hidden = false;
		std::uint32_t g_original = 0;
		ULONGLONG g_hiddenAt = 0;
		unsigned g_refreshes = 0;
		// 0.6 (user: "tdl as an occasional refresh, sometimes after a load it still needs it"): a refresh after every
		// load once the world has settled for 3 s, and one follow-up refresh 15 s after each shift / load refresh.
		bool g_afterLoad = false;
		ULONGLONG g_settledSince = 0;
		ULONGLONG g_secondAt = 0;
		bool g_lastWasSecond = false;

		bool StartRefresh(const char* why)
		{
			const auto owner = Ptr(kLodToggleOwner);
			if (!owner) {
				logger::Write("Distant: LOD refresh (%s): toggle owner not found", why);
				return false;
			}
			if (!g_hidden) {
				g_original = At<std::uint32_t>(owner, kLodToggleFlag);
				At<std::uint32_t>(owner, kLodToggleFlag) = (g_original & 0xFF) == 0 ? 1u : 0u;  // exactly what `tdl` writes
				g_hidden = true;
				logger::Write("Distant: LOD refresh (%s): distant LOD hidden (flag %u -> %u)", why, g_original, (g_original & 0xFF) == 0 ? 1u : 0u);
			}
			g_hiddenAt = GetTickCount64();  // a new reason while hidden starts the wait again
			return true;
		}
	}

	void LoadReset()
	{
		g_afterLoad = true;
		g_settledSince = 0;
		g_secondAt = 0;
	}

	void Sync(std::uintptr_t)
	{
		const ULONGLONG now = GetTickCount64();
		// Distant terrain is the visual soft tier. Its refresh need not wait for the
		// outer 5x5 data ring to attach; the native 3x3 demand area is enough.
		const bool settled = anchor::DemandReady();
		if (!g_hidden) {
			if (!settled) {
				g_settledSince = 0;
				return;
			}
			if (!g_settledSince) {
				g_settledSince = now;
			}
			if (g_afterLoad && now - g_settledSince >= 3000) {
				g_afterLoad = false;
				g_lastWasSecond = false;
				StartRefresh("after a load");
			}
			else if (g_secondAt && now >= g_secondAt) {
				g_secondAt = 0;
				g_lastWasSecond = true;
				StartRefresh("follow-up");
			}
			return;
		}
		const ULONGLONG held = now - g_hiddenAt;
		if (held < kMinHideMs || (!settled && held < kMaxHideMs)) {
			return;
		}
		const auto owner = Ptr(kLodToggleOwner);
		if (owner) {
			At<std::uint32_t>(owner, kLodToggleFlag) = g_original;
		}
		g_hidden = false;
		++g_refreshes;
		if (!g_lastWasSecond) {
			g_secondAt = GetTickCount64() + 15000;  // one follow-up refresh
		}
		g_lastWasSecond = false;
		logger::Write("Distant: LOD refresh #%u: distant LOD shown again after %.1f s (%s); flag back to %u", g_refreshes, double(held) / 1000.0,
			settled ? "world settled" : "8 s limit, world not settled", g_original);
	}

	void AfterShift(std::int32_t cx, std::int32_t cy, std::uintptr_t)
	{
		{
			char why[48];
			std::snprintf(why, sizeof(why), "shift %d, %d", cx, cy);
			g_lastWasSecond = false;
			g_secondAt = 0;
			StartRefresh(why);
		}
		// 0.3: did the shift move the other roots' nodes? (moved = world changed by -shift x 100 m)
		const float ex = float(cx) * off::kCellSize, ey = float(cy) * off::kCellSize;
		for (std::size_t i = 0; i < g_roots.size() && i < g_before.size(); ++i) {
			unsigned moved = 0, same = 0, other = 0, worldIsLocal = 0, farNodes = 0;
			for (const auto& n : g_before[i]) {
				const float* w = reinterpret_cast<const float*>(n.node + off::kNodeWorldT);
				if (std::fabs(n.wx - n.lx) < 0.01f && std::fabs(n.wy - n.ly) < 0.01f) ++worldIsLocal;
				if (std::hypot(n.wx, n.wy) > 2000.0f) ++farNodes;
				if (std::hypot(w[0] + ex - n.wx, w[1] + ey - n.wy) < 0.5f && (cx || cy)) ++moved;
				else if (std::hypot(w[0] - n.wx, w[1] - n.wy) < 0.01f) ++same;
				else ++other;
			}
			logger::Write("Distant: root %p across shift (%d, %d): %zu sampled nodes: moved with the shift %u, unchanged %u, changed otherwise %u; %u positioned directly (world = local), %u more than 2 km from the origin",
				reinterpret_cast<void*>(g_roots[i]), cx, cy, g_before[i].size(), moved, same, other, worldIsLocal, farNodes);
		}
		const auto root = modscene::SceneRoot();
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto rootNode = root ? AsNode(root) : 0;
		if (!rootNode || !player) {
			logger::Write("Distant: shift (%d, %d): no scene root", cx, cy);
			return;
		}
		const auto pp = At<game::Vec3>(player, off::kRefPosition);
		const auto ch = At<std::uintptr_t*>(rootNode, off::kNodeChildren);
		const auto cc = At<std::uint16_t>(rootNode, off::kNodeChildCount);
		logger::Write("Distant: shift (%d, %d): scene root %s with %u branches; per branch (6 levels): nodes, nodes > 2 km from the player, farthest, top classes",
			cx, cy, modscene::RttiName(root), unsigned(cc));
		for (std::uint16_t i = 0; ch && i < cc; ++i) {
			if (!ch[i]) continue;
			Census c;
			Walk(ch[i], pp, c, 0);
			std::vector<std::pair<unsigned, std::string>> top;
			for (const auto& [k, v] : c.kinds) top.emplace_back(v, k);
			std::sort(top.rbegin(), top.rend());
			std::string kinds;
			for (std::size_t k = 0; k < top.size() && k < 4; ++k) {
				kinds += (k ? ", " : "") + top[k].second + " x" + std::to_string(top[k].first);
			}
			logger::Write("Distant:   branch %u %s: %u nodes, %u far, farthest %.0f m; %s", unsigned(i), modscene::RttiName(ch[i]), c.nodes, c.farNodes, c.farthest,
				kinds.c_str());
		}
	}
}
