'use strict';
// Real pinned Three r97 + GLTFLoader parsing. Only browser image decode is supplied
// by a deterministic texture I/O fixture; no camera/WebGL claim.
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm'),assert=require('node:assert/strict');
const site=path.resolve(__dirname,'../site'),THREE=require(path.join(site,'vendor/three.js'));
const Model=require(path.join(site,'local-model.js'));
let checks=0,textureLoads=0,revokes=0,createdTextures=[];
const nativeRevoke=URL.revokeObjectURL.bind(URL);
const sandbox={THREE,console,window:{URL:{createObjectURL:URL.createObjectURL.bind(URL),revokeObjectURL:url=>{revokes++;nativeRevoke(url);}}},Blob,URL,TextDecoder,TextEncoder,ArrayBuffer,Uint8Array,DataView};
vm.createContext(sandbox);vm.runInContext(fs.readFileSync(path.join(site,'vendor/GLTFLoader.js'),'utf8'),sandbox);
const OriginalTextureLoader=THREE.TextureLoader;
THREE.TextureLoader=function(manager){this.manager=manager;};
THREE.TextureLoader.prototype.setCrossOrigin=function(){return this;};
THREE.TextureLoader.prototype.load=function(url,onLoad,progress,onError){
  const resolved=this.manager.resolveURL(url);assert.match(resolved,/^blob:/);textureLoads++;
  const texture=new THREE.Texture();texture.image={width:1,height:1};createdTextures.push(texture);
  queueMicrotask(()=>onLoad(texture));return texture;
};
function makeGLB(change,imageBytes) {
  const positions=new Float32Array([-1,-1,0,1,-1,0,0,1,.4]),indices=new Uint16Array([0,1,2]);
  const uv=new Float32Array([0,0,1,0,.5,1]);
  const png=imageBytes||Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLbtAAAAABJRU5ErkJggg==','base64');
  const chunks=[Buffer.from(positions.buffer),Buffer.from(indices.buffer),Buffer.alloc(2),Buffer.from(uv.buffer),png];
  const binary=Buffer.concat(chunks);
  const json={asset:{version:'2.0'},scene:0,scenes:[{nodes:[0]}],nodes:[{mesh:0}],
    buffers:[{byteLength:binary.length}],bufferViews:[{buffer:0,byteOffset:0,byteLength:36},{buffer:0,byteOffset:36,byteLength:6},{buffer:0,byteOffset:44,byteLength:24},{buffer:0,byteOffset:68,byteLength:png.length}],
    accessors:[{bufferView:0,componentType:5126,count:3,type:'VEC3',min:[-1,-1,0],max:[1,1,.4]},
      {bufferView:1,componentType:5123,count:3,type:'SCALAR'},
      {bufferView:2,componentType:5126,count:3,type:'VEC2'}],
    meshes:[{primitives:[{attributes:{POSITION:0,TEXCOORD_0:2},indices:1,material:0}]}],
    materials:[{pbrMetallicRoughness:{baseColorTexture:{index:0},metallicFactor:0,roughnessFactor:.7}}],
    textures:[{source:0}],images:[{bufferView:3,mimeType:'image/png'}]};
  if(change)change(json,binary);
  const text=Buffer.from(JSON.stringify(json)),padding=Buffer.alloc((4-text.length%4)%4,32),data=Buffer.concat([text,padding]);
  const bin=Buffer.concat([binary,Buffer.alloc((4-binary.length%4)%4)]),header=Buffer.alloc(20),bh=Buffer.alloc(8);
  header.writeUInt32LE(0x46546c67,0);header.writeUInt32LE(2,4);header.writeUInt32LE(28+data.length+bin.length,8);
  header.writeUInt32LE(data.length,12);header.writeUInt32LE(0x4e4f534a,16);
  bh.writeUInt32LE(bin.length,0);bh.writeUInt32LE(0x004e4942,4);
  const result=Buffer.concat([header,data,bh,bin]);return result.buffer.slice(result.byteOffset,result.byteOffset+result.byteLength);
}
function file(buffer,name='Original test visor.glb'){return {name,size:buffer.byteLength,arrayBuffer:async()=>buffer};}
async function check(name,fn){await fn();checks++;console.log('PASS '+checks+': '+name);}
function rejectMutation(fn,pattern){assert.throws(()=>Model.preflight(makeGLB(fn)),pattern);}
async function main(){
  await check('real pinned revision97 loader and browser/Node API are available',()=>{
    assert.equal(THREE.REVISION,'97');assert.equal(typeof THREE.GLTFLoader,'function');assert(Object.isFrozen(Model));assert(Object.isFrozen(Model.limits));
    const browser={};vm.createContext(browser);vm.runInContext(fs.readFileSync(path.join(site,'local-model.js'),'utf8'),browser);assert.equal(typeof browser.LocalModel.loadFile,'function');
  });
  await check('embedded indexed triangle, UV, PBR texture and exact dimensions pass preflight',()=>{
    const info=Model.preflight(makeGLB());assert.equal(info.stats.vertices,3);assert.equal(info.stats.triangles,1);assert.equal(info.stats.imagePixels,1);assert.equal(info.stats.textures,1);
  });
  await check('file magic, version, complete length, chunk type and trailing chunks fail before loader',()=>{
    for(const [offset,value] of [[0,0],[4,1],[8,1],[12,0],[16,0]]){const b=makeGLB();new DataView(b).setUint32(offset,value,true);assert.throws(()=>Model.preflight(b));}
    assert.throws(()=>Model.preflight(new ArrayBuffer(Model.limits.bytes+1)),/25 MiB/);assert.throws(()=>Model.preflight('{}'),/binary/);
  });
  await check('all external/network/data URIs and dangerous prototype metadata are rejected',()=>{
    for(const uri of ['https://example.com/a.png','file:///x','../x.png','data:image/png;base64,QQ==','blob:other'])
      rejectMutation(j=>j.images[0].uri=uri,/Embed all/);
    rejectMutation(j=>j.buffers[0].uri='remote.bin',/Embed all/);
    rejectMutation(j=>j.extras=JSON.parse('{"__proto__":{"polluted":true}}'),/unsupported metadata/);
    rejectMutation(j=>j.extras={constructor:{prototype:{x:true}}},/unsupported metadata/);assert.equal({}.polluted,undefined);
  });
  await check('compression, unknown extensions, animation, skinning and morph targets give export guidance',()=>{
    for(const extension of ['KHR_draco_mesh_compression','KHR_texture_basisu','EXT_meshopt_compression','KHR_lights_punctual'])
      rejectMutation(j=>j.extensionsUsed=[extension],/Unsupported/);
    rejectMutation(j=>j.meshes[0].primitives[0].extensions={KHR_draco_mesh_compression:{}},/Unsupported/);
    rejectMutation(j=>j.animations=[{}],/static model/);rejectMutation(j=>j.skins=[{}],/static model/);
    rejectMutation(j=>j.nodes[0].skin=0,/static meshes/);rejectMutation(j=>j.meshes[0].primitives[0].targets=[{}],/morph/);
    rejectMutation(j=>j.accessors[0].sparse={},/sparse/);rejectMutation(j=>j.cameras=[{}],/Remove cameras/);
  });
  await check('supported unlit extension is parsed and no extension can request external resources',()=>{
    Model.preflight(makeGLB(j=>{j.extensionsUsed=['KHR_materials_unlit'];j.materials[0].extensions={KHR_materials_unlit:{}};}));
    rejectMutation(j=>j.materials[0].extensions={KHR_materials_unlit:{uri:'https://example.com'}},/Embed all/);
  });
  await check('buffer offsets, strides, references, alignment and sparse accessors are bounded',()=>{
    rejectMutation(j=>j.bufferViews[0].byteOffset=1e9,/offset/);rejectMutation(j=>j.bufferViews[0].byteLength=1e9,/length/);
    rejectMutation(j=>j.bufferViews[0].byteStride=2,/stride/);rejectMutation(j=>j.accessors[0].byteOffset=1,/exceeds/);
    rejectMutation(j=>j.accessors[0].count=500001,/large/);rejectMutation(j=>j.accessors[0].bufferView=99,/embedded/);
    rejectMutation(j=>j.accessors[0].componentType=0,/component/);rejectMutation(j=>j.accessors[0].type='MAT4',/component/);
  });
  await check('nonfinite floats, mismatched attributes and out-of-range indices are rejected',()=>{
    rejectMutation((j,b)=>b.writeFloatLE(NaN,0),/invalid or excessive/);rejectMutation((j,b)=>b.writeFloatLE(Infinity,0),/invalid or excessive/);
    rejectMutation((j,b)=>b.writeUInt16LE(4,36),/index exceeds/);rejectMutation(j=>j.accessors[2].count=2,/lengths/);
    rejectMutation(j=>j.meshes[0].primitives[0].attributes.JOINTS_0=0,/Unsupported vertex/);
    rejectMutation(j=>j.meshes[0].primitives[0].mode=1,/Triangulate/);
    rejectMutation(j=>j.meshes[0].primitives[0].indices=99,/index reference/);
  });
  await check('node cycles, multiple parents, invalid transforms and deeply nested hierarchies fail',()=>{
    rejectMutation(j=>j.nodes[0].children=[0],/parentless|cycle/);
    rejectMutation(j=>{j.nodes=[{children:[1,2]},{children:[2]},{mesh:0}];},/multiple parents/);
    rejectMutation(j=>j.nodes[0].rotation=[0,0,0,0],/Normalize/);
    rejectMutation(j=>j.nodes[0].matrix=[1,0,0,1,0,1,0,0,0,0,1,0,0,0,0,1],/affine/);
    rejectMutation(j=>{j.nodes=Array.from({length:35},(_,i)=>i===34?{mesh:0}:{children:[i+1]});},/nested/);
    rejectMutation(j=>j.scenes[0].nodes=[0,0],/unique/);
  });
  await check('metadata/node/primitive/instanced geometry budgets prevent multiplicative bombs',()=>{
    rejectMutation(j=>j.nodes=Array.from({length:513},()=>({mesh:0})),/nodes list/);
    rejectMutation(j=>j.meshes[0].primitives=Array.from({length:257},()=>j.meshes[0].primitives[0]),/256/);
    rejectMutation(j=>{j.accessors[0].count=60000;},/exceeds/);
    rejectMutation(j=>{j.meshes[0].primitives=Array.from({length:256},()=>j.meshes[0].primitives[0]);j.nodes=Array.from({length:500},()=>({mesh:0}));j.scenes[0].nodes=j.nodes.map((_,i)=>i);},/150,000/);
    const huge=Array.from({length:150010},()=>0);rejectMutation(j=>j.extras=huge,/too complex/);
  });
  await check('overlapping unused accessors fail their work budget before scanning binary floats',()=>{
    rejectMutation(j=>{
      // Each accessor aliases the same small view. An offset check must not be
      // the first rejection: the total read-work budget wins before any scan.
      j.accessors=Array.from({length:1024},()=>({bufferView:0,componentType:5126,count:450000,type:'VEC4'}));
    },/validation work budget/);
  });
  await check('PNG dimensions, JPEG structure, texture limits and MIME are checked before decode',()=>{
    rejectMutation((j,b)=>b.writeUInt32BE(50000,68+16),/4096/);
    rejectMutation((j,b)=>b[68]=0,/PNG header/);rejectMutation(j=>j.images[0].mimeType='image/svg+xml',/PNG or JPEG/);
    rejectMutation(j=>j.images[0].bufferView=0,/PNG header/);rejectMutation(j=>j.textures[0].source=2,/texture image/);
    const jpeg=Buffer.from([255,216,255,192,0,17,8,0,1,0,1,3,1,17,0,2,17,0,3,17,0,255,217]);
    const valid=makeGLB(j=>j.images[0].mimeType='image/jpeg',jpeg);assert.equal(Model.preflight(valid).stats.imagePixels,1);
    jpeg[9]=255;jpeg[10]=255;assert.throws(()=>Model.preflight(makeGLB(j=>j.images[0].mimeType='image/jpeg',jpeg)),/4096/);
  });
  await check('material texture references, UV set and finite factors are validated',()=>{
    rejectMutation(j=>j.materials[0].pbrMetallicRoughness.baseColorTexture.index=3,/texture reference/);
    rejectMutation(j=>j.materials[0].pbrMetallicRoughness.baseColorTexture.texCoord=1,/UV set 0/);
    rejectMutation(j=>j.materials[0].pbrMetallicRoughness.roughnessFactor=2,/PBR factor/);
    rejectMutation(j=>j.materials[0].pbrMetallicRoughness.baseColorFactor=[1,1,1,2],/base colour/);
  });
  await check('every textured primitive requires UV0 before image or loader allocation',async()=>{
    const before=textureLoads;
    const keys=['baseColorTexture','metallicRoughnessTexture','normalTexture','occlusionTexture','emissiveTexture'];
    for(const key of keys) {
      const buffer=makeGLB(j=>{
        delete j.meshes[0].primitives[0].attributes.TEXCOORD_0;
        const material=j.materials[0];delete material.pbrMetallicRoughness.baseColorTexture;
        (['baseColorTexture','metallicRoughnessTexture'].includes(key)?material.pbrMetallicRoughness:material)[key]={index:0};
      });
      assert.throws(()=>Model.preflight(buffer),/textured mesh needs UV/);
      await assert.rejects(Model.loadFile(THREE,file(buffer)),/textured mesh needs UV/);
    }
    const untextured=makeGLB(j=>{delete j.meshes[0].primitives[0].attributes.TEXCOORD_0;delete j.materials[0].pbrMetallicRoughness.baseColorTexture;});
    assert.equal(Model.preflight(untextured).stats.vertices,3);
    assert.equal(textureLoads,before);
  });
  await check('integer UV accessors reject omitted or false normalization',()=>{
    for(const componentType of [5121,5123])for(const normalized of [undefined,false])
      rejectMutation(j=>{j.accessors[2].componentType=componentType;j.accessors[2].normalized=normalized;},/Normalize .* integer UV/);
  });
  await check('normalized integer UV0 remains compatible with the real pinned loader',async()=>{
    for(const [componentType,maximum,arrayType,write] of [[5121,255,'Uint8Array','writeUInt8'],[5123,65535,'Uint16Array','writeUInt16LE']]) {
      const bytesPerValue=componentType===5121?1:2,values=[0,0,maximum,0,Math.round(maximum/2),maximum];
      const buffer=makeGLB((j,b)=>{
        j.accessors[2].componentType=componentType;j.accessors[2].normalized=true;
        values.forEach((value,index)=>b[write](value,44+index*bytesPerValue));
      });
      assert.equal(Model.preflight(buffer).stats.vertices,3);
      const asset=await Model.loadFile(THREE,file(buffer)),rig=asset.createRig(asset.initialFit);let mesh;
      rig.group.traverse(o=>{if(o.isMesh)mesh=o;});
      assert.equal(mesh.geometry.attributes.uv.array.constructor.name,arrayType);
      assert.equal(mesh.geometry.attributes.uv.normalized,true);
      assert.deepEqual(Array.from(mesh.geometry.attributes.uv.array),values);
      assert(mesh.material.map.isTexture);rig.dispose();asset.dispose();
    }
  });
  await check('float and 32-bit unsigned accessors cannot request normalization',()=>{
    rejectMutation(j=>{j.accessors[0].normalized=true;},/Only 8-bit or 16-bit integer/);
    rejectMutation(j=>{j.accessors[2].normalized=true;},/Only 8-bit or 16-bit integer/);
    rejectMutation(j=>{j.accessors[0].componentType=5125;j.accessors[0].normalized=true;},/Only 8-bit or 16-bit integer/);
    assert.equal(Model.preflight(makeGLB(j=>{j.accessors[0].normalized=false;j.accessors[2].normalized=false;})).stats.vertices,3);
  });
  await check('unused extra UV channels from static exports remain compatible while materials use UV0',async()=>{
    const glb=makeGLB(j=>{j.meshes[0].primitives[0].attributes.TEXCOORD_2=2;});
    Model.preflight(glb);const asset=await Model.loadFile(THREE,file(glb));assert.equal(asset.stats.vertices,3);asset.dispose();
    rejectMutation(j=>j.meshes[0].primitives[0].attributes.TEXCOORD_2=99,/attribute reference/);
    rejectMutation(j=>j.materials[0].pbrMetallicRoughness.baseColorTexture.texCoord=2,/UV set 0/);
  });
  await check('real GLTFLoader parses embedded texture and retains authored proportions under fit',async()=>{
    const start=textureLoads,asset=await Model.loadFile(THREE,file(makeGLB()));assert.equal(textureLoads,start+1);
    assert.equal(asset.stats.width,2);assert.equal(asset.stats.height,2);assert(Math.abs(asset.stats.depth-.4)<1e-6);
    assert.equal(asset.name,'Original test visor.glb');const rig=asset.createRig({scale:1.4,x:.2,y:.3,z:-.2,ry:60});
    assert.equal(rig.hasMouth,false);assert.doesNotThrow(()=>rig.setMouth(NaN));rig.group.updateMatrixWorld(true);
    let mesh;rig.group.traverse(o=>{if(o.isMesh)mesh=o;});assert(mesh.material.isMeshStandardMaterial);assert(mesh.material.map.isTexture);
    const p=mesh.geometry.attributes.position,a=new THREE.Vector3().fromBufferAttribute(p,0).applyMatrix4(mesh.matrixWorld),b=new THREE.Vector3().fromBufferAttribute(p,1).applyMatrix4(mesh.matrixWorld);
    assert(Math.abs(a.distanceTo(b)-2.8)<1e-6);assert.deepEqual(rig.group.children[0].scale.toArray(),[1.4,1.4,1.4]);
    assert(Math.abs(rig.group.children[0].rotation.y-Math.PI/3)<1e-12);rig.dispose();asset.dispose();
  });
  await check('rig ownership and idempotent asset cleanup release shared resources once',async()=>{
    const asset=await Model.loadFile(THREE,file(makeGLB())),a=asset.createRig({}),b=asset.createRig({scale:.8});let mesh;
    a.group.traverse(o=>{if(o.isMesh)mesh=o;});let geometry=0,material=0,texture=0;
    mesh.geometry.addEventListener('dispose',()=>geometry++);mesh.material.addEventListener('dispose',()=>material++);mesh.material.map.addEventListener('dispose',()=>texture++);
    const parent=new THREE.Group();parent.add(a.group);parent.add(b.group);a.dispose();a.dispose();assert.equal(geometry+material+texture,0);assert.equal(parent.children.length,1);
    asset.dispose();asset.dispose();assert.deepEqual([geometry,material,texture],[1,1,1]);assert.equal(parent.children.length,0);
    assert.throws(()=>asset.createRig({}),/removed/);assert.throws(()=>b.updateFit({}),/no longer active/);
  });
  await check('invalid file type/size and read races fail without loader or texture allocation',async()=>{
    const before=textureLoads;await assert.rejects(Model.loadFile(THREE,file(makeGLB(),'a.gltf')),/binary .glb/);
    await assert.rejects(Model.loadFile(THREE,{name:'a.glb',size:Model.limits.bytes+1}),/25 MiB/);
    const bad=file(makeGLB());bad.size++;await assert.rejects(Model.loadFile(THREE,bad),/changed/);
    await assert.rejects(Model.loadFile(THREE,file(makeGLB(j=>j.images[0].uri='http://remote'))),/Embed all/);assert.equal(textureLoads,before);
  });
  await check('fit rejects nonfinite/out-of-range values without changing current transforms',async()=>{
    const asset=await Model.loadFile(THREE,file(makeGLB())),rig=asset.createRig({scale:1.2,x:.4});const before=rig.group.children[0].position.toArray();
    for(const value of [{scale:0},{x:Infinity},{ry:181},{rz:NaN}])assert.throws(()=>rig.updateFit(value));
    assert.deepEqual(rig.group.children[0].position.toArray(),before);assert.deepEqual(rig.group.children[0].scale.toArray(),[1.2,1.2,1.2]);
    rig.updateFit({scale:.5,x:-.5,rx:90});assert.equal(rig.group.children[0].rotation.x,Math.PI/2);asset.dispose();
  });
  await check('empty/invalid transformed bounds dispose successfully loaded resources on rejection',async()=>{
    const original=THREE.GLTFLoader.prototype.parse;let geometries=0,materials=0,textures=0;
    THREE.GLTFLoader.prototype.parse=function(data,path,onLoad){
      const geometry=new THREE.BufferGeometry().addAttribute('position',new THREE.BufferAttribute(new Float32Array([0,0,0,0,1,0,0,2,0]),3));
      const texture=new THREE.Texture(),material=new THREE.MeshStandardMaterial({map:texture});
      geometry.addEventListener('dispose',()=>geometries++);material.addEventListener('dispose',()=>materials++);texture.addEventListener('dispose',()=>textures++);
      const scene=new THREE.Scene();scene.add(new THREE.Mesh(geometry,material));onLoad({scene,scenes:[scene]});
    };
    try{await assert.rejects(Model.loadFile(THREE,file(makeGLB())),/invalid bounds/);assert.deepEqual([geometries,materials,textures],[1,1,1]);}
    finally{THREE.GLTFLoader.prototype.parse=original;}
  });
  await check('image decode failure revokes its only local URL and reports useful failure',async()=>{
    const old=THREE.TextureLoader.prototype.load,oldRevoke=URL.revokeObjectURL;let captured,cleanup=0;
    THREE.TextureLoader.prototype.load=function(url,onLoad,progress,onError){captured=this.manager.resolveURL(url);queueMicrotask(()=>onError(new Error('Image decode failed.')));};
    URL.revokeObjectURL=url=>{if(url===captured)cleanup++;nativeRevoke(url);};
    try{await assert.rejects(Model.loadFile(THREE,file(makeGLB())),/Image decode failed/);assert.match(captured,/^blob:/);assert.equal(cleanup,1);}
    finally{THREE.TextureLoader.prototype.load=old;URL.revokeObjectURL=oldRevoke;}
  });
  await check('forged accessor bounds cannot distort model fitting',async()=>{
    const asset=await Model.loadFile(THREE,file(makeGLB(j=>{j.accessors[0].min=[-100,-100,-100];j.accessors[0].max=[100,100,100];})));
    assert.equal(asset.stats.width,2);assert.equal(asset.stats.height,2);assert(Math.abs(asset.stats.depth-.4)<1e-6);asset.dispose();
  });
  await check('unused scenes are bounded and owned resources are released across every scene',async()=>{
    const asset=await Model.loadFile(THREE,file(makeGLB(j=>{j.scenes.push({nodes:[1]});j.nodes.push({mesh:0,translation:[3,0,0]});})));
    assert.equal(asset.stats.vertices,6);asset.dispose();
  });
  await check('unknown models use a frozen face-sized starting fit without trusting filename or metadata',async()=>{
    const asset=await Model.loadFile(THREE,file(makeGLB(),'white_tennis_cap.glb'));
    assert(Object.isFrozen(asset.initialFit));assert.equal(asset.initialFit.scale,.6);
    assert.equal(asset.initialFit.z,.5);assert.equal(asset.initialFit.ry,0);assert.match(asset.fitName,/Face-centred/);
    const rig=asset.createRig(asset.initialFit);rig.group.updateMatrixWorld(true);
    const bounds=new THREE.Box3().setFromObject(rig.group),size=bounds.getSize(new THREE.Vector3());
    assert(Math.abs(size.x-1.2)<1e-6);assert(Math.abs(size.y/size.x-1)<1e-6);
    rig.dispose();asset.dispose();
  });
  // Optional physical source files are never required by the portable verifier.
  const referenceIndex=process.argv.indexOf('--reference-models');
  if(referenceIndex!==-1){
    const directory=process.argv[referenceIndex+1];assert(directory,'Supply the owner model directory.');
    for(const [name,vertex,expectedLabel] of [['white_tennis_cap.glb',493,'White tennis visor'],['pokemon_masters_cap.glb',354,'Pokemon Masters cap'],['pokemon_masters_cap (1).glb',354,'Pokemon Masters cap']]){
      await check('exact owner GLB '+name+' keeps volume and seats its authored forehead-band vertex',async()=>{
        const bytes=fs.readFileSync(path.join(directory,name));
        const buffer=bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength);
        const asset=await Model.loadFile(THREE,file(buffer,name));assert.match(asset.fitName,new RegExp(expectedLabel));
        assert.equal(asset.initialFit.scale,.7);const rig=asset.createRig(asset.initialFit);
        rig.group.updateMatrixWorld(true);const meshes=[];rig.group.traverse(o=>{if(o.isMesh)meshes.push(o);});
        const mesh=meshes[0],p=mesh.geometry.attributes.position;
        const anchor=new THREE.Vector3().fromBufferAttribute(p,vertex).applyMatrix4(mesh.matrixWorld);
        assert(Math.abs(anchor.y-.55)<2e-5,'Forehead band Y='+anchor.y);
        assert(Math.abs(anchor.z-.25)<2e-5,'Forehead band Z='+anchor.z);
        const edge0=new THREE.Vector3().fromBufferAttribute(p,0),edge1=new THREE.Vector3().fromBufferAttribute(p,1);
        const sourceDistance=edge0.distanceTo(edge1);edge0.applyMatrix4(mesh.matrixWorld);edge1.applyMatrix4(mesh.matrixWorld);
        assert(sourceDistance>0&&edge0.distanceTo(edge1)>0);
        const bounds=new THREE.Box3().setFromObject(rig.group).getSize(new THREE.Vector3());
        assert(Math.abs(bounds.x-1.4)<2e-5);assert(Math.abs(bounds.y/bounds.x-asset.stats.height/asset.stats.width)<2e-5);
        assert(Math.abs(bounds.z/bounds.x-asset.stats.depth/asset.stats.width)<2e-5);
        const distance=edge0.distanceTo(edge1);rig.updateFit({...asset.initialFit,rx:35,ry:45,rz:20});rig.group.updateMatrixWorld(true);
        const posed0=new THREE.Vector3().fromBufferAttribute(p,0).applyMatrix4(mesh.matrixWorld),posed1=new THREE.Vector3().fromBufferAttribute(p,1).applyMatrix4(mesh.matrixWorld);
        assert(Math.abs(posed0.distanceTo(posed1)-distance)<2e-5);rig.dispose();asset.dispose();
      });
    }
  }
  console.log(JSON.stringify({checksPassed:checks,realThreeRevision:THREE.REVISION,realGLTFLoader:true,textureDecodesSuppliedByFixture:true,textureLoads,blobURLsRevokedOnSuccess:revokes,browserWebglExecuted:false,cameraExecuted:false},null,2));
}
main().finally(()=>{THREE.TextureLoader=OriginalTextureLoader;}).catch(error=>{console.error(error);process.exitCode=1;});
