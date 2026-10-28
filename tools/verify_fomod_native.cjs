// Native Vortex engine integration test. Only returns/plans isolated file copies;
// it never deploys files into the game or changes the user's Vortex profile.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const root=path.resolve(process.argv[2]),report=path.resolve(process.argv[3]);
const profile=process.argv[4]||'implicit-base',mask=Number(process.argv[5]||0);
const mode=process.argv[6]||'configure',scenario=process.argv[7]||'defaults';
const modulePath=process.argv[8]||'C:/Program Files/Vortex/resources/app.asar.unpacked/node_modules/@nexusmods/fomod-installer-native/build/Release/modinstaller.node';
const native=require(modulePath),features=JSON.parse(fs.readFileSync(path.join(root,'fomod/features.json'),'utf8'));
const prefix='SFSE/Plugins/FlyingVehicleOverhaul.features/';
const files=[];
function walk(dir){for(const name of fs.readdirSync(dir)){const full=path.join(dir,name);if(fs.statSync(full).isDirectory())walk(full);else files.push(path.relative(root,full));}}
walk(root);process.chdir(root);native.FileSystem.setDefaultCallbacks();
const logger=new native.Logger((level,message)=>{if(level>2)process.stderr.write(`[${level}] ${message}\n`);});logger.setCallbacks();
assert(native.ModInstaller.testSupported(files,['XmlScript']).supported);
let active=['Starfield.esm','sfbgs00d.esm','Vehicles.esm','SFBGS004.esm','SFBGS047.esm','sfbgs019.esm'];
if(profile==='implicit-base')active=active.filter(x=>x!=='Starfield.esm');
if(profile==='empty')active=[];
if(profile==='missing-parked'||profile==='inactive-parked')active=active.filter(x=>x!=='Vehicles.esm');
if(profile==='legacy')active.push('Vehicles - Real.esm');
if(profile==='old-city')active.push('FVO_CityTraffic.esm');
if(profile==='old-city-name')active.push('FlyingVehicleOverhaul - Placements.esm');
const installed=profile==='inactive-parked'?[...active,'Vehicles.esm']:active;
const patchUsable=['sfbgs00d.esm','Vehicles.esm','SFBGS004.esm','SFBGS047.esm','sfbgs019.esm'].every(x=>active.includes(x)) && !installed.includes('Vehicles - Real.esm');
const cityUsable=active.includes('SFBGS004.esm') && !installed.some(x=>['FVO_CityTraffic.esm','FlyingVehicleOverhaul - Placements.esm'].includes(x));
const desired=f=>f.configurable===false?false:scenario==='all-on'?true:scenario==='all-off'?false:scenario==='alternating'?features.indexOf(f)%2===0:
  scenario==='flip:'+f.id?!f.default:f.default;
const snapshots=[],handled=new Set();let select,forward;
const engine=new native.ModInstaller(activeOnly=>activeOnly?active:installed,()=> '1.16.0',()=> '1.16.244',()=> '0.2.21',
  (title,image,choose,cont,cancel)=>{select=choose;forward=cont;},()=>{},
  (steps,current)=>{
    snapshots.push({steps,current});
    if(handled.has(current))return;handled.add(current);
    setImmediate(()=>{
      try{
        const step=steps.find(s=>s.id===current),groups=step.optionalFileGroups.group;
        if(current===0){
          const addons=groups.filter(g=>g.type==='SelectAny');
          assert.equal(addons.length,1,'Only conversion patch checkbox group');
          assert.equal(addons[0].options.length,2,'City Placements and conversion patch');
          const usability=[cityUsable,patchUsable];
          for(const [i,option] of addons[0].options.entries()){
            assert.equal(option.selected,false,'Addons unchecked by default');
            assert.equal(option.type,usability[i]?'Optional':'NotUsable');
          }
          select(current,addons[0].id,addons[0].options.filter((o,i)=>(mask&(1<<i))&&usability[i]).map(o=>o.id));
          const setup=groups.find(g=>g.name==='Feature configuration');
          assert.equal(setup.options.filter(o=>o.selected).length,1);
          select(current,setup.id,[setup.options[mode==='keep'?1:0].id]);
        }else{
          assert.equal(mode,'configure','Keep-existing mode must not show feature pages');
          assert.equal(groups.length,features.filter(f=>f.page===step.name).length);
          for(const group of groups){
            const f=features.find(f=>f.label===group.name);assert(f);
            assert.equal(group.options.filter(o=>o.selected).length,1);
            assert.equal(group.options[0].selected,f.default,'Native defaults match compiled defaults');
            select(current,group.id,[group.options[desired(f)?0:1].id]);
          }
        }
        setImmediate(()=>forward(true,current));
      }catch(error){console.error(error.stack);process.exit(1);}
    });
  });
const timer=setTimeout(()=>{console.error('Native install timeout');process.exit(2);},20000);
// N-API wrappers own the callbacks; retain them across all asynchronous pages.
globalThis.fomodTestReferences={engine,logger};
engine.install(files,[],root,'',null,false,true).then(result=>{
  clearTimeout(timer);assert(result&&snapshots.length);
  assert(!result.instructions.some(x=>x.type==='error'));
  const copies=result.instructions.filter(x=>x.type==='copy');
  const normalize=x=>x.replaceAll('\\','/');
  const expected=['Docs/FlyingVehicleOverhaul-INSTALL.txt','FVO_NPCPilots.esm','SFSE/Plugins/FlyingVehicleOverhaul.dll',prefix+'mode.ini'];
  if((mask&1) && cityUsable)expected.push('FVO_CityPlacements.esm','SFSE/Plugins/FVO_CityPlacements.dll');
  if((mask&2) && patchUsable)expected.push('FVO_ParkedVehiclesPatch.esm');
  if(mode==='configure')for(const f of features)expected.push(prefix+f.section+'.'+f.key+'.ini');
  assert.deepEqual(copies.map(x=>normalize(x.destination)).sort(),expected.sort());
  assert.equal(handled.size,mode==='configure'?4:1,'Only selected setup pages are visited');
  const modeFile=copies.find(x=>normalize(x.destination)===prefix+'mode.ini');
  assert(fs.readFileSync(path.join(root,modeFile.source),'utf8').includes('enabled='+(mode==='configure'?'1':'0')));
  if(mode==='configure')for(const f of features){
    const copy=copies.find(x=>normalize(x.destination)===prefix+f.section+'.'+f.key+'.ini');
    const text=fs.readFileSync(path.join(root,copy.source),'utf8');
    assert(text.includes('section='+f.section+'\n')&&text.includes('key='+f.key+'\n')&&text.includes('value='+Number(desired(f))+'\n'));
  }
  fs.writeFileSync(report,JSON.stringify({passed:true,profile,mask,mode,scenario,steps:handled.size,snapshots,result},null,2));
  console.log(`PASS native engine ${profile} addons=${mask} ${mode} ${scenario}, ${copies.length} selected files`);process.exit(0);
}).catch(error=>{clearTimeout(timer);console.error(error.stack||error);process.exit(1);});
