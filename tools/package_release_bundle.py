"""Create the standard version-folder release set from one verified FOMOD build."""
import hashlib
import json
from pathlib import Path
import re
import zipfile
from package_fomod import build, write_zip, DLL, PILOTS, PATCH, CITY, CITY_DLL, CITY_TRAFFIC, ROUTE_GUIDE


def version_folder(release_root,version):
    if not re.fullmatch(r'\d+(?:\.\d+){1,3}(?:[-.][A-Za-z0-9][A-Za-z0-9._-]*)?',version):
        raise ValueError('Expected a version such as 0.8, 0.8.1 or 0.8.1-beta.1')
    release_root=Path(release_root).resolve()
    output=release_root/version
    if output.parent!=release_root:
        raise ValueError('Version folder must stay under the release root')
    return output


def build_release(data_dir,source_dir,sfse_dir,release_root,version,schema=None,test_results=None):
    output=version_folder(release_root,version)
    fomod=build(data_dir,source_dir,sfse_dir,output,version,schema,test_results)
    manifest=json.loads((output/'manifest.json').read_text())
    prefix=f'FlyingVehicleOverhaul-{version}'
    main_guide=f'''Flying Vehicle Overhaul {version} - main package

Manual installation: copy this archive's contents into Starfield/Data.
Mod manager: install this ZIP as a normal mod, without a FOMOD wizard.
Enable FVO_NPCPilots.esm. Requires Steam Starfield 1.16.244 and its matching SFSE.
Start through sfse_loader.exe and restart after replacing the DLL.

This main ZIP contains the core DLL and NPC pilot packages. It adds no
parked vehicles of its own.

Use either this main package OR the all-in-one {prefix}-FOMOD.zip. The FOMOD
includes the same files, optional City Placements and Parked Vehicles conversion, and grouped
feature configuration. It is an
alternative install method, not another required package.

Without a FOMOD feature profile, the main package uses existing settings or
the built-in defaults. Change features in Gameplay Options or settings.ini:
Documents/My Games/Starfield/SFSE/Logs/FlyingVehicleOverhaul/settings.ini
Player/AI Physical VTOL and experimental ground planning default off.
The default HUD uses the author's current captured layout. Saved HUD layouts
and bindings continue to override those defaults.

Replace your previous FVO mod entry rather than leaving duplicate DLLs.
When switching from FOMOD to this main ZIP, remove the previous FOMOD mod entry
so its deployed feature profile is not left behind. Your saved settings remain.
Do not install source ZIPs into the game.
'''.encode()
    with zipfile.ZipFile(fomod) as z:
        main_entries={DLL:z.read('00 Core/'+DLL),PILOTS:z.read('00 Core/'+PILOTS),
                      'Docs/FlyingVehicleOverhaul-INSTALL.txt':main_guide}
    main=output/(prefix+'.zip')
    write_zip(main,main_entries)
    # 0.8.5: the optional addons as their own ZIPs too (same names as the 0.8 page), built from the same FOMOD payload.
    extra_guide=f'''Flying Vehicle Overhaul {version} - Extra Vehicles addon (optional)

Two separate parts; install either or both into Starfield/Data with the main package:

1. New Atlantis placements: FVO_CityPlacements.esm adds parked vehicles around New
   Atlantis. Enable it in your plugin list after its masters.
2. City traffic: SFSE/Plugins/FVO_CityPlacements.dll and SFSE/Plugins/FVO_Traffic/
   (NewAtlantis.ini, a ring round New Atlantis, and HOW-TO-MAKE-ROUTES.txt). Needs no
   ESM; it uses whatever parked vehicles are there. Every .ini in FVO_Traffic is a
   route file you can edit or add.

Leave out the part you do not want: delete the ESM (or keep it disabled) for traffic
only, or delete the DLL and the FVO_Traffic folder for placements only.
Remove the old FVO_CityTraffic.esm if present.
'''.encode()
    patch_guide=f'''Flying Vehicle Overhaul {version} - Parked Vehicles conversion patch (optional)

FVO_ParkedVehiclesPatch.esm converts the Parked Vehicles mod's pack-in props into
drivable REV-8, Moon Jumper and Deimog vehicles. Copy it into Starfield/Data and
enable it after Parked Vehicles and its masters. Do not use it with the older
Vehicles - Real.esm patch.
'''.encode()
    with zipfile.ZipFile(fomod) as z:
        extra_entries={CITY:z.read('10 City Placements/'+CITY),
                       CITY_DLL:z.read('11 Route Traffic/'+CITY_DLL),
                       CITY_TRAFFIC:z.read('11 Route Traffic/'+CITY_TRAFFIC),
                       ROUTE_GUIDE:z.read('11 Route Traffic/'+ROUTE_GUIDE),
                       'Docs/FlyingVehicleOverhaul-ExtraVehicles.txt':extra_guide}
        patch_entries={PATCH:z.read('20 Parked Vehicles Patch/'+PATCH),
                       'Docs/FlyingVehicleOverhaul-ParkedVehiclesPatch.txt':patch_guide}
    extra=output/(prefix+'-ExtraVehicles.zip')
    write_zip(extra,extra_entries)
    patch=output/(prefix+'-ParkedVehiclesPatch.zip')
    write_zip(patch,patch_entries)
    source=output/(prefix+'-source.zip')
    paths={'main':main,'extra':extra,'patch':patch,'fomod':fomod,'source':source}
    bundle={'version':version,'dll_sha256':manifest['dll_sha256'],'artifacts':{
        role:{'file':file.name,'sha256':hashlib.sha256(file.read_bytes()).hexdigest(),'size':file.stat().st_size}
        for role,file in paths.items()}}
    (output/'release-bundle.json').write_text(json.dumps(bundle,indent=2)+'\n')
    sums=''.join(f"{item['sha256']}  {item['file']}\n" for item in bundle['artifacts'].values())
    (output/(prefix+'-SHA256SUMS.txt')).write_text(sums,encoding='ascii')
    (output/'SHA256SUMS.txt').write_text(sums,encoding='ascii')
    (output/'README.txt').write_text(f'''Flying Vehicle Overhaul {version}

Choose one installation method:

1. Separate ZIPs: {main.name} (required), plus optional
   {extra.name} (New Atlantis placements ESM and/or city traffic DLL + route files;
   use either or both) and {patch.name} (needs Parked Vehicles).

2. Installer: {fomod.name}
   Use Vortex/MO2 for grouped feature settings, the two Extra Vehicles toggles and the patch.
   This replaces the separate ZIPs.

Source for developers: {source.name}
Do not install the source archive into the game.

All five ZIPs belong to version {version}. Hashes are in SHA256SUMS.txt;
artifact identities are in release-bundle.json. Automated installer/build
validation does not establish live gameplay performance.
''')
    print(f'Release bundle: {output}')
    for role,file in paths.items():print(f'  {role}: {file.name} ({file.stat().st_size:,} bytes)')
    return output
