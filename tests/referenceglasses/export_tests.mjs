// Exact geometry/source verification without modifying the user's reference project.
// Run: node tests/referenceglasses/export_tests.mjs
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const manifest=JSON.parse(fs.readFileSync(path.join(repo,'tools/accessories/reference_glasses_collection_provenance.json'),'utf8'));
const reference=manifest.referenceRoot;
const dir=path.join(repo,'build/referenceglasses-tests',new Date().toISOString().replaceAll(':','-')+'-'+crypto.randomUUID().slice(0,8));
fs.mkdirSync(dir,{recursive:true});
const sha=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const inputs=[...manifest.sources.map(s=>({path:path.join(reference,s.path),sha256:s.sha256})),...['tools/accessories/export_reference_glasses_collection.mjs','tools/accessories/reference_glasses_collection_provenance.json',manifest.output.path,'windows/src/effects/overlays/accessory_mesh.h','windows/src/effects/overlays/accessory_mesh.cpp','tests/referenceglasses/export_tests.mjs','windows/src/effects/overlays/reference_wayfarer_mesh.inc','tools/accessories/reference_wayfarer_provenance.json'].map(relative=>({path:path.join(repo,relative),sha256:sha(path.join(repo,relative))}))];
const receipt={completed:false,actualExitCode:1,groups:0,assertions:0,sourceHashes:inputs,processes:[],evidenceHashes:[],failure:null};
const need=(condition,message)=>{++receipt.assertions;assert.ok(condition,message);};
try {
  for(const source of inputs)need(sha(source.path)===source.sha256,'original/export/input source hash: '+source.path);
  for(let n=1;n<=2;++n){const target=path.join(dir,'export-'+n);const result=spawnSync(process.execPath,[path.join(repo,'tools/accessories/export_reference_glasses_collection.mjs'),reference,target],{encoding:'utf8',timeout:30000,cwd:repo,windowsHide:true});
    fs.writeFileSync(path.join(dir,'export-'+n+'.stdout.log'),result.stdout??'');fs.writeFileSync(path.join(dir,'export-'+n+'.stderr.log'),result.stderr??'');
    receipt.processes.push({label:'export-'+n,actualExitCode:result.status,timedOut:result.error?.code==='ETIMEDOUT',deadlineSeconds:30,error:result.error?.message??null});
    need(result.status===0&&!result.error,'bounded actual exporter exit0');
    for(const relative of [manifest.output.path,'tools/accessories/reference_glasses_collection_provenance.json'])need(sha(path.join(target,relative))===sha(path.join(repo,relative)),'deterministic exact exported bytes: '+relative);
  }++receipt.groups;
  const THREE=await import(pathToFileURL(path.join(reference,'node_modules/three/build/three.module.js')).href);
  const {GlassesModel}=await import(pathToFileURL(path.join(reference,'src/glasses/glasses-model.js')).href);
  const {DEFAULT_PRESETS}=await import(pathToFileURL(path.join(reference,'src/processing/storage.js')).href);
  const text=fs.readFileSync(path.join(repo,manifest.output.path),'utf8');
  const symbols={aviator:'Aviator',round_tortoise:'RoundTortoise',cateye:'CatEye'};
  for(const exported of manifest.presets){
    const preset=DEFAULT_PRESETS.find(p=>p.id===exported.id);need(preset&&preset.name===exported.name,'actual source catalogue identity');
    for(const key of ['material','frameColor','templeColor','lensTint','lensOpacity'])need(exported.options[key]===preset[key],'actual source preset option '+key);
    need(exported.scaleMultiplier===preset.scaleMultiplier&&exported.frameScaleCentimeters===14,'actual source pose scale preserved independently of geometry');
    const symbol=symbols[exported.id];
    const vblock=text.match(new RegExp('kReference'+symbol+'Vertices\\[\\] = \\{([\\s\\S]*?)\\n\\};'))?.[1];
    const tblock=text.match(new RegExp('kReference'+symbol+'Triangles\\[\\] = \\{([\\s\\S]*?)\\n\\};'))?.[1];
    need(vblock&&tblock,'each selected style arrays available');
    const vertices=[...vblock.matchAll(/\{([^{}]+)\}/g)].map(m=>m[1].split(',').map(v=>Number(v.replace(/f$/,''))));
    const triangles=[...tblock.matchAll(/\{(\d+)u,(\d+)u,(\d+)u,Material::(\w+)\}/g)].map(m=>[Number(m[1]),Number(m[2]),Number(m[3]),m[4]]);
    need(vertices.length===exported.vertices&&triangles.length===exported.triangles,'bounded complete arrays');
    const model=new GlassesModel(exported.options);model.group.updateMatrixWorld(true);
    let offset=0,triangleOffset=0,ordinal=0,zeroCount=0;
    const bounds={min:[Infinity,Infinity,Infinity],max:[-Infinity,-Infinity,-Infinity]};
    model.group.traverse(child=>{
      if(!child.isMesh)return;
      const component=exported.components[ordinal],pos=child.geometry.getAttribute('position'),normal=child.geometry.getAttribute('normal'),uv=child.geometry.getAttribute('uv');
      need(component&&component.name===child.name&&component.vertexCount===pos.count&&component.firstVertex===offset,'all original components retained in source order');
      need(component.materialProperties.color==='#'+child.material.color.getHexString()&&component.materialProperties.opacity===child.material.opacity&&component.materialProperties.metalness===child.material.metalness&&component.materialProperties.roughness===child.material.roughness,'actual wired material properties retained');
      const matrix=new THREE.Matrix3().getNormalMatrix(child.matrixWorld);
      for(let i=0;i<pos.count;++i){
        const p=new THREE.Vector3(pos.getX(i),pos.getY(i),pos.getZ(i)).applyMatrix4(child.matrixWorld);
        const n=new THREE.Vector3(normal.getX(i),normal.getY(i),normal.getZ(i)).applyMatrix3(matrix).normalize();
        const v=vertices[offset+i];need(v.length===8&&v.every(Number.isFinite),'finite bounded native vertex');
        for(let c=0;c<3;++c){need(Math.fround(v[c])===Math.fround([p.x,p.y,p.z][c]),'exact transformed source coordinate');bounds.min[c]=Math.min(bounds.min[c],Math.fround(v[c]));bounds.max[c]=Math.max(bounds.max[c],Math.fround(v[c]));}
        need(Math.abs(v[3]*v[3]+v[4]*v[4]+v[5]*v[5]-1)<2e-6,'unit transformed native normal');
        if(n.lengthSq()<1e-12){++zeroCount;const repair=exported.normalRepairs.find(r=>r.componentOrdinal===ordinal&&r.vertex===i);need(repair,'explicit zero-normal provenance');const j=repair.copiedNormalFromCoincidentVertex;need(j>=0&&j<pos.count&&pos.getX(i)===pos.getX(j)&&pos.getY(i)===pos.getY(j)&&pos.getZ(i)===pos.getZ(j),'normal repair from exactly coincident original position only');n.set(normal.getX(j),normal.getY(j),normal.getZ(j)).applyMatrix3(matrix).normalize();}
        for(let c=0;c<3;++c)need(Math.fround(v[c+3])===Math.fround([n.x,n.y,n.z][c]),'exact transformed original or documented coincident normal');
        need(Math.fround(v[6])===Math.fround(uv?uv.getX(i):0)&&Math.fround(v[7])===Math.fround(uv?uv.getY(i):0),'exact source UV orientation');
      }
      const index=child.geometry.getIndex(),count=index?index.count:pos.count;need(count%3===0,'source triangular topology');
      for(let i=0;i<count;i+=3){const actual=triangles[triangleOffset++];for(let c=0;c<3;++c){const original=index?index.getX(i+c):i+c;need(actual[c]===offset+original&&actual[c]>=0&&actual[c]<vertices.length,'exact original source triangle index');}need(actual[3]===component.material,'source material classification per triangle');}
      offset+=pos.count;++ordinal;
    });model.dispose();
    need(offset===vertices.length&&triangleOffset===triangles.length&&ordinal===exported.components.length,'complete geometry/component traversal');
    need(zeroCount===2&&exported.normalRepairs.length===2,'only two original duplicate lens-end repairs');
    for(let c=0;c<3;++c)need(bounds.min[c]===exported.envelope.min[c]&&bounds.max[c]===exported.envelope.max[c],'recorded exact source bounds');
    need(exported.id!=='aviator'||exported.components.some(c=>c.name==='UpperBridge'),'source aviator upper bridge retained');
    ++receipt.groups;
  }
  const legacy=JSON.parse(fs.readFileSync(path.join(repo,'tools/accessories/reference_wayfarer_provenance.json'),'utf8'));
  need(sha(path.join(repo,legacy.output.path))===legacy.output.sha256,'previous exact Classic Wayfarer remains unchanged');
  for(const source of inputs)need(sha(source.path)===source.sha256,'final original/export/input hash unchanged: '+source.path);
  ++receipt.groups;receipt.completed=true;receipt.actualExitCode=0;
}catch(error){receipt.failure=String(error.stack??error);}
function evidence(root){for(const entry of fs.readdirSync(root,{withFileTypes:true})){const p=path.join(root,entry.name);if(entry.isDirectory())evidence(p);else receipt.evidenceHashes.push({path:p,sha256:sha(p)});}}
evidence(dir);fs.writeFileSync(path.join(dir,'receipt.json'),JSON.stringify(receipt,null,2)+'\n');
console.log(JSON.stringify({completed:receipt.completed,groups:receipt.groups,assertions:receipt.assertions,actualExitCode:receipt.actualExitCode,receipt:path.join(dir,'receipt.json'),failure:receipt.failure},null,2));
process.exitCode=receipt.actualExitCode;
