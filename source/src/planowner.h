#pragma once
// Owner and rank record (design section 9, D15; work order step 5). One record per vehicle: who drives it (owner) and where it stands on
// the right-of-way ladder (trafficrank.h). The coordinator that holds the master record belongs in the traffic addon (D15); the core keeps
// this read-only copy so it works with no addon, and NPC jobs and combat claim vehicles through the same record.
// Save-safe: nothing here is saved. It is rebuilt from what the core already knows each frame, so no saved format changes and an older
// save loads exactly as before. Collision safety ignores rank (G15): the plan's collision test treats every vehicle the same.
// Pure; tests: tests/planowner_test.cpp.
#include "trafficrank.h"
#include <cstdint>

namespace planowner {
enum class Owner : std::uint8_t { None, Traffic, Cruise, Job, Combat, Player, Parked, Count };
inline const char* Name(Owner o) { constexpr const char* n[]{ "none", "traffic", "cruise", "job", "combat", "player", "parked" }; return n[int(o)]; }
// Who may take a vehicle from whom: a higher owner takes it; the same owner refreshes its claim. The player always wins.
// Parked (managed stationary vehicle) has strength 0 so any active owner (Traffic, Cruise, Job, Combat, Player) claims it cleanly.
inline int Strength(Owner o) { constexpr int s[]{ 0, 1, 2, 3, 4, 5, 0 }; return s[int(o)]; }

constexpr int kMax = 512;
constexpr double kStale = 2.0;  // s without a refresh: the claim lapses (the pilot stopped, the vehicle unloaded)

enum class Life : std::uint8_t { None, Born, Proving, Driving, Recovering, Broken, Retiring };

struct Record {
    std::uint32_t vehicle = 0; Owner owner = Owner::None; trafficrank::Rank rank = trafficrank::Rank::Parked;
    int pilot = -1; double since = 0, seen = 0; const char* why = "";
    std::uint32_t driver = 0, model = 0; int slot = -1; Life life = Life::None; const char* lifeWhy = "";
    std::uint64_t laneIdentity = 0; int lane = -1; double laneAt = -1;  // Field Mask: the lane the committed plan heads for (route identity + lane index), core only, never saved
    // C2 (lanecoord.h): the corridor summary of the last plan, core only, never saved.
    int corridorLanes = 0, ownLane = -1, family = 0; float corridorFree[8] = {}; float corridorMeasured = 0, blockedAt = -1, stoppedFor = 0; std::uint32_t blockOwner = 0; bool reverse = false; double corridorAt = -1;
    double x = 0, y = 0, z = 0, speed = 0, placedAt = -1;  // C1: where the vehicle was at its last pilot claim (core only, never saved)
};
// C1 (coordinator data, D15 as amended 2026-11-07): one read-only row of the record, for the addon's coordinator. Versioned by size.
struct RowV1 { std::uint32_t size = sizeof(RowV1), version = 1; std::uint32_t vehicle = 0; int owner = 0, rank = 0; double x = 0, y = 0, z = 0, speed = 0, age = 0; };
typedef int (*SnapshotV1)(RowV1* out, int max);  // export "FVO_OwnerSnapshotV1": rows written; older cores have none
// Field Mask: version 2 adds the lane the committed plan is heading for. Version 1 stays unchanged. Export "FVO_OwnerSnapshotV2" (optional).
struct RowV2 { std::uint32_t size = sizeof(RowV2), version = 2; std::uint32_t vehicle = 0; int owner = 0, rank = 0; double x = 0, y = 0, z = 0, speed = 0, age = 0;
               std::uint64_t laneIdentity = 0; int lane = -1, laneAge10ms = -1; };
typedef int (*SnapshotV2)(RowV2* out, int max);
struct Change { bool changed = false; Owner from = Owner::None, to = Owner::None; bool refused = false; };

struct Table {
    Record rows[kMax]; int count = 0;
    Record* Find(std::uint32_t v) { for (int i = 0; i < count; ++i) if (rows[i].vehicle == v) return &rows[i]; return nullptr; }
    const Record* Of(std::uint32_t v, double now) const {
        for (int i = 0; i < count; ++i) if (rows[i].vehicle == v) return now - rows[i].seen <= kStale ? &rows[i] : nullptr;
        return nullptr;
    }
    // A claim. A weaker owner never takes a vehicle a stronger one still holds (refused, so the caller can trace it).
    Change Claim(std::uint32_t v, Owner o, trafficrank::Rank rank, int pilot, double now, const char* why) {
        Change c; if (!v || o == Owner::None) return c;
        Record* r = Find(v);
        if (!r) {
            if (count >= kMax) Sweep(now);
            if (count >= kMax) { c.refused = true; return c; }
            r = &rows[count++]; *r = Record{}; r->vehicle = v;
        }
        const bool live = r->owner != Owner::None && now - r->seen <= kStale;
        if (live && r->owner != o && Strength(o) < Strength(r->owner)) { c.refused = true; c.from = r->owner; c.to = o; return c; }
        c.from = live ? r->owner : Owner::None; c.to = o; c.changed = c.from != o;
        // A new owner starts clean; the same owner re-claiming after a lapse (traffic's Census, then its pilot) keeps its metadata.
        if (c.changed) { r->since = now; if (r->owner != o) { r->driver = r->model = 0; r->slot = -1; r->life = Life::None; r->lifeWhy = ""; } }
        r->owner = o; r->rank = rank; r->pilot = pilot; r->seen = now; r->why = why;
        return c;
    }
    // Census metadata only: does not acquire or refresh ownership. Reasons must outlive the record.
    bool Traffic(std::uint32_t v, std::uint32_t driver, int slot, std::uint32_t model, Life life, const char* why) {
        auto* r = Find(v); if (!r || r->owner != Owner::Traffic) return false;
        r->driver = driver; r->slot = slot; r->model = model; r->life = life; r->lifeWhy = why ? why : "";
        return true;
    }
    void Release(std::uint32_t v) { for (int i = 0; i < count; ++i) if (rows[i].vehicle == v) { rows[i] = rows[--count]; return; } }
    void Sweep(double now) { for (int i = 0; i < count;) if (now - rows[i].seen > kStale) rows[i] = rows[--count]; else ++i; }
};
}
