"""Validate a FOMOD ZIP, its optional conversion patch and all feature choices.

This evaluates the XML subset emitted by package_fomod, independently of its
generator. It does not claim to replace a visual install in Vortex/MO2.
"""
import argparse
import configparser
import itertools
import json
from pathlib import Path, PurePosixPath
import xml.etree.ElementTree as ET
import zipfile
import re
from package_fomod import masters, DLL, PILOTS, PATCH, CITY, CITY_DLL, ROUTE_GUIDE, RETIRED, PROFILE, ALWAYS_ON, ROUTES, ROUTE_FILES, TRAFFIC_DIR, COLD_START_ONLY


def dependencies_match(node, states):
    values = []
    for child in node:
        if child.tag == 'fileDependency':
            values.append(states.get(child.get('file').casefold(), 'Missing') == child.get('state'))
        elif child.tag == 'dependencies':
            values.append(dependencies_match(child, states))
        else:
            raise AssertionError('Unhandled dependency: '+child.tag)
    return any(values) if node.get('operator', 'And') == 'Or' else all(values)


def option_type(plugin, states):
    fixed = plugin.find('typeDescriptor/type')
    if fixed is not None:
        return fixed.get('name')
    dynamic = plugin.find('typeDescriptor/dependencyType')
    for pattern in dynamic.findall('patterns/pattern'):
        if dependencies_match(pattern.find('dependencies'), states):
            return pattern.find('type').get('name')
    return dynamic.find('defaultType').get('name')


def verify(path, schema=None):
    checks = 0
    def check(value, label):
        nonlocal checks
        checks += 1
        if not value:
            raise AssertionError(label)
    with zipfile.ZipFile(path) as archive:
        check(archive.testzip() is None, 'ZIP integrity')
        names = archive.namelist()
        check(len(names) == len(set(n.casefold() for n in names)), 'No case-insensitive archive collisions')
        for name in ('fomod/ModuleConfig.xml', 'fomod/info.xml'):
            raw = archive.read(name)
            check(not raw.lstrip().startswith(b'<?xml'), 'No declaration that conflicts with Vortex string re-encoding: '+name)
            check(not raw.startswith(b'\xef\xbb\xbf') and b'\0' not in raw, 'Plain UTF-8 XML without BOM/NUL: '+name)
        root = ET.fromstring(archive.read('fomod/ModuleConfig.xml'))
        check(root.find('moduleDependencies') is None, 'Core never depends on manager enumeration of the base-game master')
        check(not any(d.get('file','').casefold() == 'starfield.esm' for d in root.findall('.//fileDependency')),
              'Implicit base-game master cannot reject core or disable addons')
        check(ET.fromstring(archive.read('fomod/info.xml')).findtext('Name') == 'Flying Vehicle Overhaul', 'Installer metadata')
        if schema:
            from lxml import etree
            tree = etree.parse(str(schema))
            for item in tree.iter():
                if 'type' in item.attrib:
                    item.set('type', item.get('type').strip())
            etree.XMLSchema(tree).assertValid(etree.fromstring(archive.read('fomod/ModuleConfig.xml')))
            check(True, 'Official schema validation')
        required = {x.get('destination'): x.get('source') for x in root.findall('requiredInstallFiles/file')}
        check(set(required) == {DLL, PILOTS, 'Docs/FlyingVehicleOverhaul-INSTALL.txt'}, 'Core contains only required files and guide')
        plugins = root.findall('installSteps/installStep/optionalFileGroups/group/plugins/plugin')
        optional = [p for p in plugins if p.find('typeDescriptor/dependencyType') is not None]
        check(len(optional) == 2, 'Placements and conversion patch are gated by their masters')
        addon_groups = [g for g in root.findall('installSteps/installStep/optionalFileGroups/group')
                        if g.get('type') == 'SelectAny']
        check(len(addon_groups) == 2 and addon_groups[0].get('name') == 'Optional addons' and addon_groups[1].get('name') == 'Routes',
              'One addon group and one route group')
        addon_plugins = addon_groups[0].findall('plugins/plugin')
        check([p.get('name') for p in addon_plugins] == ['City Traffic', 'City Placements', 'Parked Vehicles conversion patch'],
              'Addon option names (0.9.0)')
        traffic, city, patch = addon_plugins
        check(optional == [city, patch], 'Only placements and the patch carry master gates')
        check(traffic.find('typeDescriptor/type').get('name') == 'Recommended', 'City traffic needs only the core and is pre-ticked')
        check({f.get('destination') for f in traffic.findall('files/file')} == {CITY_DLL, ROUTE_GUIDE},
              'City traffic installs its DLL and the route guide; routes come from the route page')
        check(traffic.find('conditionFlags/flag').attrib == {'name': 'city_traffic'} and traffic.findtext('conditionFlags/flag') == 'Yes', 'City traffic sets its flag')
        check({f.get('destination') for f in city.findall('files/file')} == {CITY}, 'Placements install only their ESM')
        check(patch.find('files/file').get('destination') == PATCH, 'Only Parked Vehicles is offered')
        referenced = set(required.values())
        for file in root.findall('.//file'):
            source, dest = file.get('source'), file.get('destination')
            check(source in names, 'Every installer source exists: '+str(source))
            check(not PurePosixPath(dest).is_absolute() and '..' not in PurePosixPath(dest).parts, 'Destination stays inside Data')
            check(file.get('alwaysInstall', 'false') == 'false' and file.get('installIfUsable', 'false') == 'false', 'No checkbox bypass')
            referenced.add(source)
        payload = {n for n in names if n.startswith(('00 Core/', '10 City Placements/', '11 Route Traffic/', '20 Parked Vehicles Patch/', '30 Feature Setup/', '40 Features/'))}
        check(set(ROUTE_FILES)=={TRAFFIC_DIR+n for _,_,_,files in ROUTES for n in files},'Route list consistent')
        check(not any(n.startswith('10 Extra Vehicles/') for n in names), 'No overlapping old city placement folder')
        check(payload == referenced, 'All payload files have one explicit install mapping')
        check(not any(n.casefold().endswith(x) for n in names for x in
                      ('fvo_scheduledtraffic.esm', 'fvo_roadtrafficpool.esm', 'galactichighwaynetwork.esm', 'vehicles - real.esm')
                      + tuple(r.casefold() for r in RETIRED)), 'Removed content not reintroduced')
        features=json.loads(archive.read('fomod/features.json'))
        check(len(features)==18 and len({f['id'] for f in features})==18,'Complete runtime profile has 18 distinct keys')
        check(len([f for f in features if f['configurable']])==6,'Only the 6 menu switches are configurable')
        hidden=[f for f in features if not f['configurable']]
        check({f['id'] for f in hidden}=={'Placement','PlacementMode','DebugLog','PhysicalAi',*ALWAYS_ON},'Removed, development, merged and always-on switches are excluded')
        steps=root.findall('installSteps/installStep')
        check([p.get('name') for p in steps]==['Core and feature setup','Flight and Navigation','NPCs and Combat','City traffic routes','Traffic options','Route beacons'],
              'Installer pages (0.9.0)')
        pages=steps[1:3]
        counts=[]
        for page in pages:
            groups=page.findall('optionalFileGroups/group');counts.append(len(groups))
            check(page.find('visible/flagDependency').attrib=={'flag':'configure_features','value':'Yes'},'Keep-existing mode hides feature page')
            for group in groups:
                feature=next(f for f in features if f['label']==group.get('name'))
                check(feature['page']==page.get('name') and group.get('type')=='SelectExactlyOne','Each feature has one on/off control in its own group')
                options=group.findall('plugins/plugin')
                check(len(options)==2,'Feature has explicit enabled and disabled choices')
                for enabled,plugin in zip((True,False),options):
                    kind=plugin.find('typeDescriptor/type').get('name')
                    check(kind==('Recommended' if enabled==feature['default'] else 'Optional'),'Installer defaults match runtime feature defaults')
                    file=plugin.find('files/file')
                    check(file.get('destination')==PROFILE+feature['section']+'.'+feature['key']+'.ini','Feature output goes to the runtime import folder')
                    ini=configparser.ConfigParser();ini.read_string(archive.read(file.get('source')).decode('ascii'))
                    check(dict(ini['feature'])=={'section':feature['section'],'key':feature['key'],'value':str(int(enabled))},'Every on/off fragment identifies the exact runtime setting')
        check(counts==[3,2],'Gameplay choices grouped 3/2 (route beacons on the traffic pages)')
        beacon=next(f for f in features if f['id']=='RouteMarkers')
        check(beacon['default'] and beacon['configurable'],'Route beacons on by default (the author\'s setting), chosen in the installer')
        routes_step,options_step,beacon_step=steps[3:]
        for step in (routes_step,options_step):
            check(step.find('visible/flagDependency').attrib=={'flag':'city_traffic','value':'Yes'},'Traffic pages need City Traffic')
        check([d.attrib for d in beacon_step.findall('visible/flagDependency')]==[{'flag':'city_traffic','value':'Yes'},{'flag':'configure_features','value':'Yes'}],
              'Beacon page needs City Traffic and installer feature setup (keep mode ignores feature files)')
        route_plugins=routes_step.findall('optionalFileGroups/group/plugins/plugin')
        check([p.get('name') for p in route_plugins]==[label for _,label,_,_ in ROUTES],'One choice per route, in order')
        for (key,_,_,_),plugin in zip(ROUTES,route_plugins):
            check(plugin.find('files') is None and plugin.findtext('conditionFlags/flag')=='Yes' and plugin.find('conditionFlags/flag').get('name')=='route_'+key,'Route choice only sets its flag')
            check(plugin.find('typeDescriptor/type').get('name')=='Recommended','Every route pre-ticked')
        arrive=options_step.findall('optionalFileGroups/group/plugins/plugin')
        check([(p.get('name'),p.findtext('conditionFlags/flag'),p.find('typeDescriptor/type').get('name')) for p in arrive]==
              [('Already flying','Yes','Recommended'),('Starts empty','No','Optional')],'Cold start choice, already flying by default')
        beacon_options=beacon_step.findall('optionalFileGroups/group/plugins/plugin')
        for enabled,plugin in zip((True,False),beacon_options):
            file=plugin.find('files/file')
            check(file.get('destination')==PROFILE+beacon['section']+'.'+beacon['key']+'.ini','Beacon choice writes the RouteMarkers switch')
            ini=configparser.ConfigParser();ini.read_string(archive.read(file.get('source')).decode('ascii'))
            check(ini['feature']['value']==str(int(enabled)),'Beacon show/hide values')
            check(plugin.find('typeDescriptor/type').get('name')==('Recommended' if enabled else 'Optional'),'Show beacons recommended')
        patterns=root.findall('conditionalFileInstalls/patterns/pattern')
        check(len(patterns)==2*len(ROUTES),'One conditional install per route and cold-start state')
        for key,label,_,files in ROUTES:
            for flag,folder in (('Yes','Flying'),('No','Empty')):
                match=[p for p in patterns if {(d.get('flag'),d.get('value')) for d in p.findall('dependencies/flagDependency')}=={('city_traffic','Yes'),('route_'+key,'Yes'),('coldstart',flag)}]
                check(len(match)==1 and match[0].find('dependencies').get('operator')=='And','Route installs need City Traffic, the route and the cold-start state: '+label)
                expected={TRAFFIC_DIR+n:f'11 Route Traffic/{folder}/{n}' for n in files if folder=='Flying' or n not in COLD_START_ONLY}
                got={f.get('destination'):f.get('source') for f in match[0].findall('files/file')}
                check(got==expected,'Route variant files: '+label+' '+folder)
                for name in expected:
                    data=archive.read(expected[name]).decode('utf-8')
                    found=re.findall(r'(?m)^\s*start_on_route\s*=\s*(\d+)',data)
                    if name[len(TRAFFIC_DIR):] in COLD_START_ONLY:check(not found,'Cold-start spots file has no route rule')
                    elif folder=='Empty':check(found==['0'],'Starts-empty variant has start_on_route = 0: '+name)
                    else:check(len(found)==1 and int(found[0])>0,'Already-flying variant keeps its cold start: '+name)
        shown={g.get('name') for p in pages for g in p.findall('optionalFileGroups/group')}
        check('Placement mode' not in shown and 'Detailed log' not in shown,'Removed option labels are not shown')
        for f in hidden:
            on=f['id'] in ALWAYS_ON
            check(f"40 Features/{f['id']}/{'Off' if on else 'On'}.ini" not in names,'A fixed switch ships only its fixed value')
            ini=configparser.ConfigParser();ini.read_string(archive.read(f"40 Features/{f['id']}/{'On' if on else 'Off'}.ini").decode('ascii'))
            check(ini['feature']['value']==('1' if on else '0'),'Fixed switch written at its fixed value in configured profiles')
        check(all(not f['default'] for f in features if f['id'] in ('PhysicalVtol','PhysicalAi','FreeRoll')),'Physical flight and free roll remain off by default')
        check(not next(f for f in features if f['id']=='Placement')['default'],'Retired randomizer compatibility key fixed off')
        mode=root.find("installSteps/installStep/optionalFileGroups/group[@name='Feature configuration']")
        check(mode is not None and mode.get('type')=='SelectExactlyOne','Setup offers configure or keep existing')
        for enabled,plugin in zip((True,False),mode.findall('plugins/plugin')):
            ini=configparser.ConfigParser();ini.read_string(archive.read(plugin.find('files/file').get('source')).decode('ascii'))
            check(ini['installer']['enabled']==str(int(enabled)),'Setup mode file matches configure/keep choice')
        core = masters(archive.read(required[PILOTS]))
        check(core == ['Starfield.esm'], 'Core pilot plugin depends only on Starfield.esm')
        declared = masters(archive.read(patch.find('files/file').get('source')))
        check(declared == ['Starfield.esm', 'sfbgs00d.esm', 'Vehicles.esm', 'SFBGS004.esm', 'SFBGS047.esm', 'sfbgs019.esm'],
              'Patch has the reviewed vehicle-content masters')
        gates = patch.findall('typeDescriptor/dependencyType/patterns/pattern')[-1]
        check([d.get('file') for d in gates.findall('dependencies/fileDependency')] ==
              [m for m in declared if m.casefold() != 'starfield.esm'], 'Gates match actual non-base masters')
        city_masters = masters(archive.read(city.find('files/file').get('source')))
        check(city_masters == ['Starfield.esm', 'SFBGS004.esm'], 'City Placements uses the base-game vehicle masters')
        city_gates = city.findall('typeDescriptor/dependencyType/patterns/pattern')[-1]
        check([d.get('file') for d in city_gates.findall('dependencies/fileDependency')] == ['SFBGS004.esm'], 'City gates match its non-base master')
        combinations = 0
        keys = [m.casefold() for m in declared] + ['vehicles - real.esm']
        for values in itertools.product(('Missing', 'Inactive', 'Active'), repeat=len(keys)):
            states = dict(zip(keys, values))
            usable = all(states[m.casefold()] == 'Active' for m in declared if m.casefold() != 'starfield.esm')
            usable = usable and states['vehicles - real.esm'] == 'Missing'
            kind = option_type(patch, states)
            check(kind == ('Optional' if usable else 'NotUsable'), 'Dependencies and legacy conflict gate patch without preselecting')
            for selected in (False, True):
                plan = dict(required)
                if selected and kind == 'Optional':
                    plan[PATCH] = patch.find('files/file').get('source')
                check((PATCH in plan) == (selected and usable), 'Only a selected usable patch deploys')
                check(set(required) <= set(plan), 'Core always installs')
                combinations += 1
        # Independently exhaust city dependencies/conflicts and all four addon
        # choices. A blocked city option must never block the conversion or core.
        city_keys = ['starfield.esm', 'sfbgs004.esm', 'fvo_citytraffic.esm', 'flyingvehicleoverhaul - placements.esm']
        for values in itertools.product(('Missing','Inactive','Active'), repeat=len(city_keys)):
            states = {m.casefold():'Active' for m in declared}
            states.update(dict(zip(city_keys,values)))
            usable = states['sfbgs004.esm']=='Active' and all(states[k]=='Missing' for k in city_keys[2:])
            check(option_type(city,states)==('Recommended' if usable else 'NotUsable'),'City masters/conflicts gate the pre-ticked option')
            for city_selected,patch_selected,traffic_selected in itertools.product((False,True), repeat=3):
                plan=dict(required)
                for selected,option in ((city_selected,city),(patch_selected,patch)):
                    if selected and option_type(option,states) in ('Optional','Recommended'):
                        for f in option.findall('files/file'):
                            plan[f.get('destination')]=f.get('source')
                if traffic_selected:
                    for f in traffic.findall('files/file'):
                        plan[f.get('destination')]=f.get('source')
                check((CITY in plan)==(city_selected and usable),'Only selected usable city placements install')
                check((CITY_DLL in plan)==traffic_selected,'City traffic installs exactly when chosen, independent of placements')
                check((CITY_DLL in plan)==(ROUTE_GUIDE in plan),'City traffic DLL and guide are atomic')
                check((PATCH in plan)==(patch_selected and states['sfbgs004.esm']=='Active'),'City conflicts do not block conversion patch')
                check(set(required)<=set(plan),'Core always installs with either addon')
                combinations+=1
        check({n for n in names if n.casefold().endswith('.esm')} ==
              {'00 Core/'+PILOTS, '20 Parked Vehicles Patch/'+PATCH, '10 City Placements/'+CITY}, 'Only pilots, City Placements and conversion ESMs shipped')
        result = {'passed': True, 'checks': checks, 'profile_choice_combinations': combinations,
                  'masters': {PILOTS: core, PATCH: declared, CITY: city_masters},
                  'schema_validated': bool(schema), 'manager_ui_tested': False}
        print(json.dumps(result, indent=2))
        return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--schema', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    result = verify(args.archive, args.schema)
    if args.report:
        args.report.write_text(json.dumps(result, indent=2)+'\n')
