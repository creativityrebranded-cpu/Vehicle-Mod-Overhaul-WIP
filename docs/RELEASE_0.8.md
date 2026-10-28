# Flying Vehicle Overhaul 0.8

This release packages the current build with the author's saved HUD layout as
the default. Existing saved layouts still override it. Physical VTOL for the
player and AI remains off by default.

- Added grouped FOMOD feature setup: Flight and Navigation, NPCs and Combat, HUD,
  and Parked Vehicles, with individual on/off choices. Keep existing settings
  skips feature setup. Placement recording and detailed logging are not installer
  choices. Selected settings import on next launch; unchanged installer profiles
  retain later in-game adjustments, HUD and bindings.
- Corrected FOMOD XML encoding and the false Starfield.esm prerequisite check.
- Added status-card/route child editing alongside panel movement and sizing,
  fixed the fourth escort row and duplicate information-panel translation.
- Added experimental ground planning/native driving, off by default, and extended
  focus fire to hostile-driven vehicles. Removed the previously available player
  ram orders and deliberate AI contact attacks; impact damage remains.
- Standardized release folders by version: main, two independent optional content
  ZIPs, all-in-one FOMOD, source and checksums. Main/manual and FOMOD paths use
  identical DLL/ESM payloads.

Build/runtime suites, installer engine and settings-import tests pass. Jemison
CPU/FPS reports remain unresolved; this is not a confirmed live performance fix.
Further gameplay and visual installer testing remain separate from those checks.
