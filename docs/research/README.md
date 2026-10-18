# Research notes

Written while building the mod (Oct 2026). They record what was found in the decompile and in test logs, with addresses. Some are proposals that were later
changed, or are superseded by `../ARCHITECTURE.md`, `../ENGINE_NOTES.md` and `../KNOWN_ISSUES.md`; where they disagree, trust those three.

| Note | About |
|---|---|
| `VEHICLE_SHAPE_CAST_RESEARCH_20261017.md` | hknpBSWorld::CastShape, body ids, vehicle size |
| `NPC_VEHICLE_ENTRY_RESEARCH_20261018.md` | how the player and an NPC get into a vehicle; what refuses it |
| `NPC_VEHICLE_STAGE1_RESEARCH_20261017.md`, `NPC_STRUCTURE_20261017.md` | NPC process levels, seats, packages, virtuals |
| `VEHICLE_ARCHITECTURE_PROPOSAL_20261017.md`, `VEHICLE_CONTROLLERS_PROFILES_PROPOSAL_20261018.md` | the controller / profile design behind `vcontract` |
| `VEHICLE_WEAPON_AIM_RESEARCH_20261017.md` | aiming NPC vehicle shots at launch |
| `TRAFFIC_DESTINATION_RESEARCH.md` | where an NPC's current package sends it |
| `CLEANUP_PROPOSAL_20261018.md` | the restructuring plan that produced the shared goal type, key table and profile table (largely done) |
