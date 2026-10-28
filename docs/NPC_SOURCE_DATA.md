# Authored crowd population source

The baseline definitions live in the game's Data/Starfield.esm, rather than in
a per-city live census. This audit reads the installed master without changing
it. Source schema: https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Core/wbDefinitionsSF1.pas
(downloaded snapshot retained with the audit).

## Definition chain

1. GBFM records containing BGSCrowdComponent_Component define crowd-region
   density (CDND), conditional populations (CDNS / CTDA), NPC proportions
   (PRPS) and named/time-dependent population scales (STRV / FLTV).
2. Placed REFR records reference those crowd-region bases and establish their
   world/cell and placement. These generate a crowd, rather than providing one
   fixed ACHR reference for every citizen.
3. NPC_ records identify crowd appearances, actor flags, factions, templates
   and AI-package/default-package-list links (PKID / DPLT / TPLT / TPTA).
4. PACK and FLST records describe configured behavior. Package conditions,
   inherited data and quest/script overrides can still affect runtime activity.

## Verified installed-master examples

| Local form | Record | Editor ID | Relevant source data |
|---|---|---|---|
| 0000D5A8 | GBFM | CrowdRegion_NewAtlantis03 | Density 0.1; day scale 1.0, night scale 0.5; six weighted NPC choices in each population |
| 00173762 | GBFM | CrowdRegion_Akila01 | Density 0.1; day scale 1.0, night scale 0.6 |
| 00202681 | GBFM | CrowdRegion_Neon01 | Density 0.1; day scale 0.8, night scale 1.2 |
| 002FFAC3 | GBFM | CrowdRegion_Cydonia01 | Density 0.1; day scale 1.0, night scale 0.6 |
| 0017186A | NPC_ | CrowdActor_NA_Male_09 | Direct package 000220D9; faction/keyword template CrowdActor_TEMPLATE_NewAtlantis |
| 000220D9 | PACK | DefaultWanderEditorLocation | Uses Sandbox template 00002CB1; no direct owner quest, script or conditions in this package |
| 00107AEA | PACK | DefaultSandboxEditorLocation | Uses Sandbox template 00002CB1 |
| 001591EA | NPC_ | LvlCitizen_UC | Uses DefaultMasterPackageList_Human 002C55CA, a broader citizen definition than the simple crowd actors |

The scan found 24 crowd-region base definitions, 95 authored REFR placements
of those bases and 163 distinct NPC templates referenced by the region pools.
These are source-record counts, not 163 live people or 95 active regions.
Some definitions have no direct placements and some regions are conditional.

Of the 163 templates, 118 directly reference DefaultWanderEditorLocation,
18 DefaultSandboxEditorLocation and three NeonCitizen_DefaultSitLinkedRef.
Twenty-four have no direct package row and need their template/default lists
reviewed before classification. A missing package row does not mean idle.
None of the 163 source templates has unique/essential/protected ACBS flags or
attached VMAD. No explicit unique-actor quest-alias links were found for their
base IDs; dynamic aliases/scripts have not been exhaustively resolved.

An additional scan checked 218 related base, template, package and list IDs
against 24 active-listed or installed official-content files. None overrides
those Starfield.esm definitions. Official content presence alone does not imply
it is enabled; checking it is conservative. Other unrelated record overrides
and script-driven behavior are outside this narrow check.

## Consequence for the traffic design

Use these authored crowd pools to select generic background population and
adjust its generation budget when pilots are introduced. Keep named/quest NPC
and service schedules outside that change. A vehicle's motion follows its
traffic state; repeated NPC boarding is not a prerequisite.

This removes the need to visit every city for a census. It does not by itself
guarantee an exact one-for-one live headcount: density, region geometry, time
conditions, crowd settings, streaming and engine caps affect how many actors
exist. Exact population parity needs the engine's generation/count policy, not
an invented conversion from density to a fixed number of people.

The exploratory runtime census is no longer linked or called. No crowd density,
NPC record, quest, save or traffic plugin was modified by this audit. Offline
data products: notes/npc-source-audit-20261024/source-index.json,
crowd-regions.json, crowd-pool.csv and override-audit.json in the workspace.
