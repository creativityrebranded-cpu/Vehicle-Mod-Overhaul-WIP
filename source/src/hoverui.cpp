#include "hoverui.h"
#include "bindings.h"
#include "features.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "notify.h"
#include "controlmap.h"
#include "controlspage.h"
#include "scannercontext.h"
#include "vehiclecamera.h"
#include "cameraroll.h"

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
		bool GetVariable(uptr movie, Value* out, const char* path)
		{
			const uptr root = *reinterpret_cast<uptr*>(movie + 0x18);
			return root && reinterpret_cast<bool (*)(uptr, Value*, const char*)>((*reinterpret_cast<void***>(root))[0x32])(root, out, path);
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
		//  - Column: Descend, Gear, Hover top to bottom, right-aligned on their key boxes' right edge (colRight), key centres
		//    colTop, colTop + colStep, colTop + 2 colStep.
		//  - Gear box: a key box with the gear number only, by its centre (gx, gy).
		// Saved to Documents\My Games\Starfield\SFSE\Logs\SeamlessVehicle\hoverui.ini.
		enum { kW, kA, kS, kD, kGearDown, kGearUp, kHover, kGear, kButtons };
		// Editor selections: each element (W, A, S, D, Descend, Gear, Hover, Gear box, Trip status, Target), each of the
		// rover widget's own clips (kSelNative + i, e.g. the Exit button), then the groups.
		enum { kSelLine = kButtons, kSelTarget, kSelRows, kSelCluster, kSelColumn, kSelExtras, kSelAll, kSelRoute = 90, kSelAttitude = 91, kSelAttitudeText = 92, kSelNative = 100 };
		constexpr int kFootEls = 8, kCarEls = 3;
		constexpr double kBaseScale = 0.75;
		// Each element has its own size (F7 editor: + / - on the selection; hoverui.ini "size_<name>"). 1 = the default look.
		constexpr double kMinSize = 0.5, kMaxSize = 2.5;
		double OverlayScale() { return kBaseScale; }
		const char* const kButtonNames[kButtons] = { "W", "A", "S", "D", "geardown", "gearup", "hover", "gear" };
		// Legacy gang layout keys retain the exact saved position now used by Target.
		const char* const kFootNames[kFootEls] = { "summon", "follow", "getin", "status", "gang", "pass", "eject", "vehicles" };
		const char* const kCarNames[kCarEls] = { "autoline", "ganginveh", "vehiclesinveh" };
		// The defaults are the user's own layout (hoverui.ini of 2026-10-21 17:5x, "use that as mod default"); F7's Delete returns to them.
		constexpr double kDefOff[kButtons][2] = { { 0.0, -21.0 }, { -24.0, 0.0 }, { 0.0, 0.0 }, { 24.0, 1.0 }, { -2.0, 0.0 }, { 0.0, 0.0 }, { 3.0, 0.0 }, { 0.0, 0.0 } };
		constexpr double kDefFoot[kFootEls][2] = { { -9.0, 45.0 }, { 165.0, 44.0 }, { -167.0, 42.0 }, { 64.0, -90.0 }, { 65.0, -175.0 }, { -29.0, -7.0 }, { -143.0, -53.0 }, { 66.0, -178.0 } };
		constexpr double kDefCar[kCarEls][2] = { { 845.0, 8.0 }, { 1007.0, -324.0 }, { 1006.0, -261.0 } };
		constexpr double kDefRouteX = 38.0, kDefRouteY = 160.0;
		// VTOL instrument and its P/B readout: separate F7 elements; the readout starts where it sat above the instrument.
		constexpr double kDefAttX = 0.0, kDefAttY = 170.0, kDefAttTextX = 0.0, kDefAttTextY = 64.0, kAttTextScale = 0.65;
		struct Layout_t
		{
			std::mutex lock;
			// Minted from the player's hoverui.ini of 2026-10-21 17:5x (every element); F7 still edits, and hoverui.ini overrides.
			double cx = 158.0, cy = -258.0, d = 60.0;  // d <= 0: measured once (W's key + label clear the S row)
			bool labelsAbove = false;  // unused: Accelerate's label is above W, the others under their keys
			double colRight = 588.0, colTop = -348.0, colStep = 54.5;
			double gx = 539.4, gy = -416.8;
			double sx = 1739.0, sy = 961.0;  // the scanner call button's key box centre, in the scanner root's coordinates
			double fx = 1665.0, fy = 961.0;  // the scanner follow button's key box centre
			double tx = 1701.0, ty = 908.0;  // the scanner status box's centre
			double routeX=kDefRouteX,routeY=kDefRouteY; // route group in HUD/scanner root coordinates
			double attitudeX=kDefAttX,attitudeY=kDefAttY,attitudeSize=1; // offset from screen centre, in HUD coordinates
			double attitudeTextX=kDefAttTextX,attitudeTextY=kDefAttTextY,attitudeTextSize=1; // the P/B readout, same frame
			int msel = 3;                    // scanner editor: 0 Summon, 1 Follow, 2 status, 3 all
			// The contextual HUD (2026-10-17): the on-foot panel (Summon, Follow, Get in, status, target) in RightMeters_mc's
			// coordinates (Summon's key centre; measured from the rover widget once), and the in-vehicle extras' offset.
			double fpx = -292.4, fpy = -237.4;
			bool haveFoot = true;
			double eox = -843.0, eoy = -224.0;
			bool scannerOld = false;          // read as the earlier top-left corner: converted once the button exists
			bool haveCluster = true, haveColumn = true, haveGearBox = true;
			bool oldRows = false;      // only the earlier per-row positions are known: derive the column from them
			double oldX[3] = {}, oldY[3] = {};  // Gear+, Gear-, Hover (the earlier order)
			double off[kButtons][2] = { { kDefOff[0][0], kDefOff[0][1] }, { kDefOff[1][0], kDefOff[1][1] }, { kDefOff[2][0], kDefOff[2][1] }, { kDefOff[3][0], kDefOff[3][1] },
				{ kDefOff[4][0], kDefOff[4][1] }, { kDefOff[5][0], kDefOff[5][1] }, { kDefOff[6][0], kDefOff[6][1] }, { kDefOff[7][0], kDefOff[7][1] } };  // per element nudges (W, A, S, D, Gear-, Gear+, Hover); the gear box uses gx, gy
			double footOff[kFootEls][2] = { { kDefFoot[0][0], kDefFoot[0][1] }, { kDefFoot[1][0], kDefFoot[1][1] }, { kDefFoot[2][0], kDefFoot[2][1] }, { kDefFoot[3][0], kDefFoot[3][1] },
				{ kDefFoot[4][0], kDefFoot[4][1] }, { kDefFoot[5][0], kDefFoot[5][1] }, { kDefFoot[6][0], kDefFoot[6][1] }, { kDefFoot[7][0], kDefFoot[7][1] } };  // per element nudges of the on-foot panel (Summon, Follow, Get in, status, target, Passenger, Eject, vehicles, Auto mode)
			double carOff[kCarEls][2] = { { kDefCar[0][0], kDefCar[0][1] }, { kDefCar[1][0], kDefCar[1][1] }, { kDefCar[2][0], kDefCar[2][1] } };    // per element nudges of the in-vehicle extras (Trip status, target, vehicles, Auto mode)
			// Per element sizes (multipliers of each element's default scale).
			double btnSize[kButtons] = { 1.35, 1.35, 1.35, 1.35, 1.35, 1.35, 1.35, 1.35 }, footSize[kFootEls] = { 1.15, 1.15, 1.15, 1.15, 1.15, 1.15, 1.15, 1.15 },
				carSize[kCarEls] = { 1.35, 1.60, 1.85 }, routeSize = 1.35;
			// The rover widget's own clips (found by name when the widget is first seen) and their nudges.
			std::vector<std::string> natNames;
			std::map<std::string, std::pair<double, double>> natOff;
			bool edit = false;
			int sel = kSelAll;
			int fsel = kFootEls;  // on foot: 0..4 one element, 5 the whole panel
		} g_lay;

		// The streamlined HUD (2026-10-21, the user): one status card, only the prompts that work now, the basic controls
		// (WASD, Alt, Hover, gear box) on the first rides and for 10 s after getting in, and a key that shows everything.
		std::atomic<bool> g_showAll{ false }, g_editOn{ false };
		std::atomic<ULONGLONG> g_boardedAt{ 0 };
		std::atomic<int> g_rides{ 0 }, g_hintsKey{ 0 };
		constexpr int kTeachRides = 3;
		constexpr ULONGLONG kTeachMs = 10000;
		bool BasicsVisible() { return g_showAll || g_editOn || g_rides <= kTeachRides || GetTickCount64() - g_boardedAt < kTeachMs; }

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
			std::snprintf(line, sizeof(line), "column = %.1f %.1f %.1f   ; key boxes' right edge, first key centre y, row step (Descend, Gear, Hover)\n", g_lay.colRight,
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
			std::snprintf(line,sizeof(line),"route = %.1f %.1f\n",g_lay.routeX,g_lay.routeY);out<<line;
			std::snprintf(line,sizeof(line),"attitude = %.1f %.1f\n",g_lay.attitudeX,g_lay.attitudeY);out<<line;
			std::snprintf(line,sizeof(line),"attitude_text = %.1f %.1f\n",g_lay.attitudeTextX,g_lay.attitudeTextY);out<<line;
			std::snprintf(line,sizeof(line),"rides = %d   ; vehicle boardings seen (the basic controls show for the first %d)\n",g_rides.load(),kTeachRides);out<<line;
			std::snprintf(line,sizeof(line),"show_all = %d   ; every hint and panel shown (the hints key toggles it)\n",g_showAll.load()?1:0);out<<line;
			// Sizes (F7 editor: + / - on the selection), 1 = default.
			auto size=[&](const char* name,double v){std::snprintf(line,sizeof(line),"size_%s = %.2f\n",name,v);out<<line;};
			for(int i=0;i<kButtons;++i)size(kButtonNames[i],g_lay.btnSize[i]);
			for(int i=0;i<kFootEls;++i)size((std::string("foot_")+kFootNames[i]).c_str(),g_lay.footSize[i]);
			for(int i=0;i<kCarEls;++i)size(kCarNames[i],g_lay.carSize[i]);
			size("route",g_lay.routeSize);
			size("attitude",g_lay.attitudeSize);
			size("attitude_text",g_lay.attitudeTextSize);
			if (g_lay.haveFoot) {
				std::snprintf(line, sizeof(line), "foot = %.1f %.1f   ; on-foot panel: Summon's key centre x y\n", g_lay.fpx, g_lay.fpy);
				out << line;
			}
			std::snprintf(line, sizeof(line), "extras = %.1f %.1f   ; in-vehicle Auto / status / target offset x y\n", g_lay.eox, g_lay.eoy);
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
			bool haveAttText = false;
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
				else if (std::sscanf(line.c_str(),"route = %lf %lf",&a,&b)==2) {g_lay.routeX=a;g_lay.routeY=b;}
				else if (std::sscanf(line.c_str(),"attitude = %lf %lf",&a,&b)==2 && std::isfinite(a) && std::isfinite(b)) {g_lay.attitudeX=a;g_lay.attitudeY=b;}
				else if (std::sscanf(line.c_str(),"attitude_text = %lf %lf",&a,&b)==2 && std::isfinite(a) && std::isfinite(b)) {g_lay.attitudeTextX=a;g_lay.attitudeTextY=b;haveAttText=true;}
				else if (std::sscanf(line.c_str(),"rides = %d",&i)==1) g_rides=std::max(0,i);
				else if (std::sscanf(line.c_str(),"show_all = %d",&i)==1) g_showAll=i!=0;
				else if (line.rfind("size_",0)==0) {
					char name[64]={};
					if(std::sscanf(line.c_str(),"size_%63s = %lf",name,&a)==2 && std::isfinite(a)) {
						a=std::clamp(a,kMinSize,kMaxSize);const std::string n=name;
						for(int k=0;k<kButtons;++k)if(n==kButtonNames[k])g_lay.btnSize[k]=a;
						for(int k=0;k<kFootEls;++k)if(n==std::string("foot_")+kFootNames[k])g_lay.footSize[k]=a;
						for(int k=0;k<kCarEls;++k)if(n==kCarNames[k])g_lay.carSize[k]=a;
						if(n=="route")g_lay.routeSize=a;
						if(n=="attitude")g_lay.attitudeSize=a;
						if(n=="attitude_text")g_lay.attitudeTextSize=a;
					}
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
			if (!haveAttText) {
				// Before the split the readout rode inside the instrument at (0, -106), 0.65 of its size: keep it there.
				g_lay.attitudeTextX=g_lay.attitudeX; g_lay.attitudeTextY=g_lay.attitudeY-106.0*g_lay.attitudeSize;
				g_lay.attitudeTextSize=g_lay.attitudeSize;
			}
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
			std::string summonKey, followKey, useKey, status, autoLine, summonLabel, coverLabel, autoKey, lifebar;
			std::string rows[kMaxRows];
			int rowCount = 0;
			std::string vbarName;  // the vehicle bar under the game's enemy bar (empty: hidden)
			float vbarShare = 0;
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
		void DropAttitude(bool release);
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
			DropAttitude(release);
			g_ui.gearShown = -1;
			g_ui.hoverTint = -1;
			g_ui.built = false;
		}

		bool SetButton(int i, const char* label)
		{
			if (std::strcmp(g_ui.text[i], label) == 0 && IsObject(g_ui.data[i])) return true;
			Value ued;
			const Value uedArgs[1] = { Str(i==kGear||i==kGearDown||i==kGearUp||i==kHover?"":kEvents[i]) };
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
			// Custom gear text must win over native binding refreshes every frame.
			const Value& btn = g_ui.buttons[kGear];
			if (!IsObject(btn)) return;
			char t[16];
			std::snprintf(t, sizeof(t), "GEAR %d", gear);
			const Value args[1] = { Str(t) };
			Value pc, inst;
			bool ok = false;
			if (GetMember(btn, "PCButton_mc", &pc) && IsObject(pc) && GetMember(pc, "ButtonInstance_mc", &inst) && IsObject(inst)) {
				SetMember(pc,"visible",Bool(true));SetMember(inst,"visible",Bool(true));
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
			SetButton(kW, g_accelHeld.load() ? "Accelerate (hold)" : "Accelerate");
			SetButton(kA, "Left");
			SetButton(kS, "Brake");
			SetButton(kD, "Right");
			// One Alt button (2026-10-21 UI review: "Descend (hold)" and "Gear (tap)" were two buttons with one key).
			SetButton(kGearDown, "Tap: gear  Hold: descend");
			// The hover key (2026-10-24: the hover toggle only; the vertical boost double tap is gone), shown in its key box.
			SetButton(kHover, hovering ? "Land" : "Hover");
			TintHover(hovering);
			if (std::strcmp(g_ui.text[kGear], " ") != 0) g_ui.gearShown = -1;  // new data: its key text is the key again
			SetButton(kGear, " ");
			for(const int i:{kGearDown,kHover}){Value pc,inst;
				if(GetMember(g_ui.buttons[i],"PCButton_mc",&pc)&&GetMember(pc,"ButtonInstance_mc",&inst)){SetMember(pc,"visible",Bool(true));SetMember(inst,"visible",Bool(true));const auto key=i==kHover?bindings::Label(bindings::Action::HoverToggle):bindings::Label(bindings::Action::Gear,VK_MENU);const Value args[]={Str(key.c_str())};Invoke(inst,"SetText",args,1);}
				Release(inst);Release(pc);
			} // no label: "GEAR n" is all inside the key box
			// The basic controls: the first rides and 10 s after getting in, then hidden (the hints key or F7 shows them).
			const bool basics = BasicsVisible();
			// The gear box only with every hint on (or in F7): the status card already reads "GEAR n".
			for (int i = 0; i < kButtons; ++i) Show(i, basics && i != kGearUp && (i != kGear || g_showAll || g_editOn));
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
		void PlaceButtonKey(const Value& b, double x, double y, bool above, double scale = OverlayScale());
		double g_btnSizeNow[kButtons] = { 1, 1, 1, 1, 1, 1, 1, 1 };  // Layout()'s copy of g_lay.btnSize (the rover widget thread)
		void PlaceKey(int i, double x, double y, bool above) { PlaceButtonKey(g_ui.buttons[i], x, y, above, kBaseScale * g_btnSizeNow[i]); }

		void PlaceButtonKey(const Value& b, double x, double y, bool above, double scale)
		{
			double px, py, pw, ph;
			KeyBox(b, px, py, pw, ph);
			// the key box is measured in the button's own (unscaled) space; a scaled button lands it at scale * that
			SetMember(b, "scaleX", Num(scale));
			SetMember(b, "scaleY", Num(scale));
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
			const double s = g_btnSizeNow[i];  // the column keeps the native size (1) times its own size
			SetMember(b, "scaleX", Num(s));
			SetMember(b, "scaleY", Num(s));
			SetMember(b, "x", Num(right - s * (px + pw)));
			SetMember(b, "y", Num(y - s * (py + ph / 2.0)));
		}

		// A key button with its key box centred on (x, y) and the label beside it (the native label-first layout).
		void PlaceCentred(int i, double x, double y)
		{
			const Value& b = g_ui.buttons[i];
			double px, py, pw, ph;
			KeyBox(b, px, py, pw, ph);
			const double s = g_btnSizeNow[i];
			SetMember(b, "scaleX", Num(s));
			SetMember(b, "scaleY", Num(s));
			SetMember(b, "x", Num(x - s * (px + pw / 2.0)));
			SetMember(b, "y", Num(y - s * (py + ph / 2.0)));
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
				for (int i = 0; i < kButtons; ++i) { off[i][0] = g_lay.off[i][0]; off[i][1] = g_lay.off[i][1]; g_btnSizeNow[i] = g_lay.btnSize[i]; }
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
			PlaceRow(kHover, colRight + off[kHover][0], colTop + colStep + off[kHover][1]);  // the row "Gear (tap)" had is gone
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

		void RenderRoute(uptr menu, uptr movie, bool scanner);
		void RenderAttitude(uptr menu, uptr movie);

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
				RenderRoute(menu, movie, false);
				RenderAttitude(menu, movie);
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
		ScannerPick g_pick;
		std::string g_routeText;
		std::string g_routeSkipKey,g_routeClearKey,g_routeRetryKey;
		std::string g_scanText;  // the scanner screen status box text
		double g_targetX = 0, g_targetY = 0;
		bool g_targetPlaced = false;

		bool TransformPoint(uptr movie, const Value& space, const char* method, double& x, double& y)
		{
			Value point, result;
			const Value coords[] = { Num(x), Num(y) };
			bool ok = false;
			if (IsObject(space) && CreateObject(movie, &point, "flash.geom.Point", coords, 2)) {
				const Value args[] = { Arg(point) };
				ok = InvokeR(space, method, args, 1, &result) && IsObject(result) &&
					GetNumber(result, "x", x) && GetNumber(result, "y", y) && std::isfinite(x) && std::isfinite(y);
			}
			Release(result); Release(point);
			return ok;
		}
		struct MonoEl;
		std::uint64_t MonoMessageDetour(uptr menu, void* msg)
		{
			const auto r = g_monoOriginal(menu, msg);
			g_monoMovie.store(*reinterpret_cast<uptr*>(menu + 0x88));
			if (g_monoMenu.exchange(menu) != menu) logger::Write("Hover UI: scanner menu %p", reinterpret_cast<void*>(menu));
			return r;
		}

		bool ScannerOpen() { return GetTickCount64() - g_monoSeen.load() < 300; }

		// Vehicle settings and controls inside Gameplay Options (2026-10-22).
		// pausemenu.swf / mainmenu.swf: root.SettingsPanel_mc (Shared.Components.SystemPanels.SettingsPanel) fills
		// CategoryPanel_mc.CategoryList_mc from the data model SettingsCategoriesData and OptionsPanel_mc.OptionsList_mc from
		// Gameplay uses PEOPanel_mc.OptionsList_mc and PEOData.aGeneralSettingsList.
		// Our rows join the live gameplay entries; no extra category is added. The data model is
		// reached through public members only: root.loaderInfo.applicationDomain.getDefinition("Shared.AS3.Data.BSUIDataManager")
		// .GetDataFromClient("PEOData").data. The panel sends SettingsPanel_CheckBoxChanged / _OpenCategory /
		// _ResetToDefaults to the game through AutoRegisterEvent<TUIEventDispatcher<...>> (vtable slot 1 (this, GFx::Value*
		// event), FUN_14152DF90 / 14152D810 / 14152DD10); ours are handled here and never reach the game.
		constexpr std::uint32_t kRvaPauseVtbl = 0x04C909A8, kRvaPauseMessage = 0x01669060;  // PauseMenu (RTTI .?AVPauseMenu@@), slot 8
		constexpr std::uint32_t kRvaMainVtbl = 0x04D0A7F0, kRvaMainMessage = 0x01ECC710;    // MainMenu (RTTI .?AVMainMenu@@), slot 8
		constexpr std::uint32_t kRvaCheckDisp = 0x04C6EFE0, kRvaCheckFn = 0x0152DF90;
		constexpr std::uint32_t kRvaOpenDisp = 0x04C6F040, kRvaOpenFn = 0x0152D810;
		constexpr std::uint32_t kRvaResetDisp = 0x04C6F080, kRvaResetFn = 0x0152DD10;
		constexpr int kSlotDispatch = 1;
		constexpr unsigned kCategoryId = 0;                  // SettingsPanel.PEO_CATEGORY = Gameplay Options
		constexpr unsigned kOldCategoryId = 900;
		constexpr unsigned kSettingBase = 0x46560000;        // "FV": our option ids
		constexpr unsigned kControlBase = kSettingBase + 0x100;
		constexpr unsigned kRestoreControls = kControlBase + unsigned(bindings::Action::Count);
		constexpr unsigned kFeatureHeader = kSettingBase + 0x200, kControlsHeader = kFeatureHeader + 1;
		constexpr std::uint32_t kRvaLinkDisp = 0x04C6F0C0, kRvaLinkFn = 0x0152CE10;
		constexpr int kCheckBox = 3;                          // SettingsOptionListEntry.SDT_CHECKBOX
		constexpr const char* kCategoryName = "Flying Vehicle Overhaul";
		using Dispatch_t = void (*)(void*, Value*);
		Message_t g_pauseOriginal = nullptr, g_mainOriginal = nullptr;
		Dispatch_t g_checkOriginal = nullptr, g_openOriginal = nullptr, g_resetOriginal = nullptr, g_linkOriginal = nullptr;
		bool g_controlsAvailable = false;
		std::atomic<uptr> g_setMenu{ 0 }, g_setMovie{ 0 };
		std::atomic<bool> g_setDirty{ false };  // our options need their values again (reset to defaults)
		struct SetState { uptr movie = 0; bool failed = false, catLogged = false, optLogged = false, noPanelLogged = false, captureUI=false; unsigned bindingGeneration=0, featureGeneration=0;
			bool ctlVisible=false, ctlLogged=false, ctlSampled=false; unsigned ctlGeneration=0; } g_set;

		std::uint64_t PauseMessageDetour(uptr menu, void* msg)
		{
			const auto r = g_pauseOriginal(menu, msg);
			g_setMovie.store(*reinterpret_cast<uptr*>(menu + 0x88));
			if (g_setMenu.exchange(menu) != menu) logger::Write("Hover UI: pause menu %p (settings: %s category)", reinterpret_cast<void*>(menu), kCategoryName);
			return r;
		}

		std::uint64_t MainMessageDetour(uptr menu, void* msg)
		{
			const auto r = g_mainOriginal(menu, msg);
			g_setMovie.store(*reinterpret_cast<uptr*>(menu + 0x88));
			if (g_setMenu.exchange(menu) != menu) logger::Write("Hover UI: main menu %p (settings: %s category)", reinterpret_cast<void*>(menu), kCategoryName);
			return r;
		}

		bool GetBoolMember(const Value& o, const char* name, bool& out)
		{
			Value v;
			const bool ok = GetMember(o, name, &v) && (v.type & 0x8F) == kBool;
			if (ok) out = v.boolean;
			Release(v);
			return ok;
		}

		// A new AS3 object of `cls` ("Object", "Array").
		bool NewObject(uptr movie, Value* out, const char* cls) { return CreateObject(movie, out, cls, nullptr, 0); }

		void SetChild(uptr movie, const Value& o, const char* name, std::initializer_list<std::pair<const char*, Value>> fields)
		{
			Value c;
			if (!NewObject(movie, &c, "Object")) return;
			for (const auto& [k, v] : fields) SetMember(c, k, v);
			SetMember(o, name, Arg(c));
			Release(c);
		}

		void SetEmptyArray(uptr movie, const Value& o, const char* name)
		{
			Value a;
			if (NewObject(movie, &a, "Array")) SetMember(o, name, Arg(a));
			Release(a);
		}

		bool OurSetting(double id)
		{
			return (id>=kSettingBase&&id<kSettingBase+unsigned(features::Id::Count)) ||
				(id>=kControlBase&&id<=kRestoreControls) || id==kFeatureHeader || id==kControlsHeader;
		}
		void NeutralPeoData(uptr movie,const Value& entry)
		{
			Value data;if(NewObject(movie,&data,"Object")){
				SetEmptyArray(movie,data,"aDescriptions");SetEmptyArray(movie,data,"aRewards");
				SetMember(entry,"peoData",Arg(data));
			}Release(data);
		}
		void TextOption(uptr movie,const Value& list,unsigned id,int type,const char* label,const char* description,bool enabled=true)
		{
			Value entry;if(NewObject(movie,&entry,"Object")){
				SetMember(entry,"uID",Num(id));SetMember(entry,"uType",Num(type));SetMember(entry,"uCategory",Num(kCategoryId));
				SetMember(entry,"sText",Str(label));SetMember(entry,"sDescription",Str(description));SetMember(entry,"sPreview",Str(""));
				SetMember(entry,"bEnabled",Bool(enabled));SetMember(entry,"bSubSetting",Bool(false));SetMember(entry,"bIsPEO",Bool(false));
				Value stepper;if(NewObject(movie,&stepper,"Object")){SetEmptyArray(movie,stepper,"aStepperOptions");SetMember(stepper,"uIndex",Num(0));SetMember(entry,"stepperData",Arg(stepper));}Release(stepper);
				NeutralPeoData(movie,entry);const Value push[]={Arg(entry)};Invoke(list,"push",push,1);
			}Release(entry);
		}
		// Gameplay uses PEOPanel / PEOData, not the general OptionsPanel / SettingsData.
		bool OurOptions(uptr movie, Value* out)
		{
			if (!NewObject(movie, out, "Array")) return false;
			TextOption(movie,*out,kFeatureHeader,5,kCategoryName,"Vehicle gameplay and display settings.",false);
			for (int i = 0; i < int(features::Id::Count); ++i) {
				const auto& f = features::InfoOf(features::Id(i));
				Value e;
				if (!NewObject(movie, &e, "Object")) continue;
				SetMember(e, "uID", Num(double(kSettingBase + i)));
				SetMember(e, "uType", Num(kCheckBox));
				SetMember(e, "uCategory", Num(kCategoryId));
				SetMember(e, "sText", Str(f.label));
				SetMember(e, "sDescription", Str(f.description));
				SetMember(e, "sPreview", Str(""));
				SetMember(e, "bEnabled", Bool(true));
				SetMember(e, "bSubSetting", Bool(false));
				SetMember(e, "bIsPEO", Bool(false));
				NeutralPeoData(movie,e);
				SetChild(movie, e, "checkBoxData", { { "bChecked", Bool(features::On(f.id)) }, { "bOnCustomText", Str("") }, { "bOffCustomText", Str("") } });
				SetChild(movie, e, "sliderData", { { "fValue", Num(0) }, { "sDisplayValue", Str("") } });
				{
					Value st;
					if (NewObject(movie, &st, "Object")) {
						SetEmptyArray(movie, st, "aStepperOptions");
						SetMember(st, "uIndex", Num(0));
						SetMember(e, "stepperData", Arg(st));
					}
					Release(st);
				}
				const Value push[1] = { Arg(e) };
				Invoke(*out, "push", push, 1);
				Release(e);
			}
			// The controls live on the game's Controls page when its hooks are in (controlspage.h); these rows are the fallback.
			if(g_controlsAvailable&&!controlspage::Installed()){
				TextOption(movie,*out,kControlsHeader,5,"Vehicle controls","Select an action, then press its new key or controller button.",false);
				for(int i=0;i<int(bindings::Action::Count);++i){
					const auto action=bindings::Action(i);
					const auto label=std::string(bindings::ActionName(action))+": "+bindings::MenuLabel(action);
					const auto help=std::string(bindings::ActionDescription(action))+"\nSelect, release the select input, then press a key or mouse button. For a controller chord, hold the first button and press the second; release a single button to bind it. Esc cancels. Backspace restores this action's defaults. Delete clears this action. Remapping one device preserves the other.";
					TextOption(movie,*out,kControlBase+unsigned(i),4,label.c_str(),help.c_str());
				}
				TextOption(movie,*out,kRestoreControls,4,"Restore vehicle controls to defaults","Restores all vehicle controls to their default bindings.");
			}
			return true;
		}

		// The game's own option array (all categories opened so far), through the public data model.
		bool GameOptions(const Value& root, Value* out)
		{
			Value li, domain, cls, provider, data;
			const Value name[1] = { Str("Shared.AS3.Data.BSUIDataManager") };
			const Value model[1] = { Str("PEOData") };
			const bool ok = GetMember(root, "loaderInfo", &li) && IsObject(li) && GetMember(li, "applicationDomain", &domain) && IsObject(domain) &&
				InvokeR(domain, "getDefinition", name, 1, &cls) && IsObject(cls) && InvokeR(cls, "GetDataFromClient", model, 1, &provider) && IsObject(provider) &&
				GetMember(provider, "data", &data) && IsObject(data) && GetMember(data, "aGeneralSettingsList", out) && IsObject(*out);
			Release(data); Release(provider); Release(cls); Release(domain); Release(li);
			return ok;
		}

		// A GFx string value's text (managed strings hold a pointer to the text pointer).
		std::string StringOf(const Value& v)
		{
			if ((v.type & 0x8F) != kString) return {};
			const char* p = (v.type & kManaged) ? *reinterpret_cast<const char* const*>(v.string) : v.string;
			return p ? p : "";
		}
		// row.<binding>.<array>.join("|") split: the names a row shows for one slot.
		std::vector<std::string> SlotNames(const Value& row, const char* binding, const char* array)
		{
			std::vector<std::string> out;
			Value b, a, joined;
			const Value sep[] = { Str("|") };
			if (GetMember(row, binding, &b) && IsObject(b) && GetMember(b, array, &a) && IsObject(a) && InvokeR(a, "join", sep, 1, &joined)) {
				const auto text = StringOf(joined);
				for (std::size_t at = 0; !text.empty();) {
					const auto bar = text.find('|', at);
					out.push_back(text.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
					if (bar == std::string::npos) break;
					at = bar + 1;
				}
			}
			Release(joined); Release(a); Release(b);
			return out;
		}
		void PushStrings(uptr movie, const Value& o, const char* name, const std::vector<std::string>& items)
		{
			Value a;
			if (NewObject(movie, &a, "Array")) {
				for (const auto& t : items) { const Value push[] = { Str(t.c_str()) }; Invoke(a, "push", push, 1); }
				SetMember(o, name, Arg(a));
			}
			Release(a);
		}

		// The mod's controls under their own header on the game's Controls page (controlspage.h). The rows show the game's own
		// names for keys and buttons, learnt from its rows (their control-map binding -> what they show); our labels otherwise.
		struct NativeNames { std::map<int, std::string> keys; std::map<std::uint32_t, std::string> pads; };
		std::string NameOf(const NativeNames& n, const bindlogic::Input& in)
		{
			if (in.vk) { const auto it = n.keys.find(in.vk); return it != n.keys.end() ? it->second : bindlogic::KeyLabel(in.vk); }
			const auto it = n.pads.find(in.pad);
			return it != n.pads.end() ? it->second : bindings::PadName(in.pad);
		}
		// action < 0: the header (divider) row.
		void OurControlRow(uptr movie, const Value& list, const NativeNames& names, int action, bool pad, double category, double sort)
		{
			Value e;
			if (!NewObject(movie, &e, "Object")) return;
			const bool divider = action < 0;
			SetMember(e, "sInputName", Str(divider ? "" : bindings::EventName(bindings::Action(action))));
			SetMember(e, "sContextName", Str(controlspage::kContextName));
			SetMember(e, "uContextID", Num(controlspage::kContextId));
			SetMember(e, "uCategory", Num(category));
			SetMember(e, "uSortIndex", Num(sort));
			SetMember(e, "bIsDivider", Bool(divider));
			SetMember(e, "bRequired", Bool(false));
			SetMember(e, "bGamepadEntry", Bool(pad));
			SetMember(e, "bReadOnly", Bool(divider));
			const auto slots = divider ? std::vector<bindlogic::Input>{} : bindings::Slots(bindings::Action(action), pad);
			for (int s = 0; s < 2; ++s) {
				Value b;
				if (!NewObject(movie, &b, "Object")) continue;
				std::vector<std::string> shown;
				if (std::size_t(s) < slots.size()) {
					const auto& in = slots[std::size_t(s)];
					if (in.mod) shown.push_back(NameOf(names, { 0, in.mod, 0 }));
					shown.push_back(NameOf(names, in));
				}
				PushStrings(movie, b, "aButtonName", pad ? shown : std::vector<std::string>{});
				PushStrings(movie, b, "aPCKeyName", pad ? std::vector<std::string>{} : shown);
				SetMember(e, s ? "AltBinding" : "MainBinding", Arg(b));
				Release(b);
			}
			const Value push[] = { Arg(e) };
			Invoke(list, "push", push, 1);
			Release(e);
		}
		std::string JoinNames(const std::vector<std::string>& v) { std::string t; for (const auto& x : v) { if (!t.empty()) t += ","; t += x; } return t; }
		void ControlsPageWork(uptr movie, const Value& panel)
		{
			Value cp, list;
			bool shown = false;
			const bool ok = controlspage::Installed() && GetMember(panel, "ControlsPanel_mc", &cp) && IsObject(cp) && GetBoolMember(cp, "visible", shown) && shown &&
				GetMember(cp, "ControlsList_mc", &list) && IsObject(list);
			g_set.ctlVisible = ok;
			double count = 0;
			if (ok) GetNumber(list, "entryCount", count);
			Value all;
			if (ok && count > 0 && NewObject(movie, &all, "Array")) {
				bool haveOurs = false, anyPad = false, anyKbm = false;
				double category = 0, sort = 0;
				NativeNames names;
				for (int i = 0; i < int(count); ++i) {
					Value e;
					const Value at[] = { Num(i) };
					if (InvokeR(list, "GetDataForEntry", at, 1, &e) && IsObject(e)) {
						const auto ctx = GetString(e, "sContextName"), ev = GetString(e, "sInputName");
						if (ctx == controlspage::kContextName) haveOurs = true;
						else {
							bool gp = false, div = false;
							GetBoolMember(e, "bGamepadEntry", gp);
							GetBoolMember(e, "bIsDivider", div);
							(gp ? anyPad : anyKbm) = true;
							double c = 0, so = 0;
							if (!div && GetNumber(e, "uCategory", c)) category = c;
							if (GetNumber(e, "uSortIndex", so)) sort = std::max(sort, so);
							const auto mainNames = SlotNames(e, "MainBinding", gp ? "aButtonName" : "aPCKeyName");
							if (!div && mainNames.size() == 1 && !mainNames[0].empty()) {
								if (gp) { if (const auto bit = bindlogic::GamePadBit(controlmap::PadFor(ctx.c_str(), ev.c_str()))) names.pads.emplace(bit, mainNames[0]); }
								else if (const int vk = controlmap::KeyFor(ctx.c_str(), ev.c_str(), 0)) names.keys.emplace(vk, mainNames[0]);
							}
							// Evidence for the first test: what the game's dividers and first rows hold.
							if (!g_set.ctlSampled && (div || i < 3)) {
								double id = 0;
								bool req = false, ro = false;
								GetNumber(e, "uContextID", id);
								GetBoolMember(e, "bRequired", req);
								GetBoolMember(e, "bReadOnly", ro);
								logger::Write("Controls page: native row %d: divider %d pad %d ctx '%s' (0x%X) event '%s' cat %.0f sort %.0f req %d ro %d main [%s|%s] alt [%s|%s]", i, div, gp,
									ctx.c_str(), unsigned(id), ev.c_str(), c, so, req, ro, JoinNames(SlotNames(e, "MainBinding", "aPCKeyName")).c_str(),
									JoinNames(SlotNames(e, "MainBinding", "aButtonName")).c_str(), JoinNames(SlotNames(e, "AltBinding", "aPCKeyName")).c_str(),
									JoinNames(SlotNames(e, "AltBinding", "aButtonName")).c_str());
							}
							const Value push[] = { Arg(e) };
							Invoke(all, "push", push, 1);
						}
					}
					Release(e);
				}
				if (!g_set.ctlSampled) {
					g_set.ctlSampled = true;
					logger::Write("Controls page: %d native rows (keyboard %d, controller %d); learnt %zu key and %zu button names", int(count), anyKbm, anyPad, names.keys.size(), names.pads.size());
				}
				if (!haveOurs || g_set.ctlGeneration != bindings::Generation()) {
					int k = 0;
					for (const bool pad : { false, true }) {
						if (pad ? !anyPad : !anyKbm) continue;
						OurControlRow(movie, all, names, -1, pad, category, sort + ++k);
						for (int a = 0; a < int(bindings::Action::Count); ++a) OurControlRow(movie, all, names, a, pad, category, sort + ++k);
					}
					double selection = 0;
					GetNumber(list, "selectedIndex", selection);
					const Value init[] = { Arg(all) };
					Invoke(list, "InitializeEntries", init, 1);
					SetMember(list, "selectedIndex", Num(std::max(0.0, selection)));
					g_set.ctlGeneration = bindings::Generation();
					if (!g_set.ctlLogged) { g_set.ctlLogged = true; logger::Write("Hover UI: settings: vehicle controls added to the Controls page under %s", controlspage::kContextName); }
				}
			}
			Release(all); Release(list); Release(cp);
		}

		void SettingsWorkBody(uptr menu, uptr movie)
		{
			if (movie != g_set.movie) g_set = SetState{ movie };
			if (g_set.failed) return;
			const Value& root = *reinterpret_cast<const Value*>(menu + 0x58);
			if (!IsObject(root)) return;
			Value panel, catPanel, catList, optPanel, optList;
			bool visible = false, gameplayVisible = false;
			if (!GetMember(root,"SettingsPanel_mc",&panel)||!IsObject(panel)) {Release(panel);bindings::CancelCapture();return;}
			const bool ready=GetBoolMember(panel,"visible",visible)&&visible&&
				GetMember(panel,"CategoryPanel_mc",&catPanel)&&IsObject(catPanel)&&
				GetMember(catPanel,"CategoryList_mc",&catList)&&IsObject(catList)&&
				GetMember(panel,"PEOPanel_mc",&optPanel)&&IsObject(optPanel)&&
				GetMember(optPanel,"OptionsList_mc",&optList)&&IsObject(optList);
			if(ready){
				// Remove only our obsolete category; retain every game/other-mod entry.
				if(!g_set.catLogged){
				double count=0;GetNumber(catList,"entryCount",count);
				bool oldCategory=false;Value categories;
				if(NewObject(movie,&categories,"Array")){
					for(int i=0;i<int(count);++i){
						Value entry;const Value at[]={Num(i)};double id=-1;
						if(InvokeR(catList,"GetDataForEntry",at,1,&entry)&&IsObject(entry)){
							GetNumber(entry,"uID",id);
							if(id==kOldCategoryId)oldCategory=true;
							else {const Value push[]={Arg(entry)};Invoke(categories,"push",push,1);}
						}Release(entry);
					}
					if(oldCategory){const Value init[]={Arg(categories)};Invoke(catList,"InitializeEntries",init,1);}
				}Release(categories);g_set.catLogged=true;
				}
				Value selected;double selectedId=-1;
				if(GetMember(catList,"selectedEntry",&selected)&&IsObject(selected))GetNumber(selected,"uID",selectedId);
				Release(selected);
				GetBoolMember(optPanel,"visible",gameplayVisible);
				gameplayVisible=gameplayVisible&&selectedId==kCategoryId;
			}
			if(!gameplayVisible)bindings::CancelCapture();
			else {
				bindings::CaptureTick();
				bool capture=bindings::Capturing();
				if(capture!=g_set.captureUI){SetMember(panel,"remappingControl",Bool(capture));g_set.captureUI=capture;}
				double count=0;GetNumber(optList,"entryCount",count);
				bool haveOurs=false;
				for(int i=0;i<int(count);++i){Value entry;const Value at[]={Num(i)};double id=-1;
					if(InvokeR(optList,"GetDataForEntry",at,1,&entry)&&IsObject(entry))GetNumber(entry,"uID",id);
					haveOurs=haveOurs||id==kFeatureHeader;Release(entry);
				}
				const auto bindingGeneration=bindings::Generation(), featureGeneration=features::Generation();
				const bool dirty=g_setDirty.exchange(false)||g_set.bindingGeneration!=bindingGeneration||g_set.featureGeneration!=featureGeneration;
				if(!haveOurs||dirty){
					Value all,ours;
					if(NewObject(movie,&all,"Array")&&OurOptions(movie,&ours)){
						// Copy the live list so entries injected by other mods survive.
						for(int i=0;i<int(count);++i){Value entry;const Value at[]={Num(i)};double id=-1;
							if(InvokeR(optList,"GetDataForEntry",at,1,&entry)&&IsObject(entry)){
								GetNumber(entry,"uID",id);if(!OurSetting(id)){const Value push[]={Arg(entry)};Invoke(all,"push",push,1);}
							}Release(entry);
						}
						// If the native provider has not arrived yet, wait for it; never
						// replace the gameplay screen with only the mod's options.
						Value native;
						bool haveNative=count>0;
						if(!haveNative&&GameOptions(root,&native)){double nativeCount=0;haveNative=GetNumber(native,"length",nativeCount)&&nativeCount>0;}
						if(haveNative){
							const Value append[]={Arg(ours)};Value merged;
							if(InvokeR(count==0&&IsObject(native)?native:all,"concat",append,1,&merged)&&IsObject(merged)){
								double selection=0;GetNumber(optList,"selectedIndex",selection);
								const Value init[]={Arg(merged)};Invoke(optList,"InitializeEntries",init,1);
								SetMember(optList,"selectedIndex",Num(std::max(0.0,selection)));
								g_set.bindingGeneration=bindings::Generation();g_set.featureGeneration=featureGeneration;
								if(!g_set.optLogged){g_set.optLogged=true;logger::Write("Hover UI: settings: vehicle options added inside Gameplay Options; controls %s",g_controlsAvailable?"on":"off (link hook unavailable)");}
							}Release(merged);
						}Release(native);
					}Release(ours);Release(all);
				}
			}
			if (visible) ControlsPageWork(movie, panel);
			else g_set.ctlVisible = false;
			// Release a capture modal even when Back / menu closure changed panels.
			if(!gameplayVisible&&g_set.captureUI){SetMember(panel,"remappingControl",Bool(false));g_set.captureUI=false;}
			Release(optList);Release(optPanel);Release(catList);Release(catPanel);Release(panel);
		}

		void SettingsWork(uptr menu, uptr movie)
		{
			__try {
				SettingsWorkBody(menu, movie);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				g_set.failed = true;
				bindings::CancelCapture();
				logger::Write("Hover UI: exception 0x%08lX in the settings menu; the %s category is off until the menu opens again", GetExceptionCode(), kCategoryName);
			}
		}

		// The dispatchers' argument is a BSGFxObject (vtable, then the event's GFx value at +8; FUN_142547820 reads it so),
		// not a bare value: reading it as one found no "params" and every one of our settings events fell through (2026-10-23).
		const Value& EventValue(Value* ev) { return *reinterpret_cast<const Value*>(reinterpret_cast<const char*>(ev) + 8); }
		// The event's params.<name> as a number; false when it has none.
		bool EventNumber(Value* ev, const char* name, double& out)
		{
			__try {
				Value params;
				const bool ok = ev && IsObject(EventValue(ev)) && GetMember(EventValue(ev), "params", &params) && IsObject(params) && GetNumber(params, name, out);
				Release(params);
				return ok;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool EventBool(Value* ev, const char* name, bool& out)
		{
			__try {
				Value params;
				const bool ok = ev && IsObject(EventValue(ev)) && GetMember(EventValue(ev), "params", &params) && IsObject(params) && GetBoolMember(params, name, out);
				Release(params);
				return ok;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		void CheckBoxDetour(void* self, Value* ev)
		{
			static std::atomic<bool> seen{ false };
			double id = 0;
			if (!seen.exchange(true)) logger::Write("Hover UI: settings: first checkbox event (setting %.0f)", EventNumber(ev, "uSettingID", id) ? id : -1.0);
			if (EventNumber(ev, "uSettingID", id) && id >= kSettingBase && id < double(kSettingBase) + int(features::Id::Count)) {
				bool checked = false;
				if(EventBool(ev, "bChecked", checked))features::Set(features::Id(int(id - kSettingBase)), checked);
				g_setDirty.store(true);
				return;  // ours: the game never sees it
			}
			g_checkOriginal(self, ev);
		}

		void OpenCategoryDetour(void* self, Value* ev)
		{
			static std::atomic<bool> seen{ false };
			double id = -1;
			if (!seen.exchange(true)) logger::Write("Hover UI: settings: first category event (category %.0f)", EventNumber(ev, "categoryID", id) ? id : -1.0);
			bindings::CancelCapture();g_setDirty.store(true);
			if (EventNumber(ev, "categoryID", id) && id == kOldCategoryId) return;
			g_openOriginal(self, ev);
		}

		void ResetDetour(void* self, Value* ev)
		{
			double id = -1;
			const bool known = EventNumber(ev, "categoryID", id);
			logger::Write("Hover UI: settings: reset to defaults (category %.0f%s)", known ? id : -1.0, g_set.ctlVisible ? ", Controls page" : "");
			if (g_set.ctlVisible) { bindings::ResetAll(); g_setDirty.store(true); }
			else if (known && unsigned(id) == kCategoryId) {
				features::ResetAll();
				bindings::ResetAll();
				g_setDirty.store(true);
			}
			g_resetOriginal(self, ev);
		}

		void LinkDetour(void* self,Value* ev)
		{
			double id=-1;
			if(EventNumber(ev,"uSettingID",id)&&id>=kControlBase&&id<=kRestoreControls){
				if(id==kRestoreControls)bindings::ResetAll();
				else bindings::BeginCapture(bindings::Action(int(id-kControlBase)));
				g_setDirty.store(true);return;
			}
			g_linkOriginal(self,ev);
		}

		bool PatchSlot(std::uint32_t vtblRva, int slot, std::uint32_t expectRva, void* detour, void** original);
		void InstallSettings()
		{
			void* orig = nullptr;
			int hooks = 0;
			if (PatchSlot(kRvaPauseVtbl, kSlotMessage, kRvaPauseMessage, reinterpret_cast<void*>(&PauseMessageDetour), &orig)) { g_pauseOriginal = reinterpret_cast<Message_t>(orig); ++hooks; }
			if (PatchSlot(kRvaMainVtbl, kSlotMessage, kRvaMainMessage, reinterpret_cast<void*>(&MainMessageDetour), &orig)) { g_mainOriginal = reinterpret_cast<Message_t>(orig); ++hooks; }
			// The change events first: without them our checkboxes would reach the game (an unknown id), so no category at all.
			const bool check = PatchSlot(kRvaCheckDisp, kSlotDispatch, kRvaCheckFn, reinterpret_cast<void*>(&CheckBoxDetour), &orig);
			if (check) g_checkOriginal = reinterpret_cast<Dispatch_t>(orig);
			const bool open = PatchSlot(kRvaOpenDisp, kSlotDispatch, kRvaOpenFn, reinterpret_cast<void*>(&OpenCategoryDetour), &orig);
			if (open) g_openOriginal = reinterpret_cast<Dispatch_t>(orig);
			const bool reset = PatchSlot(kRvaResetDisp, kSlotDispatch, kRvaResetFn, reinterpret_cast<void*>(&ResetDetour), &orig);
			if (reset) g_resetOriginal = reinterpret_cast<Dispatch_t>(orig);
			controlspage::Install();
			g_controlsAvailable=PatchSlot(kRvaLinkDisp,kSlotDispatch,kRvaLinkFn,reinterpret_cast<void*>(&LinkDetour),&orig);
			if(g_controlsAvailable)g_linkOriginal=reinterpret_cast<Dispatch_t>(orig);
			if (!check || !open) {
				g_setMenu.store(0);
				logger::Write("Hover UI: settings: the panel's event dispatchers do not match (checkbox %d, category %d); no %s category", check, open, kCategoryName);
				g_set.failed = true;
				return;
			}
			logger::Write("Hover UI: settings: %s inside Gameplay Options (%d of 2 menus, reset %s, control links %s)", kCategoryName, hooks, reset ? "on" : "off",g_controlsAvailable?"on":"off");
		}

		// One scanner element: a BasicButton whose key box shows `key`, with `label` under it (empty label: the key box
		// alone, e.g. the status box). Created in place (managed values never move).
		struct MonoEl
		{
			Value button, data;
			double scale = OverlayScale();
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
			const Value uedArgs[1] = { Str("") };  // custom display only: never bind the native boost/Shift event
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
			// The native widget can refresh its glyph after SetButtonData or a device change.
			// Reassert after every movie advance so its fallback never flashes on screen.
			Value pc, inst;
			if (GetMember(e.button, "PCButton_mc", &pc) && IsObject(pc) && GetMember(pc, "ButtonInstance_mc", &inst) && IsObject(inst)) {
				SetMember(pc,"visible",Bool(true));SetMember(inst,"visible",Bool(true));
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

		// Use the same native status boxes and action prompts as the existing contextual HUD.
		// Each information row is single-line, so it needs no custom text-field sizing or styling.
		struct RouteView {uptr movie=0,menu=0;MonoEl rows[11],retry,skip,clear;};
		RouteView g_routeViews[2];
		void RenderRoute(uptr menu,uptr movie,bool scanner)
		{
			auto&view=g_routeViews[scanner?1:0];
			if(view.movie!=movie||view.menu!=menu){view=RouteView{};view.movie=movie;view.menu=menu;}
			std::string text,retryKey,skipKey,clearKey;
			{std::lock_guard lk(g_scanLock);text=g_routeText;retryKey=g_routeRetryKey;skipKey=g_routeSkipKey;clearKey=g_routeClearKey;}
			double x,y,rs;bool picked,edit;
			{std::lock_guard lk(g_lay.lock);x=g_lay.routeX;y=g_lay.routeY;edit=g_lay.edit;rs=kBaseScale*g_lay.routeSize;
				picked=g_inVehicle.load()?g_lay.sel==kSelRoute||g_lay.sel==kSelAll:g_lay.fsel==kFootEls+1;}
			if(edit&&picked&&text.empty())text="Route: Outpost\nTravelling | 120 m\n1. Landing pad";
			if(edit&&picked){if(retryKey.empty())retryKey=" ";if(skipKey.empty())skipKey=" ";if(clearKey.empty())clearKey=" ";}
			if(!scanner&&ScannerOpen())text.clear();
			// The route is planning: the scanner shows it (the HUD's status card carries the next stop); the HUD shows the
			// whole panel only with every hint on, or while it is placed in F7.
			if(!scanner&&!g_showAll&&!(edit&&picked))text.clear();
			const bool routeVisible=!text.empty();
			if(scanner){
				std::string escorts;
				{std::lock_guard lk(g_ctx.lock);for(int i=0;i<g_ctx.rowCount&&i<kMaxRows;++i)if(!g_ctx.rows[i].empty())escorts+="\n"+g_ctx.rows[i];}
				if(!escorts.empty())text+=(text.empty()?"ESCORTS":"\nESCORTS")+escorts;
			}
			const double alpha=edit&&picked&&(GetTickCount64()/300)%2==0?0.35:1.0;
			const auto&root=*reinterpret_cast<const Value*>(menu+0x58);if(!IsObject(root))return;
			auto show=[&](MonoEl&e,const std::string&value,const char*label){
				if(!value.empty()&&!IsObject(e.button)){
					if(!CreateObject(movie,&e.button,"BasicButton",nullptr,0))return false;
					SetMember(e.button,"visible",Bool(false));const Value child[]={Arg(e.button)};Invoke(root,"addChild",child,1);
				}
				MonoShow(e,!value.empty());if(value.empty())return false;
				if(!MonoSetData(movie,e,label))return false;MonoSetKey(e,value);
				SetMember(e.button,"scaleX",Num(rs));SetMember(e.button,"scaleY",Num(rs));
				SetMember(e.button,"alpha",Num(alpha));return true;
			};
			std::size_t from=0;
			for(auto&e:view.rows){
				std::string line;
				if(from<text.size()){const auto end=text.find('\n',from);line=text.substr(from,end==std::string::npos?end:end-from);from=end==std::string::npos?text.size():end+1;}
				if(!show(e,line," "))continue;
				double px=0,py=0,w=0,h=0;KeyBox(e.button,px,py,w,h);
				SetMember(e.button,"x",Num(x-rs*px));SetMember(e.button,"y",Num(y-rs*py));y+=rs*std::max(h,32.0)+8;
			}
			const bool visible=routeVisible;
			MonoEl*actions[]={&view.retry,&view.skip,&view.clear};
			const std::string keys[]={visible?retryKey:"",visible?skipKey:"",visible?clearKey:""};
			const char*labels[]={"Retry trip","Skip stop","T T: clear route"};int shown=0;
			for(int i=0;i<3;++i)if(show(*actions[i],keys[i],labels[i]))PlaceButtonKey(actions[i]->button,x+44+140*shown++*rs/kBaseScale,y+24*rs/kBaseScale,false,rs);
		}

		// Physical VTOL instrument. Created through the same movie's CreateObject/Invoke interface as the buttons;
		// all graphics and managed values stay on the HUD Advance thread. No external overlay or SWF replacement.
		struct AttitudeView {
			Value panel, horizon, aircraft;
			MonoEl readout;
			bool built=false, failed=false;
		} g_attitudeView;

		void DropAttitude(bool release)
		{
			auto& v=g_attitudeView;
			if (release) {
				// The readout is its own child of the HUD root (moved and sized apart from the instrument).
				for (const Value* child : {&v.panel,&v.readout.button}) {
					Value parent;
					if (IsObject(*child) && GetMember(*child,"parent",&parent) && IsObject(parent)) {
						const Value args[]={Arg(*child)}; Invoke(parent,"removeChild",args,1);
					}
					Release(parent);
				}
				Release(v.readout.data); Release(v.readout.button);
				Release(v.aircraft); Release(v.horizon); Release(v.panel);
			}
			v=AttitudeView{};
		}

		bool AttitudeSprite(uptr movie, const Value& parent, Value& sprite)
		{
			if (!CreateObject(movie,&sprite,"flash.display.Sprite",nullptr,0)) return false;
			SetMember(sprite,"mouseEnabled",Bool(false)); SetMember(sprite,"mouseChildren",Bool(false));
			const Value args[]={Arg(sprite)};
			return Invoke(parent,"addChild",args,1);
		}
		bool DrawAttitude(const Value& sprite, bool aircraft)
		{
			Value graphics;
			if (!GetMember(sprite,"graphics",&graphics) || !IsObject(graphics)) { Release(graphics); return false; }
			bool ok=true;
			auto line=[&](double x1,double y1,double x2,double y2) {
				const Value from[]={Num(x1),Num(y1)},to[]={Num(x2),Num(y2)};
				ok=Invoke(graphics,"moveTo",from,2)&&ok; ok=Invoke(graphics,"lineTo",to,2)&&ok;
			};
			// Dark under-stroke keeps the instrument readable against ice, sky and bright terrain.
			for (int pass=0;pass<2;++pass) {
				const Value style[]={Num(pass?aircraft?4:2:aircraft?7:5),Num(pass?aircraft?0xFFFFFF:0x79D8FF:0x101C2C),Num(pass?0.95:0.8)};
				ok=Invoke(graphics,"lineStyle",style,3)&&ok;
				if (aircraft) {
					line(-112,0,-18,0); line(18,0,112,0);
					for (int side : {-1,1}) {
						line(side*128.0,-7,side*148.0,-7); line(side*132.0,0,side*144.0,0); line(side*136.0,7,side*140.0,7);
					}
					const Value circle[]={Num(0),Num(0),Num(10)};
					ok=Invoke(graphics,"drawCircle",circle,3)&&ok;
				} else {
					line(-160,0,160,0);
					for (int degrees : {-30,-20,-10,10,20,30}) {
						const double y=-degrees*2.0,w=degrees%20==0?44:28;
						line(-w,y,-8,y); line(8,y,w,y);
						// Tick direction identifies the horizon even while inverted.
						line(-w,y,-w,y+(degrees>0?4:-4)); line(w,y,w,y+(degrees>0?4:-4));
					}
				}
			}
			Release(graphics); return ok;
		}
		bool BuildAttitude(uptr menu,uptr movie)
		{
			auto& v=g_attitudeView;
			const auto& root=*reinterpret_cast<const Value*>(menu+0x58);
			if (!IsObject(root) || !AttitudeSprite(movie,root,v.panel)) return false;
			SetMember(v.panel,"visible",Bool(false)); SetMember(v.panel,"name",Str("SeamlessVtolAttitude"));
			if (!AttitudeSprite(movie,v.panel,v.horizon) || !AttitudeSprite(movie,v.panel,v.aircraft) ||
				!DrawAttitude(v.horizon,false) || !DrawAttitude(v.aircraft,true)) return false;
			// Clip the moving ladder to the compact instrument window; aircraft marker is outside this clip.
			Value rect;
			const Value bounds[]={Num(-165),Num(-78),Num(330),Num(156)};
			if (!CreateObject(movie,&rect,"flash.geom.Rectangle",bounds,4)) return false;
			// A fixed parent supplies the mask; scrolling the horizon itself would move the viewport too.
			Value viewport;
			bool ok=CreateObject(movie,&viewport,"flash.display.Sprite",nullptr,0);
			if (ok) {
				SetMember(viewport,"scrollRect",Arg(rect)); SetMember(viewport,"mouseEnabled",Bool(false));
				SetMember(viewport,"mouseChildren",Bool(false));
				// scrollRect maps its top-left to the parent's origin; offset it back so (0,0) remains the centre.
				SetMember(viewport,"x",Num(-165)); SetMember(viewport,"y",Num(-78));
				const Value horizon[]={Arg(v.horizon)},child[]={Arg(viewport),Num(0)};
				ok=Invoke(viewport,"addChild",horizon,1) && Invoke(v.panel,"addChildAt",child,2);
			}
			Release(viewport); Release(rect);
			if (!ok || !CreateObject(movie,&v.readout.button,"BasicButton",nullptr,0)) return false;
			const Value readout[]={Arg(v.readout.button)};
			if (!Invoke(root,"addChild",readout,1) || !MonoSetData(movie,v.readout," ")) return false;
			SetMember(v.readout.button,"mouseEnabled",Bool(false)); SetMember(v.readout.button,"mouseChildren",Bool(false));
			SetMember(v.readout.button,"name",Str("SeamlessVtolAttitudeText"));
			logger::Write("Physical VTOL HUD: native attitude indicator added; F7 selections 'VTOL attitude' and 'VTOL attitude text'");
			return true;
		}
		void WorkAttitude(uptr menu,uptr movie)
		{
			auto& v=g_attitudeView;
			vehiclecamera::Attitude attitude;
			const bool active=vehiclecamera::ReadAttitude(attitude);
			double x,y,scale,tx,ty,tscale; bool edit,picked,pickedText;
			{ std::lock_guard lk(g_lay.lock); x=g_lay.attitudeX; y=g_lay.attitudeY; scale=g_lay.attitudeSize;
				tx=g_lay.attitudeTextX; ty=g_lay.attitudeTextY; tscale=g_lay.attitudeTextSize;
				edit=g_lay.edit; picked=g_lay.sel==kSelAttitude||g_lay.sel==kSelAll; pickedText=g_lay.sel==kSelAttitudeText||g_lay.sel==kSelAll; }
			const bool preview=edit&&(picked||pickedText)&&g_inVehicle.load();
			const bool visible=g_inVehicle.load()&&!ScannerOpen()&&(active||preview);
			if (!visible || v.failed) {
				if (IsObject(v.panel)) SetMember(v.panel,"visible",Bool(false));
				MonoShow(v.readout,false); return;
			}
			if (!v.built) {
				if (!BuildAttitude(menu,movie)) {
					DropAttitude(true); v.failed=true;
					logger::Write("Physical VTOL HUD: indicator creation failed; retry after next HUD load"); return;
				}
				v.built=true;
			}
			if (!active) { attitude.pitch=0; attitude.roll=0; }
			const auto h=cameraroll::Indicator(attitude.pitch,attitude.roll);
			SetMember(v.horizon,"rotation",Num(h.rotation));
			SetMember(v.horizon,"x",Num(h.x)); SetMember(v.horizon,"y",Num(h.y));
			char text[64]; std::snprintf(text,sizeof(text),"VTOL   P %+.0f   B %+.0f",attitude.pitch*180/cameraroll::kPi,attitude.roll*180/cameraroll::kPi);
			MonoSetKey(v.readout,text); MonoShow(v.readout,true);
			const auto& root=*reinterpret_cast<const Value*>(menu+0x58);
			Value stage; double width=1920,height=1080;
			if (GetMember(root,"stage",&stage)&&IsObject(stage)) {
				GetNumber(stage,"stageWidth",width); GetNumber(stage,"stageHeight",height);
			}
			Release(stage);
			SetMember(v.panel,"x",Num(width*0.5+x)); SetMember(v.panel,"y",Num(height*0.5+y));
			SetMember(v.panel,"scaleX",Num(scale)); SetMember(v.panel,"scaleY",Num(scale));
			const bool blink=edit&&(GetTickCount64()/300)%2==0;
			SetMember(v.panel,"alpha",Num(blink&&picked?0.35:1));
			SetMember(v.panel,"visible",Bool(true));
			PlaceButtonKey(v.readout.button,width*0.5+tx,height*0.5+ty,false,kAttTextScale*tscale);
			SetMember(v.readout.button,"alpha",Num(blink&&pickedText?0.35:1));
		}
		void RenderAttitude(uptr menu,uptr movie)
		{
			__try { WorkAttitude(menu,movie); }
			__except(EXCEPTION_EXECUTE_HANDLER) {
				g_attitudeView.failed=true;
				logger::Write("Physical VTOL HUD: exception 0x%08lX; instrument disabled until HUD reload",GetExceptionCode());
			}
		}

		// ---- The contextual HUD: on-foot panel and in-vehicle extras (in the HUD movie) ----
		struct Extras
		{
			uptr movie = 0;
			Value meters, widget;          // RightMeters_mc (on foot) and HUDVehicle_mc (in the vehicle)
			MonoEl fSummon, fFollow, fGetIn, fStatus, fTarget, fPass, fEject, fRows;
			MonoEl vLine, vTarget, vRows;  // fRows / vRows: the per-vehicle line (one box, movable in the F7 editor)
			static constexpr int kCardLines = 7;  // 3 lines, plus the escorts with every hint on
			MonoEl fCard[kCardLines], vCard[kCardLines];  // the status card: on foot at the Status place, in the vehicle at the Vehicles place
			bool built = false, failed = false;
			// The vehicle bar: a copy of the game's enemy bar clip (HUDMenu_fla.EnemyHealthAnimBase_72) added to EnemyHealthMeter_mc
			// under the native one, so it follows the driver's bar on screen and fades with it.
			Value vbMeter, vbBar, vbFill, vbBg, vbName;
			double vbWidth = 0;
			bool vbBuilt = false, vbFailed = false, vbShown = false;
			std::string vbText;
			int vbPct = -1;
		} g_ex;

		MonoEl* FootEls[kFootEls] = { &g_ex.fSummon, &g_ex.fFollow, &g_ex.fGetIn, &g_ex.fStatus, &g_ex.fTarget, &g_ex.fPass, &g_ex.fEject, &g_ex.fRows };
		MonoEl* CarEls[kCarEls] = { &g_ex.vLine, &g_ex.vTarget, &g_ex.vRows };

		bool IsOurExtra(void* object)
		{
			for (MonoEl* e : CarEls) { if (object && e->button.object == object) return true; }
			for (MonoEl& e : g_ex.vCard) { if (object && e.button.object == object) return true; }
			return false;
		}

		void DropExtras(bool release)
		{
			for (MonoEl* e : FootEls) { if (release) { Release(e->data); Release(e->button); } }
			for (MonoEl* e : CarEls) { if (release) { Release(e->data); Release(e->button); } }
			for (MonoEl& e : g_ex.fCard) { if (release) { Release(e.data); Release(e.button); } }
			for (MonoEl& e : g_ex.vCard) { if (release) { Release(e.data); Release(e.button); } }
			if (release) { Release(g_ex.meters); Release(g_ex.widget); }
			if (release) { Release(g_ex.vbName); Release(g_ex.vbBg); Release(g_ex.vbFill); Release(g_ex.vbBar); Release(g_ex.vbMeter); }
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
			for (MonoEl& e : g_ex.fCard) {
				if (!CreateObject(g_ex.movie, &e.button, "BasicButton", nullptr, 0)) return false;
				const Value addArgs[1] = { Arg(e.button) };
				Invoke(g_ex.meters, "addChild", addArgs, 1);
				SetMember(e.button, "visible", Bool(false));
			}
			for (MonoEl& e : g_ex.vCard) {
				if (!CreateObject(g_ex.movie, &e.button, "BasicButton", nullptr, 0)) return false;
				SetMember(e.button, "name", Str("sv_extra"));
				const Value addArgs[1] = { Arg(e.button) };
				Invoke(g_ex.widget, "addChild", addArgs, 1);
				SetMember(e.button, "visible", Bool(false));
			}
			g_ex.built = true;
			logger::Write("Hover UI: contextual HUD added (status card; on foot: Summon, Follow / Cover; in the vehicle: Auto)");
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

		// The lifebar's lines ('\n' between them), each its own card line.
		void PushLines(std::vector<std::string>& card, const std::string& text)
		{
			std::size_t at = 0;
			while (at < text.size()) {
				const auto cut = text.find('\n', at);
				const auto line = text.substr(at, cut == std::string::npos ? std::string::npos : cut - at);
				if (!line.empty()) card.push_back(line);
				if (cut == std::string::npos) break;
				at = cut + 1;
			}
		}

		void RefreshExtras()
		{
			bool own, nearOwn, autoOn, occupied;
			int rowCount = 0;
			std::string rows[kMaxRows];
			std::string summonKey, followKey, useKey, status, autoLine, occKey, target, summonLabel, coverLabel, autoKey, lifebar;
			{
				std::lock_guard lk(g_ctx.lock);
				own = g_ctx.own; nearOwn = g_ctx.nearOwn; autoOn = g_ctx.autoOn;
				occupied = g_ctx.occupied; occKey = g_ctx.occKey;
				summonKey = g_ctx.summonKey; followKey = g_ctx.followKey; useKey = g_ctx.useKey; status = g_ctx.status;
				summonLabel = g_ctx.summonLabel; coverLabel = g_ctx.coverLabel;
				autoLine = g_ctx.autoLine; autoKey = g_ctx.autoKey; lifebar = g_ctx.lifebar;
				rowCount = g_ctx.rowCount;
				for (int i = 0; i < kMaxRows; ++i) rows[i] = g_ctx.rows[i];
			}
			const bool inVehicle = g_inVehicle.load();
			{ std::lock_guard lk(g_scanLock); target = g_scanText; }
			const bool foot = !inVehicle && own;
			g_footShown = foot;
			bool ed;
			{
				std::lock_guard lk(g_lay.lock);
				ed = g_lay.edit;
			}
			// While editing, every element shows (a sample text where it is empty), so each one can be placed.
			if (ed) {
				if (status.empty()) status = "COVER 42 m";
				if (autoLine.empty()) autoLine = "AUTO 120 m";
				if (target.empty()) target = "TARGET  1240 m";
			}
			ElSet(g_ex.fSummon, foot && !summonKey.empty(), summonLabel.empty() ? "Summon" : summonLabel.c_str(), summonKey);
			ElSet(g_ex.fFollow, foot && !followKey.empty(), coverLabel.empty() ? "Follow" : coverLabel.c_str(), followKey);
			// "Get in" is never shown (2026-10-21 UI review): facing the vehicle the game shows its own E prompt, and near it
			// but not facing, E does not board, so the hint misled.
			ElSet(g_ex.fGetIn, false, "Get in", useKey);
			(void)nearOwn;
			ElSet(g_ex.fStatus, false, " ", status);  // in the status card now
			// Text inside one rounded box (label blank), like the FOLLOWING status box.
			std::string rowsBox;
			for (int i = 0; i < kMaxRows && i < rowCount; ++i) {
				if (rows[i].empty()) continue;
				rowsBox += (rowsBox.empty() ? "" : "  |  ") + rows[i];
			}
			if (ed && rowsBox.empty()) rowsBox = "pursuit REV8 83 m";
			ElSet(g_ex.fTarget, !inVehicle && !ScannerOpen() && !target.empty(), " ", target);
			MonoTint(g_ex.movie, g_ex.fTarget, true);
			ElSet(g_ex.fRows, false, " ", rowsBox);  // the card counts the escorts; the scanner lists them
			// Facing an occupied vehicle on foot: tap = the passenger seat, hold = eject the driver (half-size prompts).
			g_ex.fPass.scale = g_ex.fEject.scale = OverlayScale();
			const bool occShow = !inVehicle && (occupied || (ed && foot));
			ElSet(g_ex.fPass, occShow, "Ride as passenger", occKey);
			ElSet(g_ex.fEject, occShow, "Hold: pull out driver", occKey);
			// The Auto prompt (the trip itself is in the status card): "Auto", or "Auto off" (tinted) while it flies.
			ElSet(g_ex.vLine, inVehicle && (ed || !autoKey.empty()), autoOn ? "Auto off" : "Auto", autoKey);
			MonoTint(g_ex.movie, g_ex.vLine, autoOn);
			ElSet(g_ex.vTarget, inVehicle && !ScannerOpen() && !target.empty(), " ", target);
			MonoTint(g_ex.movie, g_ex.vTarget, true);
			ElSet(g_ex.vRows, false, " ", rowsBox);  // the card counts the escorts; the scanner lists them
			// ---- The status card: what the vehicle is doing, in one place ----
			std::vector<std::string> card;
			std::string nextStop;
			{
				std::lock_guard lk(g_scanLock);
				const auto cut = g_routeText.find('\n');
				if (!g_routeText.empty() && cut != std::string::npos) {
					std::string name = g_routeText.substr(0, cut), rest = g_routeText.substr(cut + 1);
					if (name.rfind("Route: ", 0) == 0) name = name.substr(7);
					const auto end = rest.find('\n');
					nextStop = name + "  \xC2\xB7  " + rest.substr(0, end);
				}
			}
			int escorts = 0;
			for (int i = 0; i < kMaxRows && i < rowCount; ++i) if (!rows[i].empty()) ++escorts;
			char head[96];
			if (inVehicle) {
				std::snprintf(head, sizeof(head), "GEAR %d%s%s", g_gear.load(), g_hovering ? "  \xC2\xB7  HOVER" : "", escorts ? "  \xC2\xB7  " : "");
				std::string first = head;
				if (escorts) first += std::to_string(escorts) + (escorts == 1 ? " ESCORT" : " ESCORTS");
				card.push_back(first);
				PushLines(card, lifebar);  // the other vehicle's health, then yours
				if (!nextStop.empty()) card.push_back(nextStop);
				else if (autoOn && !autoLine.empty()) card.push_back(autoLine);
			}
			else if (own || escorts || !lifebar.empty()) {
				std::string first = status;
				if (escorts) first += (first.empty() ? "" : "  \xC2\xB7  ") + std::to_string(escorts) + (escorts == 1 ? " ESCORT" : " ESCORTS");
				if (!first.empty()) card.push_back(first);
				PushLines(card, lifebar);  // the other vehicle's health, then yours
				if (!nextStop.empty()) card.push_back(nextStop);
			}
			if (g_showAll) for (int i = 0; i < kMaxRows && i < rowCount; ++i) if (!rows[i].empty()) card.push_back(rows[i]);  // every hint on: each escort
			if (!ed && !features::On(features::Id::StatusCard)) card.clear();  // [hud] card, also the settings menu
			if (ed && card.empty()) card.push_back(inVehicle ? "GEAR 2  \xC2\xB7  HOVER" : "FOLLOW 42 m");
			auto& cardEls = inVehicle ? g_ex.vCard : g_ex.fCard;
			auto& otherEls = inVehicle ? g_ex.fCard : g_ex.vCard;
			for (int i = 0; i < Extras::kCardLines; ++i) {
				const bool on = i < int(card.size());
				ElSet(cardEls[i], on, " ", on ? card[i] : std::string());
				if (on) MonoTint(g_ex.movie, cardEls[i], i == 0 && (autoOn || g_hovering));
				ElSet(otherEls[i], false, " ", std::string());
			}
			// Places: the on-foot panel in RightMeters_mc's coordinates; the extras in the rover widget's, beside the
			// column (Auto) and under the WASD cluster (status, target).
			double fx, fy, ex, ey, cx, cy, d, colRight, colTop, colStep, fo[kFootEls][2], co[kCarEls][2], fs[kFootEls], cs[kCarEls];
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
				for (int i = 0; i < kFootEls; ++i) { fo[i][0] = g_lay.footOff[i][0]; fo[i][1] = g_lay.footOff[i][1]; fs[i] = kBaseScale * g_lay.footSize[i]; }
				for (int i = 0; i < kCarEls; ++i) { co[i][0] = g_lay.carOff[i][0]; co[i][1] = g_lay.carOff[i][1]; cs[i] = kBaseScale * g_lay.carSize[i]; }
				edit = g_lay.edit;
				sel = g_lay.sel;
				fsel = g_lay.fsel;
			}
			const bool blinkOff = edit && (GetTickCount64() / 300) % 2 == 0;
			// The scanner has a separate movie. Transfer only screen coordinates, never its HUD objects.
			double targetX = inVehicle ? cx + ex + co[1][0] : fx + 80.0 + fo[4][0];
			double targetY = inVehicle ? cy + 1.7 * d + ey + co[1][1] : fy + 98.0 + fo[4][1];
			const bool targetPlaced = TransformPoint(g_ex.movie, inVehicle ? g_ex.widget : g_ex.meters, "localToGlobal", targetX, targetY);
			{ std::lock_guard lk(g_scanLock); g_targetPlaced = targetPlaced; g_targetX = targetX; g_targetY = targetY; }
			if (foot || rowCount > 0 || occupied || !target.empty()) {
				const double at[kFootEls][2] = { { fx, fy }, { fx + 120.0, fy }, { fx + 240.0, fy }, { fx + 80.0, fy + 52.0 }, { fx + 80.0, fy + 98.0 }, { fx + 240.0, fy + 52.0 }, { fx + 240.0, fy + 98.0 }, { fx + 80.0, fy + 98.0 } };
				for (int i = 0; i < kFootEls; ++i) {
					if (!FootEls[i]->shown) continue;
					FootEls[i]->scale = fs[i];
					SetMember(FootEls[i]->button, "scaleX", Num(FootEls[i]->scale));
					SetMember(FootEls[i]->button, "scaleY", Num(FootEls[i]->scale));
					PlaceButtonKey(FootEls[i]->button, at[i][0] + fo[i][0], at[i][1] + fo[i][1], false, FootEls[i]->scale);
					const bool picked = fsel == kFootEls || fsel == i;
					SetMember(FootEls[i]->button, "alpha", Num(blinkOff && !inVehicle && picked ? 0.35 : 1.0));
				}
			}
			{
				const bool blinkCard = blinkOff && (inVehicle ? (sel == kSelExtras || sel == kSelAll || sel == kSelRows) : (fsel == kFootEls || fsel == 3));
				const double scale = inVehicle ? cs[2] : fs[3];
				const double baseX = inVehicle ? cx + ex + co[2][0] : fx + 80.0 + fo[3][0];
				double y = inVehicle ? cy + 1.7 * d + ey + co[2][1] : fy + 52.0 + fo[3][1];
				for (MonoEl& e : inVehicle ? g_ex.vCard : g_ex.fCard) {
					if (!e.shown) continue;
					SetMember(e.button, "scaleX", Num(scale));
					SetMember(e.button, "scaleY", Num(scale));
					PlaceButtonKey(e.button, baseX, y, false, scale);
					SetMember(e.button, "alpha", Num(blinkCard ? 0.35 : 1.0));
					y += scale * 44.0;
				}
			}
			if (inVehicle) {
				const double at[kCarEls][2] = { { cx + ex, cy + 0.9 * d + ey }, { cx + ex, cy + 1.7 * d + ey }, { cx + ex, cy + 1.7 * d + ey } };
				for (int i = 0; i < kCarEls; ++i) {
					if (!CarEls[i]->shown) continue;
					SetMember(CarEls[i]->button, "scaleX", Num(cs[i]));
					SetMember(CarEls[i]->button, "scaleY", Num(cs[i]));
					PlaceButtonKey(CarEls[i]->button, at[i][0] + co[i][0], at[i][1] + co[i][1], false, cs[i]);
					const bool picked = sel == kSelExtras || sel == kSelAll || sel == kSelLine + i;
					SetMember(CarEls[i]->button, "alpha", Num(blinkOff && picked ? 0.35 : 1.0));
				}
			}
		}

		// hudmenu.swf EnemyHealthMeter: the bar (EnemyHealthAnimBase_mc) shows the HudEnemyData entry under the crosshair, at
		// that enemy's screen position. Ours is the same clip, one bar lower, with the vehicle's name and health.
		bool BuildVehicleBar(uptr menu, uptr movie)
		{
			const Value& root = *reinterpret_cast<const Value*>(menu + 0x58);
			if (!IsObject(root)) return false;
			Value base, holder;
			// The meter is a root member in the script export; it may sit inside EnemyHealthHolder_mc instead (2026-10-21 run: not found).
			bool meter = GetMember(root, "EnemyHealthMeter_mc", &g_ex.vbMeter) && IsObject(g_ex.vbMeter);
			if (!meter) {
				Release(g_ex.vbMeter);
				meter = GetMember(root, "EnemyHealthHolder_mc", &holder) && IsObject(holder) && GetMember(holder, "EnemyHealthMeter_mc", &g_ex.vbMeter) && IsObject(g_ex.vbMeter);
			}
			Release(holder);
			const bool haveBase = meter && GetMember(g_ex.vbMeter, "EnemyHealthAnimBase_mc", &base) && IsObject(base);
			const bool made = haveBase && CreateObject(movie, &g_ex.vbBar, "HUDMenu_fla.EnemyHealthAnimBase_72", nullptr, 0);
			bool ok = made;
			if (!ok) logger::Write("Hover UI: vehicle bar: %s", !meter ? "no EnemyHealthMeter_mc (root or EnemyHealthHolder_mc)" : !haveBase ? "no EnemyHealthAnimBase_mc in the meter" :
				"HUDMenu_fla.EnemyHealthAnimBase_72 could not be created");
			if (ok) {
				SetMember(g_ex.vbBar, "name", Str("sv_vehiclebar"));
				const Value addArgs[1] = { Arg(g_ex.vbBar) };
				Invoke(g_ex.vbMeter, "addChild", addArgs, 1);
				const Value frame[1] = { Str("Standard") };
				Invoke(g_ex.vbBar, "gotoAndStop", frame, 1);
				for (const char* hide : { "LevelNumber_mc", "LegendaryIcon_mc", "EMDamageBar_mc", "EnemyIcon_mc", "EnemyShieldAnim_mc", "EnemyHealthBarAnim_Legendary_mc",
						 "EnemyLegendaryModIcon1_mc", "EnemyLegendaryModIcon2_mc", "EnemyLegendaryModIcon3_mc" }) {
					Value c;
					if (GetMember(g_ex.vbBar, hide, &c) && IsObject(c)) SetMember(c, "visible", Bool(false));
					Release(c);
				}
				Value anim;
				ok = GetMember(g_ex.vbBar, "EnemyHealthBarAnim_mc", &anim) && IsObject(anim) && GetMember(anim, "Fill_mc", &g_ex.vbFill) && IsObject(g_ex.vbFill) &&
					GetMember(anim, "BG_mc", &g_ex.vbBg) && IsObject(g_ex.vbBg) && GetNumber(g_ex.vbBg, "width", g_ex.vbWidth) && g_ex.vbWidth > 0;
				Release(anim);
				GetMember(g_ex.vbBar, "EnemyName_tf", &g_ex.vbName);
				double bx = 0, by = 0, bh = 0;
				GetNumber(base, "x", bx);
				GetNumber(base, "y", by);
				GetNumber(base, "height", bh);
				SetMember(g_ex.vbBar, "x", Num(bx));
				SetMember(g_ex.vbBar, "y", Num(by + (bh > 0 ? bh : 40.0) + 4.0));
				SetMember(g_ex.vbBar, "visible", Bool(false));
				logger::Write("Hover UI: vehicle bar %s (under the enemy bar at %.0f, %.0f, its height %.0f; bar width %.0f)", ok ? "added" : "incomplete", bx, by, bh, g_ex.vbWidth);
			}
			Release(base);
			if (!ok) { Release(g_ex.vbName); Release(g_ex.vbBg); Release(g_ex.vbFill); Release(g_ex.vbBar); Release(g_ex.vbMeter); }  // the next try starts clean
			return ok;
		}

		void RefreshVehicleBar()
		{
			std::string name;
			float share = 0;
			{
				std::lock_guard lk(g_ctx.lock);
				name = g_ctx.vbarName;
				share = g_ctx.vbarShare;
			}
			const bool on = !name.empty();
			if (on != g_ex.vbShown) {
				SetMember(g_ex.vbBar, "visible", Bool(on));
				g_ex.vbShown = on;
			}
			if (!on) return;
			if (name != g_ex.vbText && IsObject(g_ex.vbName)) {
				SetMember(g_ex.vbName, "text", Str(name.c_str()));
				g_ex.vbText = name;
			}
			const int pct = int(std::lround(std::clamp(share, 0.0f, 1.0f) * 100.0f));
			if (pct != g_ex.vbPct) {
				SetMember(g_ex.vbFill, "width", Num(g_ex.vbWidth * pct / 100.0));
				g_ex.vbPct = pct;
			}
		}

		void WorkExtras(uptr menu, uptr movie)
		{
			static int vbTries = 0;
			static ULONGLONG vbTriedAt = 0;
			if (!g_ex.vbBuilt && !g_ex.vbFailed && GetTickCount64() - vbTriedAt >= 2000) {
				vbTriedAt = GetTickCount64();
				g_ex.movie = movie;
				if (BuildVehicleBar(menu, movie)) g_ex.vbBuilt = true;
				else if (++vbTries >= 5) {  // the HUD may still be assembling: a few tries, then the card line only
					g_ex.vbFailed = true;
					logger::Write("Hover UI: vehicle bar could not be built (EnemyHealthMeter_mc / EnemyHealthAnimBase_72); the status card line stays");
				}
			}
			if (g_ex.vbBuilt) RefreshVehicleBar();
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
		void CaptureScannerDestination(uptr movie, const Value& root)
		{
			std::string name;
			double pickForm=0,pickHandle=0;
			// These MonocleMenu fields are private. Use its public data provider instead.
			Value manager, provider, data;
			const Value providerArgs[] = { Str("MonoclePickRefData"), Bool(false), Bool(false) };
			const bool havePick = GetVariable(movie, &manager, "Shared.AS3.Data.BSUIDataManager") && IsObject(manager) &&
				InvokeR(manager, "GetDataFromClient", providerArgs, 3, &provider) && IsObject(provider) &&
				GetMember(provider, "data", &data) && IsObject(data);
			if (havePick) { GetNumber(data, "uFormID", pickForm); GetNumber(data, "uHandle", pickHandle); }
			static int pickSource = -1;
			if (pickSource != int(havePick)) {
				pickSource = int(havePick);
				logger::Write("Scanner native pick provider: %s", havePick ? "ready" : "unavailable");
			}
			Release(data); Release(provider); Release(manager);
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
					distance = scannercontext::RangeMeters(shown);
					if (!std::isfinite(distance) || distance <= 0 || distance > 100000) distance = 0;
				}
			}
			Release(rangeText); Release(range);
			Release(text); Release(description); Release(marker); Release(panel);
			std::lock_guard lk(g_scanLock);
			g_pick={std::uint32_t(pickForm),std::uint32_t(pickHandle)};
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
				if (IsObject(root)) CaptureScannerDestination(movie, root);
				RenderRoute(menu, movie, true);
			}
			if (!movie || g_mono.failed) return;
			if (!g_mono.built) {
				const Value& root = *reinterpret_cast<const Value*>(menu + 0x58);
				if (!IsObject(root)) return;
				for (MonoEl* e : { &g_mono.status }) {
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
				logger::Write("Hover UI: scanner readout added (target status)");
			}
			// Only the status box is used now: it carries the scanner marker text (the distance to what you look at).
			std::string scan;
			bool placed;
			double x, y;
			{
				std::lock_guard lk(g_scanLock);
				scan = g_scanText;
				placed = g_targetPlaced; x = g_targetX; y = g_targetY;
			}
			bool edit;
			bool picked;
			double ts = kBaseScale;
			{
				std::lock_guard lk(g_lay.lock);
				edit = g_lay.edit;
				picked = g_inVehicle.load() ? g_lay.sel == kSelTarget || g_lay.sel == kSelExtras || g_lay.sel == kSelAll : g_lay.fsel == 4 || g_lay.fsel == kFootEls;
				ts = kBaseScale * (g_inVehicle.load() ? g_lay.carSize[1] : g_lay.footSize[4]);
			}
			if (scan.empty() && edit) scan = "TARGET  1240 m";  // shown with a sample while editing, so it can be placed
			const auto& root = *reinterpret_cast<const Value*>(menu + 0x58);
			const bool show = !scan.empty() && placed && TransformPoint(movie, root, "globalToLocal", x, y);
			MonoShow(g_mono.status, show);
			if (!show) return;
			MonoSetData(movie, g_mono.status, " ");
			MonoSetKey(g_mono.status, scan);
			MonoTint(movie, g_mono.status, true);
			const bool blinkOff = edit && (GetTickCount64() / 300) % 2 == 0;
			SetMember(g_mono.status.button, "scaleX", Num(ts));
			SetMember(g_mono.status.button, "scaleY", Num(ts));
			PlaceButtonKey(g_mono.status.button, x, y, false, ts);
			SetMember(g_mono.status.button, "alpha", Num(blinkOff && picked ? 0.35 : 1.0));
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
			// The pause / main menu: the settings panel's category (g_set.failed: the hooks are missing).
			const uptr sm = g_setMenu.load();
			if (sm && movie == g_setMovie.load() && !g_set.failed) SettingsWork(sm, movie);
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
			if (movie && movie == g_setMovie.load()) {
				bindings::CancelCapture();g_set.captureUI=false;
				g_setMenu.store(0);
				g_setMovie.store(0);
				g_set.movie = 0;
			}
			if (mono) {
				g_monoMenu.store(0);
				g_monoMovie.store(0);
				std::lock_guard lk(g_scanLock);
				g_placeName.clear(); g_placeAt = 0;
			}
			void* r = g_movieDtorOriginal(movie, flags);
			for(auto& view:g_routeViews) if(view.movie==movie) view=RouteView{};
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
		InstallSettings();
		{
			std::lock_guard lk(g_lay.lock);
			LoadLayout();
		}
		logger::Write("Hover UI: HUD menu and movie advance hooked (main thread %lu)", GetCurrentThreadId());
		return true;
	}

	bool IsScannerOpen() { return ScannerOpen(); }
	ScannerPick ScannerPickContext(){
		std::lock_guard lk(g_scanLock);
		return ScannerOpen()&&GetTickCount64()-g_placeAt<200?g_pick:ScannerPick{};
	}

	ScannerDestination ScannerDestinationContext()
	{
		std::lock_guard lk(g_scanLock);
		return ScannerOpen() && GetTickCount64() - g_placeAt < 200 ? ScannerDestination{g_placeName, g_placeDistance} : ScannerDestination{};
	}

	void SetRoute(const char* text,const char* skipKey,const char* clearKey,const char* retryKey)
	{
		std::lock_guard lk(g_scanLock);
		g_routeText=text ? text : "";
		g_routeSkipKey=skipKey?skipKey:"";g_routeClearKey=clearKey?clearKey:"";g_routeRetryKey=retryKey?retryKey:"";
	}

	void SetScan(const char* text)
	{
		std::lock_guard lk(g_scanLock);
		g_scanText = text ? text : "";
	}

	void SetState(bool inVehicle, bool hovering, int gear, bool accelHeld)
	{
		static bool was = false;
		if (inVehicle && !was) {  // a boarding: the basic controls show for kTeachMs (always on the first kTeachRides)
			g_boardedAt = GetTickCount64();
			const int rides = ++g_rides;
			{
				std::lock_guard lk(g_lay.lock);
				SaveLayout();
			}
			if (rides == kTeachRides + 1 && g_hintsKey > 0)
				notify::Show("Control hints now hide after 10 s | %s shows them", bindings::Label(bindings::Action::ShowHints, g_hintsKey > 0 ? int(g_hintsKey) : 0).c_str());
		}
		was = inVehicle;
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

	void SetFoot(bool own, bool nearOwn, const char* summonKey, const char* followKey, const char* useKey, const char* status,
		const char* summonLabel, const char* coverLabel)
	{
		std::lock_guard lk(g_ctx.lock);
		if (g_ctx.summonLabel != summonLabel) g_ctx.summonLabel = summonLabel;
		if (g_ctx.coverLabel != coverLabel) g_ctx.coverLabel = coverLabel;
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

	void SetTripStatus(bool on, const char* line)
	{
		std::lock_guard lk(g_ctx.lock);
		g_ctx.autoOn = on;
		if (g_ctx.autoLine != line) g_ctx.autoLine = line;
	}

	void SetLifebar(const char* line)
	{
		std::lock_guard lk(g_ctx.lock);
		if (g_ctx.lifebar != line) g_ctx.lifebar = line;
	}

	void SetVehicleBar(const char* name, float share)
	{
		std::lock_guard lk(g_ctx.lock);
		if (g_ctx.vbarName != name) g_ctx.vbarName = name;
		g_ctx.vbarShare = share;
	}

	void SetAutoKey(const char* key)
	{
		std::lock_guard lk(g_ctx.lock);
		if (g_ctx.autoKey != key) g_ctx.autoKey = key;
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
		g_scanText.clear(); g_targetPlaced = false;
		g_routeText.clear();
		g_routeSkipKey.clear();g_routeClearKey.clear();g_routeRetryKey.clear();
	}

	namespace
	{
		// The editor's pick order in the vehicle (caller holds g_lay.lock).
		std::vector<int> SelOrder()
		{
			std::vector<int> o;
			for (int i = 0; i <= kSelRows; ++i) if (i != kGearUp) o.push_back(i);  // (hidden: one Alt button now)
			o.push_back(kSelRoute);
			o.push_back(kSelAttitude);
			o.push_back(kSelAttitudeText);
			for (int i = 0; i < int(g_lay.natNames.size()); ++i) o.push_back(kSelNative + i);
			for (int i : { int(kSelCluster), int(kSelColumn), int(kSelExtras), int(kSelAll) }) o.push_back(i);
			return o;
		}

		// The picked element's on-screen name (caller holds g_lay.lock).
		std::string SelName(bool inVehicle)
		{
			if (!inVehicle) {
				const char* foot[kFootEls + 2] = { "Summon", "Follow", "Get in", "Status", "Target", "Passenger", "Eject", "Vehicles", "Panel", "Route" };
				return foot[std::clamp(g_lay.fsel, 0, kFootEls+1)];
			}
			const int s = g_lay.sel;
			if(s==kSelRoute)return "Route";
			if(s==kSelAttitude)return "VTOL attitude";
			if(s==kSelAttitudeText)return "VTOL attitude text";
			const char* names[kSelAll + 1] = { "W", "A", "S", "D", "Descend", "Gear", "Hover", "Gear box", "Trip status", "Target", "Vehicles", "WASD", "Column", "Extras",
				"All" };
			if (s >= 0 && s <= kSelAll) return names[s];
			if (s >= kSelNative && s - kSelNative < int(g_lay.natNames.size())) return g_lay.natNames[s - kSelNative];
			return "?";
		}
	}

	void EditTick(bool focused)
	{
		static bool down[14] = {};
		static ULONGLONG repeatAt[14] = {};
		// 10..13: + / - on the main row and the number pad (the size, while editing).
		const int keys[14] = { VK_F7, VK_PRIOR, VK_NEXT, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_HOME, VK_END, VK_DELETE, VK_OEM_PLUS, VK_ADD, VK_OEM_MINUS, VK_SUBTRACT };
		const ULONGLONG now = GetTickCount64();
		bool press[14] = {};
		for (int i = 0; i < 14; ++i) {
			const bool held = focused && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
			if (held && !down[i]) {
				press[i] = true;
				repeatAt[i] = now + 350;  // arrows, Home, End and + / - repeat while held
			}
			else if (held && ((i >= 3 && i <= 8) || i >= 10) && now >= repeatAt[i]) {
				press[i] = true;
				repeatAt[i] = now + 40;
			}
			down[i] = held;
		}
		std::lock_guard lk(g_lay.lock);
		{
			// The hints key (J, else O / Y; the first free in play and in vehicles): every hint and panel on / off.
			static bool hintsDown = false;
			if (!g_hintsKey) {
				for (const int vk : { 'J', 'O', 'Y' }) {
					if (controlmap::KeyFreeIn(vk, "MainGameplay") && controlmap::KeyFreeIn(vk, "Vehicle") && controlmap::Claim("hud", "show all hints", vk)) { g_hintsKey = vk; break; }
				}
				if (!g_hintsKey) g_hintsKey = -1;
				logger::Write("Hover UI: hints key %s (shows every hint and panel; on now: %d; rides %d)", g_hintsKey > 0 ? controlmap::KeyName(g_hintsKey).c_str() : "NONE FREE",
					g_showAll ? 1 : 0, g_rides.load());
			}
			const bool hd = focused && bindings::Held(bindings::Action::ShowHints, g_hintsKey > 0 ? int(g_hintsKey) : 0);
			if (hd && !hintsDown) {
				g_showAll = !g_showAll;
				SaveLayout();
				notify::Show(g_showAll ? "All hints shown" : "Hints: only what you need now");
				logger::Write("Hover UI: show all hints %s", g_showAll ? "ON" : "OFF");
			}
			hintsDown = hd;
		}
		g_editOn = g_lay.edit;
		if (press[0]) {
			if (!g_inVehicle.load() && !g_footShown.load() && !g_lay.edit) return;  // the buttons are visible in the rover or on foot
			g_lay.edit = !g_lay.edit;
			g_editOn = g_lay.edit;
			if (!g_lay.edit) SaveLayout();
			logger::Write("Hover UI: layout edit %s", g_lay.edit ? "ON" : "OFF, saved to hoverui.ini");
			notify::Show(g_lay.edit ? "UI edit: %s" : "UI saved", g_lay.edit ? SelName(g_inVehicle.load()).c_str() : "");
			return;
		}
		if (!g_lay.edit) return;
		if (press[10] || press[11] || press[12] || press[13]) {
			// + / -: the selected element (or every member of a selected group) grows / shrinks by 5 %, saved at once.
			const double delta = (press[10] || press[11]) ? 0.05 : -0.05;
			const bool car = g_inVehicle.load();
			std::vector<double*> picked;
			if (!car) {
				if (g_lay.fsel < kFootEls) picked.push_back(&g_lay.footSize[g_lay.fsel]);
				else if (g_lay.fsel == kFootEls) for (auto& v : g_lay.footSize) picked.push_back(&v);
				else picked.push_back(&g_lay.routeSize);
			}
			else {
				const int sel = g_lay.sel;
				const bool all = sel == kSelAll;
				for (int i = 0; i < kButtons; ++i) {
					const int group = i <= kD ? kSelCluster : i == kGear ? -1 : kSelColumn;
					if (all || sel == i || sel == group) picked.push_back(&g_lay.btnSize[i]);
				}
				for (int i = 0; i < kCarEls; ++i) if (all || sel == kSelExtras || sel == kSelLine + i) picked.push_back(&g_lay.carSize[i]);
				if (all || sel == kSelRoute) picked.push_back(&g_lay.routeSize);
				if (all || sel == kSelAttitude) picked.push_back(&g_lay.attitudeSize);
				if (all || sel == kSelAttitudeText) picked.push_back(&g_lay.attitudeTextSize);
			}
			if (picked.empty()) notify::Show("UI edit: %s keeps the game's own size", SelName(car).c_str());
			else {
				for (double* v : picked) *v = std::clamp(std::round((*v + delta) * 100.0) / 100.0, kMinSize, kMaxSize);
				SaveLayout();
				notify::Show("UI edit: %s size %.0f%%", SelName(car).c_str(), *picked.front() * 100.0);
				logger::Write("Hover UI: %s size -> %.2f (%zu elements)", SelName(car).c_str(), *picked.front(), picked.size());
			}
		}
		const double step = focused && (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 10.0 : 1.0;
		const double dx = (press[4] ? step : 0.0) - (press[3] ? step : 0.0);
		const double dy = (press[6] ? step : 0.0) - (press[5] ? step : 0.0);
		const double dd = (press[8] ? step : 0.0) - (press[7] ? step : 0.0);
		if (!g_inVehicle.load()) {
			// On foot: Page Up / Page Down pick Summon, Follow, Get in, status, target or the whole panel; the arrows move it.
			if (press[1] || press[2]) {
				g_lay.fsel = (g_lay.fsel + (press[1] ? kFootEls+1 : 1)) % (kFootEls + 2);
				notify::Show("UI edit: %s", SelName(false).c_str());
			}
			if(g_lay.fsel==kFootEls+1){
				if(dx!=0||dy!=0){g_lay.routeX+=dx;g_lay.routeY+=dy;SaveLayout();}
				if(press[9]){g_lay.routeX=kDefRouteX;g_lay.routeY=kDefRouteY;SaveLayout();}return;
			}
			if (dx != 0.0 || dy != 0.0) {
				if (g_lay.fsel == kFootEls) { g_lay.fpx += dx; g_lay.fpy += dy; }
				else { g_lay.footOff[g_lay.fsel][0] += dx; g_lay.footOff[g_lay.fsel][1] += dy; }
				SaveLayout();
			}
			if (press[9] && g_lay.fsel < kFootEls) { g_lay.footOff[g_lay.fsel][0] = kDefFoot[g_lay.fsel][0]; g_lay.footOff[g_lay.fsel][1] = kDefFoot[g_lay.fsel][1]; SaveLayout(); }
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
		if(g_lay.sel==kSelAttitude){
			if(dx!=0||dy!=0){g_lay.attitudeX+=dx;g_lay.attitudeY+=dy;SaveLayout();}
			if(press[9]){g_lay.attitudeX=kDefAttX;g_lay.attitudeY=kDefAttY;g_lay.attitudeSize=1;SaveLayout();}return;
		}
		if(g_lay.sel==kSelAttitudeText){
			if(dx!=0||dy!=0){g_lay.attitudeTextX+=dx;g_lay.attitudeTextY+=dy;SaveLayout();}
			if(press[9]){g_lay.attitudeTextX=kDefAttTextX;g_lay.attitudeTextY=kDefAttTextY;g_lay.attitudeTextSize=1;SaveLayout();}return;
		}
		if(g_lay.sel==kSelRoute){
			if(dx!=0||dy!=0){g_lay.routeX+=dx;g_lay.routeY+=dy;SaveLayout();}
			if(press[9]){g_lay.routeX=kDefRouteX;g_lay.routeY=kDefRouteY;SaveLayout();}return;
		}
		const int sel = g_lay.sel;
		const bool all = sel == kSelAll;
		bool changed = false;
		if (dx != 0.0 || dy != 0.0) {
			if (all || sel == kSelCluster) { g_lay.cx += dx; g_lay.cy += dy; }
			if (all || sel == kSelColumn) { g_lay.colRight += dx; g_lay.colTop += dy; }
			if (all || sel == kGear) { g_lay.gx += dx; g_lay.gy += dy; }
			if (all || sel == kSelExtras) { g_lay.eox += dx; g_lay.eoy += dy; }
			if(all){g_lay.routeX+=dx;g_lay.routeY+=dy;g_lay.attitudeX+=dx;g_lay.attitudeY+=dy;g_lay.attitudeTextX+=dx;g_lay.attitudeTextY+=dy;}
			if (sel >= 0 && sel < kGear) { g_lay.off[sel][0] += dx; g_lay.off[sel][1] += dy; }
			if (sel >= kSelLine && sel <= kSelRows) { g_lay.carOff[sel - kSelLine][0] += dx; g_lay.carOff[sel - kSelLine][1] += dy; }
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
			if (sel >= kSelLine && sel <= kSelRows) { g_lay.carOff[sel - kSelLine][0] = kDefCar[sel - kSelLine][0]; g_lay.carOff[sel - kSelLine][1] = kDefCar[sel - kSelLine][1]; changed = true; }
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
