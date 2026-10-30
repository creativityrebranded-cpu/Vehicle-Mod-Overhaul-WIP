"""Create the standard version-folder release set from one verified FOMOD build."""
import hashlib
import json
from pathlib import Path
import re
import zipfile
from package_fomod import build, write_zip, DLL, PILOTS, PATCH, CITY, CITY_DLL, ROUTE_GUIDE, ROUTE_FILES, TRAFFIC_DIR, TRAFFIC_ADDON_VERSION


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
includes the same files, the optional Traffic Addon, City Placements and Parked Vehicles
conversion, and grouped feature configuration. It is an
alternative install method, not another required package.

The Traffic Manager (F7 on the local map) needs the optional Traffic Addon to generate
traffic: without it F7 still opens, but no traffic flies the routes.

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

City Placements: FVO_CityPlacements.esm adds parked vehicles in nine cities. Copy it into
Starfield/Data with the main package and enable it in your plugin list after its masters.
The traffic that flies over the cities is now its own optional download, the Traffic Addon.
'''.encode()
    traffic_guide=f'''Flying Vehicle Overhaul - Traffic Addon {TRAFFIC_ADDON_VERSION} (optional, works with FVO {version})

Generates air traffic along routes. Copy this archive's contents into Starfield/Data with
the main package. Needs no ESM; it uses whatever parked vehicles are there.

Press F7 on the local map to open the Traffic Manager and make your own routes.
Autopilot near a traffic zone follows the same path.

SFSE/Plugins/FVO_Traffic.dll and SFSE/Plugins/FVO_Traffic/:
   route files for New Atlantis (NewAtlantis.ini, the ring, with its cold-start spots in
   NewAtlantis-ColdStart.ini; NewAtlantis-3D.ini, the district loop), Akila City,
   Cydonia, Eleos Retreat, Gagarin, HopeTown, Neon, New Homestead, Paradiso and Red Mile,
   and HOW-TO-MAKE-ROUTES.txt.
   Every .ini in FVO_Traffic is a route file you can edit, add or delete (delete a
   city's file to leave it without route traffic).

These route files start with traffic already flying (start_on_route = N: vehicles with
seated drivers, made once per save). Set start_on_route = 0 in a file for traffic that
comes only from parked vehicles. Route beacons: "Show traffic routes" in the game's
settings menu. The FOMOD installer offers all of this as choices.

Remove the old FVO_CityTraffic.esm if present, and SFSE/Plugins/FVO_CityPlacements.dll
(FVO_Traffic.dll replaces it; never both).
'''.encode()
    patch_guide=f'''Flying Vehicle Overhaul {version} - Parked Vehicles conversion patch (optional)

FVO_ParkedVehiclesPatch.esm converts the Parked Vehicles mod's pack-in props into
drivable REV-8, Moon Jumper and Deimog vehicles. Copy it into Starfield/Data and
enable it after Parked Vehicles and its masters. Do not use it with the older
Vehicles - Real.esm patch.
'''.encode()
    with zipfile.ZipFile(fomod) as z:
        extra_entries={CITY:z.read('10 City Placements/'+CITY),
                       'Docs/FlyingVehicleOverhaul-ExtraVehicles.txt':extra_guide}
        traffic_entries={CITY_DLL:z.read('11 Route Traffic/'+CITY_DLL),
                         **{name:z.read('11 Route Traffic/Flying/'+name[len(TRAFFIC_DIR):]) for name in ROUTE_FILES},
                         ROUTE_GUIDE:z.read('11 Route Traffic/'+ROUTE_GUIDE),
                         'Docs/FlyingVehicleOverhaul-TrafficAddon.txt':traffic_guide}
        patch_entries={PATCH:z.read('20 Parked Vehicles Patch/'+PATCH),
                       'Docs/FlyingVehicleOverhaul-ParkedVehiclesPatch.txt':patch_guide}
    extra=output/(prefix+'-ExtraVehicles.zip')
    write_zip(extra,extra_entries)
    traffic=output/f'FlyingVehicleOverhaul-TrafficAddon-{TRAFFIC_ADDON_VERSION}.zip'
    write_zip(traffic,traffic_entries)
    patch=output/(prefix+'-ParkedVehiclesPatch.zip')
    write_zip(patch,patch_entries)
    source=output/(prefix+'-source.zip')
    paths={'main':main,'traffic':traffic,'extra':extra,'patch':patch,'fomod':fomod,'source':source}
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
   {traffic.name} (Traffic Addon {TRAFFIC_ADDON_VERSION}: traffic DLL + route files for eleven routes),
   {extra.name} (City Placements ESM) and {patch.name} (needs Parked Vehicles).

2. Installer: {fomod.name}
   Use Vortex/MO2 for grouped feature settings, the Traffic Addon route by route, City Placements and the patch.
   This replaces the separate ZIPs.

Source for developers: {source.name}
Do not install the source archive into the game.

All six ZIPs belong to version {version}. Hashes are in SHA256SUMS.txt;
artifact identities are in release-bundle.json. Automated installer/build
validation does not establish live gameplay performance.
''')
    print(f'Release bundle: {output}')
    for role,file in paths.items():print(f'  {role}: {file.name} ({file.stat().st_size:,} bytes)')
    return output
