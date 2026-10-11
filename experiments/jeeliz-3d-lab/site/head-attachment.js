(function(root,factory) {
  'use strict';
  const api=factory();
  if(typeof module!=='undefined'&&module.exports)module.exports=api;
  else root.HeadAttachment=api;
})(typeof window!=='undefined'?window:globalThis,function() {
  'use strict';
  // Profiles are in the tracker head's local space, never screen pixels. Keep
  // every authored mesh's dimensions intact; correct its attachment parent.
  const profiles=Object.freeze({
    dogEars:Object.freeze({position:Object.freeze([0,0.08,-0.35]),scale:1}),
    dogMuzzle:Object.freeze({position:Object.freeze([0,0,0.4]),scale:1}),
    dogHead:Object.freeze({position:Object.freeze([0,0.22,-0.65]),scale:1})
  });
  function anchor(THREE,name,profile) {
    if(!profile||!Array.isArray(profile.position)||profile.position.length!==3||
        !profile.position.every(Number.isFinite)||!Number.isFinite(profile.scale)||profile.scale<=0)
      throw new Error('Invalid head-local attachment profile.');
    const object=new THREE.Group();object.name=name;
    object.position.fromArray(profile.position);object.scale.setScalar(profile.scale);
    return object;
  }
  function occluder(THREE,data,profile) {
    if(!data)throw new Error('The head occlusion geometry is missing.');
    const group=anchor(THREE,'Head-local depth proxy',profile);
    const geometry=new THREE.BufferGeometryLoader().parse(data);
    const mesh=new THREE.Mesh(geometry,new THREE.MeshBasicMaterial({
      colorWrite:false,depthWrite:true,depthTest:true,side:THREE.FrontSide
    }));
    mesh.name='Depth-only head';mesh.renderOrder=-1000;mesh.frustumCulled=false;
    group.add(mesh);return {group:group,mesh:mesh};
  }
  return Object.freeze({profiles:profiles,anchor:anchor,occluder:occluder});
});
