# Release layout

User preference: every release belongs in a folder named after its version.
The standard packager creates the complete set automatically:

```text
release/0.8/
  FlyingVehicleOverhaul-0.8.zip
  FlyingVehicleOverhaul-0.8-ExtraVehicles.zip
  FlyingVehicleOverhaul-0.8-ParkedVehiclesPatch.zip
  FlyingVehicleOverhaul-0.8-FOMOD.zip
  FlyingVehicleOverhaul-0.8-source.zip
  FlyingVehicleOverhaul-0.8-SHA256SUMS.txt
  SHA256SUMS.txt
  README.txt
  manifest.json
  release-bundle.json
```

The main ZIP contains only the core DLL, NPC pilot ESM and install guide, with
paths relative to Data. Each of the two optional content addons has its own
flat, independently installable ZIP and dependency guide. Gameplay feature
toggles do not get separate ZIPs. Users choose main plus optional content ZIPs,
or the alternative all-in-one FOMOD with grouped feature configuration.

`--output-dir` is the release ROOT. `--version` supplies its child folder name.
Two-component versions such as `0.8` and versions such as `0.8.1` are accepted.
No old version folder is renamed or cleaned. No release is published by packaging.
DLL startup version and requested package version must match; the package refuses
to relabel an older binary silently. Build the intended source first.

```powershell
python tools/package_release.py --output-dir release --version 0.8 `
  --data-dir inputs/Data --source-dir snapshot/plugin --sfse-dir snapshot/ref/sfse `
  --schema XmlScript5.0.xsd --test-results runtime-tests.json
python tools/verify_release_bundle.py release/0.8 --schema XmlScript5.0.xsd
```

Default format is `bundle`. `--format fomod` creates only the installer/source
pair inside the same version-folder convention. `--format legacy` explicitly
retains historical flat-archive behavior for archival workflows.

The verifier checks five archive hashes and CRCs, version identity, exact main/
optional contents, absence of developer files from player ZIPs, byte-identical
core/addon files across manual and FOMOD installation paths, and FOMOD validation.
The publication helper resolves a version folder and includes all five bundle
ZIPs plus checksums, rather than uploading only the old main/source pair.
