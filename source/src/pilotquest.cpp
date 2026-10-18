#include "pilotquest.h"

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

		constexpr std::uint32_t kLookupForm = 0x005DE3B0;   // (form id) -> TESForm*
		constexpr std::uint32_t kForceInto = 0x00CFC990;    // FUN_140cfc990(quest, aliasId, ref, 6): ReferenceAlias.ForceRefTo
		constexpr std::uint32_t kClearAlias = 0x00CFC590;   // FUN_140cfc590(quest, alias, 6, 0): ReferenceAlias.Clear
		constexpr std::uint32_t kAliasIndex = 0x00CF72D0;   // FUN_140cf72d0(quest, aliasId) -> index into quest +0x90, -1 none
		constexpr std::uint32_t kEvaluate = 0x01914750;     // FUN_141914750(actor, immediate, resetAI): console evp
		constexpr std::uint32_t kVanillaQuest = 0x001D307E; // SQ_Passengers: gives the QUST form type byte
		constexpr std::uint32_t kVanillaPack = 0x001C9CDA;  // DefaultPassengerShipInCombatPackage: the PACK form type byte
		constexpr std::uint32_t kQuestLocal = 0x800, kPackLocal = 0x801, kActQuestLocal = 0x810;
		constexpr int kSlots = 6;

		uptr g_quest = 0, g_actQuest = 0;  // g_actQuest: the second quest, whose pilot alias carries the ACTIVATE package
		int g_index = -1;               // the plugin's load index; -2: probed, not loaded
		ULONGLONG g_lastProbe = 0;
		bool g_loggedRunning = false;

		uptr Lookup(std::uint32_t id)
		{
			__try {
				return EngineFn<uptr (*)(std::uint32_t)>(kLookupForm)(id);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		std::uint8_t TypeOf(uptr form) { return form ? At<std::uint8_t>(form, 0x2E) : 0; }

		void Probe()
		{
			const ULONGLONG now = GetTickCount64();
			if (g_lastProbe && now - g_lastProbe < 5000) return;
			g_lastProbe = now;
			if (g_quest) {
				// still the same form? (a save load rebuilds forms)
				if (Lookup((std::uint32_t(g_index) << 24) | kQuestLocal) == g_quest) return;
				g_quest = 0;
				g_actQuest = 0;
				g_index = -1;
				g_loggedRunning = false;
			}
			const std::uint8_t qType = TypeOf(Lookup(kVanillaQuest)), pType = TypeOf(Lookup(kVanillaPack));
			if (!qType || !pType) return;  // the game's forms are not up yet
			for (std::uint32_t i = 1; i < 0xFE; ++i) {
				const uptr q = Lookup((i << 24) | kQuestLocal), p = Lookup((i << 24) | kPackLocal);
				if (q && p && TypeOf(q) == qType && TypeOf(p) == pType) {
					g_quest = q;
					g_index = int(i);
					g_actQuest = Lookup((i << 24) | kActQuestLocal);
					if (g_actQuest && TypeOf(g_actQuest) != qType) g_actQuest = 0;
					logger::Write("Pilot quest: SeamlessPilots.esm found at load index %02X: quest %08X (%s), package %08X", i, (i << 24) | kQuestLocal,
						(At<std::uint8_t>(q, 0x114) & 1) ? "running" : "NOT running", (i << 24) | kPackLocal);
					return;
				}
			}
			if (g_index == -1) {
				g_index = -2;
				logger::Write("Pilot quest: SeamlessPilots.esm is not loaded (enable it in Plugins.txt); NPCs cannot be sent to vehicles by quest package");
			}
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
			logger::Write("Pilot quest: assign refused: %s", g_quest ? "the quest is not running" : "SeamlessPilots.esm is not loaded");
			return false;
		}
		Clear(slot);
		Clear(kSlots + slot);
		const bool v = Force(kSlots + slot, vehicle);  // the vehicle first: the package's target
		const bool p = Force(slot, npc);
		logger::Write("Pilot quest: slot %d: vehicle alias %d <- %08X (%s), pilot alias %d <- %08X (%s)", slot, kSlots + slot, At<std::uint32_t>(vehicle, off::kFormId), v ? "ok" : "FAULT",
			slot, At<std::uint32_t>(npc, off::kFormId), p ? "ok" : "FAULT");
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
			const std::uint32_t idx = EngineFn<std::uint32_t (*)(uptr, std::uint32_t)>(kAliasIndex)(g_quest, std::uint32_t(g_diag.slot));
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
				pkgOk ? int(At<std::int8_t>(pkg, 0x3C)) : -1, (std::uint32_t(g_index) << 24) | kPackLocal, (std::uint32_t(g_index) << 24) | (kPackLocal + kSlots - 1),
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

	bool Retarget(int slot, uptr ref, uptr npc)
	{
		if (slot < 0 || slot >= kSlots || !g_quest || !ref) return false;
		const bool ok = Force(kSlots + slot, ref);
		logger::Write("Pilot quest: slot %d: vehicle alias %d retargeted to %08X (%s)", slot, kSlots + slot, At<std::uint32_t>(ref, off::kFormId), ok ? "ok" : "FAULT");
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
			logger::Write("Pilot quest: the Activate quest %s", g_actQuest ? "is not running" : "is not in the plugin (regenerate SeamlessPilots.esm)");
			return false;
		}
		ClearIn(g_actQuest, slot);
		ClearIn(g_actQuest, kSlots + slot);
		const bool v = ForceIn(g_actQuest, kSlots + slot, vehicle);
		const bool p = ForceIn(g_actQuest, slot, npc);
		logger::Write("Pilot quest: ACTIVATE slot %d: vehicle alias %d <- %08X (%s), pilot alias %d <- %08X (%s)", slot, kSlots + slot, At<std::uint32_t>(vehicle, off::kFormId), v ? "ok" : "FAULT",
			slot, At<std::uint32_t>(npc, off::kFormId), p ? "ok" : "FAULT");
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
		Clear(slot);
		Clear(kSlots + slot);
		ClearIn(g_actQuest, slot);
		ClearIn(g_actQuest, kSlots + slot);
		logger::Write("Pilot quest: slot %d released", slot);
	}
}
