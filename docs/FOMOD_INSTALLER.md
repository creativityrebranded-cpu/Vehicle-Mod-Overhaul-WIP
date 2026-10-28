# FOMOD packaging

The default `tools/package_release.py` output is a version-folder release bundle:
main, two separate optional content ZIPs, the all-in-one FOMOD, source and hashes.
See [release layout](RELEASE_PACKAGING.md). The FOMOD is an alternative installation
method, rather than an addon to the main ZIP.

Feature setup now offers four separate pages, with an Enabled/Disabled choice
for each item inside its group:

- Flight and Navigation: hover-or-drive, free roll, flight assist, player Physical
  VTOL, AI Physical VTOL, experimental ground planner/native driving.
- NPCs and Combat: NPC vehicle use, combat AI, vehicle damage, crash witnesses.
- HUD: vehicle health bars and status card.
- Parked Vehicles: placement support.

Placement recording mode and Detailed log are no longer installer choices.
The configured profile supplies their off values internally so the existing
runtime's complete-profile validation still succeeds. Neither has an enabled
payload or a selectable installer control. Keep-existing mode still preserves
saved preferences. Parked-vehicle support and the HUD editor remain available.

The first page also offers **Keep existing gameplay settings**, which hides all
feature pages. The current runtime defaults are preselected when configuring;
experimental physics and ground planning remain off unless explicitly enabled.

The installer deploys one mode file and selected feature fragments under
`Data/SFSE/Plugins/FlyingVehicleOverhaul.features/`. The updated DLL imports
choices into its existing Documents `settings.ini` on the next game launch.
Only changed installer choices are imported after the first application. Later
in-game menu edits therefore survive unchanged installer profiles. Reinstalling
identical choices preserves those later edits; change the installer choice to
apply that feature again. The first pre-import settings file is backed up as
`settings.ini.before-fomod`; the last imported choices are tracked in
`settings.ini.fomod-applied`. HUD, bindings and unrelated INI keys are preserved.
Without a profile, the DLL retains its previous behavior. An incomplete or
invalid profile is rejected before changing settings.

Packaging requires a source and DLL with the feature-import bridge; it refuses
an older DLL that would silently ignore the new installer choices.

- Required core: `FlyingVehicleOverhaul.dll`, `FVO_NPCPilots.esm`, install guide.
- Optional, unchecked: New Atlantis parked vehicles (`FVO_CityPlacements.esm`).
- Optional, unchecked: Parked Vehicles conversion (`FVO_ParkedVehiclesPatch.esm`).

The optional components are independent. Each becomes selectable only when
its non-base masters listed inside the ESM are active. `Starfield.esm` is implicit
in the Starfield installation context, so it is excluded from both core and addon
plugin-enumeration gates. Some manager callbacks omit that always-loaded master;
requiring it falsely blocked the whole installation. The conversion patch is
also disabled while the old `Vehicles - Real.esm` is installed, to prevent
stacking the old and renamed conversion patches. It lists its exact required
plugins in the option description. Install/enable missing masters, then rerun
the FOMOD to select the addon.

The latest local package deliberately omits legacy City Traffic, scheduled
traffic, the GHN road pool and external mods. A core-only choice adds no new
parked vehicle population. This packaging change does not establish or fix the
cause of reported FPS drops.

Replace the old FVO mod entry when upgrading, so unchecked content from its old
archive is removed. A FOMOD cannot remove files supplied by another mod entry
or a manual installation. See the packaged guide for migration and manual
installation instructions.

Example using an explicitly built source snapshot:

```powershell
python tools/package_release.py --version 0.8 `
  --output-dir out --data-dir inputs/Data --source-dir snapshot/plugin `
  --sfse-dir snapshot/ref/sfse --schema XmlScript5.0.xsd `
  --test-results runtime-tests.json
python tools/verify_fomod.py out/0.8/FlyingVehicleOverhaul-0.8-FOMOD.zip `
  --schema XmlScript5.0.xsd --report installer-tests.json
```

`--format legacy` retains the old flat layout explicitly for archival workflows.
The FOMOD builder uses only explicit core/optional content files, checks source
build inputs, verifies ZIP contents, and writes hashes plus a separate source
archive. It does not publish a release or change the game installation.

The validation checks the official Nexus FOMOD 5.0 schema and all 8,748
dependency-state/checkbox combinations. The upstream schema contains whitespace
before one QName (`type=" xs:string"`); the validator trims type-attribute
whitespace to preserve the intended schema while allowing libxml2 to read it.
These are automated checks; the native Vortex/MO2 installation UI still needs a
manual smoke test.

XML compatibility fix: both XML documents omit their optional encoding declaration.
Vortex native installer 0.13.4 rewrites script strings as UTF-16 but only strips
double-quoted encoding attributes. ElementTree's single-quoted UTF-8 declaration
therefore caused Xml_InvalidCharacter at line 1, position 39. The old ZIP was
reproduced against the installed Vortex engine; the fixed ZIP passes its native
validation, default-choice state and copy-instruction checks across eight addon/
dependency scenarios. This exercises the real engine through callbacks, without
deploying anything into the game; visual UI testing remains separate.

References: [Nexus installer documentation](https://github.com/Nexus-Mods/Vortex/wiki/MODDINGWIKI-Users-General-How-to-create-mod-installers),
[official schema](https://github.com/Nexus-Mods/fomod-installer/blob/master/src/InstallScripting/XmlScript/Schemas/XmlScript5.0.xsd).
