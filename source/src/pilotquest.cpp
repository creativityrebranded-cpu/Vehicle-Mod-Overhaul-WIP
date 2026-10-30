#include "pilotquest.h"
#include "runtimeforms.h"
#include "pilotlayout.h"

#include "addresses.h"
#include "game.h"
#include "log.h"

#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pilotquest
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		constexpr std::uint32_t kForceInto = 0x00CFC990;    // FUN_140cfc990(quest, aliasId, ref, 6): ReferenceAlias.ForceRefTo
		constexpr std::uint32_t kClearAlias = 0x00CFC590;   // FUN_140cfc590(quest, alias, 6, 0): ReferenceAlias.Clear
		constexpr std::uint32_t kAliasIndex = 0x00CF72D0;   // FUN_140cf72d0(quest, aliasId) -> index into quest +0x90, -1 none
		constexpr std::uint32_t kEvaluate = 0x01914750;     // FUN_141914750(actor, immediate, resetAI): console evp
		constexpr std::uint32_t kVanillaQuest = 0x001D307E; // SQ_Passengers: gives the QUST form type byte
		constexpr std::uint32_t kVanillaPack = 0x001C9CDA;  // DefaultPassengerShipInCombatPackage: the PACK form type byte
		constexpr int kSlots = pilotlayout::kSlots;
		// Alias ids (make_pilots_esm.py, 2026-10-29): slots 0..5 keep the first layout (pilot 0..5, vehicle 6..11) so saves made with
		// the 6-slot plugin keep their fills; slots 6..11 have pilot aliases 12..17 and vehicle aliases 18..23.
		// Four blocks (slot first, count), each its pilot aliases then its vehicle aliases: 0..5 (0..5, 6..11), 6..11 (12..17, 18..23),
		// 12..63 (24..75, 76..127), 64.. (128.., then its vehicle aliases); make_pilots_esm.py's BLOCKS. Older saves keep their fills.
		constexpr int kBlockFirst[4] = { 0, 6, 12, 64 }, kBlockCount[4] = { 6, 6, 52, kSlots - 64 };
		constexpr int BlockBase(int b) { int base = 0; for (int i = 0; i < b; ++i) base += 2 * kBlockCount[i]; return base; }
		constexpr int BlockOf(int slot) { return slot < 6 ? 0 : slot < 12 ? 1 : slot < 64 ? 2 : 3; }
		constexpr int PilotAlias(int slot) { return BlockBase(BlockOf(slot)) + (slot - kBlockFirst[BlockOf(slot)]); }
		constexpr int VehicleAlias(int slot) { return BlockBase(BlockOf(slot)) + kBlockCount[BlockOf(slot)] + (slot - kBlockFirst[BlockOf(slot)]); }
		static_assert(PilotAlias(5) == 5 && VehicleAlias(5) == 11 && PilotAlias(6) == 12 && VehicleAlias(6) == 18 && PilotAlias(12) == 24 &&
			VehicleAlias(12) == 76 && VehicleAlias(63) == 127 && PilotAlias(64) == 128 && VehicleAlias(kSlots - 1) == 2 * kSlots - 1);

		uptr g_quest = 0, g_actQuest = 0;  // g_actQuest: the second quest, whose pilot alias carries the ACTIVATE package
		std::uint32_t g_questId=0;bool g_missingReported=false;
		ULONGLONG g_lastProbe = 0;
#ifdef SP_RUNTIME_PERF_TEST
        ULONGLONG g_testNow=10000;
#endif
		bool g_loggedRunning = false;

		uptr Lookup(std::uint32_t id) {return runtimeforms::Lookup(id);}

		std::uint8_t TypeOf(uptr form) { return form ? At<std::uint8_t>(form, 0x2E) : 0; }

		void Probe()
		{
#ifdef SP_RUNTIME_PERF_TEST
            const ULONGLONG now=g_testNow;
#else
			const ULONGLONG now = GetTickCount64();
#endif
			if (g_lastProbe && now - g_lastProbe < 5000) return;
			g_lastProbe = now;
            if(g_quest){
                const auto current=runtimeforms::Resolve(pilotlayout::kBoardQuest);
                if(current==g_questId&&Lookup(current)==g_quest)return;
                g_quest=g_actQuest=0;g_questId=0;g_loggedRunning=false;
            }
            const auto qType=TypeOf(Lookup(kVanillaQuest)),pType=TypeOf(Lookup(kVanillaPack));
            if(!qType||!pType)return;
            const auto questId=runtimeforms::Resolve(pilotlayout::kBoardQuest);
            const auto packId=runtimeforms::Resolve(pilotlayout::kBoardPackages.At(0));
            const auto quest=Lookup(questId),package=Lookup(packId);
            if(quest&&package&&TypeOf(quest)==qType&&TypeOf(package)==pType){
                g_quest=quest;g_questId=questId;g_missingReported=false;
                g_actQuest=Lookup(runtimeforms::Resolve(pilotlayout::kActivateQuest));
                if(g_actQuest&&TypeOf(g_actQuest)!=qType)g_actQuest=0;
                logger::Write("Pilot quest: %s resolved: quest %08X (%s), package %08X",pilotlayout::kBoardQuest.plugin,
                    questId,(At<std::uint8_t>(quest,0x114)&1)?"running":"NOT running",packId);
                return;
            }
            if(!g_missingReported){g_missingReported=true;
                logger::Write("Pilot quest: %s is not loaded or its forms differ; NPC quest boarding unavailable",pilotlayout::kBoardQuest.plugin);}

		}

		bool ForceIn(uptr quest, int aliasId, uptr ref)
		{
			__try {
				EngineFn<void (*)(uptr, std::uint32_t, uptr, int)>(kForceInto)(quest, std::uint32_t(aliasId), ref, 6);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool Force(int aliasId, uptr ref) { return ForceIn(g_quest, aliasId, ref); }

		void ClearIn(uptr quest, int aliasId)
		{
			if (!quest) return;
			__try {
				const std::uint32_t idx = EngineFn<std::uint32_t (*)(uptr, std::uint32_t)>(kAliasIndex)(quest, std::uint32_t(aliasId));
				if (idx == 0xFFFFFFFFu) return;
				const uptr alias = reinterpret_cast<uptr*>(At<uptr>(quest, 0x90))[idx];
				if (alias) EngineFn<void (*)(uptr, uptr, int, uptr)>(kClearAlias)(quest, alias, 6, 0);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}
	}

	namespace
	{
		void Clear(int aliasId) { ClearIn(g_quest, aliasId); }
	}

	bool Available()
	{
		Probe();
		if (!g_quest) return false;
		const bool running = (At<std::uint8_t>(g_quest, 0x114) & 1) != 0;
		if (running && !g_loggedRunning) {
			g_loggedRunning = true;
			logger::Write("Pilot quest: the quest is running");
		}
		return running;
	}

	bool Assign(int slot, uptr npc, uptr vehicle)
	{
		if (slot < 0 || slot >= kSlots || !npc || !vehicle) return false;
		if (!Available()) {
			logger::Write("Pilot quest: assign refused: %s", g_quest ? "the quest is not running" : "FVO_NPCPilots.esm is not loaded");
			return false;
		}
		Clear(PilotAlias(slot));
		Clear(VehicleAlias(slot));
		const bool v = Force(VehicleAlias(slot), vehicle);  // the vehicle first: the package's target
		const bool p = Force(PilotAlias(slot), npc);
		logger::Write("Pilot quest: slot %d: vehicle alias %d <- %08X (%s), pilot alias %d <- %08X (%s)", slot, VehicleAlias(slot), At<std::uint32_t>(vehicle, off::kFormId), v ? "ok" : "FAULT",
			PilotAlias(slot), At<std::uint32_t>(npc, off::kFormId), p ? "ok" : "FAULT");
		if (!v || !p) return false;
		__try {
			EngineFn<void (*)(uptr, int, int)>(kEvaluate)(npc, 1, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
		return true;
	}

	namespace
	{
		struct Diag
		{
			ULONGLONG at = 0;
			int slot = -1;
			uptr npc = 0;
		} g_diag;

		bool Readable(uptr p, std::size_t n) { return p && !IsBadReadPtr(reinterpret_cast<void*>(p), n); }

		void RunDiag()
		{
			const uptr npc = g_diag.npc;
			const std::uint32_t npcId = At<std::uint32_t>(npc, off::kFormId);
			// What the pilot alias object holds: any qword in its first 0x80 bytes equal to the actor, or its form id.
			const std::uint32_t idx = EngineFn<std::uint32_t (*)(uptr, std::uint32_t)>(kAliasIndex)(g_quest, std::uint32_t(PilotAlias(g_diag.slot)));
			uptr alias = 0;
			if (idx != 0xFFFFFFFFu) alias = reinterpret_cast<uptr*>(At<uptr>(g_quest, 0x90))[idx];
			char hits[160] = "";
			if (Readable(alias, 0x80)) {
				for (std::uint32_t o = 0; o < 0x80; o += 8) {
					const uptr q = At<uptr>(alias, o);
					if (q == npc || std::uint32_t(q) == npcId) {
						char t[24];
						sprintf_s(t, " +%02X", o);
						strcat_s(hits, t);
					}
				}
			}
			// The actor's current package (ActorPackage inline at process +0x18, TESPackage* at +0x20).
			const uptr proc = At<uptr>(npc, 0x228);
			uptr pkg = proc && Readable(proc + 0x20, 8) ? At<uptr>(proc, 0x20) : 0;
			const bool pkgOk = Readable(pkg, 0x40);
			logger::Write("Pilot quest: slot %d diagnose: alias object %p (index %d) holds the NPC at:%s; NPC %08X runs package %08X type %d (ours are %08X..%08X); quest running %d",
				g_diag.slot, reinterpret_cast<void*>(alias), int(idx), hits[0] ? hits : " NOWHERE", npcId, pkgOk ? At<std::uint32_t>(pkg, 0x28) : 0,
				pkgOk ? int(At<std::int8_t>(pkg, 0x3C)) : -1, runtimeforms::Resolve(pilotlayout::kBoardPackages.At(0)), runtimeforms::Resolve(pilotlayout::kBoardPackagesRest.At(std::size_t(kSlots-65))),
				(At<std::uint8_t>(g_quest, 0x114) & 1) ? 1 : 0);
		}
	}

	void Diagnose(int slot, uptr npc, unsigned delayMs)
	{
		g_diag.slot = slot;
		g_diag.npc = npc;
		g_diag.at = GetTickCount64() + delayMs;
	}

	void Tick()
	{
		if (!g_diag.at || GetTickCount64() < g_diag.at) return;
		g_diag.at = 0;
		__try {
			RunDiag();
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			logger::Write("Pilot quest: diagnose faulted");
		}
	}
    void Reset(){g_quest=g_actQuest=0;g_questId=0;g_lastProbe=0;g_loggedRunning=false;g_missingReported=false;g_diag={};}

	bool Retarget(int slot, uptr ref, uptr npc)
	{
		if (slot < 0 || slot >= kSlots || !g_quest || !ref) return false;
		const bool ok = Force(VehicleAlias(slot), ref);
		logger::Write("Pilot quest: slot %d: vehicle alias %d retargeted to %08X (%s)", slot, VehicleAlias(slot), At<std::uint32_t>(ref, off::kFormId), ok ? "ok" : "FAULT");
		__try {
			EngineFn<void (*)(uptr, int, int)>(kEvaluate)(npc, 1, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
		return ok;
	}

	bool AssignActivate(int slot, uptr npc, uptr vehicle)
	{
		if (slot < 0 || slot >= kSlots || !npc || !vehicle) return false;
		if (!g_actQuest || !(At<std::uint8_t>(g_actQuest, 0x114) & 1)) {
			logger::Write("Pilot quest: the Activate quest %s", g_actQuest ? "is not running" : "is not in the plugin (regenerate FVO_NPCPilots.esm)");
			return false;
		}
		ClearIn(g_actQuest, PilotAlias(slot));
		ClearIn(g_actQuest, VehicleAlias(slot));
		const bool v = ForceIn(g_actQuest, VehicleAlias(slot), vehicle);
		const bool p = ForceIn(g_actQuest, PilotAlias(slot), npc);
		logger::Write("Pilot quest: ACTIVATE slot %d: vehicle alias %d <- %08X (%s), pilot alias %d <- %08X (%s)", slot, VehicleAlias(slot), At<std::uint32_t>(vehicle, off::kFormId), v ? "ok" : "FAULT",
			PilotAlias(slot), At<std::uint32_t>(npc, off::kFormId), p ? "ok" : "FAULT");
		if (!v || !p) return false;
		__try {
			EngineFn<void (*)(uptr, int, int)>(kEvaluate)(npc, 1, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
		return true;
	}

	void Release(int slot)
	{
		if (slot < 0 || slot >= kSlots || !g_quest) return;
		Clear(PilotAlias(slot));
		Clear(VehicleAlias(slot));
		ClearIn(g_actQuest, PilotAlias(slot));
		ClearIn(g_actQuest, VehicleAlias(slot));
		logger::Write("Pilot quest: slot %d released", slot);
	}
    bool RouteTraffic(int slot,uptr npc,uptr marker) {
        if(slot<0||slot>=18||!npc||!marker)return false;
        const auto quest=runtimeforms::LookupChecked(runtimeforms::Resolve({"FVO_ScheduledTraffic.esm",0x800}));
        if(!quest)return false;
        __try {
            if(TypeOf(quest)!=TypeOf(Lookup(kVanillaQuest))||!(At<std::uint8_t>(quest,0x114)&1))return false;
            ClearIn(quest,slot);ClearIn(quest,18+slot);
            if(!ForceIn(quest,18+slot,marker)||!ForceIn(quest,slot,npc)){ClearIn(quest,slot);ClearIn(quest,18+slot);return false;}
            EngineFn<void (*)(uptr,int,int)>(kEvaluate)(npc,1,0);return true;
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    void ReleaseTraffic(int slot,uptr npc) {
        if(slot<0||slot>=18)return;
        const auto quest=runtimeforms::LookupChecked(runtimeforms::Resolve({"FVO_ScheduledTraffic.esm",0x800}));
        if(!quest)return;
        ClearIn(quest,slot);ClearIn(quest,18+slot);
        if(npc) __try {EngineFn<void (*)(uptr,int,int)>(kEvaluate)(npc,1,0);} __except(EXCEPTION_EXECUTE_HANDLER){}
    }
#ifdef SP_RUNTIME_PERF_TEST
    namespace testing {void Clock(ULONGLONG now){g_testNow=now;}}
#endif
}
