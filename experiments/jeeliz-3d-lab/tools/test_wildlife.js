'use strict';
// Real pinned Three.js geometry and resource checks. WebGL/live fit is separate.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const site=path.resolve(__dirname,'../site');
const THREE=require(path.join(site,'vendor/three.js'));
const Wildlife=require(path.join(site,'wildlife-rigs.js'));
const Pose=require(path.join(site,'pose.js'));
const read=name=>JSON.parse(fs.readFileSync(path.join(site,name),'utf8'));
const data={tiger:[read('assets/tiger/TigerHead.json')],werewolf:[read('assets/werewolf/werewolf_not_animated.json')]};
const maps={tiger:Array.from({length:2},()=>new THREE.Texture()),werewolf:Array.from({length:7},()=>new THREE.Texture())};
const rigs={};let checks=0,assertions=0;
function verify(value,message){assert(value,message);assertions++;}
function equal(actual,expected,message){assert.deepEqual(actual,expected,message);assertions++;}
function check(name,run){run();checks++;console.log('PASS '+checks+': '+name);}
function points(geometry){return geometry.isBufferGeometry?Array.from({length:geometry.attributes.position.count},(_,i)=>
 new THREE.Vector3().fromBufferAttribute(geometry.attributes.position,i)):geometry.vertices.map(v=>v.clone());}
function scale(mesh,value){equal(mesh.scale.toArray(),[value,value,value]);}
function finiteGeometry(geometry){
 verify(points(geometry).every(p=>p.toArray().every(Number.isFinite)));
 if(geometry.isBufferGeometry){
  const p=geometry.attributes.position,n=geometry.attributes.normal,uv=geometry.attributes.uv;
  verify(Array.from(n.array).every(Number.isFinite));verify(Array.from(uv.array).every(Number.isFinite));
  verify(Array.from(geometry.index.array).every(i=>Number.isInteger(i)&&i>=0&&i<p.count));
 }else{
  verify(geometry.faces.every(f=>[f.a,f.b,f.c].every(i=>Number.isInteger(i)&&i>=0&&i<geometry.vertices.length)));
  verify(geometry.faces.every(f=>f.normal.toArray().every(Number.isFinite)&&f.vertexNormals.every(n=>n.toArray().every(Number.isFinite))));
  verify(geometry.faceVertexUvs[0].every(face=>face.every(uv=>uv.toArray().every(Number.isFinite))));
 }
 geometry.computeBoundingBox();const size=geometry.boundingBox.getSize(new THREE.Vector3());
 verify(size.toArray().every(v=>Number.isFinite(v)&&v>0));
}
async function main(){
check('UMD API supports only tiger and werewolf with pinned Three revision97',()=>{
 equal(THREE.REVISION,'97');equal(Object.keys(Wildlife).sort(),['create','load','supports']);
 for(const type of ['tiger','werewolf'])verify(Wildlife.supports(type));
 for(const type of ['dog','constructor','__proto__','Tiger',null,{},2])verify(!Wildlife.supports(type));
});
check('unknown types, absent data and invalid texture collections fail closed',()=>{
 for(const type of ['dog','constructor','__proto__']){assert.throws(()=>Wildlife.create(THREE,type,data.tiger,maps.tiger));assertions++;}
 for(const type of ['tiger','werewolf']){
  for(const bad of [undefined,null,[],[null],[{},{}]]){assert.throws(()=>Wildlife.create(THREE,type,bad,null));assertions++;}
  for(const bad of [[],[{}],{},[null]]){assert.throws(()=>Wildlife.create(THREE,type,data[type],bad));assertions++;}
 }
});
for(const type of ['tiger','werewolf'])rigs[type]=Wildlife.create(THREE,type,data[type],maps[type]);
const tiger=rigs.tiger,wolf=rigs.werewolf;
check('genuine tiger indexed geometry retains4 authored material groups and UVs',()=>{
 equal(tiger.mesh.geometry.attributes.position.count,1165);
 equal(tiger.mesh.geometry.groups.map(g=>g.materialIndex),[0,1,2,3]);
 equal(tiger.mesh.geometry.attributes.uv.count,1165);
 equal(tiger.mesh.material[2].map,maps.tiger[0]);equal(tiger.mesh.material[1].map,maps.tiger[1]);
 finiteGeometry(tiger.mesh.geometry);verify(tiger.mesh.geometry.index.count>6000);
});
check('genuine static werewolf retains11970 vertices and16808 triangles with3 face materials',()=>{
 equal(wolf.mesh.geometry.vertices.length,11970);equal(wolf.mesh.geometry.faces.length,16808);
 equal(new Set(wolf.mesh.geometry.faces.map(f=>f.materialIndex)),new Set([0,1,2]));
 equal(wolf.mesh.geometry.morphTargets.length,0);equal(wolf.hasMouth,false);equal(wolf.mesh.morphTargetInfluences,undefined);
 finiteGeometry(wolf.mesh.geometry);
});
check('werewolf uses all7 authored diffuse, normal and alpha texture maps',()=>{
 const [head,fur,teeth]=wolf.mesh.material;
 equal([head.map,head.normalMap,head.alphaMap,fur.map,fur.normalMap,fur.alphaMap,teeth.map],maps.werewolf);
 verify(head.transparent&&fur.transparent);verify(head.alphaTest>0&&fur.alphaTest>0);
 equal(fur.depthWrite,false);equal(fur.normalScale.toArray(),[2,2]);
 verify(wolf.mesh.material.every(m=>!m.morphTargets&&m.depthTest));
});
check('neutral vertices survive documented original demo calibration without dynamic deformation',()=>{
 equal(Array.from(tiger.mesh.geometry.attributes.position.array),Array.from(Float32Array.from(data.tiger[0].data.attributes.position.array)));
 const authored=new THREE.JSONLoader().parse({...data.werewolf[0],materials:[]}).geometry;
 equal(wolf.mesh.geometry.vertices.map(p=>p.toArray()),authored.vertices.map(p=>p.toArray()));authored.dispose();
 equal(tiger.mesh.scale.toArray(),[2,3,2]);scale(wolf.mesh,7);equal(tiger.mouthCavity.scale.toArray(),[.24,.3,1]);
 equal(tiger.mesh.position.toArray(),[0,.2,-.48]);equal(wolf.mesh.position.toArray(),[0,-1.2,-.5]);
 equal(tiger.mesh.parent,tiger.group);equal(wolf.mesh.parent,wolf.group);
});
check('tiger front eye apertures are small smooth ellipses and do not preserve detached stripes',()=>{
 for(const material of [tiger.mesh.material[1],tiger.mesh.material[2]]){
  const shader={vertexShader:THREE.ShaderLib.lambert.vertexShader,fragmentShader:THREE.ShaderLib.lambert.fragmentShader};
  material.onBeforeCompile(shader);verify(shader.vertexShader.includes('vTigerPosition = position;'));
  equal(shader.uniforms.tigerEyeRadii.value.toArray(),[.105,.055]);
  verify(shader.fragmentShader.includes('/ tigerEyeRadii;'));
  verify(shader.fragmentShader.includes('smoothstep(0.82,1.08,length(eyeL))'));
  verify(shader.fragmentShader.includes('mix(1.0,eyeAlpha,eyeDepth)'));
  verify(!shader.fragmentShader.includes('darkCoverage'));
  verify(shader.fragmentShader.indexOf('diffuseColor.a *= mix')<shader.fragmentShader.indexOf('#include <alphatest_fragment>'));
  verify(!shader.fragmentShader.includes('samplerVideo'));verify(material.transparent&&material.alphaTest>0);
  // Aperture areas are less than one sixth of the old video-compositing
  // footprint. Their intended world centres remain aligned to the original
  // artist's eye coordinates, even after calibration.
  const [rx,ry]=shader.uniforms.tigerEyeRadii.value.toArray();
  verify(rx*ry < (.2/.6)*.2/6);
  tiger.group.updateMatrixWorld(true);
  const l=new THREE.Vector3(.25,.15,.36).applyMatrix4(tiger.mesh.matrixWorld);
  const r=new THREE.Vector3(-.25,.15,.36).applyMatrix4(tiger.mesh.matrixWorld);
  verify(Math.abs(l.x-.5)<1e-12&&Math.abs(r.x+.5)<1e-12);
  verify(Math.abs(l.y-.65)<1e-12&&Math.abs(r.y-.65)<1e-12);
  verify(Math.abs(l.z-.24)<1e-12&&Math.abs(r.z-.24)<1e-12);
 }
});
check('real tiger mouth cavity has rounded silhouette recessed behind its authored muzzle',()=>{
 const geometry=tiger.mouthCavity.geometry;
 equal(geometry.type,'CircleBufferGeometry');verify(geometry.attributes.position.count>=40);
 for(const p of points(geometry))verify(p.x*p.x+p.y*p.y<=1+1e-6);
 tiger.group.updateMatrixWorld(true);
 const sphere=new THREE.Box3().setFromObject(tiger.mouthCavity);
 verify(Math.abs(sphere.max.x-.24)<1e-7&&Math.abs(sphere.min.x+.24)<1e-7);
 const source=tiger.mesh.geometry.attributes.position;let muzzleFront=-Infinity;
 for(let i=0;i<source.count;i++)if(Math.abs(source.getX(i))<.12&&source.getY(i)<.05)
  muzzleFront=Math.max(muzzleFront,new THREE.Vector3().fromBufferAttribute(source,i).applyMatrix4(tiger.mesh.matrixWorld).z);
 verify(muzzleFront>tiger.mouthCavity.position.z);
});
const base=Float32Array.from(tiger.mesh.geometry.attributes.position.array);
check('tiger mouth0..1 moves only its real lower jaw through a bounded15degree hinge',()=>{
 equal(tiger.hasMouth,true);let changed=0;
 for(let step=0;step<=20;step++){
  const mouth=step/20,angle=mouth*Math.PI/12,c=Math.cos(angle),s=Math.sin(angle);tiger.setMouth(mouth);
  const live=tiger.mesh.geometry.attributes.position.array;
  for(let i=0;i<live.length;i+=3){
   const y=base[i+1],z=base[i+2],lower=y+z*.2<=0;
   verify(Math.abs(live[i]-base[i])<1e-7);
   verify(Math.abs(live[i+1]-(lower?c*y-s*z:y))<1e-7);
   verify(Math.abs(live[i+2]-(lower?s*y+c*z:z))<1e-7);
   if(step===20&&lower&&Math.abs(live[i+1]-y)>.001)changed++;
   verify(Math.hypot(live[i+1]-y,live[i+2]-z)<=2*Math.hypot(y,z)*Math.sin(Math.PI/24)+1e-7);
  }
  finiteGeometry(tiger.mesh.geometry);equal(tiger.mesh.scale.toArray(),[2,3,2]);equal(tiger.mouthCavity.scale.toArray(),[.24,.3,1]);
  equal(tiger.group.userData.mouth,mouth);equal(tiger.mouthCavity.visible,mouth>.025);
 }
 verify(changed>100);tiger.setMouth(0);equal(Array.from(tiger.mesh.geometry.attributes.position.array),Array.from(base));
});
check('negative, oversized, nonnumeric and nonfinite mouth values stay bounded',()=>{
 for(const [input,expected] of [[-10,0],[10,1],[NaN,0],[Infinity,0],[-Infinity,0],[undefined,0],['1',0],[.5,.5]]){
  tiger.setMouth(input);equal(tiger.group.userData.mouth,expected);finiteGeometry(tiger.mesh.geometry);
 }
 tiger.setMouth(0);
 const staticVertices=wolf.mesh.geometry.vertices.map(p=>p.toArray());
 for(const mouth of [-2,0,.5,1,5,NaN,Infinity])wolf.setMouth(mouth);
 equal(wolf.mesh.geometry.vertices.map(p=>p.toArray()),staticVertices);equal(wolf.mesh.geometry.morphTargets.length,0);
});
const camera=new THREE.PerspectiveCamera(35,800/600,.1,100);
const state={detected:1,x:0,y:0,s:.21,rx:0,ry:0,rz:0};
for(const type of ['tiger','werewolf'])check(type+' actual calibrated geometry remains finite and ray-visible through yaw+-65 pitch+-25',()=>{
 const rig=rigs[type],parent=new THREE.Group();parent.add(rig.group);
 const vertices=points(rig.mesh.geometry),a=vertices[0],b=vertices[Math.floor(vertices.length/2)];
 const calibration=type==='tiger'?[2,3,2]:[7,7,7],s=new THREE.Vector3(...calibration);
 const fittedDistance=a.clone().multiply(s).distanceTo(b.clone().multiply(s));
 for(const yaw of [-65,0,65])for(const pitch of [-25,0,25]){
  verify(Pose.apply(THREE,parent,{...state,ry:yaw*Math.PI/180,rx:pitch*Math.PI/180,rz:.17},camera));
  camera.updateMatrixWorld(true);equal(rig.mesh.scale.toArray(),calibration);
  verify(rig.mesh.matrixWorld.elements.every(Number.isFinite));
  const worldA=a.clone().applyMatrix4(rig.mesh.matrixWorld),worldB=b.clone().applyMatrix4(rig.mesh.matrixWorld);
  verify(Math.abs(worldA.distanceTo(worldB)-fittedDistance)<1e-10);
  const axes=[0,4,8].map(i=>Math.hypot(...rig.mesh.matrixWorld.elements.slice(i,i+3)));
  verify(axes.every((length,i)=>Math.abs(length-calibration[i])<1e-12));
  let hitCount=0;
  for(let i=0;i<vertices.length;i+=Math.ceil(vertices.length/12)){
   const world=vertices[i].clone().applyMatrix4(rig.mesh.matrixWorld);verify(world.z<0);
   const projected=world.clone().project(camera);verify(projected.toArray().every(Number.isFinite));
   if(new THREE.Raycaster(new THREE.Vector3(),world.normalize()).intersectObject(rig.mesh).length)hitCount++;
  }
  verify(hitCount>0);finiteGeometry(rig.mesh.geometry);
 }
});
check('invalid tracker poses hide a real wildlife rig rather than contaminating its matrix',()=>{
 for(const bad of [{...state,ry:NaN},{...state,s:0},{...state,rx:Infinity}]){
  equal(Pose.apply(THREE,tiger.group,bad,camera),false);equal(tiger.group.visible,false);
  verify(tiger.group.matrixWorld.elements.every(Number.isFinite));
 }
});
check('corrupt real tiger arrays, triangle indices and material references are rejected',()=>{
 for(const mutate of [d=>{d.data.attributes.position.array[0]=NaN;},d=>{d.data.attributes.uv.array[0]=Infinity;},
   d=>{d.data.index.array[0]=999999;},d=>{d.data.groups[0].materialIndex=4;},
   d=>{for(let i=2;i<d.data.attributes.position.array.length;i+=3)d.data.attributes.position.array[i]=0;}]){
  const copy=structuredClone(data.tiger[0]);mutate(copy);assert.throws(()=>Wildlife.create(THREE,'tiger',[copy],maps.tiger));assertions++;
 }
});
check('corrupt real werewolf coordinates and UVs are rejected by the actual legacy loader path',()=>{
 for(const mutate of [d=>{d.vertices[0]=NaN;},d=>{d.uvs[0][0]=Infinity;}]){
  const copy=structuredClone(data.werewolf[0]);mutate(copy);assert.throws(()=>Wildlife.create(THREE,'werewolf',[copy],maps.werewolf));assertions++;
 }
});
check('owned real mesh geometry/material resources dispose exactly once; cached textures survive',()=>{
 for(const type of ['tiger','werewolf']){
  const rig=rigs[type],resources=new Set();let disposed=0,texturesDisposed=0;
  rig.group.traverse(o=>{if(o.geometry)resources.add(o.geometry);if(o.material)for(const material of [].concat(o.material))resources.add(material);});
  resources.forEach(r=>r.addEventListener('dispose',()=>disposed++));maps[type].forEach(map=>map.addEventListener('dispose',()=>texturesDisposed++));
  rig.dispose();equal(disposed,resources.size);rig.dispose();rig.setMouth(1);equal(disposed,resources.size);equal(texturesDisposed,0);
  equal(rig.group.visible,false);
 }
});
const originalFetch=global.fetch;let fetched=[];
class TextureLoader {
 load(name,success,progress,failure){const texture=new THREE.Texture();setImmediate(()=>success(texture));return texture;}
}
try{
 global.fetch=async name=>{fetched.push(name);return {ok:true,json:async()=>data[name.includes('/tiger/')?'tiger':'werewolf'][0]};};
 const loaded={};for(const type of ['tiger','werewolf'])loaded[type]=await Wildlife.load({...THREE,TextureLoader},type);
 check('asset loading requests exact local pinned models and assigns sRGB to color maps only',()=>{
  equal(fetched,['assets/tiger/TigerHead.json','assets/werewolf/werewolf_not_animated.json']);
  equal(loaded.tiger.maps.map(m=>m.encoding),[THREE.sRGBEncoding,THREE.sRGBEncoding]);
  equal(loaded.werewolf.maps.map(m=>m.encoding),[THREE.sRGBEncoding,THREE.LinearEncoding,THREE.LinearEncoding,
    THREE.sRGBEncoding,THREE.LinearEncoding,THREE.LinearEncoding,THREE.sRGBEncoding]);
  equal(loaded.tiger.data[0],data.tiger[0]);equal(loaded.werewolf.data[0],data.werewolf[0]);
 });
 await assert.rejects(Wildlife.load({...THREE,TextureLoader},'constructor'));assertions++;checks++;console.log('PASS '+checks+': unknown load fails before fetching');
 global.fetch=async()=>({ok:false});
 await assert.rejects(Wildlife.load({...THREE,TextureLoader},'tiger'),/Missing model/);assertions++;checks++;console.log('PASS '+checks+': missing model fails explicitly');
 global.fetch=async()=>({ok:true,json:async()=>data.werewolf[0]});
 let released=0,requested=0;
 class FailingLoader {
  load(name,success,progress,failure){requested++;const texture=new THREE.Texture();texture.addEventListener('dispose',()=>released++);
   setTimeout(()=>name.endsWith('head_normal.jpg')?failure():success(texture),name.endsWith('teeth_diffuse.jpg')?5:0);return texture;}
 }
 await assert.rejects(Wildlife.load({...THREE,TextureLoader:FailingLoader},'werewolf'),/Missing texture/);assertions++;
 check('failed texture batch settles late loads and disposes all6 successful newly acquired maps',()=>{equal(requested,7);equal(released,6);});
 for(const type of ['tiger','werewolf'])loaded[type].maps.forEach(map=>map.dispose());
}finally{global.fetch=originalFetch;}
console.log(JSON.stringify({checksPassed:checks,assertions,realThreeRevision:THREE.REVISION,tigerVertices:1165,
 werewolfVertices:11970,werewolfTriangles:16808,werewolfMorphFrames:0,poseCases:18,mouthSamples:21,
 browserWebglExecuted:false,cameraExecuted:false},null,2));
}
main().catch(error=>{console.error(error);process.exitCode=1;});