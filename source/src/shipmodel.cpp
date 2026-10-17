#include "shipmodel.h"

#include "addresses.h"
#include "game.h"
#include "camtune.h"
#include "log.h"
#include "telemetry.h"

#include <Windows.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace shipmodel
{
	namespace
	{
		// NiAVObject / NiNode layout (1.16.244; see the engine reference).
		constexpr std::uint32_t kName = 0x10;          // BSFixedString
		constexpr std::uint32_t kParent = 0x38;
		constexpr std::uint32_t kLocal = 0x40;         // rotation 3x4 floats, translate +0x30, scale +0x3C
		constexpr std::uint32_t kWorldRotate = 0x80;     // 3x4 floats
		constexpr std::uint32_t kWorldTranslate = 0xB0;
		constexpr float kCaptureDistance = 400.0f;      // the hull is loaded within this distance of the player
		constexpr float kShipRadius = 60.0f;            // ship parts in a shared cell: within this of the ship
		constexpr float kStandingEye = 1.65f;           // first-person camera height above the player's feet
		constexpr float kSeatedEye = 1.15f;             // eye height above a seat point
		constexpr std::uint32_t kBound = 0x100;        // centre xyz, radius +0x0C
		constexpr std::uint32_t kCollision = 0x110;    // collision object (its +0x10 points back to the node)
		constexpr std::uint32_t kFlags = 0x118;        // bit 0: hidden
		constexpr std::uint32_t kChildren = 0x138;
		constexpr std::uint32_t kChildCount = 0x142;
		// vtable slots
		constexpr std::uint32_t kVIsNode = 0x20;
		constexpr std::uint32_t kVCreateClone = 0x148;
		constexpr std::uint32_t kVProcessClone = 0x178;
		constexpr std::uint32_t kVUpdateDownward = 0x248;
		constexpr std::uint32_t kVAttachChild = 0x2A0;
		constexpr std::uint32_t kVDetachChild = 0x2B8;

		std::mutex g_lock;
		struct Shared
		{
			bool want = false;
			std::uintptr_t vehicleTarget = 0;
			bool capture = false;
			bool capturePilot = false;
			bool pilotReady = false;
			float pitch = 0;
			float roll = 0;
			float heading = 0;
			bool haveHeading = false;
			bool reset = false;
			bool ready = false;
			bool unavailable = false;
			float cockpit[3] = {};
		} g_shared;

		// Owned by the player-update thread.
		struct Local
		{
			std::uintptr_t clone = 0;       // our reference held (never released: no destructor risk)
			std::uintptr_t ship = 0;        // ship it was cloned from
			std::uintptr_t attachedTo = 0;  // player root it is attached to
			std::uintptr_t vehicle = 0;   // vehicle ref whose visual children were hidden
			std::vector<std::uintptr_t> hidden;
			float cockpit[3] = {};
			float containerRot[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
			std::uintptr_t pilot = 0;  // seated snapshot of the player (our reference held)
			ULONGLONG lastPlaceLog = 0;
			double placeMsMax = 0;
			ULONGLONG lastMissLog = 0, lastAutoCapture = 0;
		} g;

		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		template <class Fn>
		Fn VFn(std::uintptr_t obj, std::uint32_t slot) { return reinterpret_cast<Fn>((*reinterpret_cast<void***>(obj))[slot / 8]); }

		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		std::uintptr_t AsNode(std::uintptr_t obj) { return obj ? VFn<std::uintptr_t (*)(std::uintptr_t)>(obj, kVIsNode)(obj) : 0; }
		void AddRef(std::uintptr_t obj) { InterlockedIncrement(reinterpret_cast<volatile LONG*>(obj + 8)); }

		// Reads a node name; tolerant of the string layout (direct chars or a pool entry with a 0x18 header).
		bool ReadName(std::uintptr_t obj, char* out, size_t cap)
		{
			out[0] = 0;
			__try {
				const auto p = *reinterpret_cast<const char* const*>(obj + kName);
				if (!p) {
					return false;
				}
				for (const char* s : { p, p + 0x18 }) {
					size_t n = 0;
					while (n + 1 < cap && s[n] >= 0x20 && s[n] < 0x7F) {
						++n;
					}
					if (n >= 2 && s[n] == 0) {
						std::memcpy(out, s, n);
						out[n] = 0;
						return true;
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
			out[0] = 0;
			return false;
		}

		bool Contains(const char* s, const char* sub)
		{
			for (; *s; ++s) {
				size_t i = 0;
				while (sub[i] && std::tolower(static_cast<unsigned char>(s[i])) == sub[i]) {
					++i;
				}
				if (!sub[i]) {
					return true;
				}
			}
			return false;
		}

		template <class F>
		void Walk(std::uintptr_t obj, int depth, int maxDepth, F&& f)
		{
			if (!obj) {
				return;
			}
			f(obj, depth);
			if (depth >= maxDepth) {
				return;
			}
			if (const auto node = AsNode(obj)) {
				const auto n = At<std::uint16_t>(node, kChildCount);
				const auto kids = At<std::uintptr_t*>(node, kChildren);
				for (unsigned i = 0; kids && i < n; ++i) {
					Walk(kids[i], depth + 1, maxDepth, f);
				}
			}
		}

		// Only visual: drop collision objects (a node with one takes its world transform from physics).
		int StripCollision(std::uintptr_t root)
		{
			int stripped = 0;
			Walk(root, 0, 64, [&](std::uintptr_t o, int) {
				const auto c = At<std::uintptr_t>(o, kCollision);
				if (c && At<std::uintptr_t>(c, 0x10) == o) {
					At<std::uintptr_t>(o, kCollision) = 0;  // the cloned collision object is leaked, never added to a world
					++stripped;
				}
			});
			return stripped;
		}

		std::uintptr_t HomeShip(std::uintptr_t player)
		{
			std::uintptr_t ship = 0;
			reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t*)>(game::Base() + addr::kGetPlayerHomeShip)(player, &ship);
			if (ship) {
				reinterpret_cast<void (*)(std::uintptr_t)>(game::Base() + addr::kRefRelease)(ship);  // ships persist; we keep the pointer only for comparison
			}
			return ship;
		}

		std::uintptr_t Root3D(std::uintptr_t ref)
		{
			const auto data = At<std::uintptr_t>(ref, off::kRefLoadedData);
			return data ? At<std::uintptr_t>(data, off::kLoadedData3D) : 0;
		}

		// Engine clone of one 3D tree (NiCloningProcess as the clone pools build it, FUN_140659c00); collision removed.
		std::uintptr_t CloneTree(std::uintptr_t root, int* stripped)
		{
			alignas(16) std::uint8_t cp[0x80] = {};
			const auto cpp = reinterpret_cast<std::uintptr_t>(cp);
			At<std::uint32_t>(cpp, 0x20) = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kCloneCopyType);
			At<std::uint16_t>(cpp, 0x24) = 0x2400;
			At<std::uint8_t>(cpp, 0x26) = 1;
			const float one = *reinterpret_cast<float*>(game::Base() + addr::kCloneScale);
			for (int i = 0; i < 3; ++i) {
				At<float>(cpp, 0x28 + i * 4) = one;
			}
			const auto clone = VFn<std::uintptr_t (*)(std::uintptr_t, void*)>(root, kVCreateClone)(root, cp);
			if (!clone) {
				return 0;
			}
			VFn<void (*)(std::uintptr_t, void*)>(root, kVProcessClone)(root, cp);  // (cloning-process map memory is leaked)
			AddRef(clone);
			if (stripped) {
				*stripped += StripCollision(clone);
			}
			At<std::uint64_t>(clone, kFlags) &= ~1ull;
			return clone;
		}

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
			inline static void* const kVtbl[4] = { reinterpret_cast<void*>(&Noop), reinterpret_cast<void*>(&Noop), reinterpret_cast<void*>(&Invoke),
				reinterpret_cast<void*>(&Noop) };
		};

		// The ship's look: its hull is the module references in its exterior ship cell (the ship's own parent cell);
		// the ship reference's own 3D is only a placeholder. Each module's 3D is copied under one container at its
		// place relative to the ship (world axes); the container undoes the ship's rotation (transpose of the ship
		// root's engine-made world rotation), so under the player's root the nose points where the player faces.
		// Only possible while the hull is loaded (the player outside near the ship).
		bool Clone(std::uintptr_t player, bool vehicleMode)
		{
			const auto ship = HomeShip(player);
			if (!ship) {
				return false;
			}
			const auto pc = At<std::uintptr_t>(player, off::kRefParentCell);
			if (!pc || !At<std::uintptr_t>(pc, off::kCellWorldspace)) {
				return false;  // inside (the ship interior): the hull is not loaded
			}
			const auto sp = At<game::Vec3>(ship, off::kRefPosition);
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			if (std::hypot(sp.x - pp.x, sp.y - pp.y) > kCaptureDistance) {
				return false;
			}
			const auto shipRoot = AsNode(Root3D(ship));
			const auto cell = At<std::uintptr_t>(ship, off::kRefParentCell);
			if (!shipRoot || !cell) {
				return false;
			}
			const bool sharedCell = At<std::uintptr_t>(cell, off::kCellWorldspace) != 0;  // an exterior ship cell has none
			const ULONGLONG cloneStart = GetTickCount64();
			std::vector<std::uintptr_t> refs;
			CollectRefs cb{ CollectRefs::kVtbl, &refs };
			EngineFn<int (*)(std::uintptr_t, CollectRefs*, char)>(addr::kForEachRefInCell)(cell, &cb, 0);

			int stripped = 0;
			const auto container = CloneTree(shipRoot, &stripped);  // the placeholder root: an empty node to hold the hull
			if (!container) {
				logger::Write("Ship model: container clone failed");
				return false;
			}
			const auto shipT = At<game::Vec3>(shipRoot, kWorldTranslate);
			const float yaw = At<game::Vec3>(ship, off::kRefRotation).z;
			const auto toShip = [&](const game::Vec3& w, float o[3]) {
				const float dx = w.x - shipT.x, dy = w.y - shipT.y;
				o[0] = dx * std::cos(yaw) - dy * std::sin(yaw);  // right = (cos, -sin)
				o[1] = dx * std::sin(yaw) + dy * std::cos(yaw);  // forward = (sin, cos)
				o[2] = w.z - shipT.z;
			};
			int modules = 0;
			int engineNodesLogged = 0;
			bool cockpit = false;
			float forwardMost = -1e30f;
			char name[96];
			for (const auto r : refs) {
				if (r == ship) {
					continue;
				}
				const auto root = Root3D(r);
				if (!root) {
					continue;
				}
			if (vehicleMode) {
				// Exterior modules only. The old player clone copied thousands of cell objects at a spaceport.
				char moduleName[96];
				ReadName(root, moduleName, sizeof(moduleName));
				if (!Contains(moduleName, "smod_")) {
					continue;
				}
			}
				if (sharedCell) {
					// The ship stands in a shared world cell (e.g. a spaceport): only ship parts near this ship.
					const auto rp = At<game::Vec3>(r, off::kRefPosition);
					if (std::hypot(rp.x - sp.x, rp.y - sp.y) > kShipRadius) {
						continue;
					}
					char nm[96];
					ReadName(root, nm, sizeof(nm));
					if (!(Contains(nm, "smod_") || Contains(nm, "ship"))) {
						continue;
					}
				}
				const auto copy = CloneTree(root, &stripped);
				if (!copy) {
					continue;
				}
				// Local = the module's world transform, translation relative to the ship (world axes).
				float* m = reinterpret_cast<float*>(copy + kLocal);
				std::memcpy(m, reinterpret_cast<const float*>(root + kWorldRotate), 12 * sizeof(float));
				const auto wt = At<game::Vec3>(root, kWorldTranslate);
				m[12] = wt.x - shipT.x;
				m[13] = wt.y - shipT.y;
				m[14] = wt.z - shipT.z;
				m[15] = At<float>(root, kWorldTranslate + 0xC);  // scale
				VFn<void (*)(std::uintptr_t, std::uintptr_t, char)>(container, kVAttachChild)(container, copy, 0);
				++modules;
				float s[3];
				toShip(wt, s);
				ReadName(root, name, sizeof(name));
				if (vehicleMode && Contains(name, "engine") && engineNodesLogged < 48) {
					// Report candidate exhaust sockets without assuming their name, type or that a copied emitter runs.
					char childName[96];
					Walk(root, 0, 12, [&](std::uintptr_t o, int depth) {
						if (engineNodesLogged >= 48 || depth == 0 || !ReadName(o, childName, sizeof(childName))) return;
						if (!Contains(childName, "fx") && !Contains(childName, "exhaust") && !Contains(childName, "thrust") &&
							!Contains(childName, "flame") && !Contains(childName, "boost") && !Contains(childName, "nozzle")) return;
						float q[3];
						toShip(At<game::Vec3>(o, kWorldTranslate), q);
						logger::Write("Ship vehicle: engine node %s/%s at (%.2f, %.2f, %.2f)", name, childName, q[0], q[1], q[2]);
						++engineNodesLogged;
					});
				}
				if (!cockpit && name[0] && Contains(name, "cockpit")) {
					std::memcpy(g.cockpit, s, sizeof(s));
					cockpit = true;
					logger::Write("Ship model: cockpit module '%s' at (%.1f, %.1f, %.1f)", name, s[0], s[1], s[2]);
					// The pilot's eye inside the cockpit (the space flight first-person view): a camera node, else the
					// pilot seat. Logged so the choice can be checked.
					int listed = 0;
					bool haveCam = false, haveSeat = false;
					float cam[3] = {}, seat[3] = {};
					char sub[96];
					Walk(root, 0, 12, [&](std::uintptr_t o, int depth) {
						if (depth == 0 || !ReadName(o, sub, sizeof(sub))) {
							return;
						}
						const bool isCam = Contains(sub, "camera") || Contains(sub, "cam_") || Contains(sub, "eye");
						const bool isSeat = Contains(sub, "seat") || Contains(sub, "pilot") || Contains(sub, "chair") || Contains(sub, "furniture");
						if (!isCam && !isSeat) {
							return;
						}
						float q[3];
						toShip(At<game::Vec3>(o, kWorldTranslate), q);
						if (listed++ < 30) {
							logger::Write("Ship model:   cockpit node %s at (%.2f, %.2f, %.2f)", sub, q[0], q[1], q[2]);
						}
						if (isCam && !haveCam) {
							std::memcpy(cam, q, sizeof(q));
							haveCam = true;
						}
						else if (isSeat && !haveSeat) {
							std::memcpy(seat, q, sizeof(q));
							haveSeat = true;
						}
					});
					// g.cockpit = the ship point that goes at the player's feet, so the pilot's eye is at the camera.
					if (haveCam) {
						g.cockpit[0] = cam[0];
						g.cockpit[1] = cam[1];
						g.cockpit[2] = cam[2] - kStandingEye;
						logger::Write("Ship model: first-person eye = cockpit camera node");
					}
					else if (haveSeat) {
						g.cockpit[0] = seat[0];
						g.cockpit[1] = seat[1];
						g.cockpit[2] = seat[2] + kSeatedEye - kStandingEye;
						logger::Write("Ship model: first-person eye = pilot seat + %.2f m", kSeatedEye);
					}
					else {
						g.cockpit[2] += kSeatedEye - kStandingEye;
						logger::Write("Ship model: no seat or camera node in the cockpit; eye = module origin + %.2f m", kSeatedEye);
					}
				}
				else if (!cockpit && s[1] > forwardMost) {
					forwardMost = s[1];
					std::memcpy(g.cockpit, s, sizeof(s));
				}
				if (modules <= 40) {
					logger::Write("Ship model: module %s at (%.1f, %.1f, %.1f)", name[0] ? name : "?", s[0], s[1], s[2]);
				}
			}
			if (modules == 0) {
				logger::Write("Ship model: no loaded hull modules in the ship's cell (%zu refs)", refs.size());
				return false;
			}
			if (!cockpit) {
				logger::Write("Ship model: no cockpit module, using the front-most module (%.1f, %.1f, %.1f)", g.cockpit[0], g.cockpit[1], g.cockpit[2]);
			}
			// Container rotation = transpose of the ship's world rotation (undo the ship's facing).
			const float* sr = reinterpret_cast<const float*>(shipRoot + kWorldRotate);
			for (int row = 0; row < 3; ++row) {
				for (int col = 0; col < 3; ++col) {
					g.containerRot[row * 4 + col] = sr[col * 4 + row];
				}
				g.containerRot[row * 4 + 3] = 0.0f;
			}
			g.clone = container;
			g.ship = ship;
			logger::Write("Ship model: copied the hull (%d modules, %d collision objects removed) in %.0f ms", modules, stripped,
				double(GetTickCount64() - cloneStart));
			telemetry::Record("ship_model_clone", "\"modules\":%d,\"stripped\":%d,\"cockpit\":[%.1f,%.1f,%.1f]", modules, stripped, g.cockpit[0], g.cockpit[1],
				g.cockpit[2]);
			std::lock_guard lk(g_lock);
			g_shared.ready = true;
			g_shared.unavailable = false;
			std::memcpy(g_shared.cockpit, g.cockpit, sizeof(g.cockpit));
			return true;
		}

		void RestoreBody()
		{
			for (const auto o : g.hidden) {
				At<std::uint64_t>(o, kFlags) &= ~1ull;
			}
			g.hidden.clear();
		}

		void Detach()
		{
			if (!g.attachedTo) {
				return;
			}
			if (g.pilot && At<std::uintptr_t>(g.pilot, kParent) == g.attachedTo) {
				VFn<void (*)(std::uintptr_t, std::uintptr_t)>(g.attachedTo, kVDetachChild)(g.attachedTo, g.pilot);
			}
			if (At<std::uintptr_t>(g.clone, kParent) == g.attachedTo) {
				VFn<void (*)(std::uintptr_t, std::uintptr_t)>(g.attachedTo, kVDetachChild)(g.attachedTo, g.clone);
				RestoreBody();
			}
			g.hidden.clear();
			g.attachedTo = 0;
			g.vehicle = 0;
			logger::Write("Ship model: hidden");
		}

		void Attach(std::uintptr_t root, std::uintptr_t vehicle)
		{
			// The player's own body is hidden while flying (third person shows the ship only; no pilot).
			{
				const auto n = At<std::uint16_t>(root, kChildCount);
				const auto kids = At<std::uintptr_t*>(root, kChildren);
				for (unsigned i = 0; kids && i < n; ++i) {
					if (kids[i] && kids[i] != g.clone && kids[i] != g.pilot && !(At<std::uint64_t>(kids[i], kFlags) & 1)) {
						At<std::uint64_t>(kids[i], kFlags) |= 1;
						g.hidden.push_back(kids[i]);
					}
				}
			}
			VFn<void (*)(std::uintptr_t, std::uintptr_t, char)>(root, kVAttachChild)(root, g.clone, 0);
			g.attachedTo = root;
			g.vehicle = vehicle;
			logger::Write("Ship model: shown on the player (root flags 0x%llX, %zu body parts hidden)", At<std::uint64_t>(root, kFlags), g.hidden.size());
		}

		// Clone placement under the player root: the cockpit on the player, nose pitched with the view.
		void Place(float pitch, float roll, float yawDelta)
		{
			float* m = reinterpret_cast<float*>(g.clone + kLocal);
			// The cockpit on the player: offsets in the ship's frame (x right, y forward), as the player root's axes.
#if defined(SP_FEATURE_VEHICLE)
			const float t[3] = { -g.cockpit[0], -g.cockpit[1], -g.cockpit[2] };  // unused by vehicle placement
#else
			const auto eye = camtune::EyeOffset();  // in-game tuning (camtune.h)
			const float t[3] = { -(g.cockpit[0] + eye.right), -(g.cockpit[1] + eye.forward), -(g.cockpit[2] + eye.up) };
#endif
			// Attitude about the cockpit: A = Rz(heading - player heading) * Rx(pitch) * Ry(roll); local = A * containerRot,
			// translation = A * t. (Rz: positive turns the nose right, as the game's yaw.)
			const float cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll), sr = std::sin(roll);
			const float B[3][3] = { { cr, 0.0f, sr }, { sp * sr, cp, -sp * cr }, { -cp * sr, sp, cp * cr } };
			const float cy = std::cos(yawDelta), sy = std::sin(yawDelta);
			const float Z[3][3] = { { cy, sy, 0.0f }, { -sy, cy, 0.0f }, { 0.0f, 0.0f, 1.0f } };
			float A[3][3];
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 3; ++c) {
					A[r][c] = Z[r][0] * B[0][c] + Z[r][1] * B[1][c] + Z[r][2] * B[2][c];
				}
			}
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 3; ++c) {
					m[r * 4 + c] = A[r][0] * g.containerRot[c] + A[r][1] * g.containerRot[4 + c] + A[r][2] * g.containerRot[8 + c];
				}
				m[r * 4 + 3] = 0.0f;
				m[12 + r] = A[r][0] * t[0] + A[r][1] * t[1] + A[r][2] * t[2];
			}
			m[15] = 1.0f;  // scale
			alignas(16) std::uint8_t data[0x80] = {};  // NiUpdateData: no controllers, no physics
			LARGE_INTEGER t0, t1, f;
			QueryPerformanceCounter(&t0);
			VFn<void (*)(std::uintptr_t, void*)>(g.clone, kVUpdateDownward)(g.clone, data);
			QueryPerformanceCounter(&t1);
			QueryPerformanceFrequency(&f);
			const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
			g.placeMsMax = std::max(g.placeMsMax, ms);
			const ULONGLONG nowMs = GetTickCount64();
			if (nowMs - g.lastPlaceLog > 10000) {
				g.lastPlaceLog = nowMs;
				logger::Write("Ship model: per-frame update %.2f ms (worst %.2f ms in 10 s)", ms, g.placeMsMax);
				telemetry::Record("ship_model_update", "\"ms\":%.2f,\"worst_ms\":%.2f", ms, g.placeMsMax);
				g.placeMsMax = 0;
			}
		}

		void PlaceOnVehicle()
		{
			// Module transforms were copied in world axes relative to the landed ship. Undo the source ship's
			// world rotation; the vehicle root then supplies the moving chassis pose, including bank and pitch.
			float* m = reinterpret_cast<float*>(g.clone + kLocal);
			std::memcpy(m, g.containerRot, sizeof(g.containerRot));
			m[12] = m[13] = m[14] = 0.0f;
			m[15] = 1.0f;
			alignas(16) std::uint8_t data[0x80] = {};
			VFn<void (*)(std::uintptr_t, void*)>(g.clone, kVUpdateDownward)(g.clone, data);
		}
	}

	void Show(bool show)
	{
		std::lock_guard lk(g_lock);
		g_shared.want = show;
	}

	void ShowOnVehicle(std::uintptr_t vehicle)
	{
		std::lock_guard lk(g_lock);
		g_shared.vehicleTarget = vehicle;
		g_shared.want = vehicle != 0;
	}

	void SetPitch(float radians)
	{
		std::lock_guard lk(g_lock);
		g_shared.pitch = radians;
	}

	void SetAttitude(float pitch, float roll)
	{
		std::lock_guard lk(g_lock);
		g_shared.pitch = pitch;
		g_shared.roll = roll;
	}

	void SetHeading(float yaw)
	{
		std::lock_guard lk(g_lock);
		g_shared.heading = yaw;
		g_shared.haveHeading = true;
	}

	void Capture()
	{
		std::lock_guard lk(g_lock);
		g_shared.capture = true;
	}

	void CapturePilot()
	{
		std::lock_guard lk(g_lock);
		g_shared.capturePilot = true;
	}

	bool PilotReady()
	{
		std::lock_guard lk(g_lock);
		return g_shared.pilotReady;
	}

	bool CockpitOffset(float out[3])
	{
		std::lock_guard lk(g_lock);
		if (!g_shared.ready) {
			return false;
		}
		std::memcpy(out, g_shared.cockpit, sizeof(g_shared.cockpit));
		return true;
	}

	bool Ready()
	{
		std::lock_guard lk(g_lock);
		return g_shared.ready;
	}

	bool Unavailable()
	{
		std::lock_guard lk(g_lock);
		return g_shared.unavailable;
	}

	void Reset()
	{
		std::lock_guard lk(g_lock);
		g_shared = Shared{};
		g_shared.reset = true;
	}

	void OnPlayerUpdate(std::uintptr_t player)
	{
		Shared s;
		{
			std::lock_guard lk(g_lock);
			s = g_shared;
			g_shared.capture = false;
			g_shared.capturePilot = false;
			g_shared.reset = false;
		}
		if (s.reset) {
			// The player's 3D (and the body parts we hid) are gone with the load; keep nothing.
			g = Local{};
		}
		const auto root = AsNode(Root3D(s.vehicleTarget ? s.vehicleTarget : player));
		if (s.capturePilot && root) {
			// The player as posed right now (seated): a frozen copy flown as the pilot.
			int stripped = 0;
			const auto copy = CloneTree(root, &stripped);
			if (copy) {
				g.pilot = copy;
				std::lock_guard lk(g_lock);
				g_shared.pilotReady = true;
			}
			logger::Write("Ship model: pilot snapshot %s (%d collision objects removed)", copy ? "taken" : "failed", stripped);
		}

		if (!s.want) {
			if (g.attachedTo) {
				if (g.attachedTo == root || (g.vehicle && Root3D(g.vehicle) == g.attachedTo)) {
					Detach();
				}
				else {
					At<std::uintptr_t>(g.clone, kParent) = 0;  // the old player 3D is gone
					g.hidden.clear();
					g.attachedTo = 0;
					g.vehicle = 0;
				}
			}
		}
		const ULONGLONG nowMs = GetTickCount64();
		// The vehicle-mode ship visual must be prepared while the landed exterior's module 3D is loaded.
		// Once the player enters the interior, Clone() correctly refuses to read unloaded exterior modules.
		// Limit attempts to one every five seconds and retain the first successful assembly across cell changes.
#if defined(SP_FEATURE_VEHICLE)
		const bool autoCapture = !g.clone && nowMs - g.lastAutoCapture >= 5000;
#else
		const bool autoCapture = false;  // ship flight removed: no background hull copies
#endif
		if (autoCapture) {
			g.lastAutoCapture = nowMs;
		}
		if ((s.want || s.capture || autoCapture) && (!g.clone || (!g.attachedTo && HomeShip(player) != g.ship))) {
			if (!Clone(player,
#if defined(SP_FEATURE_VEHICLE)
				true
#else
				s.vehicleTarget != 0
#endif
			)) {
				const ULONGLONG ms = GetTickCount64();
				if (!autoCapture && ms - g.lastMissLog > 5000) {
					g.lastMissLog = ms;
					logger::Write("Ship model: the home ship's 3D is not loaded here, nothing to copy yet");
				}
				if (s.want && !g.clone) {
					std::lock_guard lk(g_lock);
					g_shared.unavailable = true;
				}
			}
		}
		if (!s.want || !g.clone || !root) {
			return;
		}
		if (g.attachedTo != root || At<std::uintptr_t>(g.clone, kParent) != root) {
			if (g.attachedTo && g.attachedTo != root) {
				// A different vehicle may still be loaded. Restore it before moving the hull.
				if (g.vehicle && Root3D(g.vehicle) == g.attachedTo) {
					Detach();
				}
				else {
					At<std::uintptr_t>(g.clone, kParent) = 0;
				}
			}
			g.hidden.clear();
			Attach(root, s.vehicleTarget);
		}
		if (s.vehicleTarget) {
			PlaceOnVehicle();
		}
		else {
			const float playerYaw = At<game::Vec3>(player, off::kRefRotation).z;
			Place(s.pitch, s.roll, s.haveHeading ? s.heading - playerYaw : 0.0f);
		}
	}
}
