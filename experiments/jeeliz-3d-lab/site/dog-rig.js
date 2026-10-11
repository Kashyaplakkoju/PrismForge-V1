(function(root, factory) {
  'use strict';
  const api = factory();
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.DogRig = api;
})(typeof window !== 'undefined' ? window : globalThis, function() {
  'use strict';
  const paths = ['assets/dog/dog_ears.json','assets/dog/dog_nose.json',
    'assets/dog/dog_tongue.json','assets/helmet/face.json'];
  const textures = ['texture_ears.jpg','alpha_ears_256.jpg','normal_ears.jpg',
    'texture_nose.jpg','normal_nose.jpg','dog_tongue.jpg','tongue_alpha_256.jpg'];
  async function load(THREE) {
    const data = await Promise.all(paths.map(async function(name) {
      const response = await fetch(name);
      if (!response.ok) throw new Error('Missing model: ' + name);
      return response.json();
    }));
    const maps = await Promise.all(textures.map(function(name) {
      return new Promise(function(resolve, reject) {
        new THREE.TextureLoader().load('assets/dog/' + name, resolve, undefined,
          function() { reject(new Error('Missing texture: ' + name)); });
      });
    }));
    return {data:data,maps:maps};
  }
  function create(THREE, data, maps) {
    const group = new THREE.Group();
    group.name = 'Official Jeeliz dog rig';
    const attachment=typeof window!=='undefined'?window.HeadAttachment:require('./head-attachment.js');
    const earsAnchor=attachment.anchor(THREE,'Crown attachment',attachment.profiles.dogEars);
    const muzzleAnchor=attachment.anchor(THREE,'Muzzle attachment',attachment.profiles.dogMuzzle);
    const head=attachment.occluder(THREE,data[3],attachment.profiles.dogHead);
    group.add(earsAnchor,muzzleAnchor,head.group);
    const geometryLoader = new THREE.BufferGeometryLoader();
    const earsGeometry = geometryLoader.parse(data[0]);
    const noseGeometry = geometryLoader.parse(data[1]);
    const tongueGeometry = new THREE.JSONLoader().parse(data[2]).geometry;
    tongueGeometry.computeVertexNormals();
    tongueGeometry.computeMorphNormals();
    const map = function(index) { return maps ? maps[index] : null; };
    const ears = new THREE.Mesh(earsGeometry, new THREE.MeshPhongMaterial({
      map:map(0),alphaMap:map(1),transparent:true,alphaTest:0.05,
      bumpMap:map(2),bumpScale:0.0075,shininess:1.5,specular:0xffffff,side:THREE.DoubleSide
    }));
    ears.name = 'Dog ears';
    ears.scale.setScalar(0.025);
    ears.position.y = -0.3;
    const nose = new THREE.Mesh(noseGeometry,new THREE.MeshPhongMaterial({
      map:map(3),bumpMap:map(4),bumpScale:0.005,shininess:1.5,specular:0xffffff
    }));
    nose.name = 'Dog nose';
    nose.scale.setScalar(0.018);
    nose.position.set(0,-0.05,0.15);
    const tongue = new THREE.Mesh(tongueGeometry,new THREE.MeshPhongMaterial({
      map:map(5),alphaMap:map(6),transparent:true,alphaTest:0.05,
      morphTargets:true,morphNormals:true,side:THREE.DoubleSide,shininess:5
    }));
    tongue.name = 'Animated dog tongue';
    tongue.scale.setScalar(2);
    tongue.position.y = -0.28;
    tongue.visible = false;
    [ears,nose,tongue].forEach(function(mesh) { mesh.frustumCulled=false; });
    earsAnchor.add(ears);muzzleAnchor.add(nose,tongue);
    function setMouth(coefficient) {
      const mouth = Number.isFinite(coefficient) ? Math.max(0,Math.min(1,coefficient)) : 0;
      tongue.visible = mouth > 0.12;
      const influences = tongue.morphTargetInfluences;
      influences.fill(0);
      const frame = mouth * (influences.length - 1);
      const first = Math.floor(frame), last = Math.min(first + 1,influences.length - 1);
      if (first === last) influences[first] = 1;
      else { influences[first] = 1 - (frame-first);influences[last] = frame-first; }
      tongue.userData.mouth = mouth;
    }
    function dispose() {
      group.traverse(function(object) {
        if (object.geometry) object.geometry.dispose();
        if (object.material) object.material.dispose();
      });
    }
    return {group:group,ears:ears,nose:nose,tongue:tongue,
      earsAnchor:earsAnchor,muzzleAnchor:muzzleAnchor,occluder:head.mesh,
      setMouth:setMouth,dispose:dispose};
  }
  function lights(THREE,scene) {
    scene.add(new THREE.AmbientLight(0xffffff,0.8));
    const key = new THREE.DirectionalLight(0xffffff,0.65);
    key.position.set(3,5,8);scene.add(key);
    const fill = new THREE.DirectionalLight(0xaecbff,0.25);
    fill.position.set(-4,1,3);scene.add(fill);
  }
  return Object.freeze({load:load,create:create,lights:lights});
});
