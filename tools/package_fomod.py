"""Build core with optional City Placements and Parked Vehicles conversion; no randomizer."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import xml.etree.ElementTree as ET
import zipfile

DLL = 'SFSE/Plugins/FlyingVehicleOverhaul.dll'
PILOTS = 'FVO_NPCPilots.esm'
PATCH = 'FVO_ParkedVehiclesPatch.esm'
CITY = 'FVO_CityPlacements.esm'
CITY_DLL = 'SFSE/Plugins/FVO_CityPlacements.dll'
CITY_TRAFFIC = 'SFSE/Plugins/FVO_Traffic/NewAtlantis.ini'
ROUTE_GUIDE = 'SFSE/Plugins/FVO_Traffic/HOW-TO-MAKE-ROUTES.txt'
# Superseded experimental city content must not overlap current placements.
RETIRED = ('FVO_CityTraffic.esm',)
# The base game is implicit in the Starfield install context. Managers need not
# include its always-loaded master in their reported optional-plugin lists.
IMPLICIT_MASTERS = {'starfield.esm'}
SCHEMA_URL = 'https://raw.githubusercontent.com/Nexus-Mods/fomod-installer/master/src/InstallScripting/XmlScript/Schemas/XmlScript5.0.xsd'
PROFILE = 'SFSE/Plugins/FlyingVehicleOverhaul.features/'
FEATURE_PAGES = ('Flight and Navigation','NPCs and Combat','HUD')  # the 0.8 pages: an update keeps the same installer layout
FIXED_FEATURES = {'Placement':False,'PlacementMode':False,'DebugLog':False}
FEATURE_GROUPS = {
    'HoverOrDrive':FEATURE_PAGES[0], 'FreeRoll':FEATURE_PAGES[0], 'FlightAssist':FEATURE_PAGES[0],
    'PhysicalVtol':FEATURE_PAGES[0], 'PhysicalAi':FEATURE_PAGES[0], 'NativeGround':FEATURE_PAGES[0],
    'NpcVehicles':FEATURE_PAGES[1], 'CombatAi':FEATURE_PAGES[1], 'VehicleDamage':FEATURE_PAGES[1], 'Witnesses':FEATURE_PAGES[1], 'Traffic':FEATURE_PAGES[1], 'RouteMarkers':FEATURE_PAGES[1],
    'HealthBars':FEATURE_PAGES[2], 'StatusCard':FEATURE_PAGES[2],
    'Placement':None, 'PlacementMode':None, 'DebugLog':None,
}


def feature_definitions(source_dir):
    source = (source_dir/'src/features.cpp').read_text(encoding='utf-8')
    if 'featureinstaller::Apply' not in source or not (source_dir/'src/featureinstaller.h').is_file():
        raise ValueError('This source does not support FOMOD feature import; rebuild the current plugin first')
    pattern = r'\{\s*Id::(\w+),\s*"([^"\n]*)",\s*"([^"\n]*)",\s*L"([^"\n]*)",\s*L"([^"\n]*)",\s*(true|false)\s*\}'
    items = []
    for name,label,description,section,key,default in re.findall(pattern,source):
        items.append({'id':name,'label':label,'description':description,'section':section,'key':key,'default':default=='true',
                      'page':FEATURE_GROUPS[name],'configurable':name not in FIXED_FEATURES})
    if len(items)!=17 or len({(f['section'],f['key']) for f in items})!=len(items):
        raise ValueError('Feature list changed or could not be parsed; review installer coverage')
    return items


def feature_payload(features):
    entries={'30 Feature Setup/Apply.ini':b'[installer]\nenabled=1\n',
             '30 Feature Setup/Keep.ini':b'[installer]\nenabled=0\n'}
    for feature in features:
        for enabled in ((False,True) if feature['configurable'] else (FIXED_FEATURES[feature['id']],)):
            state='On' if enabled else 'Off'
            entries[f"40 Features/{feature['id']}/{state}.ini"]=(
                f"[feature]\nsection={feature['section']}\nkey={feature['key']}\nvalue={int(enabled)}\n").encode('ascii')
    return entries


def masters(data):
    """Read actual TES4 masters; reject truncated records instead of guessing."""
    if data[:4] != b'TES4' or len(data) < 24:
        raise ValueError('Expected a TES4 plugin header')
    end = 24 + struct.unpack_from('<I', data, 4)[0]
    if end > len(data):
        raise ValueError('Truncated TES4 header')
    result, at = [], 24
    while at < end:
        if at + 6 > end:
            raise ValueError('Truncated TES4 subrecord')
        tag, length = data[at:at+4], struct.unpack_from('<H', data, at+4)[0]
        at += 6
        if at + length > end:
            raise ValueError('Truncated TES4 subrecord data')
        if tag == b'MAST':
            result.append(data[at:at+length].rstrip(b'\0').decode('utf-8'))
        at += length
    return result


def xml_bytes(root):
    ET.indent(root, space='  ')
    # Vortex's native installer rewrites the string as UTF-16 and strips only
    # double-quoted encoding declarations. ElementTree emits single quotes,
    # leaving a conflicting UTF-8 declaration (Xml_InvalidCharacter at 1:39).
    # XML declarations are optional; omit them for both supported managers.
    return ET.tostring(root, encoding='utf-8', xml_declaration=False)


def add_dependencies(parent, required):
    deps = ET.SubElement(parent, 'dependencies', operator='And')
    for name in required:
        if name.casefold() in IMPLICIT_MASTERS:
            continue
        ET.SubElement(deps, 'fileDependency', file=name, state='Active')


def option(parent, name, description, source, destination, required, conflicts=(), companions=()):
    plugin = ET.SubElement(parent, 'plugin', name=name)
    ET.SubElement(plugin, 'description').text = description
    files = ET.SubElement(plugin, 'files')
    ET.SubElement(files, 'file', source=source, destination=destination, priority='0')
    for extra_source, extra_destination in companions:
        ET.SubElement(files, 'file', source=extra_source, destination=extra_destination, priority='0')
    typed = ET.SubElement(ET.SubElement(plugin, 'typeDescriptor'), 'dependencyType')
    ET.SubElement(typed, 'defaultType', name='NotUsable')
    patterns = ET.SubElement(typed, 'patterns')
    for conflict in conflicts:
        pattern = ET.SubElement(patterns, 'pattern')
        deps = ET.SubElement(pattern, 'dependencies', operator='Or')
        for state in ('Active', 'Inactive'):
            ET.SubElement(deps, 'fileDependency', file=conflict, state=state)
        ET.SubElement(pattern, 'type', name='NotUsable')
    pattern = ET.SubElement(patterns, 'pattern')
    add_dependencies(pattern, required)
    ET.SubElement(pattern, 'type', name='Optional')


def module_config(dependencies, features):
    ET.register_namespace('xsi', 'http://www.w3.org/2001/XMLSchema-instance')
    root = ET.Element('config', {'{http://www.w3.org/2001/XMLSchema-instance}noNamespaceSchemaLocation':
                                'http://qconsulting.ca/fo3/ModConfig5.0.xsd'})
    ET.SubElement(root, 'moduleName').text = 'Flying Vehicle Overhaul'
    files = ET.SubElement(root, 'requiredInstallFiles')
    for name in (DLL, PILOTS, 'Docs/FlyingVehicleOverhaul-INSTALL.txt'):
        ET.SubElement(files, 'file', source='00 Core/'+name, destination=name, priority='0')
    steps = ET.SubElement(root, 'installSteps', order='Explicit')
    step = ET.SubElement(steps, 'installStep', name='Core and feature setup')
    groups = ET.SubElement(step, 'optionalFileGroups', order='Explicit')
    core = ET.SubElement(ET.SubElement(ET.SubElement(groups, 'group', name='Core (always installed)', type='SelectAll'),
                                     'plugins', order='Explicit'), 'plugin', name='Flying Vehicle Overhaul core')
    ET.SubElement(core, 'description').text = (
        'Required: hover controls, autopilot, NPC vehicle support and HUD. Requires Steam Starfield 1.16.244 and its SFSE build. '
        'It adds no parked vehicles of its own. '
        'Choose your feature settings on the following grouped pages, or keep your current settings. '
        'When upgrading, replace your old FVO installation instead of enabling two copies.')
    ET.SubElement(ET.SubElement(core, 'typeDescriptor'), 'type', name='Required')
    # 0.8.5: the 0.8 group and option names, so a manager update keeps a user's earlier choices.
    addons = ET.SubElement(ET.SubElement(groups, 'group', name='Optional addons (unchecked by default)', type='SelectAny'),
                           'plugins', order='Explicit')
    # 0.8.5: route traffic (the addon DLL + traffic files) works with any parked vehicles, so it is its own choice.
    traffic = ET.SubElement(addons, 'plugin', name='New Atlantis City Traffic')
    ET.SubElement(traffic, 'description').text = (
        'Replaces the 0.8 FVO_CityTraffic.esm (remove it if another mod entry still supplies it). '
        'Traffic that flies routes defined in plain text files: SFSE/Plugins/FVO_Traffic/NewAtlantis.ini (a two-lane ring round '
        'New Atlantis with four exits and landing places) and HOW-TO-MAKE-ROUTES.txt. Uses any parked vehicles (vanilla, Carfield, '
        'Parked Vehicles or the New Atlantis parked vehicles below). Needs only the core.')
    files = ET.SubElement(traffic, 'files')
    for name in (CITY_DLL, CITY_TRAFFIC, ROUTE_GUIDE):
        ET.SubElement(files, 'file', source='11 Route Traffic/'+name, destination=name, priority='0')
    ET.SubElement(ET.SubElement(traffic, 'typeDescriptor'), 'type', name='Optional')
    option(addons, 'New Atlantis City Placements',
           'Adds parked vehicles around New Atlantis. Vehicle types stay as authored. Remove the superseded FVO_CityTraffic.esm if installed. '
           'Requires active: '+', '.join(dependencies[CITY])+'.',
           '10 City Placements/'+CITY, CITY, dependencies[CITY],
           ('FVO_CityTraffic.esm', 'FlyingVehicleOverhaul - Placements.esm'))
    option(addons, 'Parked Vehicles conversion patch',
           'Converts Parked Vehicles pack-in props into usable REV-8, Moon Jumper and Deimog vehicles, '
           'including its procedural placements. Removes their duplicate decorative wheels and turrets. '
           'Locations come from Parked Vehicles; this adds no FVO city placements. '
           'Requires active: '+', '.join(dependencies[PATCH])+'. '
           'Remove the old Vehicles - Real.esm patch before selecting this replacement.',
           '20 Parked Vehicles Patch/'+PATCH, PATCH, dependencies[PATCH], ('Vehicles - Real.esm',))
    setup = ET.SubElement(ET.SubElement(groups,'group',name='Feature configuration',type='SelectExactlyOne'),'plugins',order='Explicit')
    for apply in (True,False):
        plugin=ET.SubElement(setup,'plugin',name='Choose features in this installer' if apply else 'Keep existing gameplay settings')
        ET.SubElement(plugin,'description').text=(
            'Choose feature switches on the following pages. They apply on the next game launch. Your HUD, bindings and unrelated settings stay intact. '
            'Later gameplay-menu changes remain saved until you change that feature in the installer.' if apply else
            'Keep your saved gameplay settings. A fresh installation uses the standard feature defaults.')
        files=ET.SubElement(plugin,'files')
        ET.SubElement(files,'file',source='30 Feature Setup/'+('Apply.ini' if apply else 'Keep.ini'),destination=PROFILE+'mode.ini',priority='0')
        if apply:
            for feature in features:
                if feature['configurable']:continue
                ET.SubElement(files,'file',source=f"40 Features/{feature['id']}/Off.ini",
                              destination=PROFILE+feature['section']+'.'+feature['key']+'.ini',priority='0')
        ET.SubElement(ET.SubElement(plugin,'conditionFlags'),'flag',name='configure_features').text='Yes' if apply else 'No'
        ET.SubElement(ET.SubElement(plugin,'typeDescriptor'),'type',name='Recommended' if apply else 'Optional')
    for page in FEATURE_PAGES:
        step=ET.SubElement(steps,'installStep',name=page)
        ET.SubElement(ET.SubElement(step,'visible',operator='And'),'flagDependency',flag='configure_features',value='Yes')
        page_groups=ET.SubElement(step,'optionalFileGroups',order='Explicit')
        for feature in features:
            if not feature['configurable'] or feature['page']!=page:continue
            options=ET.SubElement(ET.SubElement(page_groups,'group',name=feature['label'],type='SelectExactlyOne'),'plugins',order='Explicit')
            for enabled in (True,False):
                state='On' if enabled else 'Off'
                plugin=ET.SubElement(options,'plugin',name='Enabled' if enabled else 'Disabled')
                ET.SubElement(plugin,'description').text=feature['description']+' '+('Enabled.' if enabled else 'Disabled.')
                files=ET.SubElement(plugin,'files')
                ET.SubElement(files,'file',source=f"40 Features/{feature['id']}/{state}.ini",
                              destination=PROFILE+feature['section']+'.'+feature['key']+'.ini',priority='0')
                ET.SubElement(ET.SubElement(plugin,'typeDescriptor'),'type',name='Recommended' if enabled==feature['default'] else 'Optional')
    return xml_bytes(root)


def write_zip(path, entries):
    temp = path.with_suffix('.zip.building')
    with zipfile.ZipFile(temp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(entries.items()):
            if PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts or '\\' in name:
                raise ValueError('Unsafe archive path: '+name)
            archive.writestr(name, data)
    with zipfile.ZipFile(temp) as archive:
        if archive.testzip() is not None:
            raise ValueError('ZIP CRC validation failed')
        for name, data in entries.items():
            if archive.read(name) != data:
                raise ValueError('Packaged data mismatch: '+name)
    temp.replace(path)


def build(data_dir, source_dir, sfse_dir, output, version, schema=None, test_results=None):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', version):
        raise ValueError('Invalid version label')
    if not (sfse_dir/'sfse/PluginAPI.h').is_file():
        raise FileNotFoundError('SFSE headers missing: '+str(sfse_dir))
    inputs = {name: (data_dir/name).read_bytes() for name in (DLL, PILOTS, PATCH, CITY, CITY_DLL, CITY_TRAFFIC, ROUTE_GUIDE)}
    if inputs[DLL][:2] != b'MZ':
        raise ValueError('Expected a Windows DLL')
    if b'Installer features:' not in inputs[DLL]:
        raise ValueError('DLL lacks the feature-import bridge; rebuild it before packaging feature choices')
    requested=re.match(r'\d+(?:\.\d+){1,3}',version)
    binary_version=re.search(rb'Flying Vehicle Overhaul \(FVO\) ([0-9.]+) loaded',inputs[DLL])
    normalize=lambda value:tuple((list(map(int,value.split('.')))+[0]*4)[:4])
    if not requested or not binary_version or normalize(requested.group())!=normalize(binary_version.group(1).decode()):
        raise ValueError('Package version does not match the built DLL startup version; rebuild the correct source')
    if inputs[CITY_DLL][:2] != b'MZ':
        raise ValueError('Expected the matching City Placements addon DLL')
    features = feature_definitions(source_dir)
    dependencies = {name: masters(inputs[name]) for name in (PILOTS, PATCH, CITY)}
    if dependencies[PILOTS] != ['Starfield.esm']:
        raise ValueError('Core pilot plugin acquired new dependencies; review installer requirements')
    config = module_config(dependencies, features)
    if schema is not None:
        from lxml import etree
        schema_tree = etree.parse(str(schema))
        # The upstream schema contains type=" xs:string". Trim QName whitespace
        # for libxml2, preserving the schema's intended type and constraints.
        for item in schema_tree.iter():
            if 'type' in item.attrib:
                item.set('type', item.get('type').strip())
        etree.XMLSchema(schema_tree).assertValid(etree.fromstring(config))
    readme = f'''Flying Vehicle Overhaul - {version}

Use Vortex or Mod Organizer 2 to install the FOMOD ZIP and follow its installer.
Core always installs: FlyingVehicleOverhaul.dll, FVO_NPCPilots.esm and this guide.
Extra Vehicles addon, two independent choices (both unchecked):
- New Atlantis placements: FVO_CityPlacements.esm adds parked vehicles around New
  Atlantis. Requires: {', '.join(dependencies[CITY])}. Enable it after its masters.
- City traffic: SFSE/Plugins/FVO_CityPlacements.dll and the traffic files in
  SFSE/Plugins/FVO_Traffic (NewAtlantis.ini: a ring round New Atlantis;
  HOW-TO-MAKE-ROUTES.txt: how to write your own). Needs only the core and uses
  whatever parked vehicles are there. Every *.ini in FVO_Traffic is read.
Remove the old FVO_CityTraffic.esm and FlyingVehicleOverhaul - Placements.esm if present.
Optional, unchecked: FVO_ParkedVehiclesPatch.esm converts Parked Vehicles'
pack-in props into usable REV-8, Moon Jumper and Deimog vehicles, including its
procedural placements. Duplicate decorative wheels and turrets are disabled.
Conversion locations come from Vehicles.esm; the city addon is independent.
Patch masters: {', '.join(dependencies[PATCH])}.
Install and activate these masters first. Load the patch after all of them.
Remove the obsolete Vehicles - Real.esm patch before selecting this replacement.

Runtime vehicle randomization has been removed. Carfield, Parked Vehicles and
City Placements keep their authored vehicle types. Old placement.enabled settings
cannot reactivate randomization. The conversion patch works independently.
More live vehicles can still increase CPU/physics load.

Feature setup: choose on/off switches during installation, or keep your saved
gameplay settings. Selections apply on the next game launch. Only changed
installer choices are imported after that, so later menu adjustments survive
unchanged installer profiles. Reinstall and change a feature's choice to apply
that switch again. Reinstalling identical choices retains later menu changes.
Your original settings are backed up as settings.ini.before-fomod. HUD layout,
bindings and unrelated INI settings are preserved. Feature files are installed
under SFSE/Plugins/FlyingVehicleOverhaul.features; restart after changing them.

Requires Steam Starfield 1.16.244 and the corresponding SFSE build.
Enable FVO_NPCPilots.esm. Start the game through SFSE.

Upgrading: replace the previous FVO mod entry; do not keep two core DLLs.
Older FVO_CityTraffic.esm and
Vehicles - Real.esm are not included. Remove superseded FVO files from the mod
entry that supplied them. This installer cannot remove files supplied by a
different mod or manual install. Reinstall this package to change its choices.
Removing old plugins can cause missing-plugin warnings in saves that used them.

Manual install: copy only the contents of '00 Core' into Starfield/Data.
For Extra Vehicles city traffic, also copy '11 Route Traffic' contents into Data.
For Extra Vehicles placements, also copy '10 City Placements' contents into Data
and enable FVO_CityPlacements.esm after its masters.
For the optional patch, also copy '20 Parked Vehicles Patch' contents into Data
and enable FVO_ParkedVehiclesPatch.esm after its masters.
Do not copy the entire FOMOD archive into Data.

Live performance and gameplay verification remain separate from installer/build
validation.
'''.encode()
    info = ET.Element('fomod')
    ET.SubElement(info, 'Name').text = 'Flying Vehicle Overhaul'
    ET.SubElement(info, 'Author').text = 'creativityrebranded-cpu'
    numeric = re.match(r'\d+(?:\.\d+){1,3}', version)
    version_attrs = {'MachineVersion': numeric.group(0)} if numeric else {}
    ET.SubElement(info, 'Version', version_attrs).text = version
    ET.SubElement(info, 'Description').text = 'Core, grouped gameplay feature choices and an optional Parked Vehicles conversion patch.'
    entries = {'fomod/ModuleConfig.xml': config, 'fomod/info.xml': xml_bytes(info),
               'fomod/features.json': (json.dumps(features,indent=2)+'\n').encode(),
               'README.txt': readme, '00 Core/Docs/FlyingVehicleOverhaul-INSTALL.txt': readme,
               '00 Core/'+DLL: inputs[DLL], '00 Core/'+PILOTS: inputs[PILOTS],
               '20 Parked Vehicles Patch/'+PATCH: inputs[PATCH],
               '10 City Placements/'+CITY: inputs[CITY],
               '11 Route Traffic/'+CITY_DLL: inputs[CITY_DLL],
               '11 Route Traffic/'+CITY_TRAFFIC: inputs[CITY_TRAFFIC],
               '11 Route Traffic/'+ROUTE_GUIDE: inputs[ROUTE_GUIDE]}
    entries.update(feature_payload(features))
    output.mkdir(parents=True, exist_ok=True)
    archive = output/f'FlyingVehicleOverhaul-{version}-FOMOD.zip'
    write_zip(archive, entries)
    sources = {}
    for folder in ('src', 'tests'):
        for path in (source_dir/folder).rglob('*'):
            if path.is_file() and path.suffix in ('.cpp', '.h', '.hpp', '.esm', '.md'):
                sources['plugin/'+path.relative_to(source_dir).as_posix()] = path.read_bytes()
    cmake = (source_dir/'CMakeLists.txt').read_bytes()
    sources['plugin/CMakeLists.txt'] = cmake
    for path in sfse_dir.rglob('*'):
        if '.git' not in path.parts and path.is_file() and (path.suffix in ('.h', '.hpp', '.inl', '.inc') or path.name.startswith('LICENSE')):
            sources['ref/sfse/'+path.relative_to(sfse_dir).as_posix()] = path.read_bytes()
    for name in re.findall(r'\b(?:src|tests)/[\w/.-]+\.(?:cpp|h)', cmake.decode()):
        if 'plugin/'+name not in sources:
            raise ValueError('Missing source build input: '+name)
    sources['tools/package_fomod.py'] = Path(__file__).read_bytes()
    verifier = Path(__file__).with_name('verify_fomod.py')
    if verifier.is_file():
        sources['tools/verify_fomod.py'] = verifier.read_bytes()
    native_verifier = Path(__file__).with_name('verify_fomod_native.cjs')
    if native_verifier.is_file():
        sources['tools/verify_fomod_native.cjs'] = native_verifier.read_bytes()
    for name in ('package_release.py','package_release_bundle.py','verify_release_bundle.py'):
        helper=Path(__file__).with_name(name)
        if helper.is_file():sources['tools/'+name]=helper.read_bytes()
    guide = Path(__file__).resolve().parents[1]/'docs/FOMOD_INSTALLER.md'
    if guide.is_file():
        sources['Docs/FOMOD_INSTALLER.md'] = guide.read_bytes()
    for name in ('RELEASE_PACKAGING.md',f'RELEASE_{version}.md'):
        document=Path(__file__).resolve().parents[1]/'docs'/name
        if document.is_file():sources['Docs/'+name]=document.read_bytes()
    sources['README.txt'] = (readme.decode()+'\nBuild: cmake -S plugin -B build -G "Visual Studio 17 2022" -A x64\n'
                            'cmake --build build --config Release --target FlyingVehicleOverhaul\n').encode()
    if test_results:
        sources['validation/runtime-tests.json'] = test_results.read_bytes()
    source_archive = output/f'FlyingVehicleOverhaul-{version}-source.zip'
    write_zip(source_archive, sources)
    manifest = {'version': version, 'dll_sha256': hashlib.sha256(inputs[DLL]).hexdigest(),
                'dependencies': dependencies, 'features':features, 'schema_validated': schema is not None,
                'schema_source': SCHEMA_URL if schema else None,
                'schema_sha256': hashlib.sha256(schema.read_bytes()).hexdigest() if schema else None,
                'fomod': {k: hashlib.sha256(v).hexdigest() for k, v in entries.items()},
                'source': {k: hashlib.sha256(v).hexdigest() for k, v in sources.items()}}
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    (output/'README.txt').write_bytes(readme)
    (output/'SHA256SUMS.txt').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n'
                                                for p in (archive, source_archive)), encoding='ascii')
    print(f'Created {archive}: {len(entries)} installer files; {len(sources)} source files; DLL {manifest["dll_sha256"]}')
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('data-dir', 'source-dir', 'sfse-dir', 'output-dir'):
        parser.add_argument('--'+flag, type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--schema', type=Path)
    parser.add_argument('--test-results', type=Path)
    args = parser.parse_args()
    build(args.data_dir, args.source_dir, args.sfse_dir, args.output_dir, args.version, args.schema, args.test_results)


if __name__ == '__main__':
    main()
