#include "hoverui.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "notify.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Scaleform GFx from the decompile of 1.16.244 (layouts as CommonLibSF's RE::Scaleform::GFx, checked against the game):
//  - GFx::Value (0x30): +0x10 ObjectInterface*, +0x18 type (0x8F mask: 2 bool, 5 number, 6 string, 8 object, 10 display
//    object; 0x40 = managed), +0x20 value. FUN_142555B20 (GameMenuBase::LoadMovie) uses exactly this.
//  - ObjectInterface: GetMember FUN_1433528A0, SetMember FUN_143352C90 ("SetMember aborted"), Invoke FUN_1433530B0,
//    ObjectRelease FUN_1433524A0 (all (iface, data, ...), last argument "is display object").
//  - Movie::CreateObject FUN_143189060: movie+0x18 (AS3 MovieRoot), vtable +0x170 (slot 46, FUN_1433676E0,
//    "Exception in CreateObject"): (root, Value* out, const char* class, const Value* args, uint32 count).
//  - IMenu: +0x58 the root object (Value), +0x88 the movie. HUDMenu vtable 0x144D099D0 (RTTI .?AVHUDMenu@@); its slot 7
//    (FUN_141EC74A0) runs on the HUD's thread but only rarely (1 call in 30 s): used only to learn which menu is the HUD.
//  - MovieImpl vtable 0x143BCFEE8 (RTTI .?AVMovieImpl@GFx@Scaleform@@) slot 24 = Advance (FUN_14318C860:
//    float (movie, float dt, int catchUp, bool capture), calls the AS3 root's AdvanceFrame): the per-frame tick of every
//    movie. The buttons are updated there, for the HUD's movie only.
// hudmenu.swf (JPEXS export): HUDVehicle (root.RightMeters_mc.HUDVehicle_mc) shows GetUpButton_mc with
// new ButtonBaseData("", new UserEventData("VehicleExit", null, ExitVehicleEvent)); BasicButton is exported by
// ButtonClips.swf, which the HUD imports.
namespace hoverui
{
	namespace
	{
		using uptr = std::uintptr_t;

		struct Value
		{
			Value* prev = nullptr;
			Value* next = nullptr;
			void* iface = nullptr;
			std::int32_t type = 0;
			std::int32_t pad = 0;
			union {
				double number;
				const char* string;
				void* object;
				bool boolean;
				std::uint64_t raw = 0;
			};
			std::size_t aux = 0;
		};
		static_assert(sizeof(Value) == 0x30);

		constexpr std::int32_t kBool = 2, kNumber = 5, kString = 6, kManaged = 0x40;
		constexpr std::uint32_t kRvaGetMember = 0x033528A0, kRvaSetMember = 0x03352C90, kRvaInvoke = 0x033530B0, kRvaRelease = 0x033524A0;
		constexpr std::uint32_t kRvaHudMenuVtbl = 0x04D099D0, kRvaHudUpdate = 0x01EC74A0;
		constexpr int kSlotUpdate = 7;
		constexpr std::uint32_t kRvaMovieImplVtbl = 0x03BCFEE8, kRvaMovieAdvance = 0x0318C860, kRvaMovieDtor = 0x03198DC0;
		constexpr int kSlotAdvance = 24;

		bool IsObject(const Value& v) { const int t = v.type & 0x8F; return t == 8 || t == 9 || t == 10; }
		bool IsDisplay(const Value& v) { return (v.type & 0x8F) == 10; }
		uptr Fn(std::uint32_t rva) { return game::Base() + rva; }

		bool GetMember(const Value& o, const char* name, Value* out)
		{
			return reinterpret_cast<bool (*)(void*, void*, const char*, Value*, bool)>(Fn(kRvaGetMember))(o.iface, o.object, name, out, IsDisplay(o));
		}
		bool SetMember(const Value& o, const char* name, const Value& v)
		{
			return reinterpret_cast<bool (*)(void*, void*, const char*, const Value*, bool)>(Fn(kRvaSetMember))(o.iface, o.object, name, &v, IsDisplay(o));
		}
		bool Invoke(const Value& o, const char* name, const Value* args, std::size_t n)
		{
			return reinterpret_cast<bool (*)(void*, void*, Value*, const char*, const Value*, std::size_t, bool)>(Fn(kRvaInvoke))(o.iface, o.object, nullptr, name, args,
				n, IsDisplay(o));
		}
		bool InvokeR(const Value& o, const char* name, const Value* args, std::size_t n, Value* result)
		{
			return reinterpret_cast<bool (*)(void*, void*, Value*, const char*, const Value*, std::size_t, bool)>(Fn(kRvaInvoke))(o.iface, o.object, result, name, args,
				n, IsDisplay(o));
		}
		void Release(Value& v)
		{
			if ((v.type & kManaged) && v.iface && v.object) {
				reinterpret_cast<void (*)(void*, Value*, void*)>(Fn(kRvaRelease))(v.iface, &v, v.object);
			}
			v = Value{};
		}
		bool CreateObject(uptr movie, Value* out, const char* cls, const Value* args, std::uint32_t n)
		{
			const uptr root = *reinterpret_cast<uptr*>(movie + 0x18);
			if (!root) return false;
			auto** vt = *reinterpret_cast<void***>(root);
			reinterpret_cast<void (*)(uptr, Value*, const char*, const Value*, std::uint32_t)>(vt[46])(root, out, cls, args, n);
			return IsObject(*out);
		}
		// A borrowed view of a managed value for passing as an argument (the owner keeps it alive).
		Value Arg(const Value& v)
		{
			Value a;
			a.iface = v.iface;
			a.type = v.type & ~kManaged;
			a.raw = v.raw;
			return a;
		}
		Value Str(const char* s) { Value v; v.type = kString; v.string = s; return v; }
		Value Num(double d) { Value v; v.type = kNumber; v.number = d; return v; }
		Value Bool(bool b) { Value v; v.type = kBool; v.boolean = b; return v; }

		// ---- Layout (edited on the main thread, applied in the HUD update) ----
		// In the rover widget's own coordinates:
		//  - WASD cluster: by the S key's centre (cx, cy) and the key distance d (A and D at cx -/+ d level with S, W at
		//    cy - d). Labels all under (or all above) their keys.
		//  - Column: Gear-, Gear+, Hover top to bottom, right-aligned on their key boxes' right edge (colRight), key centres
		//    colTop, colTop + colStep, colTop + 2 colStep.
		//  - Gear box: a key box with the gear number only, by its centre (gx, gy).
		// Saved to Documents\My Games\Starfield\SFSE\Logs\SeamlessVehicle\hoverui.ini.
		enum { kW, kA, kS, kD, kGearDown, kGearUp, kHover, kGear, kButtons };
		// Editor selections: each element (W, A, S, D, Gear-, Gear+, Hover, Gear box, Auto, Auto line, Gang), each of the
		// rover widget's own clips (kSelNative + i, e.g. the Exit button), then the groups.
		enum { kSelAuto = kButtons, kSelLine, kSelGang, kSelRows, kSelMode, kSelCluster, kSelColumn, kSelExtras, kSelAll, kSelNative = 100 };
		constexpr int kFootEls = 9, kCarEls = 5;
		const char* const kFootNames[kFootEls] = { "summon", "follow", "getin", "status", "gang", "pass", "eject", "vehicles", "automode" };
		const char* const kCarNames[kCarEls] = { "auto", "autoline", "ganginveh", "vehiclesinveh", "automodeinveh" };
		constexpr double kDefOff[kButtons][2] = { { 0.0, -31.0 }, { -23.0, 0.0 }, { 0.0, 0.0 }, { 24.0, 0.0 }, { -2.0, 0.0 } };
		struct Layout_t
		{
			std::mutex lock;
			// Minted from the player's hoverui.ini of 2026-10-17 20:4x (every element); F7 still edits, and hoverui.ini overrides.
			double cx = 236.0, cy = -170.0, d = 60.0;  // d <= 0: measured once (W's key + label clear the S row)
			bool labelsAbove = false;  // unused: Accelerate's label is above W, the others under their keys
			double colRight = 650.0, colTop = -246.0, colStep = 54.5;
			double gx = 477.4, gy = 50.2;
			double sx = 399.0, sy = 1011.0;  // the scanner call button's key box centre, in the scanner root's coordinates
			double fx = 325.0, fy = 1011.0;  // the scanner follow button's key box centre
			double tx = 361.0, ty = 958.0;  // the scanner status box's centre
			int msel = 3;                    // scanner editor: 0 Summon, 1 Follow, 2 status, 3 all
			// The contextual HUD (2026-10-17): the on-foot panel (Summon, Follow, Get in, status, gang) in RightMeters_mc's
			// coordinates (Summon's key centre; measured from the rover widget once), and the in-vehicle extras' offset.
			double fpx = -268.4, fpy = -160.4;
			bool haveFoot = true;
			double eox = -650.0, eoy = -205.0;
			bool scannerOld = false;          // read as the earlier top-left corner: converted once the button exists
			bool haveCluster = true, haveColumn = true, haveGearBox = true;
			bool oldRows = false;      // only the earlier per-row positions are known: derive the column from them
			double oldX[3] = {}, oldY[3] = {};  // Gear+, Gear-, Hover (the earlier order)
			double off[kButtons][2] = { { kDefOff[0][0], kDefOff[0][1] }, { kDefOff[1][0], kDefOff[1][1] }, { kDefOff[2][0], kDefOff[2][1] }, { kDefOff[3][0], kDefOff[3][1] }, { kDefOff[4][0], kDefOff[4][1] } };  // per element nudges (W, A, S, D, Gear-, Gear+, Hover); the gear box uses gx, gy
			double footOff[kFootEls][2] = { { 0.0, -40.0 }, { 120.0, -39.0 }, { -120.0, -40.0 }, { 110.0, 90.0 }, { -56.0, 45.0 }, { -120.0, -64.0 }, { -120.0, -82.0 }, { -56.0, 89.0 }, { 0.0, -40.0 } };  // per element nudges of the on-foot panel (Summon, Follow, Get in, status, gang, Passenger, Eject, vehicles, Auto mode)
			double carOff[kCarEls][2] = { { 342.0, 172.0 }, { 766.0, 376.0 }, { 342.0, 289.0 }, { 342.0, 333.0 }, { 342.0, 226.0 } };    // per element nudges of the in-vehicle extras (Auto, Auto line, gang, vehicles, Auto mode)
			// The rover widget's own clips (found by name when the widget is first seen) and their nudges.
			std::vector<std::string> natNames;
			std::map<std::string, std::pair<double, double>> natOff;
			bool edit = false;
			int sel = kSelAll;
			int fsel = kFootEls;  // on foot: 0..4 one element, 5 the whole panel
		} g_lay;

		std::wstring IniPath()
		{
			PWSTR docs = nullptr;
			std::wstring path;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
				path = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessVehicle";
				SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
				path += L"\\hoverui.ini";
			}
			CoTaskMemFree(docs);
			return path;
		}

		void SaveLayout()  // caller holds g_lay.lock
		{
			std::ofstream out(IniPath(), std::ios::trunc);
			char line[160];
			out << "; SeamlessVehicle hover buttons, in the rover widget's coordinates\n";
			std::snprintf(line, sizeof(line), "cluster = %.1f %.1f %.1f %d   ; S key centre x y, key distance, labels above (1) or under (0)\n", g_lay.cx, g_lay.cy,
				g_lay.d, g_lay.labelsAbove ? 1 : 0);
			out << line;
			std::snprintf(line, sizeof(line), "column = %.1f %.1f %.1f   ; key boxes' right edge, first key centre y, row step (Gear-, Gear+, Hover)\n", g_lay.colRight,
				g_lay.colTop, g_lay.colStep);
			out << line;
			std::snprintf(line, sizeof(line), "gearbox = %.1f %.1f   ; centre x y\n", g_lay.gx, g_lay.gy);
			out << line;
			const char* names[7] = { "W", "A", "S", "D", "geardown", "gearup", "hover" };
			for (int i = 0; i < 7; ++i) {
				std::snprintf(line, sizeof(line), "nudge_%s = %.1f %.1f\n", names[i], g_lay.off[i][0], g_lay.off[i][1]);
				out << line;
			}
			std::snprintf(line, sizeof(line), "scanner_key = %.1f %.1f   ; the scanner's summon button, key box centre x y\n", g_lay.sx, g_lay.sy);
			out << line;
			std::snprintf(line, sizeof(line), "scanner_follow = %.1f %.1f   ; the scanner's follow button, key box centre x y\n", g_lay.fx, g_lay.fy);
			out << line;
			std::snprintf(line, sizeof(line), "scanner_status = %.1f %.1f   ; the scanner's status box, centre x y\n", g_lay.tx, g_lay.ty);
			out << line;
			if (g_lay.haveFoot) {
				std::snprintf(line, sizeof(line), "foot = %.1f %.1f   ; on-foot panel: Summon's key centre x y\n", g_lay.fpx, g_lay.fpy);
				out << line;
			}
			std::snprintf(line, sizeof(line), "extras = %.1f %.1f   ; in-vehicle Auto / status / gang offset x y\n", g_lay.eox, g_lay.eoy);
			out << line;
			for (int i = 0; i < kFootEls; ++i) {
				std::snprintf(line, sizeof(line), "foot_%s = %.1f %.1f\n", kFootNames[i], g_lay.footOff[i][0], g_lay.footOff[i][1]);
				out << line;
			}
			for (int i = 0; i < kCarEls; ++i) {
				std::snprintf(line, sizeof(line), "nudge_%s = %.1f %.1f\n", kCarNames[i], g_lay.carOff[i][0], g_lay.carOff[i][1]);
				out << line;
			}
			for (const auto& [name, o] : g_lay.natOff) {
				std::snprintf(line, sizeof(line), "native %s = %.1f %.1f   ; the rover widget's own clip, nudge x y\n", name.c_str(), o.first, o.second);
				out << line;
			}
		}

		void LoadLayout()  // caller holds g_lay.lock
		{
			std::ifstream in(IniPath());
			std::string line;
			double row[5][2] = {};
			int rows = 0;
			while (std::getline(in, line)) {
				double a = 0, b = 0, c = 0;
				int above = 0, i = -1;
				if (std::sscanf(line.c_str(), "cluster = %lf %lf %lf %d", &a, &b, &c, &above) >= 3) {
					g_lay.oldRows = false;
					g_lay.cx = a; g_lay.cy = b; g_lay.d = c; g_lay.labelsAbove = above != 0; g_lay.haveCluster = true;
				}
				else if (std::sscanf(line.c_str(), "column = %lf %lf %lf", &a, &b, &c) == 3) {
					g_lay.colRight = a; g_lay.colTop = b; g_lay.colStep = c; g_lay.haveColumn = true;
				}
				else if (line.rfind("nudge_", 0) == 0) {
					const char* names[7] = { "W", "A", "S", "D", "geardown", "gearup", "hover" };
					for (int k = 0; k < 7; ++k) {
						char fmt[48];
						std::snprintf(fmt, sizeof(fmt), "nudge_%s = %%lf %%lf", names[k]);
						if (std::sscanf(line.c_str(), fmt, &a, &b) == 2) { g_lay.off[k][0] = a; g_lay.off[k][1] = b; }
					}
					for (int k = 0; k < kCarEls; ++k) {
						char fmt[48];
						std::snprintf(fmt, sizeof(fmt), "nudge_%s = %%lf %%lf", kCarNames[k]);
						if (std::sscanf(line.c_str(), fmt, &a, &b) == 2) { g_lay.carOff[k][0] = a; g_lay.carOff[k][1] = b; }
					}
				}
				else if (line.rfind("foot_", 0) == 0) {
					for (int k = 0; k < kFootEls; ++k) {
						char fmt[48];
						std::snprintf(fmt, sizeof(fmt), "foot_%s = %%lf %%lf", kFootNames[k]);
						if (std::sscanf(line.c_str(), fmt, &a, &b) == 2) { g_lay.footOff[k][0] = a; g_lay.footOff[k][1] = b; }
					}
				}
				else if (line.rfind("native ", 0) == 0) {
					char name[96] = {};
					if (std::sscanf(line.c_str(), "native %95s = %lf %lf", name, &a, &b) == 3) g_lay.natOff[name] = { a, b };
				}
				else if (std::sscanf(line.c_str(), "scanner_follow = %lf %lf", &a, &b) == 2) { g_lay.fx = a; g_lay.fy = b; }
				else if (std::sscanf(line.c_str(), "foot = %lf %lf", &a, &b) == 2) { g_lay.fpx = a; g_lay.fpy = b; g_lay.haveFoot = true; }
				else if (std::sscanf(line.c_str(), "extras = %lf %lf", &a, &b) == 2) { g_lay.eox = a; g_lay.eoy = b; }
				else if (std::sscanf(line.c_str(), "scanner_status = %lf %lf", &a, &b) == 2) { g_lay.tx = a; g_lay.ty = b; }
				else if (std::sscanf(line.c_str(), "scanner_key = %lf %lf", &a, &b) == 2) {
					g_lay.sx = a; g_lay.sy = b; g_lay.scannerOld = false;
				}
				else if (std::sscanf(line.c_str(), "scanner_call = %lf %lf", &a, &b) == 2) {
					g_lay.sx = a; g_lay.sy = b; g_lay.scannerOld = true;
				}
				else if (std::sscanf(line.c_str(), "gearbox = %lf %lf", &a, &b) == 2) {
					g_lay.gx = a; g_lay.gy = b; g_lay.haveGearBox = true;
				}
				else if (std::sscanf(line.c_str(), "gearup = %lf %lf", &a, &b) == 2) { g_lay.oldX[0] = a; g_lay.oldY[0] = b; g_lay.oldRows = true; }
				else if (std::sscanf(line.c_str(), "geardown = %lf %lf", &a, &b) == 2) { g_lay.oldX[1] = a; g_lay.oldY[1] = b; g_lay.oldRows = true; }
				else if (std::sscanf(line.c_str(), "hover = %lf %lf", &a, &b) == 2) { g_lay.oldX[2] = a; g_lay.oldY[2] = b; g_lay.oldRows = true; }
				else if (std::sscanf(line.c_str(), "row%d = %lf %lf", &i, &a, &b) == 3 && i >= 0 && i < 5) {
					row[i][0] = a; row[i][1] = b; ++rows;
				}
			}
			if (rows == 5) {
				// The first editor's rows: Accelerate, Brake, Gear+ (2), Gear- (3), Hover (4).
				g_lay.cx = row[1][0] + 40.0; g_lay.cy = row[1][1] + 15.0; g_lay.d = 60.0; g_lay.haveCluster = true;
				for (int k = 0; k < 3; ++k) { g_lay.oldX[k] = row[2 + k][0]; g_lay.oldY[k] = row[2 + k][1]; }
				g_lay.oldRows = true;
				g_lay.haveColumn = false;
			}
			if (g_lay.oldRows) g_lay.haveColumn = false;
			logger::Write("Hover UI: layout read (cluster %d, column %d, gear box %d, earlier rows %d)", g_lay.haveCluster, g_lay.haveColumn, g_lay.haveGearBox,
				g_lay.oldRows);
		}

		// ---- State (main thread -> HUD update) ----
		std::atomic<bool> g_inVehicle{ false }, g_hovering{ false }, g_reset{ false }, g_accelHeld{ false };
		struct Context
		{
			std::mutex lock;
			bool own = false, nearOwn = false, autoOn = false, occupied = false;
			std::string occKey;
			std::string summonKey, followKey, useKey, status, autoKey, autoLine, gangText, gangKey;
			std::string modeKey, modeLabel;
			ULONGLONG modeFlashUntil = 0;
			int gang = 0;
			std::string rows[kMaxRows];
			int rowCount = 0;
		} g_ctx;
		std::atomic<bool> g_footShown{ false };
		std::atomic<int> g_gear{ 2 };
		std::mutex g_keyLock;
		std::string g_accelName = "W", g_brakeName = "S";

		// ---- HUD side (only touched inside the HUD menu's update) ----
		using Update_t = void (*)(uptr);
		Update_t g_original = nullptr;
		using Advance_t = float (*)(uptr, float, int, bool);
		Advance_t g_advanceOriginal = nullptr;
		std::atomic<uptr> g_hudMenu{ 0 }, g_hudMovie{ 0 };
		// The movie's scalar deleting destructor (MovieImpl vtable slot 0, FUN_143198DC0(this, flags): dtor
		// FUN_143189D20, then free when flags & 1). Back at the main menu the HUD movie is destroyed; our menu pointer
		// and values stayed behind and the next load touched them (2026-10-17 17:26: crash on menu -> load / new game,
		// nothing logged). Now its death forgets them.
		using MovieDtor_t = void* (*)(uptr, unsigned);
		MovieDtor_t g_movieDtorOriginal = nullptr;
		// Layout defaults from the player's screenshot, in units of the native [E] button's height (32 px there).
		constexpr double kShotE = 32.0, kShotBarRight = 311.0, kShotAboveLabel = 50.0, kShotRow = 30.0;
		const char* const kEvents[kButtons] = { "Vehicle:VehicleAccel", "Vehicle:VehicleTurnLeft", "Vehicle:VehicleBrake", "Vehicle:VehicleTurnRight",
			"Vehicle:VehicleBoost", "Vehicle:VehicleBoost", "Vehicle:VehicleVertBoost", "Vehicle:VehicleBoost" };
		struct Ui
		{
			uptr movie = 0;
			Value widget;              // HUDVehicle_mc
			Value buttons[kButtons];   // BasicButton instances
			Value data[kButtons];      // their ButtonBaseData (kept alive)
			int gearShown = -1;
			unsigned gearFrames = 0;
			int hoverTint = -1;        // the Hover key box's tint applied (0 white, 1 blue), -1 none yet
			char text[kButtons][64] = {};
			bool shown[kButtons] = {};
			bool built = false, failed = false;
			// The rover widget's own clips (not ours), with where the game put them (the nudges add to that).
			struct Nat
			{
				Value clip;
				std::string name;
				double bx = 0, by = 0, alpha = 1.0;
				bool dimmed = false;
			};
			static constexpr int kMaxNat = 32;
			Nat nat[kMaxNat];
			int natCount = 0;
		} g_ui;
		unsigned g_calls = 0;
		ULONGLONG g_lastReport = 0;

		void DropExtras(bool release);
		void Drop(bool release)
		{
			for (int i = 0; i < kButtons; ++i) {
				if (release) {
					Release(g_ui.data[i]);
					Release(g_ui.buttons[i]);
				}
				g_ui.data[i] = Value{};
				g_ui.buttons[i] = Value{};
				g_ui.text[i][0] = 0;
				g_ui.shown[i] = false;
			}
			for (int i = 0; i < g_ui.natCount; ++i) {
				auto& n = g_ui.nat[i];
				if (release && IsObject(n.clip)) {
					// The same movie stays: put the game's clip back where it was, so the next build reads its own place.
					SetMember(n.clip, "x", Num(n.bx));
					SetMember(n.clip, "y", Num(n.by));
					if (n.dimmed) SetMember(n.clip, "alpha", Num(n.alpha));
					Release(n.clip);
				}
				n = Ui::Nat{};
			}
			g_ui.natCount = 0;
			if (release) Release(g_ui.widget);
			g_ui.widget = Value{};
			DropExtras(release);
			g_ui.gearShown = -1;
			g_ui.hoverTint = -1;
			g_ui.built = false;
		}

		bool SetButton(int i, const char* label)
		{
			if (std::strcmp(g_ui.text[i], label) == 0 && IsObject(g_ui.data[i])) return true;
			Value ued;
			const Value uedArgs[1] = { Str(kEvents[i]) };
			if (!CreateObject(g_ui.movie, &ued, "Shared.Components.ButtonControls.ButtonData.UserEventData", uedArgs, 1)) {
				logger::Write("Hover UI: could not create UserEventData (%s)", kEvents[i]);
				return false;
			}
			// Managed values are linked into the movie's value list by their own address: create them where they live
			// and never copy or move them afterwards.
			Release(g_ui.data[i]);
			const Value dataArgs[2] = { Str(label), Arg(ued) };
			const bool ok = CreateObject(g_ui.movie, &g_ui.data[i], "Shared.Components.ButtonControls.ButtonData.ButtonBaseData", dataArgs, 2);
			Release(ued);
			if (!ok) {
				logger::Write("Hover UI: could not create ButtonBaseData (%s)", label);
				return false;
			}
			const Value setArgs[1] = { Arg(g_ui.data[i]) };
			Invoke(g_ui.buttons[i], "SetButtonData", setArgs, 1);
			std::snprintf(g_ui.text[i], sizeof(g_ui.text[i]), "%s", label);
			return true;
		}

		bool GetNumber(const Value& o, const char* name, double& out)
		{
			Value v;
			if (!GetMember(o, name, &v)) return false;
			const int t = v.type & 0x8F;
			bool ok = true;
			if (t == kNumber) out = v.number;
			else if (t == 3) out = double(static_cast<std::int32_t>(v.raw));
			else if (t == 4) out = double(static_cast<std::uint32_t>(v.raw));
			else ok = false;
			Release(v);
			return ok;
		}

		void Show(int i, bool on)
		{
			if (g_ui.shown[i] == on || !IsObject(g_ui.buttons[i])) return;
			SetMember(g_ui.buttons[i], "visible", Bool(on));
			g_ui.shown[i] = on;
		}

		// The Gear button's key box shows the gear number instead of its key (PCButton_mc.ButtonInstance_mc.SetText).
		// Re-asserted every 60 frames: the button rewrites its key text when the control map changes.
		void SetGearText(int gear)
		{
			if (gear == g_ui.gearShown && ++g_ui.gearFrames % 60 != 0) return;
			const Value& btn = g_ui.buttons[kGear];
			if (!IsObject(btn)) return;
			char t[16];
			std::snprintf(t, sizeof(t), "GEAR %d", gear);
			const Value args[1] = { Str(t) };
			Value pc, inst;
			bool ok = false;
			if (GetMember(btn, "PCButton_mc", &pc) && IsObject(pc) && GetMember(pc, "ButtonInstance_mc", &inst) && IsObject(inst)) {
				ok = Invoke(inst, "SetText", args, 1);
			}
			Release(inst);
			Release(pc);
			if (!ok) {
				static bool told = false;
				if (!told) { told = true; logger::Write("Hover UI: gear number: PCButton_mc.ButtonInstance_mc.SetText not reached"); }
			}
			g_ui.gearShown = gear;
		}

		// The Hover key box turns a very slight blue while hovering: a flash.geom.ColorTransform on its PCButton_mc
		// (the button itself only tints the inner ButtonInstance_mc, so this one stays).
		void TintHover(bool on)
		{
			if (g_ui.hoverTint == int(on) || !IsObject(g_ui.buttons[kHover])) return;
			Value pc, tr, ct;
			if (GetMember(g_ui.buttons[kHover], "PCButton_mc", &pc) && IsObject(pc) && GetMember(pc, "transform", &tr) && IsObject(tr)) {
				const Value args[8] = { Num(on ? 0.80 : 1.0), Num(on ? 0.88 : 1.0), Num(1.0), Num(1.0), Num(0.0), Num(0.0), Num(0.0), Num(0.0) };
				if (CreateObject(g_ui.movie, &ct, "flash.geom.ColorTransform", args, 8)) {
					SetMember(tr, "colorTransform", Arg(ct));
					g_ui.hoverTint = int(on);
				}
			}
			Release(ct);
			Release(tr);
			Release(pc);
		}

		bool IsOurExtra(void* object);

		// A string member (managed strings hold a pointer to the text pointer).
		std::string GetString(const Value& o, const char* name)
		{
			Value v;
			std::string s;
			if (GetMember(o, name, &v) && (v.type & 0x8F) == kString) {
				const char* p = (v.type & kManaged) ? *reinterpret_cast<const char* const*>(v.string) : v.string;
				if (p) s = p;
			}
			Release(v);
			return s;
		}

		// The rover widget's own clips (the Exit button and the rest): each one can be nudged in the editor.
		void FindNatives()
		{
			double count = 0;
			if (!GetNumber(g_ui.widget, "numChildren", count)) return;
			std::vector<std::string> names;
			for (int i = 0; i < int(count) && g_ui.natCount < Ui::kMaxNat; ++i) {
				auto& n = g_ui.nat[g_ui.natCount];
				const Value args[1] = { Num(i) };
				if (!InvokeR(g_ui.widget, "getChildAt", args, 1, &n.clip) || !IsObject(n.clip) || IsOurExtra(n.clip.object)) {
					Release(n.clip);
					continue;
				}
				n.name = GetString(n.clip, "name");
				if (n.name.rfind("sv_", 0) == 0) {
					Release(n.clip);
					n = Ui::Nat{};
					continue;
				}
				if (n.name.empty()) {
					char t[24];
					std::snprintf(t, sizeof(t), "child%d", i);
					n.name = t;
				}
				GetNumber(n.clip, "x", n.bx);
				GetNumber(n.clip, "y", n.by);
				names.push_back(n.name);
				logger::Write("Hover UI: rover widget clip %d '%s' at (%.1f, %.1f)", i, n.name.c_str(), n.bx, n.by);
				++g_ui.natCount;
			}
			std::lock_guard lk(g_lay.lock);
			g_lay.natNames = names;
		}

		bool Build(uptr menu)
		{
			const Value& root = *reinterpret_cast<const Value*>(menu + 0x58);
			if (!IsObject(root)) return false;
			Value meters;
			if (!GetMember(root, "RightMeters_mc", &meters) || !IsObject(meters)) {
				Release(meters);
				logger::Write("Hover UI: root.RightMeters_mc not found");
				return false;
			}
			const bool okWidget = GetMember(meters, "HUDVehicle_mc", &g_ui.widget) && IsObject(g_ui.widget);
			Release(meters);
			if (!okWidget) {
				logger::Write("Hover UI: RightMeters_mc.HUDVehicle_mc not found");
				return false;
			}
			FindNatives();
			for (int i = 0; i < kButtons; ++i) {
				if (!CreateObject(g_ui.movie, &g_ui.buttons[i], "BasicButton", nullptr, 0)) {
					logger::Write("Hover UI: could not create BasicButton (ButtonClips.swf not loaded in the HUD?)");
					return false;
				}
				SetMember(g_ui.buttons[i], "name", Str("sv_button"));
				const Value addArgs[1] = { Arg(g_ui.buttons[i]) };
				Invoke(g_ui.widget, "addChild", addArgs, 1);
				SetMember(g_ui.buttons[i], "visible", Bool(false));
			}
			g_ui.built = true;
			logger::Write("Hover UI: %d native buttons added to the HUD's rover widget", kButtons);
			return true;
		}

		void Refresh()
		{
			const bool inVehicle = g_inVehicle.load(), hovering = g_hovering.load();
			if (!inVehicle) {
				for (int i = 0; i < kButtons; ++i) Show(i, false);
				return;
			}
			std::string accel, brake;
			{
				std::lock_guard lk(g_keyLock);
				accel = g_accelName;
				brake = g_brakeName;
			}
			char up[32], down[32];
			std::snprintf(up, sizeof(up), "+%s  Gear+", accel.c_str());
			std::snprintf(down, sizeof(down), "+%s  Gear-", brake.c_str());
			SetButton(kW, g_accelHeld.load() ? "Accelerate (hold)" : "Accelerate");
			SetButton(kA, "Left");
			SetButton(kS, "Brake");
			SetButton(kD, "Right");
			SetButton(kGearDown, down);
			SetButton(kGearUp, up);
			SetButton(kHover, hovering ? "Land" : "Hover");
			TintHover(hovering);
			if (std::strcmp(g_ui.text[kGear], " ") != 0) g_ui.gearShown = -1;  // new data: its key text is the key again
			SetButton(kGear, " ");  // no label: "GEAR n" is all inside the key box
			for (int i = 0; i < kButtons; ++i) Show(i, true);
			SetGearText(g_gear.load());
		}

		void KeyBox(const Value& b, double& px, double& py, double& pw, double& ph)
		{
			Value pc;
			px = py = pw = ph = 0;
			if (GetMember(b, "PCButton_mc", &pc) && IsObject(pc)) {
				GetNumber(pc, "x", px);
				GetNumber(pc, "y", py);
				GetNumber(pc, "width", pw);
				GetNumber(pc, "height", ph);
			}
			Release(pc);
		}

		// Places a key button so that its key box is centred on (x, y), with its label centred under (or above) the key.
		// MinimalButton's public clips: PCButton_mc (the key box) and Label_mc (LabelInstance_mc.Label_tf); the button's
		// own alignment only moves LabelInstance_mc inside Label_mc, so moving Label_mc sticks.
		// The rendered bounds of a clip in the button's coordinates (AS3 getBounds(button)).
		bool BoundsIn(const Value& clip, const Value& space, double& x, double& y, double& w, double& h)
		{
			Value rect;
			const Value args[1] = { Arg(space) };
			bool ok = InvokeR(clip, "getBounds", args, 1, &rect) && IsObject(rect);
			ok = ok && GetNumber(rect, "x", x) && GetNumber(rect, "y", y) && GetNumber(rect, "width", w) && GetNumber(rect, "height", h);
			Release(rect);
			return ok;
		}

		// Places a key button so that its key box is centred on (x, y), with its label centred under (or above) the key,
		// 3 px away, measured from the label's rendered bounds (MinimalButton's public Label_mc; the button's own
		// alignment only moves LabelInstance_mc inside it, so moving Label_mc sticks).
		void PlaceButtonKey(const Value& b, double x, double y, bool above, double scale = 1.0);
		void PlaceKey(int i, double x, double y, bool above) { PlaceButtonKey(g_ui.buttons[i], x, y, above); }

		void PlaceButtonKey(const Value& b, double x, double y, bool above, double scale)
		{
			double px, py, pw, ph;
			KeyBox(b, px, py, pw, ph);
			// the key box is measured in the button's own (unscaled) space; a scaled button lands it at scale * that
			SetMember(b, "x", Num(x - scale * (px + pw / 2.0)));
			SetMember(b, "y", Num(y - scale * (py + ph / 2.0)));
			Value lab;
			double bx, by, bw, bh, lx = 0, ly = 0;
			if (GetMember(b, "Label_mc", &lab) && IsObject(lab) && BoundsIn(lab, b, bx, by, bw, bh) && bw > 0.0) {
				GetNumber(lab, "x", lx);
				GetNumber(lab, "y", ly);
				const double gap = 3.0;
				const double wantTop = above ? py - gap - bh : py + ph + gap;
				SetMember(lab, "x", Num(lx + (px + pw / 2.0) - (bx + bw / 2.0)));
				SetMember(lab, "y", Num(ly + wantTop - by));
			}
			Release(lab);
		}

		double KeyAndLabelHeight(int i)
		{
			const Value& b = g_ui.buttons[i];
			double px, py, pw, ph;
			KeyBox(b, px, py, pw, ph);
			double lh = 0;
			Value lab;
			double bx, by, bw, bh;
			if (GetMember(b, "Label_mc", &lab) && IsObject(lab) && BoundsIn(lab, b, bx, by, bw, bh)) lh = bh;
			Release(lab);
			return ph + lh;
		}

		// A row: its key box's right edge at `right`, the key centred on y.
		void PlaceRow(int i, double right, double y)
		{
			const Value& b = g_ui.buttons[i];
			double px, py, pw, ph;
			KeyBox(b, px, py, pw, ph);
			SetMember(b, "x", Num(right - (px + pw)));
			SetMember(b, "y", Num(y - (py + ph / 2.0)));
		}

		// A key button with its key box centred on (x, y) and the label beside it (the native label-first layout).
		void PlaceCentred(int i, double x, double y)
		{
			const Value& b = g_ui.buttons[i];
			double px, py, pw, ph;
			KeyBox(b, px, py, pw, ph);
			SetMember(b, "x", Num(x - (px + pw / 2.0)));
			SetMember(b, "y", Num(y - (py + ph / 2.0)));
		}

		void Layout()
		{
			double ex = 0, ey = 0, eh = kShotE;
			Value e;
			if (GetMember(g_ui.widget, "GetUpButton_mc", &e) && IsObject(e)) {
				GetNumber(e, "x", ex);
				GetNumber(e, "y", ey);
				double h = 0;
				if (GetNumber(e, "height", h) && h > 4.0) eh = h;
			}
			Release(e);
			const double k = eh / kShotE;
			const double barRight = ex + kShotBarRight * k, labelTop = ey - kShotAboveLabel * k, row = kShotRow * k;
			double cx, cy, d, colRight, colTop, colStep, gx, gy, off[kButtons][2];
			bool above, edit;
			int sel;
			{
				std::lock_guard lk(g_lay.lock);
				bool save = false;
				if (!g_lay.haveCluster) {
					g_lay.cx = barRight - 220.0 * k; g_lay.cy = labelTop - 2.0 * row; g_lay.d = 60.0 * k; g_lay.haveCluster = true; save = true;
				}
				if (!g_lay.haveColumn) {
					if (g_lay.oldRows) {
						// The earlier rows (top-left corners): their key boxes' right edges and centres, Gear- on top.
						double right = -1e9, top = 1e9, bottom = -1e9;
						const int idx[3] = { kGearUp, kGearDown, kHover };
						for (int r = 0; r < 3; ++r) {
							double px, py, pw, ph;
							KeyBox(g_ui.buttons[idx[r]], px, py, pw, ph);
							right = std::max(right, g_lay.oldX[r] + px + pw);
							const double c = g_lay.oldY[r] + py + ph / 2.0;
							top = std::min(top, c);
							bottom = std::max(bottom, c);
						}
						g_lay.colRight = right; g_lay.colTop = top; g_lay.colStep = (bottom - top) / 2.0;
					}
					else {
						g_lay.colRight = barRight; g_lay.colTop = labelTop - 3.0 * row; g_lay.colStep = row;
					}
					g_lay.haveColumn = true;
					save = true;
				}
				if (!g_lay.haveGearBox) {
					g_lay.gx = g_lay.colRight + 40.0 * k; g_lay.gy = g_lay.colTop + g_lay.colStep * 0.5; g_lay.haveGearBox = true; save = true;
				}
				if (save) {
					SaveLayout();
					logger::Write("Hover UI: layout completed from the E button at (%.1f, %.1f) height %.1f", ex, ey, eh);
				}
				if (g_lay.d <= 0.0) {
					g_lay.d = KeyAndLabelHeight(kW) + 3.0 + 10.0;  // key + gap + label + clearance
					SaveLayout();
				}
				cx = g_lay.cx; cy = g_lay.cy; d = g_lay.d; above = g_lay.labelsAbove;
				colRight = g_lay.colRight; colTop = g_lay.colTop; colStep = g_lay.colStep;
				gx = g_lay.gx; gy = g_lay.gy;
				for (int i = 0; i < kButtons; ++i) { off[i][0] = g_lay.off[i][0]; off[i][1] = g_lay.off[i][1]; }
				edit = g_lay.edit;
				sel = g_lay.sel;
			}
			(void)above;
			// All labels under their keys. One distance d for W above S and A / D beside S; each element can be nudged.
			PlaceKey(kW, cx + off[kW][0], cy - d + off[kW][1], false);
			PlaceKey(kA, cx - d + off[kA][0], cy + off[kA][1], false);
			PlaceKey(kS, cx + off[kS][0], cy + off[kS][1], false);
			PlaceKey(kD, cx + d + off[kD][0], cy + off[kD][1], false);
			PlaceRow(kGearDown, colRight + off[kGearDown][0], colTop + off[kGearDown][1]);
			PlaceRow(kGearUp, colRight + off[kGearUp][0], colTop + colStep + off[kGearUp][1]);
			PlaceRow(kHover, colRight + off[kHover][0], colTop + 2.0 * colStep + off[kHover][1]);
			PlaceCentred(kGear, gx, gy);
			const bool blinkOff = edit && (GetTickCount64() / 300) % 2 == 0;
			// The widget's own clips: where the game put them plus their nudges.
			for (int i = 0; i < g_ui.natCount; ++i) {
				auto& n = g_ui.nat[i];
				if (!IsObject(n.clip)) continue;
				double ox = 0, oy = 0;
				{
					std::lock_guard lk(g_lay.lock);
					if (auto it = g_lay.natOff.find(n.name); it != g_lay.natOff.end()) { ox = it->second.first; oy = it->second.second; }
				}
				if (ox != 0.0 || oy != 0.0 || edit) {
					SetMember(n.clip, "x", Num(n.bx + ox));
					SetMember(n.clip, "y", Num(n.by + oy));
				}
				const bool picked = sel == kSelAll || sel == kSelNative + i;
				if (blinkOff && picked) {
					if (!n.dimmed) {
						GetNumber(n.clip, "alpha", n.alpha);
						SetMember(n.clip, "alpha", Num(n.alpha * 0.35));
						n.dimmed = true;
					}
				}
				else if (n.dimmed) {
					SetMember(n.clip, "alpha", Num(n.alpha));
					n.dimmed = false;
				}
			}
			for (int i = 0; i < kButtons; ++i) {
				const int group = i <= kD ? kSelCluster : i == kGear ? -1 : kSelColumn;
				const bool picked = sel == kSelAll || sel == i || sel == group;
				bool autoOn;
				{
					std::lock_guard lk(g_ctx.lock);
					autoOn = g_ctx.autoOn;
				}
				const bool dim = autoOn && i <= kD;  // Auto flies: the keys only take over
				SetMember(g_ui.buttons[i], "alpha", Num(blinkOff && picked ? 0.35 : dim ? 0.45 : 1.0));
			}
		}

		void WorkExtras(uptr menu, uptr movie);
		void UpdateDetour(uptr menu)
		{
			g_original(menu);
			g_hudMovie.store(*reinterpret_cast<uptr*>(menu + 0x88));
			if (g_hudMenu.exchange(menu) != menu) logger::Write("Hover UI: HUD menu %p (movie %p)", reinterpret_cast<void*>(menu), *reinterpret_cast<void**>(menu + 0x88));
		}

		void Work(uptr menu)
		{
			++g_calls;
			const ULONGLONG now = GetTickCount64();
			if (now - g_lastReport >= 60000) {
				if (!g_lastReport) logger::Write("Hover UI: HUD movie advances on thread %lu", GetCurrentThreadId());
				g_lastReport = now;
				if (logger::Verbose()) logger::Write("Hover UI: HUD movie advanced %u times in the last minute", g_calls);
				g_calls = 0;
			}
			__try {
				const uptr movie = *reinterpret_cast<uptr*>(menu + 0x88);
				if (movie != g_ui.movie) {
					Drop(false);  // the old movie (and its value list) is gone: forget, never release
					g_ui.movie = movie;
					g_ui.failed = false;
				}
				else if (g_reset.exchange(false)) {
					Drop(true);   // same movie: release properly and rebuild
					g_ui.failed = false;
				}
				if (!movie || g_ui.failed) return;
				WorkExtras(menu, movie);
				if (!g_ui.built && !(g_inVehicle.load() && Build(menu))) {
					if (g_inVehicle.load()) g_ui.failed = !g_ui.built;
					return;
				}
				Refresh();
				if (g_inVehicle.load()) Layout();
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				g_ui.failed = true;
				logger::Write("Hover UI: exception 0x%08lX in the HUD update; native buttons disabled until the next load", GetExceptionCode());
			}
		}


		// ---- Scanner (MonocleMenu, monoclemenu.swf): the rover call button ----
		// MonocleMenu vtable 0x144C84D70 (RTTI .?AVMonocleMenu@@): slot 8 = its ProcessMessage (FUN_1415F9080), which
		// runs when the scanner opens; it tells which movie is the scanner's. The button is a BasicButton on the
		// scanner's root, its key box showing the call key (SetText), positioned with F7 while the scanner is open.
		constexpr std::uint32_t kRvaMonoVtbl = 0x04C84D70, kRvaMonoMessage = 0x015F9080;
		constexpr int kSlotMessage = 8;
		using Message_t = std::uint64_t (*)(uptr, void*);
		Message_t g_monoOriginal = nullptr;
		std::atomic<uptr> g_monoMenu{ 0 }, g_monoMovie{ 0 };
		std::atomic<ULONGLONG> g_monoSeen{ 0 };  // last scanner frame (the scanner counts as open for 300 ms after)
		std::atomic<bool> g_callAvail{ false }, g_followOn{ false };
		std::atomic<int> g_status{ 0 };  // 0 idle, 1 summoning, 2 following
		std::mutex g_callLock;
		std::string g_callKey = "N", g_followKey = "U";
		std::mutex g_scanLock;
		std::string g_placeName;
		ULONGLONG g_placeAt = 0;
		double g_placeDistance = 0;
		std::string g_scanText;  // the scanner screen status box text
		struct MonoEl;
		std::uint64_t MonoMessageDetour(uptr menu, void* msg)
		{
			const auto r = g_monoOriginal(menu, msg);
			g_monoMovie.store(*reinterpret_cast<uptr*>(menu + 0x88));
			if (g_monoMenu.exchange(menu) != menu) logger::Write("Hover UI: scanner menu %p", reinterpret_cast<void*>(menu));
			return r;
		}

		bool ScannerOpen() { return GetTickCount64() - g_monoSeen.load() < 300; }

		// One scanner element: a BasicButton whose key box shows `key`, with `label` under it (empty label: the key box
		// alone, e.g. the status box). Created in place (managed values never move).
		struct MonoEl
		{
			Value button, data;
			double scale = 1.0;       // 0.5: the Passenger / Eject prompts are half size
			std::string label, key;
			bool shown = false;
			int tint = -1;
			unsigned frames = 0;
		};

		struct Mono
		{
			uptr movie = 0, menu = 0;  // a new scanner menu each time it opens; its movie may reuse an old address
			MonoEl summon, follow, status;
			bool built = false, failed = false;
		} g_mono;

		bool MonoSetData(uptr movie, MonoEl& e, const char* label)
		{
			if (e.label == label && IsObject(e.data)) return true;
			Release(e.data);
			Value ued;
			const Value uedArgs[1] = { Str("Vehicle:VehicleBoost") };  // any event: the key text is set by MonoSetKey
			bool ok = false;
			if (CreateObject(movie, &ued, "Shared.Components.ButtonControls.ButtonData.UserEventData", uedArgs, 1)) {
				const Value dataArgs[2] = { Str(label), Arg(ued) };
				if (CreateObject(movie, &e.data, "Shared.Components.ButtonControls.ButtonData.ButtonBaseData", dataArgs, 2)) {
					const Value setArgs[1] = { Arg(e.data) };
					Invoke(e.button, "SetButtonData", setArgs, 1);
					ok = true;
				}
			}
			Release(ued);
			e.label = label;
			e.key.clear();  // new data: its key text is the event's key again
			return ok;
		}

		void MonoSetKey(MonoEl& e, const std::string& key)
		{
			// Re-asserted every 60 frames: the button rewrites its key text when the control map changes.
			if (key == e.key && ++e.frames % 60 != 0) return;
			Value pc, inst;
			if (GetMember(e.button, "PCButton_mc", &pc) && IsObject(pc) && GetMember(pc, "ButtonInstance_mc", &inst) && IsObject(inst)) {
				const Value args[1] = { Str(key.c_str()) };
				Invoke(inst, "SetText", args, 1);
			}
			Release(inst);
			Release(pc);
			e.key = key;
		}

		void MonoShow(MonoEl& e, bool on)
		{
			if (e.shown == on || !IsObject(e.button)) return;
			SetMember(e.button, "visible", Bool(on));
			e.shown = on;
		}

		// The status box turns the slight hover blue while it shows a state.
		void MonoTint(uptr movie, MonoEl& e, bool on)
		{
			if (e.tint == int(on) || !IsObject(e.button)) return;
			Value pc, tr, ct;
			if (GetMember(e.button, "PCButton_mc", &pc) && IsObject(pc) && GetMember(pc, "transform", &tr) && IsObject(tr)) {
				const Value args[8] = { Num(on ? 0.80 : 1.0), Num(on ? 0.88 : 1.0), Num(1.0), Num(1.0), Num(0.0), Num(0.0), Num(0.0), Num(0.0) };
				if (CreateObject(movie, &ct, "flash.geom.ColorTransform", args, 8)) {
					SetMember(tr, "colorTransform", Arg(ct));
					e.tint = int(on);
				}
			}
			Release(ct);
			Release(tr);
			Release(pc);
		}

		// ---- The contextual HUD: on-foot panel and in-vehicle extras (in the HUD movie) ----
		struct Extras
		{
			uptr movie = 0;
			Value meters, widget;          // RightMeters_mc (on foot) and HUDVehicle_mc (in the vehicle)
			MonoEl fSummon, fFollow, fGetIn, fStatus, fGang, fPass, fEject, fRows, fMode;
			MonoEl vAuto, vLine, vGang, vRows, vMode;  // fRows / vRows: the per-vehicle line (one box, movable in the F7 editor)
			bool built = false, failed = false;
		} g_ex;

		MonoEl* FootEls[kFootEls] = { &g_ex.fSummon, &g_ex.fFollow, &g_ex.fGetIn, &g_ex.fStatus, &g_ex.fGang, &g_ex.fPass, &g_ex.fEject, &g_ex.fRows, &g_ex.fMode };
		MonoEl* CarEls[kCarEls] = { &g_ex.vAuto, &g_ex.vLine, &g_ex.vGang, &g_ex.vRows, &g_ex.vMode };

		bool IsOurExtra(void* object)
		{
			for (MonoEl* e : CarEls) { if (object && e->button.object == object) return true; }
			return false;
		}

		void DropExtras(bool release)
		{
			for (MonoEl* e : FootEls) { if (release) { Release(e->data); Release(e->button); } }
			for (MonoEl* e : CarEls) { if (release) { Release(e->data); Release(e->button); } }
			if (release) { Release(g_ex.meters); Release(g_ex.widget); }
			g_ex = Extras{};
		}

		bool BuildExtras(uptr menu)
		{
			const Value& root = *reinterpret_cast<const Value*>(menu + 0x58);
			if (!IsObject(root)) return false;
			if (!GetMember(root, "RightMeters_mc", &g_ex.meters) || !IsObject(g_ex.meters)) return false;
			if (!GetMember(g_ex.meters, "HUDVehicle_mc", &g_ex.widget) || !IsObject(g_ex.widget)) return false;
			for (MonoEl* e : FootEls) {
				if (!CreateObject(g_ex.movie, &e->button, "BasicButton", nullptr, 0)) return false;
				const Value addArgs[1] = { Arg(e->button) };
				Invoke(g_ex.meters, "addChild", addArgs, 1);
				SetMember(e->button, "visible", Bool(false));
			}
			for (MonoEl* e : CarEls) {
				if (!CreateObject(g_ex.movie, &e->button, "BasicButton", nullptr, 0)) return false;
				SetMember(e->button, "name", Str("sv_extra"));  // FindNatives skips ours by this name
				const Value addArgs[1] = { Arg(e->button) };
				Invoke(g_ex.widget, "addChild", addArgs, 1);
				SetMember(e->button, "visible", Bool(false));
			}
			g_ex.built = true;
			logger::Write("Hover UI: contextual HUD added (on foot: Summon, Follow, Get in, status, gang; in the vehicle: Auto, status, gang)");
			return true;
		}

		// One element: shown or hidden, its label and its key-box text.
		void ElSet(MonoEl& e, bool on, const char* label, const std::string& key)
		{
			MonoShow(e, on);
			if (!on) return;
			MonoSetData(g_ex.movie, e, label);
			MonoSetKey(e, key.empty() ? std::string(" ") : key);
		}

		void RefreshExtras()
		{
			bool own, nearOwn, autoOn, occupied;
			int gang, rowCount = 0;
			std::string rows[kMaxRows];
			std::string summonKey, followKey, useKey, status, autoKey, autoLine, gangText, gangKey, occKey, modeKey, modeLabel;
			bool modeFlash = false;
			{
				std::lock_guard lk(g_ctx.lock);
				modeKey = g_ctx.modeKey; modeLabel = g_ctx.modeLabel; modeFlash = GetTickCount64() < g_ctx.modeFlashUntil;
				own = g_ctx.own; nearOwn = g_ctx.nearOwn; autoOn = g_ctx.autoOn; gang = g_ctx.gang;
				occupied = g_ctx.occupied; occKey = g_ctx.occKey;
				summonKey = g_ctx.summonKey; followKey = g_ctx.followKey; useKey = g_ctx.useKey; status = g_ctx.status;
				autoKey = g_ctx.autoKey; autoLine = g_ctx.autoLine; gangText = g_ctx.gangText; gangKey = g_ctx.gangKey;
				rowCount = g_ctx.rowCount;
				for (int i = 0; i < kMaxRows; ++i) rows[i] = g_ctx.rows[i];
			}
			const bool inVehicle = g_inVehicle.load();
			const bool foot = !inVehicle && own;
			g_footShown = foot;
			bool ed;
			{
				std::lock_guard lk(g_lay.lock);
				ed = g_lay.edit;
			}
			// While editing, every element shows (a sample text where it is empty), so each one can be placed.
			if (ed) {
				if (status.empty()) status = "FOLLOWING 42 m";
				if (autoLine.empty()) autoLine = "AUTO 120 m";
				if (gangText.empty() || gang <= 0) { gangText = "Gang 2"; gangKey = "."; }
				if (autoKey.empty()) autoKey = " ";
				if (modeKey.empty()) modeKey = " ";
				if (modeLabel.empty()) modeLabel = "Auto: Stop";
			}
			ElSet(g_ex.fSummon, foot && !summonKey.empty(), "Summon", summonKey);
			ElSet(g_ex.fFollow, foot && !followKey.empty(), g_followOn.load() ? "Stop follow" : "Follow", followKey);
			ElSet(g_ex.fGetIn, foot && (nearOwn || ed), "Get in", useKey);
			ElSet(g_ex.fStatus, foot && !status.empty(), " ", status);
			MonoTint(g_ex.movie, g_ex.fStatus, !status.empty());
			// Text inside one rounded box (label blank), like the FOLLOWING status box.
			const std::string gangBox = gangText + (gangKey.empty() ? std::string() : "  [" + gangKey + "]");
			std::string rowsBox;
			for (int i = 0; i < kMaxRows && i < rowCount; ++i) {
				if (rows[i].empty()) continue;
				rowsBox += (rowsBox.empty() ? "" : "  |  ") + rows[i];
			}
			if (ed && rowsBox.empty()) rowsBox = "pursuit REV8 83 m";
			ElSet(g_ex.fGang, !inVehicle && (gang > 0 || (ed && foot)), " ", gangBox);
			ElSet(g_ex.fRows, !inVehicle && (gang > 0 || (ed && foot)) && !rowsBox.empty(), " ", rowsBox);
			// Facing an occupied vehicle on foot: tap = the passenger seat, hold = eject the driver (half-size prompts).
			g_ex.fPass.scale = g_ex.fEject.scale = 0.5;
			const bool occShow = !inVehicle && (occupied || (ed && foot));
			ElSet(g_ex.fPass, occShow, "Passenger", occKey);
			ElSet(g_ex.fEject, occShow, "Double tap: eject", occKey);
			// The Auto behaviour button: on foot (with a vehicle of your own) and in the vehicle; blue for a moment after a press.
			ElSet(g_ex.fMode, foot && !modeKey.empty(), modeLabel.c_str(), modeKey);
			MonoTint(g_ex.movie, g_ex.fMode, modeFlash);
			ElSet(g_ex.vMode, inVehicle && !modeKey.empty(), modeLabel.c_str(), modeKey);
			MonoTint(g_ex.movie, g_ex.vMode, modeFlash);
			ElSet(g_ex.vAuto, inVehicle && !autoKey.empty(), autoOn ? "Auto on" : "Auto", autoKey);
			MonoTint(g_ex.movie, g_ex.vAuto, autoOn);
			ElSet(g_ex.vLine, inVehicle && (autoOn || ed) && !autoLine.empty(), " ", autoLine);
			MonoTint(g_ex.movie, g_ex.vLine, true);
			ElSet(g_ex.vGang, inVehicle && (gang > 0 || ed), " ", gangBox);
			ElSet(g_ex.vRows, inVehicle && (gang > 0 || ed) && !rowsBox.empty(), " ", rowsBox);
			if (ed) gang = std::max(gang, 1);
			// Places: the on-foot panel in RightMeters_mc's coordinates; the extras in the rover widget's, beside the
			// column (Auto) and under the WASD cluster (status, gang).
			double fx, fy, ex, ey, cx, cy, d, colRight, colTop, colStep, fo[kFootEls][2], co[kCarEls][2];
			bool edit;
			int sel, fsel;
			{
				std::lock_guard lk(g_lay.lock);
				if (!g_lay.haveFoot) {
					double wx = 0, wy = 0;
					GetNumber(g_ex.widget, "x", wx);
					GetNumber(g_ex.widget, "y", wy);
					g_lay.fpx = wx + g_lay.cx - g_lay.d;
					g_lay.fpy = wy + g_lay.cy;
					g_lay.haveFoot = true;
					SaveLayout();
					logger::Write("Hover UI: on-foot panel placed at (%.1f, %.1f) (the rover widget's WASD place)", g_lay.fpx, g_lay.fpy);
				}
				fx = g_lay.fpx; fy = g_lay.fpy; ex = g_lay.eox; ey = g_lay.eoy;
				cx = g_lay.cx; cy = g_lay.cy; d = g_lay.d; colRight = g_lay.colRight; colTop = g_lay.colTop; colStep = g_lay.colStep;
				for (int i = 0; i < kFootEls; ++i) { fo[i][0] = g_lay.footOff[i][0]; fo[i][1] = g_lay.footOff[i][1]; }
				for (int i = 0; i < kCarEls; ++i) { co[i][0] = g_lay.carOff[i][0]; co[i][1] = g_lay.carOff[i][1]; }
				edit = g_lay.edit;
				sel = g_lay.sel;
				fsel = g_lay.fsel;
			}
			const bool blinkOff = edit && (GetTickCount64() / 300) % 2 == 0;
			if (foot || gang > 0 || occupied) {
				const double at[kFootEls][2] = { { fx, fy }, { fx + 120.0, fy }, { fx + 240.0, fy }, { fx + 80.0, fy + 52.0 }, { fx + 80.0, fy + 98.0 }, { fx + 240.0, fy + 52.0 }, { fx + 240.0, fy + 98.0 }, { fx + 80.0, fy + 98.0 }, { fx + 360.0, fy } };
				for (int i = 0; i < kFootEls; ++i) {
					if (!FootEls[i]->shown) continue;
					if (FootEls[i]->scale != 1.0) {
						SetMember(FootEls[i]->button, "scaleX", Num(FootEls[i]->scale));
						SetMember(FootEls[i]->button, "scaleY", Num(FootEls[i]->scale));
					}
					PlaceButtonKey(FootEls[i]->button, at[i][0] + fo[i][0], at[i][1] + fo[i][1], false, FootEls[i]->scale);
					const bool picked = fsel == kFootEls || fsel == i;
					SetMember(FootEls[i]->button, "alpha", Num(blinkOff && !inVehicle && picked ? 0.35 : 1.0));
				}
			}
			if (inVehicle) {
				const double at[kCarEls][2] = { { colRight + 70.0 + ex, colTop + 2.0 * colStep + ey }, { cx + ex, cy + 0.9 * d + ey }, { cx + ex, cy + 1.7 * d + ey }, { cx + ex, cy + 1.7 * d + ey }, { colRight + 70.0 + ex, colTop + 2.0 * colStep + ey } };
				for (int i = 0; i < kCarEls; ++i) {
					if (!CarEls[i]->shown) continue;
					PlaceButtonKey(CarEls[i]->button, at[i][0] + co[i][0], at[i][1] + co[i][1], false);
					const bool picked = sel == kSelExtras || sel == kSelAll || sel == kSelAuto + i;
					SetMember(CarEls[i]->button, "alpha", Num(blinkOff && picked ? 0.35 : 1.0));
				}
			}
		}

		void WorkExtras(uptr menu, uptr movie)
		{
			if (!g_ex.built && !g_ex.failed) {
				g_ex.movie = movie;
				if (!BuildExtras(menu)) {
					g_ex.failed = true;
					logger::Write("Hover UI: contextual HUD could not be built (RightMeters_mc / HUDVehicle_mc / BasicButton)");
				}
			}
			if (g_ex.built) RefreshExtras();
		}

		// monoclemenu.swf: SetRightInfoDisplayState selects "MapMarker" only for an aimed POI.
		// NearbyPOIDisplay is area context and must never select a destination on its own.
		void CaptureScannerDestination(const Value& root)
		{
			std::string name;
			Value panel, marker, description, text, range, rangeText;
			double distance = 0;
			if (GetMember(root, "RightInfoDisplay_mc", &panel) && IsObject(panel) &&
				GetString(panel, "currentLabel") == "MapMarker" &&
				GetMember(panel, "MapMarkerInfo_mc", &marker) && IsObject(marker) &&
				GetMember(marker, "description_mc", &description) && IsObject(description) &&
				GetMember(description, "description_tf", &text) && IsObject(text)) {
				name = GetString(text, "text");
				// The first description line is the location; subsequent lines describe its state.
				name = name.substr(0, name.find_first_of("\r\n"));
				const auto last = name.find_last_not_of(" \t");
				name = last == std::string::npos ? "" : name.substr(0, last + 1);
				const auto first = name.find_first_not_of(" \t");
				if (first != std::string::npos) name.erase(0, first);
				if (GetMember(root, "range_mc", &range) && IsObject(range) &&
					GetMember(range, "range_tf", &rangeText) && IsObject(rangeText)) {
					const auto shown = GetString(rangeText, "text");
					distance = std::strtod(shown.c_str(), nullptr);
					if (!std::isfinite(distance) || distance <= 0 || distance > 100000) distance = 0;
				}
			}
			Release(rangeText); Release(range);
			Release(text); Release(description); Release(marker); Release(panel);
			std::lock_guard lk(g_scanLock);
			g_placeName = name;
			g_placeDistance = distance;
			g_placeAt = GetTickCount64();
		}

		void MonoWorkBody(uptr menu)
		{
			const uptr movie = *reinterpret_cast<uptr*>(menu + 0x88);
			if (movie != g_mono.movie || menu != g_mono.menu) {
				g_mono = Mono{};  // a new scanner (menu or movie): the old objects went with the old one (13:57:54 AV)
				g_mono.movie = movie;
				g_mono.menu = menu;
			}
			if (movie) {
				const auto& root = *reinterpret_cast<const Value*>(menu + 0x58);
				if (IsObject(root)) CaptureScannerDestination(root);
			}
			if (!movie || g_mono.failed) return;
			if (!g_mono.built) {
				const Value& root = *reinterpret_cast<const Value*>(menu + 0x58);
				if (!IsObject(root)) return;
				for (MonoEl* e : { &g_mono.summon, &g_mono.follow, &g_mono.status }) {
					if (!CreateObject(movie, &e->button, "BasicButton", nullptr, 0)) {
						g_mono.failed = true;
						logger::Write("Hover UI: scanner: could not create BasicButton");
						return;
					}
					const Value addArgs[1] = { Arg(e->button) };
					Invoke(root, "addChild", addArgs, 1);
					SetMember(e->button, "visible", Bool(false));
				}
				g_mono.built = true;
				logger::Write("Hover UI: scanner buttons added (Summon, Follow, status)");
			}
			// Only the status box is used now: it carries the scanner marker text (the distance to what you look at).
			std::string scan;
			{
				std::lock_guard lk(g_scanLock);
				scan = g_scanText;
			}
			bool edit;
			int sel;
			double x, y;
			{
				std::lock_guard lk(g_lay.lock);
				x = g_lay.tx;
				y = g_lay.ty;
				edit = g_lay.edit;
				sel = g_lay.msel;
			}
			MonoShow(g_mono.summon, false);
			MonoShow(g_mono.follow, false);
			if (scan.empty() && edit) scan = "TARGET  1240 m";  // shown with a sample while editing, so it can be placed
			const bool show = !scan.empty();
			MonoShow(g_mono.status, show);
			if (!show) return;
			MonoSetData(movie, g_mono.status, " ");
			MonoSetKey(g_mono.status, scan);
			MonoTint(movie, g_mono.status, true);
			const bool blinkOff = edit && (GetTickCount64() / 300) % 2 == 0;
			PlaceButtonKey(g_mono.status.button, x, y, false);
			SetMember(g_mono.status.button, "alpha", Num(blinkOff && (sel == 3 || sel == 2) ? 0.35 : 1.0));
		}

		void MonoWork(uptr menu)
		{
			g_monoSeen = GetTickCount64();
			__try {
				MonoWorkBody(menu);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				g_mono.failed = true;
				logger::Write("Hover UI: exception 0x%08lX in the scanner update; scanner button off until the next load", GetExceptionCode());
			}
		}

		float AdvanceDetour(uptr movie, float dt, int catchUp, bool capture)
		{
			const float r = g_advanceOriginal(movie, dt, catchUp, capture);
			// Only the movie the menu was seen with (never read through a menu pointer that may be gone).
			const uptr menu = g_hudMenu.load();
			if (menu && movie == g_hudMovie.load()) {
				Work(menu);
			}
			// The scanner screen: only its status box is used (2026-10-18: the scanner marker distance).
			const uptr mm = g_monoMenu.load();
			if (mm && movie == g_monoMovie.load()) MonoWork(mm);
			return r;
		}

		void* MovieDtorDetour(uptr movie, unsigned flags)
		{
			const bool hud = movie && (movie == g_hudMovie.load() || movie == g_ui.movie);
			const bool mono = movie && (movie == g_monoMovie.load() || movie == g_mono.movie);
			if (hud) {
				g_hudMenu.store(0);
				g_hudMovie.store(0);
			}
			if (mono) {
				g_monoMenu.store(0);
				g_monoMovie.store(0);
				std::lock_guard lk(g_scanLock);
				g_placeName.clear(); g_placeAt = 0;
			}
			void* r = g_movieDtorOriginal(movie, flags);
			// After the movie unlinked its values (they live in our memory): forget them, never release.
			if (hud) {
				Drop(false);
				g_ui.movie = 0;
				g_ui.failed = false;
				logger::Write("Hover UI: HUD movie %p destroyed; buttons forgotten", reinterpret_cast<void*>(movie));
			}
			if (mono) {
				g_mono = Mono{};
				logger::Write("Hover UI: scanner movie %p destroyed", reinterpret_cast<void*>(movie));
			}
			return r;
		}

		bool PatchSlot(std::uint32_t vtblRva, int slot, std::uint32_t expectRva, void* detour, void** original)
		{
			auto** p = reinterpret_cast<void**>(game::Base() + vtblRva) + slot;
			// The slot holds the game's own function, or another plugin's detour (Gravspace patches the same HUD slots and loads first): chain to it.
			if (reinterpret_cast<uptr>(*p) != game::Base() + expectRva) {
				MEMORY_BASIC_INFORMATION mbi{};
				const bool foreign = VirtualQuery(*p, &mbi, sizeof(mbi)) && mbi.AllocationBase && reinterpret_cast<uptr>(mbi.AllocationBase) != game::Base();
				if (!foreign) return false;
				logger::Write("Hover UI: vtable slot %d (rva %08X) was already patched by another plugin; chaining to it", slot, vtblRva);
			}
			DWORD old = 0;
			if (!VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &old)) return false;
			*original = *p;
			*p = detour;
			VirtualProtect(p, sizeof(void*), old, &old);
			return true;
		}
	}

	bool Install()
	{
		void* orig = nullptr;
		if (!PatchSlot(kRvaHudMenuVtbl, kSlotUpdate, kRvaHudUpdate, reinterpret_cast<void*>(&UpdateDetour), &orig)) {
			logger::Write("Hover UI: HUDMenu vtable slot %d is not the expected function; native buttons off", kSlotUpdate);
			return false;
		}
		g_original = reinterpret_cast<Update_t>(orig);
		if (!PatchSlot(kRvaMovieImplVtbl, kSlotAdvance, kRvaMovieAdvance, reinterpret_cast<void*>(&AdvanceDetour), &orig)) {
			logger::Write("Hover UI: MovieImpl vtable slot %d is not Advance; native buttons off", kSlotAdvance);
			return false;
		}
		g_advanceOriginal = reinterpret_cast<Advance_t>(orig);
		if (PatchSlot(kRvaMovieImplVtbl, 0, kRvaMovieDtor, reinterpret_cast<void*>(&MovieDtorDetour), &orig)) {
			g_movieDtorOriginal = reinterpret_cast<MovieDtor_t>(orig);
		}
		else {
			// Without it a stale menu would crash the next load: no native buttons.
			logger::Write("Hover UI: MovieImpl vtable slot 0 is not the destructor; native buttons off");
			return false;
		}
		if (PatchSlot(kRvaMonoVtbl, kSlotMessage, kRvaMonoMessage, reinterpret_cast<void*>(&MonoMessageDetour), &orig)) {
			g_monoOriginal = reinterpret_cast<Message_t>(orig);
		}
		else {
			logger::Write("Hover UI: MonocleMenu vtable slot %d is not the expected function; no scanner button", kSlotMessage);
		}
		{
			std::lock_guard lk(g_lay.lock);
			LoadLayout();
		}
		logger::Write("Hover UI: HUD menu and movie advance hooked (main thread %lu)", GetCurrentThreadId());
		return true;
	}

	bool IsScannerOpen() { return ScannerOpen(); }
	ScannerDestination ScannerDestinationContext()
	{
		std::lock_guard lk(g_scanLock);
		return ScannerOpen() && GetTickCount64() - g_placeAt < 200 ? ScannerDestination{g_placeName, g_placeDistance} : ScannerDestination{};
	}

	void SetScan(const char* text)
	{
		std::lock_guard lk(g_scanLock);
		g_scanText = text ? text : "";
	}

	void SetState(bool inVehicle, bool hovering, int gear, bool accelHeld)
	{
		g_inVehicle = inVehicle;
		g_hovering = hovering;
		g_gear = gear;
		g_accelHeld = accelHeld;
	}

	void SetKeyNames(const char* accel, const char* brake)
	{
		std::lock_guard lk(g_keyLock);
		if (g_accelName != accel) g_accelName = accel;
		if (g_brakeName != brake) g_brakeName = brake;
	}

	void SetCall(bool available, const char* key)
	{
		g_callAvail = available;
		std::lock_guard lk(g_callLock);
		if (g_callKey != key) g_callKey = key;
	}

	void SetFollow(bool on) { g_followOn = on; }

	void SetStatus(int state) { g_status = state; }

	void SetFollowKey(const char* key)
	{
		std::lock_guard lk(g_callLock);
		if (g_followKey != key) g_followKey = key;
	}

	void SetFoot(bool own, bool nearOwn, const char* summonKey, const char* followKey, const char* useKey, const char* status)
	{
		std::lock_guard lk(g_ctx.lock);
		g_ctx.own = own;
		g_ctx.nearOwn = nearOwn;
		if (g_ctx.summonKey != summonKey) g_ctx.summonKey = summonKey;
		if (g_ctx.followKey != followKey) g_ctx.followKey = followKey;
		if (g_ctx.useKey != useKey) g_ctx.useKey = useKey;
		if (g_ctx.status != status) g_ctx.status = status;
	}

	void SetOccupied(bool facing, const char* key)
	{
		std::lock_guard lk(g_ctx.lock);
		g_ctx.occupied = facing;
		if (g_ctx.occKey != key) g_ctx.occKey = key;
	}

	void SetAuto(const char* key, bool on, const char* line)
	{
		std::lock_guard lk(g_ctx.lock);
		g_ctx.autoOn = on;
		if (g_ctx.autoKey != key) g_ctx.autoKey = key;
		if (g_ctx.autoLine != line) g_ctx.autoLine = line;
	}

	void SetAutoMode(const char* key, const char* label, bool pressed)
	{
		std::lock_guard lk(g_ctx.lock);
		if (g_ctx.modeKey != key) g_ctx.modeKey = key;
		if (g_ctx.modeLabel != label) g_ctx.modeLabel = label;
		if (pressed) g_ctx.modeFlashUntil = GetTickCount64() + 450;
	}

	void SetGang(int count, const char* profile, const char* key)
	{
		// (2026-10-20, the user: the NPCs' names instead of "Gang n"): the caller's text is the names; no text = the count as before.
		char t[96];
		if (profile && profile[0]) std::snprintf(t, sizeof(t), "%s", profile);
		else std::snprintf(t, sizeof(t), "Gang %d", count);
		std::lock_guard lk(g_ctx.lock);
		g_ctx.gang = count;
		if (g_ctx.gangText != t) g_ctx.gangText = t;
		if (g_ctx.gangKey != key) g_ctx.gangKey = key;
	}

	void SetRows(int count, const char* const* lines)
	{
		std::lock_guard lk(g_ctx.lock);
		g_ctx.rowCount = std::clamp(count, 0, kMaxRows);
		for (int i = 0; i < kMaxRows; ++i) {
			const std::string t = i < g_ctx.rowCount && lines && lines[i] ? lines[i] : "";
			if (g_ctx.rows[i] != t) g_ctx.rows[i] = t;
		}
	}

	void Reset() {
		g_reset = true;
		std::lock_guard lk(g_scanLock);
		g_placeName.clear(); g_placeAt = 0;
	}

	namespace
	{
		// The editor's pick order in the vehicle (caller holds g_lay.lock).
		std::vector<int> SelOrder()
		{
			std::vector<int> o;
			for (int i = 0; i <= kSelMode; ++i) o.push_back(i);
			for (int i = 0; i < int(g_lay.natNames.size()); ++i) o.push_back(kSelNative + i);
			for (int i : { int(kSelCluster), int(kSelColumn), int(kSelExtras), int(kSelAll) }) o.push_back(i);
			return o;
		}

		// The picked element's on-screen name (caller holds g_lay.lock).
		std::string SelName(bool inVehicle)
		{
			if (!inVehicle) {
				const char* foot[kFootEls + 1] = { "Summon", "Follow", "Get in", "Status", "Gang", "Passenger", "Eject", "Vehicles", "Auto mode", "Panel" };
				return foot[std::clamp(g_lay.fsel, 0, kFootEls)];
			}
			const int s = g_lay.sel;
			const char* names[kSelAll + 1] = { "W", "A", "S", "D", "Gear-", "Gear+", "Hover", "Gear box", "Auto", "Auto line", "Gang", "Vehicles", "Auto mode", "WASD", "Column", "Extras",
				"All" };
			if (s >= 0 && s <= kSelAll) return names[s];
			if (s >= kSelNative && s - kSelNative < int(g_lay.natNames.size())) return g_lay.natNames[s - kSelNative];
			return "?";
		}
	}

	void EditTick(bool focused)
	{
		static bool down[10] = {};
		static ULONGLONG repeatAt[10] = {};
		const int keys[10] = { VK_F7, VK_PRIOR, VK_NEXT, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_HOME, VK_END, VK_DELETE };
		const ULONGLONG now = GetTickCount64();
		bool press[10] = {};
		for (int i = 0; i < 10; ++i) {
			const bool held = focused && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
			if (held && !down[i]) {
				press[i] = true;
				repeatAt[i] = now + 350;  // arrows, Home and End repeat while held
			}
			else if (held && i >= 3 && i <= 8 && now >= repeatAt[i]) {
				press[i] = true;
				repeatAt[i] = now + 40;
			}
			down[i] = held;
		}
		std::lock_guard lk(g_lay.lock);
		if (press[0]) {
			if (!g_inVehicle.load() && !g_footShown.load() && !g_lay.edit) return;  // the buttons are visible in the rover or on foot
			g_lay.edit = !g_lay.edit;
			if (!g_lay.edit) SaveLayout();
			logger::Write("Hover UI: layout edit %s", g_lay.edit ? "ON" : "OFF, saved to hoverui.ini");
			notify::Show(g_lay.edit ? "UI edit: %s" : "UI saved", g_lay.edit ? SelName(g_inVehicle.load()).c_str() : "");
			return;
		}
		if (!g_lay.edit) return;
		const double step = focused && (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 10.0 : 1.0;
		const double dx = (press[4] ? step : 0.0) - (press[3] ? step : 0.0);
		const double dy = (press[6] ? step : 0.0) - (press[5] ? step : 0.0);
		const double dd = (press[8] ? step : 0.0) - (press[7] ? step : 0.0);
		if (!g_inVehicle.load()) {
			// On foot: Page Up / Page Down pick Summon, Follow, Get in, status, gang or the whole panel; the arrows move it.
			if (press[1] || press[2]) {
				g_lay.fsel = (g_lay.fsel + (press[1] ? kFootEls : 1)) % (kFootEls + 1);
				notify::Show("UI edit: %s", SelName(false).c_str());
			}
			if (dx != 0.0 || dy != 0.0) {
				if (g_lay.fsel == kFootEls) { g_lay.fpx += dx; g_lay.fpy += dy; }
				else { g_lay.footOff[g_lay.fsel][0] += dx; g_lay.footOff[g_lay.fsel][1] += dy; }
				SaveLayout();
			}
			if (press[9] && g_lay.fsel < kFootEls) { g_lay.footOff[g_lay.fsel][0] = g_lay.footOff[g_lay.fsel][1] = 0.0; SaveLayout(); }
			return;
		}
		if (press[1] || press[2]) {
			const auto order = SelOrder();
			auto it = std::find(order.begin(), order.end(), g_lay.sel);
			int at = it == order.end() ? 0 : int(it - order.begin());
			at = (at + (press[1] ? int(order.size()) - 1 : 1)) % int(order.size());
			g_lay.sel = order[at];
			notify::Show("UI edit: %s", SelName(true).c_str());
		}
		if (ScannerOpen()) {
			// In the scanner: Page Up / Page Down pick Summon, Follow, the status box or all; the arrows move them.
			if (press[1]) g_lay.msel = (g_lay.msel + 3) % 4;
			if (press[2]) g_lay.msel = (g_lay.msel + 1) % 4;
			const double sstep = focused && (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 10.0 : 1.0;
			const double sdx = (press[4] ? sstep : 0.0) - (press[3] ? sstep : 0.0), sdy = (press[6] ? sstep : 0.0) - (press[5] ? sstep : 0.0);
			if (sdx != 0.0 || sdy != 0.0) {
				const int m = g_lay.msel;
				if (m == 0 || m == 3) { g_lay.sx += sdx; g_lay.sy += sdy; }
				if (m == 1 || m == 3) { g_lay.fx += sdx; g_lay.fy += sdy; }
				if (m == 2 || m == 3) { g_lay.tx += sdx; g_lay.ty += sdy; }
				SaveLayout();
			}
			return;
		}
		const int sel = g_lay.sel;
		const bool all = sel == kSelAll;
		bool changed = false;
		if (dx != 0.0 || dy != 0.0) {
			if (all || sel == kSelCluster) { g_lay.cx += dx; g_lay.cy += dy; }
			if (all || sel == kSelColumn) { g_lay.colRight += dx; g_lay.colTop += dy; }
			if (all || sel == kGear) { g_lay.gx += dx; g_lay.gy += dy; }
			if (all || sel == kSelExtras) { g_lay.eox += dx; g_lay.eoy += dy; }
			if (sel >= 0 && sel < kGear) { g_lay.off[sel][0] += dx; g_lay.off[sel][1] += dy; }
			if (sel >= kSelAuto && sel <= kSelMode) { g_lay.carOff[sel - kSelAuto][0] += dx; g_lay.carOff[sel - kSelAuto][1] += dy; }
			for (int i = 0; i < int(g_lay.natNames.size()); ++i) {
				if (all || sel == kSelNative + i) {
					auto& o = g_lay.natOff[g_lay.natNames[i]];
					o.first += dx;
					o.second += dy;
				}
			}
			changed = true;
		}
		if (press[9]) {
			// Delete: the picked element back to its default place.
			if (sel >= 0 && sel < kGear) { g_lay.off[sel][0] = kDefOff[sel][0]; g_lay.off[sel][1] = kDefOff[sel][1]; changed = true; }
			if (sel >= kSelAuto && sel <= kSelMode) { g_lay.carOff[sel - kSelAuto][0] = g_lay.carOff[sel - kSelAuto][1] = 0.0; changed = true; }
			if (sel >= kSelNative && sel - kSelNative < int(g_lay.natNames.size())) { g_lay.natOff.erase(g_lay.natNames[sel - kSelNative]); changed = true; }
		}
		if (dd != 0.0) {
			// Home / End: the WASD keys move apart / together at equal distance; the column's rows likewise.
			if (sel == kSelCluster || (sel >= kW && sel <= kD)) { g_lay.d = std::max(20.0, g_lay.d + dd); changed = true; }
			if (sel == kSelColumn || (sel >= kGearDown && sel <= kHover)) { g_lay.colStep = std::max(10.0, g_lay.colStep + dd); changed = true; }
		}
		if (changed) SaveLayout();
	}
}
