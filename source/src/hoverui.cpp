#include "hoverui.h"

#include "addresses.h"
#include "game.h"
#include "log.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

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
		// Editor selections: each element (W, A, S, D, Gear-, Gear+, Hover, Gear box), then the groups.
		enum { kSelCluster = kButtons, kSelColumn, kSelAll, kSelCount };
		struct Layout_t
		{
			std::mutex lock;
			// The player's layout (hoverui.ini, 2026-10-17) is the default; hoverui.ini overrides it.
			// Minted from the player's final hoverui.ini (2026-10-17); F7 still edits, and hoverui.ini overrides.
			double cx = 176.0, cy = -170.0, d = 60.0;  // d <= 0: measured once (W's key + label clear the S row)
			bool labelsAbove = false;  // unused: Accelerate's label is above W, the others under their keys
			double colRight = 590.0, colTop = -246.0, colStep = 54.5;
			double gx = 417.4, gy = 50.2;
			double sx = 1500.0, sy = 900.0;  // the scanner call button's key box centre, in the scanner root's coordinates
			double fx = 1610.0, fy = 900.0;  // the scanner follow button's key box centre
			double tx = 1760.0, ty = 900.0;  // the scanner status box's centre
			int msel = 3;                    // scanner editor: 0 Summon, 1 Follow, 2 status, 3 all
			bool scannerOld = false;          // read as the earlier top-left corner: converted once the button exists
			bool haveCluster = true, haveColumn = true, haveGearBox = true;
			bool oldRows = false;      // only the earlier per-row positions are known: derive the column from them
			double oldX[3] = {}, oldY[3] = {};  // Gear+, Gear-, Hover (the earlier order)
			double off[kButtons][2] = { { 0.0, -31.0 }, { -23.0, 0.0 }, { 0.0, 0.0 }, { 24.0, 0.0 } };  // per element nudges (W, A, S, D, Gear-, Gear+, Hover); the gear box uses gx, gy
			bool edit = false;
			int sel = kSelAll;
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
				}
				else if (std::sscanf(line.c_str(), "scanner_follow = %lf %lf", &a, &b) == 2) { g_lay.fx = a; g_lay.fy = b; }
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
		} g_ui;
		unsigned g_calls = 0;
		ULONGLONG g_lastReport = 0;

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
			if (release) Release(g_ui.widget);
			g_ui.widget = Value{};
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
			for (int i = 0; i < kButtons; ++i) {
				if (!CreateObject(g_ui.movie, &g_ui.buttons[i], "BasicButton", nullptr, 0)) {
					logger::Write("Hover UI: could not create BasicButton (ButtonClips.swf not loaded in the HUD?)");
					return false;
				}
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
		void PlaceButtonKey(const Value& b, double x, double y, bool above);
		void PlaceKey(int i, double x, double y, bool above) { PlaceButtonKey(g_ui.buttons[i], x, y, above); }

		void PlaceButtonKey(const Value& b, double x, double y, bool above)
		{
			double px, py, pw, ph;
			KeyBox(b, px, py, pw, ph);
			SetMember(b, "x", Num(x - (px + pw / 2.0)));
			SetMember(b, "y", Num(y - (py + ph / 2.0)));
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
			for (int i = 0; i < kButtons; ++i) {
				const int group = i <= kD ? kSelCluster : i == kGear ? -1 : kSelColumn;
				const bool picked = sel == kSelAll || sel == i || sel == group;
				SetMember(g_ui.buttons[i], "alpha", Num(blinkOff && picked ? 0.35 : 1.0));
			}
		}

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
				logger::Write("Hover UI: HUD movie advanced %u times in the last minute", g_calls);
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

		void MonoWorkBody(uptr menu)
		{
			const uptr movie = *reinterpret_cast<uptr*>(menu + 0x88);
			if (movie != g_mono.movie || menu != g_mono.menu) {
				g_mono = Mono{};  // a new scanner (menu or movie): the old objects went with the old one (13:57:54 AV)
				g_mono.movie = movie;
				g_mono.menu = menu;
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
			const bool avail = g_callAvail.load();
			const int state = g_status.load();  // 0 idle, 1 summoning, 2 following
			std::string callKey, followKey;
			{
				std::lock_guard lk(g_callLock);
				callKey = g_callKey;
				followKey = g_followKey;
			}
			MonoShow(g_mono.summon, avail);
			MonoShow(g_mono.follow, avail && !followKey.empty());
			MonoShow(g_mono.status, avail && state != 0);
			if (!avail) return;
			MonoSetData(movie, g_mono.summon, "Summon");
			MonoSetKey(g_mono.summon, callKey);
			MonoSetData(movie, g_mono.follow, "Follow");
			MonoSetKey(g_mono.follow, followKey);
			MonoSetData(movie, g_mono.status, " ");
			MonoSetKey(g_mono.status, state == 2 ? std::string("FOLLOWING") : state == 1 ? std::string("SUMMONING") : std::string(" "));
			MonoTint(movie, g_mono.status, state != 0);
			double x[3], y[3];
			bool edit;
			int sel;
			{
				std::lock_guard lk(g_lay.lock);
				if (g_lay.scannerOld) {
					// The earlier top-left corner (label beside the key): keep the key where the player put it.
					double px, py, pw, ph;
					KeyBox(g_mono.summon.button, px, py, pw, ph);
					if (pw > 0.0) {
						g_lay.sx += px + pw / 2.0;
						g_lay.sy += py + ph / 2.0;
						g_lay.scannerOld = false;
						SaveLayout();
					}
				}
				x[0] = g_lay.sx; y[0] = g_lay.sy;
				x[1] = g_lay.fx; y[1] = g_lay.fy;
				x[2] = g_lay.tx; y[2] = g_lay.ty;
				edit = g_lay.edit;
				sel = g_lay.msel;
			}
			MonoEl* els[3] = { &g_mono.summon, &g_mono.follow, &g_mono.status };
			const bool blinkOff = edit && (GetTickCount64() / 300) % 2 == 0;
			for (int i = 0; i < 3; ++i) {
				PlaceButtonKey(els[i]->button, x[i], y[i], false);
				SetMember(els[i]->button, "alpha", Num(blinkOff && (sel == 3 || sel == i) ? 0.35 : 1.0));
			}
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
			const uptr mono = g_monoMenu.load();
			if (mono && movie == g_monoMovie.load()) {
				MonoWork(mono);
			}
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
			if (reinterpret_cast<uptr>(*p) != game::Base() + expectRva) return false;
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

	void Reset() { g_reset = true; }

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
			if (!g_inVehicle.load() && !ScannerOpen() && !g_lay.edit) return;  // the buttons are visible in the rover or the scanner
			g_lay.edit = !g_lay.edit;
			if (!g_lay.edit) SaveLayout();
			logger::Write("Hover UI: layout edit %s", g_lay.edit ? "ON" : "OFF, saved to hoverui.ini");
			return;
		}
		if (!g_lay.edit) return;
		if (press[1]) g_lay.sel = (g_lay.sel + kSelCount - 1) % kSelCount;
		if (press[2]) g_lay.sel = (g_lay.sel + 1) % kSelCount;
		const double step = focused && (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 10.0 : 1.0;
		const double dx = (press[4] ? step : 0.0) - (press[3] ? step : 0.0);
		const double dy = (press[6] ? step : 0.0) - (press[5] ? step : 0.0);
		const double dd = (press[8] ? step : 0.0) - (press[7] ? step : 0.0);
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
			if (sel >= 0 && sel < kGear) { g_lay.off[sel][0] += dx; g_lay.off[sel][1] += dy; }
			changed = true;
		}
		if (dd != 0.0) {
			// Home / End: the WASD keys move apart / together at equal distance; the column's rows likewise.
			if (sel == kSelCluster || (sel >= kW && sel <= kD)) { g_lay.d = std::max(20.0, g_lay.d + dd); changed = true; }
			if (sel == kSelColumn || (sel >= kGearDown && sel <= kHover)) { g_lay.colStep = std::max(10.0, g_lay.colStep + dd); changed = true; }
		}
		if (changed) SaveLayout();
	}
}
