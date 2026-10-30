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
CITY_DLL = 'SFSE/Plugins/FVO_Traffic.dll'  # FVO Traffic (was FVO_CityPlacements.dll before 2026-10-29)
OLD_CITY_DLL = 'SFSE/Plugins/FVO_CityPlacements.dll'  # never install both
CITY_TRAFFIC = 'SFSE/Plugins/FVO_Traffic/NewAtlantis.ini'
ROUTE_GUIDE = 'SFSE/Plugins/FVO_Traffic/HOW-TO-MAKE-ROUTES.txt'
TRAFFIC_DIR = 'SFSE/Plugins/FVO_Traffic/'
HEADER_IMAGE = 'fomod/images/header.png'  # 0.9.0: the banner (docs/images/fomod-header.png)
HEADER_PATH = r'fomod\images\header.png'  # as FOMOD files usually write it
# 0.9.0: one installer choice per city route (the user: "the routes for each city and make it in the fomod capable of
# selecting which you want"). key, label, description (None: made from the file), files.
ROUTES = (
    ('NewAtlantisRing', 'New Atlantis: Ring',
     'The slanted ring round New Atlantis: high over the MAST, low over the spaceport, with four exits and landing places.',
     ('NewAtlantis.ini', 'NewAtlantis-ColdStart.ini')),
    ('NewAtlantisLoop', 'New Atlantis: District loop',
     'A 3D loop through the spaceport, commercial district, embassies, lodge and residential areas, 25 m over the rooftops.',
     ('NewAtlantis-3D.ini',)),
    ('AkilaCity', 'Akila City', None, ('AkilaCity.ini',)),
    ('Cydonia', 'Cydonia', None, ('Cydonia.ini',)),
    ('EleosRetreat', 'Eleos Retreat', None, ('EleosRetreat.ini',)),
    ('Gagarin', 'Gagarin', None, ('Gagarin.ini',)),
    ('HopeTown', 'HopeTown', None, ('HopeTown.ini',)),
    ('Neon', 'Neon', None, ('Neon.ini',)),
    ('NewHomestead', 'New Homestead', None, ('NewHomestead.ini',)),
    ('Paradiso', 'Paradiso', None, ('Paradiso.ini',)),
    ('RedMile', 'Red Mile', None, ('RedMile.ini',)),
)
COLD_START_ONLY = ('NewAtlantis-ColdStart.ini',)  # spawn spots only: not installed when traffic starts empty
ROUTE_FILES = tuple(TRAFFIC_DIR+name for _, _, _, files in ROUTES for name in files)
VARIANTS = (('Yes', 'Flying'), ('No', 'Empty'))  # coldstart flag -> payload folder under '11 Route Traffic/'


def starts_empty(name, data):
    """The same route file with no cold-start traffic (start_on_route = 0)."""
    text = data.decode('utf-8')
    new, count = re.subn(r'(?m)^(\s*start_on_route\s*=\s*)\d+', r'\g<1>0', text)
    if count != 1:
        raise ValueError(f'{name}: expected one start_on_route line, found {count}')
    return new.encode('utf-8')


def route_description(label, description, data):
    if description:
        return description
    text = data.decode('utf-8')
    exits, places = text.count('[exit.'), text.count('[place.')
    return f'A traffic loop over {label} with {exits} exit(s) and {places} landing place(s).'
# Superseded experimental city content must not overlap current placements.
RETIRED = ('FVO_CityTraffic.esm', OLD_CITY_DLL)
# The base game is implicit in the Starfield install context. Managers need not
# include its always-loaded master in their reported optional-plugin lists.
IMPLICIT_MASTERS = {'starfield.esm'}
SCHEMA_URL = 'https://raw.githubusercontent.com/Nexus-Mods/fomod-installer/master/src/InstallScripting/XmlScript/Schemas/XmlScript5.0.xsd'
PROFILE = 'SFSE/Plugins/FlyingVehicleOverhaul.features/'
FEATURE_PAGES = ('Flight and Navigation','NPCs and Combat')  # 0.8.6: the streamlined menu (features.h InMenu); page names kept
# Not offered: removed or development switches (written off) and the always-on features (written on; the DLL ignores them anyway).
ALWAYS_ON = ('CombatAi','HoverOrDrive','VehicleDamage','HealthBars','StatusCard','Witnesses','NativeGround','WallPass')
FIXED_FEATURES = {'Placement':False,'PlacementMode':False,'DebugLog':False,'PhysicalAi':False, **{name:True for name in ALWAYS_ON}}
FEATURE_GROUPS = {
    'PhysicalVtol':FEATURE_PAGES[0], 'FlightAssist':FEATURE_PAGES[0], 'FreeRoll':FEATURE_PAGES[0],
    'NpcVehicles':FEATURE_PAGES[1], 'Traffic':FEATURE_PAGES[1], 'RouteMarkers':'Route beacons', 'WallPass':None,
    'HoverOrDrive':None, 'NativeGround':None, 'CombatAi':None, 'VehicleDamage':None, 'Witnesses':None, 'HealthBars':None, 'StatusCard':None,
    'PhysicalAi':None, 'Placement':None, 'PlacementMode':None, 'DebugLog':None,
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
    if len(items)!=18 or len({(f['section'],f['key']) for f in items})!=len(items):
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


def option(parent, name, description, source, destination, required, conflicts=(), companions=(), usable='Optional'):
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
    ET.SubElement(pattern, 'type', name=usable)


def module_config(dependencies, features, route_texts):
    ET.register_namespace('xsi', 'http://www.w3.org/2001/XMLSchema-instance')
    root = ET.Element('config', {'{http://www.w3.org/2001/XMLSchema-instance}noNamespaceSchemaLocation':
                                'http://qconsulting.ca/fo3/ModConfig5.0.xsd'})
    ET.SubElement(root, 'moduleName').text = 'Flying Vehicle Overhaul'
    ET.SubElement(root, 'moduleImage', path=HEADER_PATH, showImage='true', showFade='true', height='-1')
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
    ET.SubElement(core, 'image', path=HEADER_PATH)
    ET.SubElement(ET.SubElement(core, 'typeDescriptor'), 'type', name='Required')
    # 0.9.0: City Traffic and City Placements are pre-ticked; the routes are chosen city by city on their own page.
    addons = ET.SubElement(ET.SubElement(groups, 'group', name='Optional addons', type='SelectAny'),
                           'plugins', order='Explicit')
    traffic = ET.SubElement(addons, 'plugin', name='City Traffic')
    ET.SubElement(traffic, 'description').text = (
        'Traffic that flies routes over the cities, from plain text files in SFSE/Plugins/FVO_Traffic. Choose the cities on '
        'the next page. Includes HOW-TO-MAKE-ROUTES.txt for writing your own. Uses any parked vehicles (vanilla, Carfield, '
        'Parked Vehicles or City Placements). Needs only the core. Replaces the 0.8 FVO_CityTraffic.esm (remove it if '
        'another mod entry still supplies it).')
    files = ET.SubElement(traffic, 'files')
    for name in (CITY_DLL, ROUTE_GUIDE):
        ET.SubElement(files, 'file', source='11 Route Traffic/'+name, destination=name, priority='0')
    ET.SubElement(ET.SubElement(traffic, 'conditionFlags'), 'flag', name='city_traffic').text = 'Yes'
    ET.SubElement(ET.SubElement(traffic, 'typeDescriptor'), 'type', name='Recommended')
    option(addons, 'City Placements',
           'Adds parked vehicles in nine cities, for NPCs and traffic to use. Vehicle types stay as authored. Remove the superseded '
           'FVO_CityTraffic.esm if installed. Requires active: '+', '.join(dependencies[CITY])+'.',
           '10 City Placements/'+CITY, CITY, dependencies[CITY],
           ('FVO_CityTraffic.esm', 'FlyingVehicleOverhaul - Placements.esm'), usable='Recommended')
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
                ET.SubElement(files,'file',source=f"40 Features/{feature['id']}/{'On' if FIXED_FEATURES[feature['id']] else 'Off'}.ini",
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
    # 0.9.0: city routes, the cold-start choice and the route beacons, shown with City Traffic.
    step=ET.SubElement(steps,'installStep',name='City traffic routes')
    ET.SubElement(ET.SubElement(step,'visible',operator='And'),'flagDependency',flag='city_traffic',value='Yes')
    routes=ET.SubElement(ET.SubElement(ET.SubElement(step,'optionalFileGroups',order='Explicit'),'group',name='Routes',type='SelectAny'),'plugins',order='Explicit')
    for key,label,description,names in ROUTES:
        plugin=ET.SubElement(routes,'plugin',name=label)
        ET.SubElement(plugin,'description').text=route_description(label,description,route_texts[names[0]])+' Uncheck to leave this city without route traffic.'
        ET.SubElement(ET.SubElement(plugin,'conditionFlags'),'flag',name='route_'+key).text='Yes'
        ET.SubElement(ET.SubElement(plugin,'typeDescriptor'),'type',name='Recommended')
    step=ET.SubElement(steps,'installStep',name='Traffic options')
    ET.SubElement(ET.SubElement(step,'visible',operator='And'),'flagDependency',flag='city_traffic',value='Yes')
    arrive=ET.SubElement(ET.SubElement(ET.SubElement(step,'optionalFileGroups',order='Explicit'),'group',name='Traffic when you arrive',type='SelectExactlyOne'),'plugins',order='Explicit')
    for flag,label,text,kind in (('Yes','Already flying','Vehicles with seated drivers start on the routes, so a city has traffic as soon as you arrive. They are made once per save and reused.','Recommended'),
                                 ('No','Starts empty','No vehicles are made: route traffic comes only from parked vehicles that NPCs walk to and take.','Optional')):
        plugin=ET.SubElement(arrive,'plugin',name=label)
        ET.SubElement(plugin,'description').text=text
        ET.SubElement(ET.SubElement(plugin,'conditionFlags'),'flag',name='coldstart').text=flag
        ET.SubElement(ET.SubElement(plugin,'typeDescriptor'),'type',name=kind)
    beacons=next(f for f in features if f['id']=='RouteMarkers')
    step=ET.SubElement(steps,'installStep',name='Route beacons')
    visible=ET.SubElement(step,'visible',operator='And')
    ET.SubElement(visible,'flagDependency',flag='city_traffic',value='Yes')
    ET.SubElement(visible,'flagDependency',flag='configure_features',value='Yes')
    options=ET.SubElement(ET.SubElement(ET.SubElement(step,'optionalFileGroups',order='Explicit'),'group',name=beacons['label'],type='SelectExactlyOne'),'plugins',order='Explicit')
    for enabled in (True,False):
        plugin=ET.SubElement(options,'plugin',name='Show' if enabled else 'Hide')
        ET.SubElement(plugin,'description').text=('Beacons stand along every traffic lane, at its exits and at landing places, in the world you are in. '
            'Only the beacons: the routes themselves fly either way. Also a switch in the game\'s settings menu.' if enabled else
            'No beacons; traffic still flies its routes.')
        files=ET.SubElement(plugin,'files')
        ET.SubElement(files,'file',source=f"40 Features/RouteMarkers/{'On' if enabled else 'Off'}.ini",
                      destination=PROFILE+beacons['section']+'.'+beacons['key']+'.ini',priority='0')
        ET.SubElement(ET.SubElement(plugin,'typeDescriptor'),'type',name='Recommended' if enabled==beacons['default'] else 'Optional')
    # Each chosen route installs its file(s) in the variant the cold-start choice picked.
    conditional=ET.SubElement(ET.SubElement(root,'conditionalFileInstalls'),'patterns')
    for key,label,description,names in ROUTES:
        for flag,folder in VARIANTS:
            chosen=[name for name in names if folder=='Flying' or name not in COLD_START_ONLY]
            pattern=ET.SubElement(conditional,'pattern')
            deps=ET.SubElement(pattern,'dependencies',operator='And')
            ET.SubElement(deps,'flagDependency',flag='city_traffic',value='Yes')
            ET.SubElement(deps,'flagDependency',flag='route_'+key,value='Yes')
            ET.SubElement(deps,'flagDependency',flag='coldstart',value=flag)
            files=ET.SubElement(pattern,'files')
            for name in chosen:
                ET.SubElement(files,'file',source=f'11 Route Traffic/{folder}/{name}',destination=TRAFFIC_DIR+name,priority='0')
    return xml_bytes(root)


def write_zip(path, entries):
    temp = path.with_suffix('.zip.building')
    with zipfile.ZipFile(temp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(entries.items()):
            if PurePosixPath(name).is_absolute() or '..' in PurePosixPath(name).parts or '\\' in name:
                raise ValueError('Unsafe archive path: '+name)
            # 2026-10-29: the 0.8.6 Parked Vehicles Patch zip carried the whole FOMOD zip inside it and was quarantined on Nexus.
            if name.lower().endswith(('.zip', '.7z', '.rar')):
                raise ValueError('An archive inside a download: '+name)
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
    inputs = {name: (data_dir/name).read_bytes() for name in (DLL, PILOTS, PATCH, CITY, CITY_DLL, ROUTE_GUIDE, *ROUTE_FILES)}
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
        raise ValueError('Expected the matching FVO Traffic addon DLL')
    features = feature_definitions(source_dir)
    dependencies = {name: masters(inputs[name]) for name in (PILOTS, PATCH, CITY)}
    if dependencies[PILOTS] != ['Starfield.esm']:
        raise ValueError('Core pilot plugin acquired new dependencies; review installer requirements')
    route_texts = {name[len(TRAFFIC_DIR):]: inputs[name] for name in ROUTE_FILES}
    config = module_config(dependencies, features, route_texts)
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
Optional addons (City Traffic and City Placements are ticked by default):
- City Traffic: SFSE/Plugins/FVO_Traffic.dll and route files in
  SFSE/Plugins/FVO_Traffic, chosen city by city: New Atlantis (ring and district
  loop), Akila City, Cydonia, Eleos Retreat, Gagarin, HopeTown, Neon, New Homestead,
  Paradiso and Red Mile. HOW-TO-MAKE-ROUTES.txt explains writing your own; every
  *.ini in FVO_Traffic is read. Traffic can start already flying (vehicles with
  seated drivers, made once per save) or empty. Route beacons can be shown or hidden.
- City Placements: FVO_CityPlacements.esm adds parked vehicles in nine cities.
  Requires: {', '.join(dependencies[CITY])}. Enable it after its masters.
Remove the old FVO_CityTraffic.esm and FlyingVehicleOverhaul - Placements.esm if present.
Remove SFSE/Plugins/FVO_CityPlacements.dll if present: FVO_Traffic.dll replaces it (never both).
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
For city traffic, copy '11 Route Traffic/SFSE' into Data, then the route files you want
from '11 Route Traffic/Flying' (traffic already flying) or '11 Route Traffic/Empty'
(starts empty) into Data/SFSE/Plugins/FVO_Traffic.
For City Placements, also copy '10 City Placements' contents into Data
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
    ET.SubElement(info, 'Description').text = 'Core, grouped gameplay feature choices, city traffic routes city by city, City Placements and an optional Parked Vehicles conversion patch.'
    header = Path(__file__).resolve().parents[1]/'docs/images/fomod-header.png'
    if header.read_bytes()[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('FOMOD header image must be a PNG')
    entries = {'fomod/ModuleConfig.xml': config, 'fomod/info.xml': xml_bytes(info), HEADER_IMAGE: header.read_bytes(),
               'fomod/features.json': (json.dumps(features,indent=2)+'\n').encode(),
               'README.txt': readme, '00 Core/Docs/FlyingVehicleOverhaul-INSTALL.txt': readme,
               '00 Core/'+DLL: inputs[DLL], '00 Core/'+PILOTS: inputs[PILOTS],
               '20 Parked Vehicles Patch/'+PATCH: inputs[PATCH],
               '10 City Placements/'+CITY: inputs[CITY],
               '11 Route Traffic/'+CITY_DLL: inputs[CITY_DLL],
               '11 Route Traffic/'+ROUTE_GUIDE: inputs[ROUTE_GUIDE]}
    entries.update(feature_payload(features))
    for name,data in route_texts.items():
        entries['11 Route Traffic/Flying/'+name]=data
        if name not in COLD_START_ONLY:
            entries['11 Route Traffic/Empty/'+name]=starts_empty(name,data)
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
