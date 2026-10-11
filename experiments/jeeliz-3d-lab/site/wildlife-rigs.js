(function(root,factory) {
  'use strict';
  const api=factory();
  if(typeof module!=='undefined'&&module.exports)module.exports=api;
  else root.WildlifeRigs=api;
})(typeof window!=='undefined'?window:globalThis,function() {
  'use strict';
  const configurations=Object.freeze({
    tiger:{model:'assets/tiger/TigerHead.json',textures:['headTexture2.png','white.png'],colors:[0,1]},
    werewolf:{model:'assets/werewolf/werewolf_not_animated.json',textures:[
      'head_diffuse.png','head_normal.jpg','head_alpha.jpg','fur_diffuse.jpg',
      'fur_normal.png','fur_alpha.jpg','teeth_diffuse.jpg'],colors:[0,3,6]}
  });
  function supports(type) {return typeof type==='string'&&Object.prototype.hasOwnProperty.call(configurations,type);}
  function configuration(type) {
    if(!supports(type))throw new Error('Unknown wildlife filter: '+String(type));
    return configurations[type];
  }
  async function load(THREE,type) {
    const config=configuration(type);
    const response=await fetch(config.model);
    if(!response.ok)throw new Error('Missing model: '+config.model);
    const data=[await response.json()];
    let failure=null;
    // Settle every requested texture before cleanup, including late successes.
    // Returned maps belong to the caller's asset cache; rigs only borrow them.
    const maps=await Promise.all(config.textures.map(function(name,index) {
      const path='assets/'+type+'/'+name;
      return new Promise(function(resolve,reject) {
        new THREE.TextureLoader().load(path,function(texture) {
          if(config.colors.indexOf(index)!==-1)texture.encoding=THREE.sRGBEncoding;
          resolve(texture);
        },undefined,function(){reject(new Error('Missing texture: '+path));});
      }).catch(function(error){failure=failure||error;return null;});
    }));
    if(failure){maps.forEach(function(map){if(map)map.dispose();});throw failure;}
    return {data:data,maps:maps};
  }
  function validateGeometry(geometry,materialCount) {
    function finite(values) {return Array.from(values).every(Number.isFinite);}
    function triangle(indices,count) {return indices.every(function(i){return Number.isInteger(i)&&i>=0&&i<count;});}
    if(geometry.isBufferGeometry) {
      const p=geometry.attributes.position,uv=geometry.attributes.uv;
      if(!p||p.itemSize!==3||p.count<3||!finite(p.array)||!uv||uv.itemSize!==2||uv.count!==p.count||!finite(uv.array))
        throw new Error('Invalid wildlife position or UV geometry.');
      const indices=geometry.index?Array.from(geometry.index.array):Array.from({length:p.count},function(_,i){return i;});
      if(!indices.length||indices.length%3||!triangle(indices,p.count))throw new Error('Invalid wildlife triangle indices.');
      if(!geometry.groups.length||geometry.groups.some(function(g){return !Number.isInteger(g.start)||g.start<0||
        !Number.isInteger(g.count)||g.count<=0||g.count%3||g.start+g.count>indices.length||
        !Number.isInteger(g.materialIndex)||g.materialIndex<0||g.materialIndex>=materialCount;}))
        throw new Error('Invalid wildlife material groups.');
    } else {
      const vertices=geometry.vertices,faces=geometry.faces,uvs=geometry.faceVertexUvs[0];
      if(!vertices||vertices.length<3||!vertices.every(function(v){return finite(v.toArray());})||!faces||!faces.length||
        !faces.every(function(f){return triangle([f.a,f.b,f.c],vertices.length)&&Number.isInteger(f.materialIndex)&&
          f.materialIndex>=0&&f.materialIndex<materialCount;})||!uvs||uvs.length!==faces.length||
        !uvs.every(function(face){return face.length===3&&face.every(function(uv){return finite(uv.toArray());});}))
        throw new Error('Invalid wildlife triangle or UV geometry.');
    }
    geometry.computeBoundingBox();
    const bounds=geometry.boundingBox;
    const size=[bounds.max.x-bounds.min.x,bounds.max.y-bounds.min.y,bounds.max.z-bounds.min.z];
    if(!finite(size)||size.some(function(value){return value<=0;}))throw new Error('Invalid wildlife volume.');
  }
  function tigerFaceMaterial(THREE,texture) {
    const material=new THREE.MeshLambertMaterial({map:texture,transparent:true,alphaTest:0.025,side:THREE.FrontSide});
    // The original demo blends a distorted orange video sample over enormous
    // eye areas. A transparent-overlay renderer needs genuine small apertures
    // instead: otherwise whole cheeks vanish and detached stripes remain.
    material.onBeforeCompile=function(shader) {
      shader.uniforms=shader.uniforms||{};
      shader.uniforms.tigerEyeRadii={value:new THREE.Vector2(0.105,0.055)};
      shader.vertexShader='varying vec3 vTigerPosition;\n'+shader.vertexShader;
      shader.vertexShader=shader.vertexShader.replace('#include <begin_vertex>',
        '#include <begin_vertex>\nvTigerPosition = position;');
      shader.fragmentShader='varying vec3 vTigerPosition;\nuniform vec2 tigerEyeRadii;\n'+shader.fragmentShader;
      shader.fragmentShader=shader.fragmentShader.replace('#include <alphatest_fragment>',[
        'vec2 eyeL = (vTigerPosition.xy - vec2(0.25,0.15)) / tigerEyeRadii;',
        'vec2 eyeR = (vTigerPosition.xy - vec2(-0.25,0.15)) / tigerEyeRadii;',
        'float eyeAlpha = smoothstep(0.82,1.08,length(eyeL)) * smoothstep(0.82,1.08,length(eyeR));',
        // Only the front eye shell opens. Nose, muzzle and rear head remain
        // opaque; stripes no longer bridge the opening as floating fragments.
        'float eyeDepth = smoothstep(0.18,0.23,vTigerPosition.z) * (1.0 - smoothstep(0.48,0.56,vTigerPosition.z));',
        'diffuseColor.a *= mix(1.0,eyeAlpha,eyeDepth);',
        '#include <alphatest_fragment>'
      ].join('\n'));
    };
    return material;
  }
  function create(THREE,type,data,maps) {
    const config=configuration(type);
    if(!Array.isArray(data)||data.length!==1||!data[0]||typeof data[0]!=='object')throw new Error('Missing wildlife model data.');
    if(maps!==null&&maps!==undefined&&(!Array.isArray(maps)||maps.length!==config.textures.length||
      !maps.every(function(map){return map&&map.isTexture;})))throw new Error('Invalid wildlife texture maps.');
    const group=new THREE.Group();group.name='Official Jeeliz '+type+' rig';
    const geometries=[],materials=[];
    let disposed=false,mesh=null,mouthCavity=null,basePositions=null,lastMouth=-1;
    function ownGeometry(geometry){geometries.push(geometry);return geometry;}
    function ownMaterial(material){materials.push(material);return material;}
    function map(index){return maps?maps[index]:null;}
    function dispose() {
      if(disposed)return;disposed=true;group.visible=false;
      Array.from(new Set(geometries)).forEach(function(geometry){geometry.dispose();});
      Array.from(new Set(materials)).forEach(function(material){material.dispose();});
      basePositions=null;
    }
    function setMouth(coefficient) {
      if(disposed||type!=='tiger')return;
      const mouth=Number.isFinite(coefficient)?Math.max(0,Math.min(1,coefficient)):0;
      if(mouth===lastMouth)return;lastMouth=mouth;
      const positions=mesh.geometry.attributes.position.array;
      const angle=mouth*Math.PI/12,cos=Math.cos(angle),sin=Math.sin(angle);
      for(let i=0;i<positions.length;i+=3) {
        const y=basePositions[i+1],z=basePositions[i+2];
        positions[i]=basePositions[i];
        // Same 15-degree lower-jaw hinge as the pinned tiger demo, evaluated
        // on the actual geometry so normals/bounds follow the visible mouth.
        positions[i+1]=y+z*0.2<=0?cos*y-sin*z:y;
        positions[i+2]=y+z*0.2<=0?sin*y+cos*z:z;
      }
      mesh.geometry.attributes.position.needsUpdate=true;
      mesh.geometry.computeVertexNormals();mesh.geometry.computeBoundingBox();mesh.geometry.computeBoundingSphere();
      mouthCavity.visible=mouth>0.025;group.userData.mouth=mouth;
    }
    try {
      if(type==='tiger') {
        const geometry=ownGeometry(new THREE.BufferGeometryLoader().parse(data[0]));
        validateGeometry(geometry,4);basePositions=new Float32Array(geometry.attributes.position.array);
        const skin=ownMaterial(tigerFaceMaterial(THREE,map(0)));
        const eyes=ownMaterial(tigerFaceMaterial(THREE,map(1)));
        mesh=new THREE.Mesh(geometry,[ownMaterial(new THREE.MeshLambertMaterial({color:0xffffff})),eyes,skin,
          ownMaterial(new THREE.MeshBasicMaterial({color:0x331100}))]);
        mesh.name='Textured tiger head';
        // Restore this asset's documented upstream (2,3,2) calibration.
        // Its source coordinates are compressed vertically for that demo;
        // imposing a scalar2 produced the short, overly wide mask. This is
        // fixed asset calibration, never a face-dependent deformation.
        mesh.scale.set(2,3,2);mesh.position.set(0,0.2,-0.48);
        mouthCavity=new THREE.Mesh(ownGeometry(new THREE.CircleBufferGeometry(1,40)),
          ownMaterial(new THREE.MeshBasicMaterial({color:0x170807,side:THREE.FrontSide})));
        mouthCavity.scale.set(0.24,0.3,1);
        mouthCavity.name='Recessed rounded tiger mouth interior';mouthCavity.position.set(0,-0.35,0.5);mouthCavity.frustumCulled=false;
        group.add(mesh,mouthCavity);setMouth(0);
      } else {
        // JSONLoader would build unused embedded materials. Suppress only
        // those temporary materials while retaining every authored face/UV.
        const geometry=ownGeometry(new THREE.JSONLoader().parse(Object.assign({},data[0],{materials:[]})).geometry);
        validateGeometry(geometry,3);geometry.computeVertexNormals();
        const head=ownMaterial(new THREE.MeshPhongMaterial({map:map(0),normalMap:map(1),alphaMap:map(2),
          side:THREE.FrontSide,shininess:10,transparent:true,alphaTest:0.035}));
        const fur=ownMaterial(new THREE.MeshPhongMaterial({map:map(3),normalMap:map(4),alphaMap:map(5),
          shininess:20,transparent:true,alphaTest:0.08,normalScale:new THREE.Vector2(2,2),depthWrite:false}));
        const teeth=ownMaterial(new THREE.MeshPhongMaterial({map:map(6),shininess:0,reflectivity:0}));
        mesh=new THREE.Mesh(geometry,[head,fur,teeth]);mesh.name='Static textured werewolf head';
        mesh.scale.setScalar(7);mesh.position.set(0,-1.2,-0.5);group.add(mesh);
      }
      mesh.frustumCulled=false;group.userData.filterType=type;group.userData.hasMouth=type==='tiger';
      return {group:group,mesh:mesh,mouthCavity:mouthCavity,hasMouth:type==='tiger',setMouth:setMouth,dispose:dispose};
    } catch(error){dispose();throw error;}
  }
  return Object.freeze({supports:supports,load:load,create:create});
});