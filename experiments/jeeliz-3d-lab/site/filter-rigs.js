(function(root,factory) {
  'use strict';
  const api=factory();
  if(typeof module!=='undefined'&&module.exports) module.exports=api;
  else root.FilterRigs=api;
})(typeof window!=='undefined'?window:globalThis,function() {
  'use strict';
  const configurations={
    helmet:{models:['assets/helmet/helmet.json','assets/helmet/visiere.json','assets/helmet/face.json'],
      textures:['assets/helmet/diffuse_helmet.jpg']},
    mask:{models:['assets/mask/mask.json'],textures:['assets/mask/mask.png']}
  };
  function extension(type) {
    return [window.WildlifeRigs,window.OriginalRigs].find(function(provider){
      return provider&&provider.supports(type);
    });
  }
  async function load(THREE,type) {
    const provider=extension(type);
    if(provider)return provider.load(THREE,type);
    if(type==='dog') return window.DogRig.load(THREE);
    const config=configurations[type];
    if(!config) throw new Error('Unknown filter.');
    const data=await Promise.all(config.models.map(async function(path) {
      const response=await fetch(path);
      if(!response.ok) throw new Error('Missing model: '+path);
      return response.json();
    }));
    const maps=await Promise.all(config.textures.map(function(path) {
      return new Promise(function(resolve,reject) {
        new THREE.TextureLoader().load(path,function(texture) {
          texture.encoding=THREE.sRGBEncoding;resolve(texture);
        },undefined,function(){reject(new Error('Missing texture: '+path));});
      });
    }));
    return {data:data,maps:maps};
  }
  function create(THREE,type,data,maps,showReferenceHead) {
    const provider=extension(type);
    if(provider)return provider.create(THREE,type,data,maps,showReferenceHead);
    if(type==='dog') {
      const dog=window.DogRig.create(THREE,data,maps);dog.hasMouth=true;return dog;
    }
    const group=new THREE.Group();
    const loader=new THREE.BufferGeometryLoader();
    let occluder=null,helmet=null,visor=null,mask=null;
    const texture=maps?maps[0]:null;
    if(type==='helmet') {
      helmet=new THREE.Mesh(loader.parse(data[0]),new THREE.MeshPhongMaterial({
        map:texture,shininess:50,specular:0x8c8c8c
      }));
      helmet.name='Official Rupy helmet';
      visor=new THREE.Mesh(loader.parse(data[1]),new THREE.MeshStandardMaterial({
        color:0xd5e5ed,transparent:true,opacity:0.35,roughness:0.15,metalness:0.1,
        side:THREE.FrontSide,depthWrite:false
      }));
      visor.name='Transparent helmet visor';
      [helmet,visor].forEach(function(mesh) {
        mesh.scale.setScalar(0.037);mesh.position.set(0,-0.3,-0.5);mesh.rotation.x=0.5;
        mesh.frustumCulled=false;group.add(mesh);
      });
      const faceGeometry=loader.parse(data[2]);
      faceGeometry.computeVertexNormals();
      occluder=new THREE.Mesh(faceGeometry,new THREE.MeshBasicMaterial({
        colorWrite:false,depthWrite:true,depthTest:true,side:THREE.FrontSide
      }));
      occluder.name='Authored head depth occluder';
      occluder.scale.setScalar(1.12);occluder.position.set(0,0.3,-0.25);
      occluder.renderOrder=-1000;occluder.frustumCulled=false;group.add(occluder);
      if(showReferenceHead) {
        const reference=new THREE.Mesh(faceGeometry.clone(),new THREE.MeshPhongMaterial({
          color:0x8d9692,shininess:8
        }));
        reference.name='Model-preview head reference';
        reference.scale.copy(occluder.scale);reference.position.copy(occluder.position);
        group.add(reference);
      }
    } else if(type==='mask') {
      mask=new THREE.Mesh(loader.parse(data[0]),new THREE.MeshLambertMaterial({
        map:texture,transparent:true,alphaTest:0.02,side:THREE.FrontSide
      }));
      mask.name='Official Anonymous mask';
      mask.scale.setScalar(0.065);mask.position.set(0,-0.75,0.35);
      mask.frustumCulled=false;group.add(mask);
    } else throw new Error('Unknown filter.');
    function dispose() {
      group.traverse(function(object){
        if(object.geometry)object.geometry.dispose();
        if(object.material)object.material.dispose();
      });
    }
    return {hasMouth:false,group:group,helmet:helmet,visor:visor,mask:mask,occluder:occluder,
      setMouth:function(){},dispose:dispose};
  }
  return Object.freeze({load:load,create:create});
});
