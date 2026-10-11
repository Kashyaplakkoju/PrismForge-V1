"use strict";
// Real geometry through the public router, local-file asset acquisition only.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const site=path.resolve(__dirname,'../site');
const THREE=require(path.join(site,'vendor/three.js'));
global.window={HeadAttachment:require(path.join(site,'head-attachment.js'))};
window.DogRig=require(path.join(site,'dog-rig.js'));
window.WildlifeRigs=require(path.join(site,'wildlife-rigs.js'));
window.OriginalRigs=require(path.join(site,'original-rigs.js'));
const Rigs=require(path.join(site,'filter-rigs.js'));
const files=[];
global.fetch=async relative=>{
 assert(!relative.includes('..')&&!relative.includes(':')&&!relative.startsWith('/'));
 files.push(relative);return {ok:true,json:async()=>JSON.parse(fs.readFileSync(path.join(site,relative),'utf8'))};
};
// Stub only image transport in Node; geometry, materials, groups and disposal are real Three.
const ImageLoader=THREE.TextureLoader;
THREE.TextureLoader=class {load(relative,onLoad){
 assert(fs.statSync(path.join(site,relative)).size>0);files.push(relative);
 const texture=new THREE.Texture();onLoad(texture);return texture;
}};
let count=0;
function check(name,fn){fn();count++;console.log('PASS '+count+': '+name);}
async function main(){
 const types=['helmet','mask','dog','tiger','werewolf','astronaut','cyber'];
 const html=fs.readFileSync(path.join(site,'index.html'),'utf8');
 check('exactly seven distinct selectable 3D filters',()=>{
  const values=[...html.matchAll(/<option value="([a-z]+)">/g)].map(match=>match[1]);
  assert.deepEqual(values,types);
 });
 for(const type of types){
  const loaded=await Rigs.load(THREE,type);
  const rig=Rigs.create(THREE,type,loaded.data,loaded.maps,false);
  const allGeometries=new Set(),allMaterials=new Set();let meshCount=0;
  rig.group.traverse(object=>{
   if(!object.isMesh)return;meshCount++;
   if(object.geometry)allGeometries.add(object.geometry);
   for(const material of [].concat(object.material||[]))allMaterials.add(material);
  });
  check(type+' public route returns real renderable geometry and correct mouth capability',()=>{
   assert(rig.group.isObject3D&&meshCount>0);assert(allGeometries.size>0&&allMaterials.size>0);
   assert.equal(!!rig.hasMouth,type==='dog'||type==='tiger');
   for(const input of [-1,0,0.5,1,2,NaN,Infinity])rig.setMouth(input);
   rig.group.updateMatrixWorld(true);
   rig.group.traverse(object=>{assert(object.matrixWorld.elements.every(Number.isFinite));});
  });
  const geometriesDisposed=new Set(),materialsDisposed=new Set();let textureDisposals=0;
  for(const item of allGeometries)item.addEventListener('dispose',()=>geometriesDisposed.add(item));
  for(const item of allMaterials)item.addEventListener('dispose',()=>materialsDisposed.add(item));
  for(const texture of loaded.maps)texture.addEventListener('dispose',()=>textureDisposals++);
  rig.dispose();
  check(type+' disposes every owned mesh resource while retaining cached textures',()=>{
   assert.equal(geometriesDisposed.size,allGeometries.size);
   assert.equal(materialsDisposed.size,allMaterials.size);
   assert.equal(textureDisposals,0);
  });
 }
 const before=files.length;
 await assert.rejects(()=>Rigs.load(THREE,'nonexistent'),/Unknown filter/);
 check('unknown selection fails before acquiring assets',()=>assert.equal(files.length,before));
 THREE.TextureLoader=ImageLoader;
 console.log(JSON.stringify({checksPassed:count,realThreeRevision:THREE.REVISION,cameraRequested:false,
  imageTransportMocked:true,browserWebglExecuted:false,localAssetReads:files.length},null,2));
}
main().catch(error=>{console.error(error);process.exitCode=1;});
