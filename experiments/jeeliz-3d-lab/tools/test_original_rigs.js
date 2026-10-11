'use strict';
// Actual pinned Three r97 geometry and physical-pose tests; no WebGL/camera claim.
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const assert=require('node:assert/strict');
const site=path.resolve(__dirname,'../site');
const THREE=require(path.join(site,'vendor/three.js'));
const Rigs=require(path.join(site,'original-rigs.js'));
const Pose=require(path.join(site,'pose.js'));
const head=JSON.parse(fs.readFileSync(path.join(site,'assets/helmet/face.json'),'utf8'));
let checks=0,poseCases=0,trianglesChecked=0;
async function check(name,fn) { await fn();checks++;console.log('PASS '+checks+': '+name); }
function meshes(rig) {
  const result=[];rig.group.traverse(o=>{if(o.geometry)result.push(o);});return result;
}
function materials(rig) {
  return new Set(meshes(rig).flatMap(o=>Array.isArray(o.material)?o.material:[o.material]));
}
function triangleAreaSquared(p,a,b,c) {
  const va=new THREE.Vector3().fromBufferAttribute(p,a);
  const vb=new THREE.Vector3().fromBufferAttribute(p,b);
  const vc=new THREE.Vector3().fromBufferAttribute(p,c);
  return vb.sub(va).cross(vc.sub(va)).lengthSq();
}
function inspectGeometry(rig) {
  let count=0;
  for(const object of meshes(rig)) {
    const g=object.geometry,p=g.attributes.position,n=g.attributes.normal,index=g.index;
    assert(p&&p.count>=3,object.name+' positions');
    assert(n&&n.count===p.count,object.name+' normals');
    assert(Array.from(p.array).every(Number.isFinite),object.name+' finite positions');
    assert(Array.from(n.array).every(Number.isFinite),object.name+' finite normals');
    const elements=index?index.count:p.count;
    assert.equal(elements%3,0);
    for(let i=0;i<elements;i+=3) {
      const a=index?index.getX(i):i,b=index?index.getX(i+1):i+1,c=index?index.getX(i+2):i+2;
      assert([a,b,c].every(value=>Number.isInteger(value)&&value>=0&&value<p.count),object.name+' indices');
      assert(triangleAreaSquared(p,a,b,c)>1e-20,object.name+' nondegenerate triangle '+i);
      count++;
    }
    g.computeBoundingBox();const size=g.boundingBox.getSize(new THREE.Vector3());
    assert(size.x>0&&size.y>0&&size.z>0,object.name+' has actual three-axis volume');
    assert.equal(object.frustumCulled,false);
  }
  trianglesChecked+=count;
  return count;
}
function colorWritingOpaque(object) {
  const values=Array.isArray(object.material)?object.material:[object.material];
  return values.some(m=>m.colorWrite&&(!m.transparent||m.opacity>=.99));
}
async function main() {
  await check('real vendored Three revision97 is loaded',()=>assert.equal(THREE.REVISION,'97'));
  await check('UMD supports only the two intended distinct types',()=>{
    for(const type of ['astronaut','cyber'])assert.equal(Rigs.supports(type),true);
    for(const type of ['',null,undefined,'helmet','mask','dog','__proto__','constructor'])
      assert.equal(Rigs.supports(type),false);
    assert(Object.isFrozen(Rigs));
    const sandbox={};vm.createContext(sandbox);
    vm.runInContext(fs.readFileSync(path.join(site,'original-rigs.js'),'utf8'),sandbox);
    assert.equal(sandbox.OriginalRigs.supports('astronaut'),true);
    assert.equal(sandbox.OriginalRigs.supports('cyber'),true);
  });
  await check('unknown types and missing head data fail before geometry allocation',async()=>{
    const oldFetch=global.fetch;let calls=0;
    global.fetch=async()=>{calls++;throw new Error('unexpected request');};
    try {
      for(const type of ['unknown',null,'__proto__']) {
        await assert.rejects(Rigs.load(THREE,type),/Unknown original filter/);
        assert.throws(()=>Rigs.create(THREE,type,[head],[]),/Unknown original filter/);
      }
      assert.throws(()=>Rigs.create(THREE,'astronaut',[],[]),/occlusion geometry/);
      assert.throws(()=>Rigs.create(THREE,'cyber',null,[]),/occlusion geometry/);
      assert.equal(calls,0);
    } finally { global.fetch=oldFetch; }
  });
  await check('load fetches only the existing local shared head asset and no textures',async()=>{
    const oldFetch=global.fetch,calls=[];
    global.fetch=async url=>{calls.push(url);return {ok:true,json:async()=>head};};
    try {
      for(const type of ['astronaut','cyber']) {
        const result=await Rigs.load(THREE,type);
        assert.equal(result.data.length,1);assert.equal(result.data[0],head);
        assert.deepEqual(result.maps,[]);
      }
      assert.deepEqual(calls,['assets/helmet/face.json','assets/helmet/face.json']);
      global.fetch=async()=>({ok:false});
      await assert.rejects(Rigs.load(THREE,'cyber'),/Missing model/);
    } finally { global.fetch=oldFetch; }
  });
  const rigs=['astronaut','cyber'].map(type=>({type,rig:Rigs.create(THREE,type,[head],[])}));
  const camera=new THREE.PerspectiveCamera(35,800/600,.1,100);
  camera.updateMatrixWorld(true);
  for(const {type,rig} of rigs) {
    await check(type+' all actual triangles are finite, bounded, nondegenerate and three-dimensional',()=>{
      const triangles=inspectGeometry(rig);
      assert(triangles>5000);assert(meshes(rig).length>=30);
      assert.equal(rig.hasMouth,false);
      for(const value of [0,1,NaN,Infinity])assert.doesNotThrow(()=>rig.setMouth(value));
      assert.equal(rig.group.userData.originalAuthorship,'PrismForge mathematically authored geometry');
    });
    await check(type+' keeps the fitted shared proxy behind front attachments and depth-only',()=>{
      assert.equal(rig.occluder.material.colorWrite,false);
      assert.equal(rig.occluder.material.depthWrite,true);
      assert.equal(rig.occluder.material.depthTest,true);
      assert.equal(rig.occluder.material.side,THREE.FrontSide);
      assert.equal(rig.occluder.renderOrder,-1000);
      assert.deepEqual(rig.occluder.parent.position.toArray(),[0,.22,-.65]);
      assert.deepEqual(rig.occluder.scale.toArray(),[1,1,1]);
      rig.group.updateMatrixWorld(true);
      const box=new THREE.Box3().setFromObject(rig.occluder);
      const front=type==='astronaut'?rig.visor:rig.mask;
      const frontBox=new THREE.Box3().setFromObject(front);
      assert(frontBox.max.z>box.max.z+.3);
    });
    await check(type+' both eye apertures have no opaque front surface in actual ray intersections',()=>{
      rig.group.updateMatrixWorld(true);
      for(const side of [-1,1])for(const x of [.20,.28,.36])for(const y of [.20,.28,.36]) {
        const ray=new THREE.Raycaster(new THREE.Vector3(side*x,y,2),new THREE.Vector3(0,0,-1));
        const heads=ray.intersectObject(rig.occluder);assert(heads.length>0);
        const opaque=ray.intersectObject(rig.group,true).filter(h=>colorWritingOpaque(h.object));
        assert(opaque.every(h=>h.distance>heads[0].distance-.0001),type+' clear eye '+[side*x,y]);
      }
      assert(rig.visor.material || Array.isArray(rig.visor.material));
      const clear=Array.isArray(rig.visor.material)?rig.visor.material[0]:rig.visor.material;
      assert(clear.transparent&&clear.opacity>0&&clear.opacity<.35);
      assert.equal(clear.depthWrite,false);assert.equal(clear.side,THREE.FrontSide);
    });
    await check(type+' 45 yaw/pitch/roll poses retain dimensions and fixed head-local placement',()=>{
      const parent=new THREE.Group();parent.add(rig.group);
      const samples=meshes(rig).map(object=>{
        const positions=object.geometry.attributes.position;
        return {object,points:[0,Math.floor(positions.count/3),positions.count-1].map(i=>
          new THREE.Vector3().fromBufferAttribute(positions,i)),array:Array.from(positions.array)};
      });
      let baselines=null;let maxX=0,maxY=0;
      for(const yaw of [-65,-35,0,35,65])for(const pitch of [-25,0,25])for(const roll of [-12,0,12]) {
        assert(Pose.apply(THREE,parent,{detected:1,x:0,y:0,s:.20,
          rx:pitch*Math.PI/180,ry:yaw*Math.PI/180,rz:roll*Math.PI/180},camera));
        const distances=samples.map(sample=>{
          const points=sample.points.map(p=>p.clone().applyMatrix4(sample.object.matrixWorld));
          points.forEach(point=>{
            assert(point.toArray().every(Number.isFinite));assert(point.z<-.1);
            const projected=point.clone().project(camera);
            maxX=Math.max(maxX,Math.abs(projected.x));maxY=Math.max(maxY,Math.abs(projected.y));
          });
          assert.deepEqual(sample.object.scale.toArray(),[1,1,1]);
          assert(sample.object.matrixWorld.elements.every(Number.isFinite));
          return [points[0].distanceTo(points[1]),points[0].distanceTo(points[2])];
        });
        if(!baselines)baselines=distances;
        distances.forEach((d,i)=>d.forEach((value,j)=>assert(Math.abs(value-baselines[i][j])<1e-11)));
        poseCases++;
      }
      for(const sample of samples)
        assert.deepEqual(Array.from(sample.object.geometry.attributes.position.array),sample.array);
      assert(maxX<1&&maxY<1,type+' synthetic .20 scale viewport bounds '+[maxX,maxY]);
      console.log(type+' sampled projection bounds '+JSON.stringify({maxX,maxY}));
      parent.remove(rig.group);
    });
    await check(type+' real head rays hide far-side geometry at yaw plus/minus65',()=>{
      const parent=new THREE.Group();parent.add(rig.group);
      for(const yaw of [-65,65]) {
        Pose.apply(THREE,parent,{detected:1,x:0,y:0,s:.21,rx:0,ry:yaw*Math.PI/180,rz:0},camera);
        const inverse=new THREE.Matrix4().getInverse(parent.matrixWorld);
        const counts={left:{hidden:0,visible:0},right:{hidden:0,visible:0}};
        for(const object of meshes(rig)) {
          if(object===rig.occluder)continue;
          const positions=object.geometry.attributes.position;
          for(let i=0;i<positions.count;i+=8) {
            const point=new THREE.Vector3().fromBufferAttribute(positions,i).applyMatrix4(object.matrixWorld);
            const local=point.clone().applyMatrix4(inverse),side=local.x<0?'left':'right';
            const hits=new THREE.Raycaster(new THREE.Vector3(),point.clone().normalize()).intersectObject(rig.occluder);
            counts[side][hits.length&&hits[0].distance<point.length()-.0001?'hidden':'visible']++;
          }
        }
        const far=counts[yaw<0?'left':'right'],near=counts[yaw<0?'right':'left'];
        assert(far.hidden>0);assert(far.hidden>near.hidden);assert(near.visible>100);
        console.log(type+' actual depth rays yaw '+yaw+' '+JSON.stringify(counts));
      }
      parent.remove(rig.group);
    });
    await check(type+' disposal releases every own geometry and material exactly once without shared textures',()=>{
      const geometries=new Set(meshes(rig).map(o=>o.geometry)),mats=materials(rig);
      let disposedGeometry=0,disposedMaterial=0,disposedTexture=0;
      geometries.forEach(g=>g.addEventListener('dispose',()=>disposedGeometry++));
      mats.forEach(m=>m.addEventListener('dispose',()=>disposedMaterial++));
      const texture=new THREE.Texture();texture.addEventListener('dispose',()=>disposedTexture++);
      mats.values().next().value.map=texture;
      // Detached parts still belong to the rig and must be released.
      const part=meshes(rig).find(o=>o!==rig.occluder);part.parent.remove(part);
      rig.dispose();rig.dispose();
      assert.equal(disposedGeometry,geometries.size);assert.equal(disposedMaterial,mats.size);
      assert.equal(disposedTexture,0);
    });
  }
  await check('the two designs differ in silhouette, topology and primary construction',()=>{
    const astro=Rigs.create(THREE,'astronaut',[head],[]),cyber=Rigs.create(THREE,'cyber',[head],[]);
    const a=new THREE.Box3().setFromObject(astro.shell).getSize(new THREE.Vector3());
    const c=new THREE.Box3().setFromObject(cyber.mask).getSize(new THREE.Vector3());
    assert(a.x>c.x*1.4);assert(a.z>c.z*1.3);
    assert(astro.shell&&!astro.mask);assert(cyber.mask&&!cyber.shell);
    assert(astro.shell.geometry.attributes.position.count>cyber.mask.geometry.attributes.position.count*4);
    astro.dispose();cyber.dispose();
  });
  await check('optional model-preview reference is owned and uses the same fitted head frame',()=>{
    const rig=Rigs.create(THREE,'cyber',[head],[],true);
    assert(rig.referenceHead);assert.equal(rig.referenceHead.parent,rig.occluder.parent);
    assert.notEqual(rig.referenceHead.geometry,rig.occluder.geometry);
    let count=0;rig.referenceHead.geometry.addEventListener('dispose',()=>count++);
    rig.dispose();assert.equal(count,1);
  });
  console.log(JSON.stringify({checksPassed:checks,realThreeRevision:THREE.REVISION,
    trianglesChecked,physicalPoseCases:poseCases,browserWebglExecuted:false,cameraExecuted:false},null,2));
}
main().catch(error=>{console.error(error);process.exitCode=1;});
