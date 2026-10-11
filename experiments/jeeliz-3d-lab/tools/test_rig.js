'use strict';
// Actual pinned Three.js math/geometry/morph checks. No WebGL or camera claim.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'..'),site=path.join(root,'site');
const THREE=require(path.join(site,'vendor/three.js'));
const DogRig=require(path.join(site,'dog-rig.js'));
global.window={DogRig,HeadAttachment:require(path.join(site,'head-attachment.js'))};
const FilterRigs=require(path.join(site,'filter-rigs.js'));
const Pose=require(path.join(site,'pose.js'));
const read=name=>JSON.parse(fs.readFileSync(path.join(site,name),'utf8'));
let checks=0;
function check(name,run){run();checks++;console.log('PASS '+checks+': '+name);}
const helmetData=['helmet.json','visiere.json','face.json'].map(name=>read('assets/helmet/'+name));
const helmet=FilterRigs.create(THREE,'helmet',helmetData,null,false);
const mask=FilterRigs.create(THREE,'mask',[read('assets/mask/mask.json')],null,false);
const dog=DogRig.create(THREE,[...['dog_ears.json','dog_nose.json','dog_tongue.json'].map(name=>read('assets/dog/'+name)),read('assets/helmet/face.json')],null);
check('compatible actual pinned renderer revision97',()=>assert.equal(THREE.REVISION,'97'));
function scale(mesh,value){assert.deepEqual(mesh.scale.toArray(),[value,value,value]);}
check('helmet and visor preserve authored uniform scales and transforms',()=>{
 for(const mesh of [helmet.helmet,helmet.visor]){
  scale(mesh,0.037);assert.deepEqual(mesh.position.toArray(),[0,-0.3,-0.5]);assert.equal(mesh.rotation.x,0.5);
  assert(mesh.geometry.attributes.position.count>1000);assert(mesh.geometry.attributes.uv.count>1000);
 }
});
check('authored face proxy is depth-only and rendered first',()=>{
 scale(helmet.occluder,1.12);assert.deepEqual(helmet.occluder.position.toArray(),[0,0.3,-0.25]);
 assert.equal(helmet.occluder.material.colorWrite,false);
 assert.equal(helmet.occluder.material.depthWrite,true);
 assert.equal(helmet.occluder.material.depthTest,true);
 assert(helmet.occluder.renderOrder<helmet.helmet.renderOrder);
});
check('visor uses translucent real mesh with bounded physical material',()=>{
 assert(helmet.visor.material.transparent);
 assert(helmet.visor.material.opacity>0&&helmet.visor.material.opacity<1);
 assert.equal(helmet.visor.material.depthWrite,false);
});
check('mask preserves authored uniform scale, offset and genuine UV geometry',()=>{
 scale(mask.mask,0.065);assert.deepEqual(mask.mask.position.toArray(),[0,-0.75,0.35]);
 assert(mask.mask.geometry.attributes.position.count>1000);
 assert(mask.mask.geometry.attributes.uv.count>1000);
});
check('dog parts preserve three different upstream uniform scales and offsets',()=>{
 scale(dog.ears,0.025);assert.equal(dog.ears.position.y,-0.3);
 scale(dog.nose,0.018);assert.deepEqual(dog.nose.position.toArray(),[0,-0.05,0.15]);
 scale(dog.tongue,2);assert.equal(dog.tongue.position.y,-0.28);
});
check('actual tongue contains26 independent full3D morph geometries',()=>{
 assert.equal(dog.tongue.geometry.morphTargets.length,26);
 const base=dog.tongue.geometry.morphTargets[0].vertices;
 assert(base.length>300);
 assert(dog.tongue.geometry.morphTargets.at(-1).vertices.some((point,index)=>point.distanceTo(base[index])>0.001));
});
check('mouth0 hides tongue; mouth0.5 blends real frames12/13 with normalized weights',()=>{
 dog.setMouth(0);assert.equal(dog.tongue.visible,false);
 dog.setMouth(0.5);assert.equal(dog.tongue.visible,true);
 assert.equal(dog.tongue.morphTargetInfluences[12],0.5);
 assert.equal(dog.tongue.morphTargetInfluences[13],0.5);
 assert.equal(dog.tongue.morphTargetInfluences.reduce((a,b)=>a+b,0),1);
});
check('mouth bounds and nonfinite expression cannot generate invalid morph weights',()=>{
 for(const coefficient of [-5,0,0.8,1,8,NaN,Infinity]){
  dog.setMouth(coefficient);
  assert(dog.tongue.morphTargetInfluences.every(weight=>Number.isFinite(weight)&&weight>=0&&weight<=1));
  assert.equal(dog.tongue.morphTargetInfluences.reduce((a,b)=>a+b,0),1);
 }
});
const camera=new THREE.PerspectiveCamera(35,800/600,0.1,100);
const parent=new THREE.Object3D();parent.add(helmet.group);
const baseState={detected:1,x:0,y:0,s:0.37,rx:0,ry:0,rz:0};
check('head pose puts real rig in front of perspective camera with finite matrix',()=>{
 parent.visible=true;assert(Pose.apply(THREE,parent,baseState,camera));
 assert(parent.position.z<0);assert(parent.matrixWorld.elements.every(Number.isFinite));
});
check('yaw/pitch/roll rotate the whole authored rig without altering model proportions',()=>{
 const before=new THREE.Box3().setFromObject(helmet.helmet).getSize(new THREE.Vector3());
 Pose.apply(THREE,parent,{...baseState,rx:0.18,ry:0.7,rz:-0.12},camera);
 assert.equal(parent.rotation.order,'ZYX');assert.equal(parent.rotation.y,0.7);
 scale(helmet.helmet,0.037);scale(helmet.visor,0.037);
 assert(parent.matrixWorld.elements.every(Number.isFinite));
 assert(before.length()>0);
});
check('tracker hysteresis hides lost face and rejects NaN/zero-sized state',()=>{
 assert.equal(Pose.visibility(false,{...baseState,detected:0.9}),true);
 assert.equal(Pose.visibility(true,{...baseState,detected:0.8}),true);
 assert.equal(Pose.visibility(true,{...baseState,detected:0.4}),false);
 assert.equal(Pose.visibility(true,{...baseState,s:0}),false);
 assert.equal(Pose.visibility(true,{...baseState,ry:NaN}),false);
});
check('authored occluder intersects actual head-view rays and masks some farther helmet triangles',()=>{
 Pose.apply(THREE,parent,baseState,camera);camera.updateMatrixWorld(true);
 const ray=new THREE.Raycaster(),hitCounts={occluder:0,helmet:0,occludedRear:0};
 for(let y=-0.7;y<=0.7;y+=0.07)for(let x=-0.7;x<=0.7;x+=0.07){
  ray.setFromCamera(new THREE.Vector2(x,y),camera);
  const head=ray.intersectObject(helmet.occluder);
  const shell=ray.intersectObject(helmet.helmet);
  if(head.length)hitCounts.occluder++;if(shell.length)hitCounts.helmet++;
  if(head.length&&shell.some(hit=>hit.distance>head[0].distance))hitCounts.occludedRear++;
 }
 assert(hitCounts.occluder>0);assert(hitCounts.helmet>0);assert(hitCounts.occludedRear>0);
 console.log('Real geometry ray proof: '+JSON.stringify(hitCounts));
});
check('synthetic full helmet crown stays inside4:3viewport at yaw plus/minus35degrees',()=>{
 for(const yaw of [-35,0,35]){
  Pose.apply(THREE,parent,{...baseState,s:0.21,ry:yaw*Math.PI/180},camera);
  camera.updateMatrixWorld(true);camera.updateProjectionMatrix();
  const positions=helmet.helmet.geometry.attributes.position;
  let maxX=0,maxY=0;
  for(let i=0;i<positions.count;i++){
   const projected=new THREE.Vector3().fromBufferAttribute(positions,i).applyMatrix4(helmet.helmet.matrixWorld).project(camera);
   maxX=Math.max(maxX,Math.abs(projected.x));maxY=Math.max(maxY,Math.abs(projected.y));
  }
  console.log('Full helmet projection yaw '+yaw+': '+JSON.stringify({maxX,maxY}));
  assert(maxX<1&&maxY<1);
 }
});
const dogParent=new THREE.Group();dogParent.add(dog.group);
check('dog attachment parents and depth proxy share the tracked head, with no screen-space correction',()=>{
 assert.equal(dog.ears.parent,dog.earsAnchor);assert.equal(dog.nose.parent,dog.muzzleAnchor);
 assert.equal(dog.tongue.parent,dog.muzzleAnchor);
 assert.equal(dog.earsAnchor.parent,dog.group);assert.equal(dog.muzzleAnchor.parent,dog.group);
 assert.equal(dog.occluder.parent.parent,dog.group);
 assert.equal(dog.occluder.material.colorWrite,false);assert.equal(dog.occluder.material.depthWrite,true);
 assert(dog.occluder.renderOrder<dog.ears.renderOrder);assert.equal(dog.ears.material.depthTest,true);
});
check('crown attachment seats ear roots behind the muzzle instead of on the frontal nose plane',()=>{
 dog.group.updateMatrixWorld(true);
 // Independently measured authored lower/root vertex centroids, before any fitting.
 const rootLeft=new THREE.Vector3(-24.117646,38.396513,13.199873).applyMatrix4(dog.ears.matrixWorld);
 const noseFront=new THREE.Vector3(0,5,8).applyMatrix4(dog.nose.matrixWorld);
 assert(rootLeft.y>0.7&&rootLeft.y<0.9);assert(rootLeft.z<0.1);
 assert(noseFront.z-rootLeft.z>0.5);assert(rootLeft.x<-.5);
});
check('all dog attachments keep authored dimensions and fixed head-local separation through9 physical poses',()=>{
 const a=new THREE.Vector3(),b=new THREE.Vector3();let reference=null;
 const shape=dog.ears.geometry.attributes.position.array;
 const authored=read('assets/dog/dog_ears.json').data.attributes.position.array;
 assert.deepEqual(Array.from(shape),Array.from(Float32Array.from(authored)));
 for(const yaw of [-60,0,60])for(const pitch of [-20,0,20]){
  Pose.apply(THREE,dogParent,{...baseState,rx:pitch*Math.PI/180,ry:yaw*Math.PI/180,rz:0.2},camera);
  dog.ears.getWorldPosition(a);dog.nose.getWorldPosition(b);
  const distance=a.distanceTo(b);if(reference===null)reference=distance;
  assert(Math.abs(distance-reference)<1e-12);
  scale(dog.ears,.025);scale(dog.nose,.018);scale(dog.tongue,2);
 }
});
check('actual dog head rays hide far ear surfaces on both yaw directions, keeping near ear visible',()=>{
 const positions=dog.ears.geometry.attributes.position;
 for(const yaw of [-65,65]){
  Pose.apply(THREE,dogParent,{...baseState,s:.3,ry:yaw*Math.PI/180},camera);
  camera.updateMatrixWorld(true);
  const counts={left:{hidden:0,visible:0},right:{hidden:0,visible:0}};
  for(let i=0;i<positions.count;i+=3){
   const point=new THREE.Vector3().fromBufferAttribute(positions,i);
   const side=point.x<0?'left':'right';point.applyMatrix4(dog.ears.matrixWorld);
   const distance=point.length();
   const hits=new THREE.Raycaster(new THREE.Vector3(),point.clone().normalize()).intersectObject(dog.occluder);
   counts[side][hits.length&&hits[0].distance<distance-.0001?'hidden':'visible']++;
  }
  const far=counts[yaw<0?'left':'right'],near=counts[yaw<0?'right':'left'];
  assert(far.hidden>0);assert(far.hidden>near.hidden);assert(near.visible>100);
  console.log('Dog depth proof yaw '+yaw+': '+JSON.stringify(counts));
 }
});
check('bad attachment definitions fail before creating unusable geometry',()=>{
 const Attach=window.HeadAttachment;
 for(const profile of [null,{position:[0,0,NaN],scale:1},{position:[0,0],scale:1},
   {position:[0,0,0],scale:0},{position:[0,0,0],scale:Infinity}])
   assert.throws(()=>Attach.anchor(THREE,'invalid',profile));
 assert.throws(()=>Attach.occluder(THREE,null,Attach.profiles.dogHead));
});
const meshes=[helmet.helmet,helmet.visor,helmet.occluder,mask.mask,dog.ears,dog.nose,dog.tongue,dog.occluder];
check('all real mesh positions and triangle indices are finite and bounded',()=>{
 meshes.forEach(mesh=>{
  if(mesh.geometry.isBufferGeometry){
   const positions=mesh.geometry.attributes.position.array;
   assert(Array.from(positions).every(Number.isFinite));
   if(mesh.geometry.index)assert(Array.from(mesh.geometry.index.array).every(i=>i>=0&&i<mesh.geometry.attributes.position.count));
  }else{
   assert(mesh.geometry.vertices.every(point=>point.toArray().every(Number.isFinite)));
   assert(mesh.geometry.faces.every(face=>[face.a,face.b,face.c].every(i=>i>=0&&i<mesh.geometry.vertices.length)));
  }
 });
});
console.log(JSON.stringify({checksPassed:checks,realThreeRevision:THREE.REVISION,geometryMeshes:meshes.length,
 tongueMorphFrames:dog.tongue.geometry.morphTargets.length,browserWebglExecuted:false,cameraExecuted:false},null,2));
helmet.dispose();mask.dispose();dog.dispose();
