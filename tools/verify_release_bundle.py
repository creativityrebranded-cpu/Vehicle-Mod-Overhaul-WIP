"""Check version-folder layout and consistency of every install method."""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET
import zipfile
from package_fomod import DLL,PILOTS,PATCH,CITY,CITY_DLL,ROUTE_GUIDE,ROUTE_FILES,TRAFFIC_DIR
from verify_fomod import verify


def verify_bundle(folder,schema=None):
    metadata=json.loads((folder/'release-bundle.json').read_text())
    version=metadata['version'];assert folder.name==version,'Release folder must equal its version'
    artifacts=metadata['artifacts'];assert set(artifacts)=={'main','extra','patch','fomod','source'}
    archives={}
    for role,item in artifacts.items():
        assert Path(item['file']).name==item['file'],'Artifact must be inside version folder'
        path=folder/item['file'];assert hashlib.sha256(path.read_bytes()).hexdigest()==item['sha256']
        assert path.stat().st_size==item['size'] and f'-{version}' in path.name
        with zipfile.ZipFile(path) as archive:
            assert archive.testzip() is None
            archives[role]={name:archive.read(name) for name in archive.namelist()}
    expected={
        'main':{DLL,PILOTS,'Docs/FlyingVehicleOverhaul-INSTALL.txt'},
        'extra':{CITY,CITY_DLL,ROUTE_GUIDE,*ROUTE_FILES,'Docs/FlyingVehicleOverhaul-ExtraVehicles.txt'},
        'patch':{PATCH,'Docs/FlyingVehicleOverhaul-ParkedVehiclesPatch.txt'},
    }
    for role,names in expected.items():assert set(archives[role])==names,(role,'unexpected deployed files')
    fomod=archives['fomod']
    assert archives['main'][DLL]==fomod['00 Core/'+DLL]
    assert archives['main'][PILOTS]==fomod['00 Core/'+PILOTS]
    assert hashlib.sha256(archives['main'][DLL]).hexdigest()==metadata['dll_sha256']
    assert archives['extra'][CITY]==fomod['10 City Placements/'+CITY]
    for name in (CITY_DLL,ROUTE_GUIDE):assert archives['extra'][name]==fomod['11 Route Traffic/'+name]
    for name in ROUTE_FILES:assert archives['extra'][name]==fomod['11 Route Traffic/Flying/'+name[len(TRAFFIC_DIR):]],'Extra ZIP routes = installer flying variant'
    assert archives['patch'][PATCH]==fomod['20 Parked Vehicles Patch/'+PATCH]
    assert ET.fromstring(fomod['fomod/info.xml']).findtext('Version')==version
    assert f'Flying Vehicle Overhaul (FVO) {version} loaded'.encode() in archives['main'][DLL]
    for role in ('main','extra','patch','fomod'):
        assert not any(n.lower().endswith(('.exe','.pdb','.obj','.cpp','.h','.cjs','.py')) for n in archives[role]),'Developer files leaked into player ZIP'
    sums=(folder/'SHA256SUMS.txt').read_text()
    assert sums==(folder/f'FlyingVehicleOverhaul-{version}-SHA256SUMS.txt').read_text()
    assert set(sums.splitlines())=={f"{x['sha256']}  {x['file']}" for x in artifacts.values()}
    installer=verify(folder/artifacts['fomod']['file'],schema)
    result={'passed':True,'version':version,'archives':len(archives),'core_payloads_identical':True,
            'player_zips_free_of_developer_files':True,'installer_checks':installer['checks']}
    print(json.dumps(result,indent=2));return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path)
    p.add_argument('--schema',type=Path);p.add_argument('--report',type=Path);args=p.parse_args()
    result=verify_bundle(args.folder,args.schema)
    if args.report:args.report.write_text(json.dumps(result,indent=2)+'\n')
