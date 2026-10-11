(function(root,factory) {
  'use strict';
  const api=factory();
  if(typeof module!=='undefined'&&module.exports)module.exports=api;
  else root.LocalModel=api;
})(typeof window!=='undefined'?window:globalThis,function() {
  'use strict';
  const limits=Object.freeze({bytes:25*1024*1024,jsonBytes:2*1024*1024,nodes:512,
    primitives:256,vertices:150000,triangles:150000,images:16,textureSize:4096,
    imagePixels:48*1024*1024,accessors:1024,bufferViews:1024,accessorComponents:6000000,depth:32});
  const components={5120:[1,'getInt8'],5121:[1,'getUint8'],5122:[2,'getInt16'],
    5123:[2,'getUint16'],5125:[4,'getUint32'],5126:[4,'getFloat32']};
  const sizes={SCALAR:1,VEC2:2,VEC3:3,VEC4:4};
  const safeExtensions=new Set(['KHR_materials_unlit']);
  function reject(message) {throw new Error(message);}
  function requireTrue(value,message) {if(!value)reject(message);}
  function object(value,message) {requireTrue(value&&typeof value==='object'&&!Array.isArray(value),message);return value;}
  function integer(value,min,max,message) {requireTrue(Number.isSafeInteger(value)&&value>=min&&value<=max,message);return value;}
  function finite(value,min,max,message) {requireTrue(Number.isFinite(value)&&value>=min&&value<=max,message);return value;}
  function list(json,key,max,required) {
    const value=json[key];if(value===undefined&&!required)return [];
    requireTrue(Array.isArray(value)&&value.length<=max&&(!required||value.length>0),'Invalid or oversized '+key+' list.');return value;
  }
  function reference(index,array,message) {integer(index,0,array.length-1,message);return array[index];}
  function vector(value,n,min,max,message) {
    requireTrue(Array.isArray(value)&&value.length===n,message);value.forEach(v=>finite(v,min,max,message));
  }
  function imageSize(bytes,mime) {
    const view=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
    if(mime==='image/png') {
      requireTrue(bytes.length>=33&&[137,80,78,71,13,10,26,10].every((v,i)=>bytes[i]===v)&&
        view.getUint32(8)===13&&view.getUint32(12)===0x49484452,'The embedded PNG header is invalid.');
      return [view.getUint32(16),view.getUint32(20)];
    }
    requireTrue(mime==='image/jpeg'&&bytes.length>=4&&bytes[0]===255&&bytes[1]===216,'Use embedded PNG or JPEG images.');
    let p=2;
    while(p<bytes.length) {
      requireTrue(bytes[p++]===255,'The embedded JPEG header is invalid.');
      while(p<bytes.length&&bytes[p]===255)p++;
      requireTrue(p<bytes.length,'The embedded JPEG header is truncated.');
      const marker=bytes[p++];
      if(marker===0xd9||marker===0xda)break;
      if(marker===0x01||(marker>=0xd0&&marker<=0xd8))continue;
      requireTrue(p+2<=bytes.length,'The embedded JPEG header is truncated.');
      const length=view.getUint16(p);requireTrue(length>=2&&p+length<=bytes.length,'The embedded JPEG segment is invalid.');
      if([0xc0,0xc1,0xc2].includes(marker)) {
        requireTrue(length>=8,'The embedded JPEG dimensions are missing.');
        return [view.getUint16(p+5),view.getUint16(p+3)];
      }
      if(marker>=0xc0&&marker<=0xcf&&![0xc4,0xc8,0xcc].includes(marker))reject('Export this image as standard baseline or progressive JPEG.');
      p+=length;
    }
    reject('The embedded JPEG dimensions are missing.');
  }
  function preflight(input) {
    requireTrue(input instanceof ArrayBuffer,'Choose a binary .glb file.');
    requireTrue(input.byteLength>=28&&input.byteLength<=limits.bytes,'Choose a GLB smaller than 25 MiB.');
    const view=new DataView(input);
    requireTrue(view.getUint32(0,true)===0x46546c67&&view.getUint32(4,true)===2,'Export a binary glTF 2.0 (.glb) model.');
    requireTrue(view.getUint32(8,true)===input.byteLength,'The GLB file length is invalid.');
    const jsonLength=view.getUint32(12,true);
    requireTrue(view.getUint32(16,true)===0x4e4f534a&&jsonLength>0&&jsonLength<=limits.jsonBytes&&jsonLength%4===0&&20+jsonLength+8<=input.byteLength,'The GLB JSON chunk is invalid or too large.');
    let json;
    try {json=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(new Uint8Array(input,20,jsonLength)));}
    catch(error) {reject('The GLB metadata is not valid UTF-8 JSON.');}
    object(json,'The GLB metadata must be an object.');
    requireTrue(json.asset&&json.asset.version==='2.0'&&(!json.asset.minVersion||json.asset.minVersion==='2.0'),'Export glTF version 2.0.');
    const binHeader=20+jsonLength,binLength=view.getUint32(binHeader,true),binStart=binHeader+8;
    requireTrue(view.getUint32(binHeader+4,true)===0x004e4942&&binLength%4===0&&binStart+binLength===input.byteLength,'Use one embedded binary GLB chunk.');
    let walked=0;
    function inspect(value,depth) {
      requireTrue(depth<=48&&++walked<=150000,'The GLB metadata is too complex.');
      if(typeof value==='number')requireTrue(Number.isFinite(value),'The GLB contains a non-finite number.');
      if(!value||typeof value!=='object')return;
      if(Array.isArray(value)){value.forEach(v=>inspect(v,depth+1));return;}
      for(const key of Object.keys(value)) {
        requireTrue(!['__proto__','prototype','constructor'].includes(key),'The GLB contains an unsupported metadata key.');
        if(key==='uri')reject('Embed all buffers and PNG/JPEG textures in one GLB; external files and links are not supported.');
        if(key==='extensions') {
          object(value[key],'Invalid extension metadata.');
          for(const name of Object.keys(value[key]))requireTrue(safeExtensions.has(name),'Unsupported '+name+'. Export without compression or optional extensions.');
        }
        inspect(value[key],depth+1);
      }
    }
    inspect(json,0);
    for(const key of ['extensionsUsed','extensionsRequired'])
      for(const name of list(json,key,16,false))requireTrue(safeExtensions.has(name),'Unsupported '+name+'. Export without Draco, KTX or compression.');
    requireTrue(!list(json,'animations',1,false).length&&!list(json,'skins',1,false).length,'Export a static model without animations or skinning.');
    requireTrue(!list(json,'cameras',1,false).length,'Remove cameras from the exported model.');
    const buffers=list(json,'buffers',1,true);
    requireTrue(buffers.length===1&&Number.isSafeInteger(buffers[0].byteLength)&&buffers[0].byteLength>0&&buffers[0].byteLength<=binLength&&binLength-buffers[0].byteLength<=3,'The embedded buffer length is invalid.');
    const bvs=list(json,'bufferViews',limits.bufferViews,true),accessors=list(json,'accessors',limits.accessors,true);
    const images=list(json,'images',limits.images,false),textures=list(json,'textures',limits.images,false);
    const samplers=list(json,'samplers',32,false),materials=list(json,'materials',128,false);
    const meshes=list(json,'meshes',128,true),nodes=list(json,'nodes',limits.nodes,true),scenes=list(json,'scenes',8,true);
    if(json.scene!==undefined)reference(json.scene,scenes,'The default scene is invalid.');
    bvs.forEach(bv=>{
      object(bv,'Invalid buffer view.');requireTrue(bv.buffer===0,'Only the embedded GLB buffer is supported.');
      const start=integer(bv.byteOffset===undefined?0:bv.byteOffset,0,buffers[0].byteLength,'Invalid buffer offset.');
      const length=integer(bv.byteLength,1,buffers[0].byteLength,'Invalid buffer length.');
      requireTrue(start+length<=buffers[0].byteLength,'A buffer view exceeds its buffer.');
      if(bv.byteStride!==undefined)requireTrue(Number.isInteger(bv.byteStride)&&bv.byteStride>=4&&bv.byteStride<=252&&bv.byteStride%4===0,'Invalid interleaved buffer stride.');
      if(bv.target!==undefined)requireTrue([34962,34963].includes(bv.target),'Invalid buffer view target.');
    });
    // Cap total work before scanning any binary data. Repeated/unused accessors
    // can alias the same buffer, so the file byte limit alone cannot bound reads.
    let accessorComponents=0;
    accessors.forEach(a=>{
      object(a,'Invalid geometry accessor.');
      const size=sizes[a.type];requireTrue(size,'Unsupported geometry component format.');
      const count=integer(a.count,1,450000,'A geometry accessor is too large.');
      accessorComponents+=count*size;
      requireTrue(accessorComponents<=limits.accessorComponents,'Reduce duplicated or unused geometry accessors; the model exceeds the validation work budget.');
    });
    const descriptors=accessors.map(a=>{
      object(a,'Invalid geometry accessor.');requireTrue(!a.sparse,'Export without sparse geometry accessors.');
      const bv=reference(a.bufferView,bvs,'Geometry must use an embedded buffer view.');
      const info=components[a.componentType],size=sizes[a.type];requireTrue(info&&size,'Unsupported geometry component format.');
      const count=integer(a.count,1,450000,'A geometry accessor is too large.');
      const local=integer(a.byteOffset===undefined?0:a.byteOffset,0,bv.byteLength,'Invalid geometry offset.');
      const stride=bv.byteStride||info[0]*size,start=(bv.byteOffset||0)+local;
      requireTrue(local%info[0]===0&&start%info[0]===0&&stride%info[0]===0&&stride>=info[0]*size&&local+(count-1)*stride+size*info[0]<=bv.byteLength,'Geometry exceeds its buffer view.');
      if(a.normalized!==undefined)requireTrue(typeof a.normalized==='boolean','Invalid normalized geometry flag.');
      requireTrue(!a.normalized||![5125,5126].includes(a.componentType),'Only 8-bit or 16-bit integer accessors can be normalized.');
      for(const key of ['min','max'])if(a[key]!==undefined)vector(a[key],size,-1e20,1e20,'Invalid geometry bounds.');
      const descriptor={start:binStart+start,stride,count,size,component:a.componentType,normalized:!!a.normalized,bytes:info[0],read:info[1]};
      if(a.componentType===5126)for(let i=0;i<count;i++)for(let c=0;c<size;c++)
        finite(view.getFloat32(descriptor.start+i*stride+c*4,true),-1e8,1e8,'Geometry contains an invalid or excessive coordinate.');
      return descriptor;
    });
    let pixels=0;
    images.forEach(image=>{
      object(image,'Invalid embedded image.');const bv=reference(image.bufferView,bvs,'Textures must be embedded PNG/JPEG images.');
      requireTrue(!bv.byteStride,'Images cannot use interleaved buffer views.');
      const dimension=imageSize(new Uint8Array(input,binStart+(bv.byteOffset||0),bv.byteLength),image.mimeType);
      dimension.forEach(v=>integer(v,1,limits.textureSize,'Reduce each texture to 4096 pixels per side or smaller.'));
      pixels+=dimension[0]*dimension[1];requireTrue(pixels<=limits.imagePixels,'Reduce the total texture resolution.');
    });
    samplers.forEach(s=>{
      object(s,'Invalid texture sampler.');
      if(s.magFilter!==undefined)requireTrue([9728,9729].includes(s.magFilter),'Invalid texture magnification filter.');
      if(s.minFilter!==undefined)requireTrue([9728,9729,9984,9985,9986,9987].includes(s.minFilter),'Invalid texture reduction filter.');
      for(const key of ['wrapS','wrapT'])if(s[key]!==undefined)requireTrue([33071,33648,10497].includes(s[key]),'Invalid texture wrapping.');
    });
    textures.forEach(t=>{object(t,'Invalid texture.');reference(t.source,images,'Invalid texture image.');if(t.sampler!==undefined)reference(t.sampler,samplers,'Invalid texture sampler.');});
    function tex(value) {
      object(value,'Invalid material texture.');reference(value.index,textures,'Invalid material texture reference.');
      requireTrue(value.texCoord===undefined||value.texCoord===0,'Export material texture coordinates as UV set 0.');
    }
    materials.forEach(m=>{
      object(m,'Invalid material.');requireTrue(!m.alphaMode||['OPAQUE','MASK','BLEND'].includes(m.alphaMode),'Invalid material alpha mode.');
      if(m.alphaCutoff!==undefined)finite(m.alphaCutoff,0,1,'Invalid material alpha cutoff.');
      if(m.doubleSided!==undefined)requireTrue(typeof m.doubleSided==='boolean','Invalid double-sided material flag.');
      const p=m.pbrMetallicRoughness||{};object(p,'Invalid PBR material.');
      if(p.baseColorFactor!==undefined)vector(p.baseColorFactor,4,0,1,'Invalid base colour.');
      for(const key of ['metallicFactor','roughnessFactor'])if(p[key]!==undefined)finite(p[key],0,1,'Invalid PBR factor.');
      for(const key of ['baseColorTexture','metallicRoughnessTexture'])if(p[key]!==undefined)tex(p[key]);
      if(m.emissiveFactor!==undefined)vector(m.emissiveFactor,3,0,1,'Invalid emissive colour.');
      for(const key of ['normalTexture','occlusionTexture','emissiveTexture'])if(m[key]!==undefined)tex(m[key]);
      if(m.normalTexture&&m.normalTexture.scale!==undefined)finite(m.normalTexture.scale,-100,100,'Invalid normal strength.');
      if(m.occlusionTexture&&m.occlusionTexture.strength!==undefined)finite(m.occlusionTexture.strength,0,1,'Invalid occlusion strength.');
    });
    let primitives=0;const indexMaximums=new Map();
    const meshStats=meshes.map(mesh=>{
      object(mesh,'Invalid mesh.');requireTrue(!mesh.weights,'Export a static model without morph targets.');
      requireTrue(Array.isArray(mesh.primitives)&&mesh.primitives.length>0,'The model has an empty mesh.');
      let vertices=0,triangles=0;
      for(const p of mesh.primitives) {
        requireTrue(++primitives<=limits.primitives,'Reduce the model to 256 mesh primitives or fewer.');
        object(p,'Invalid mesh primitive.');requireTrue(!p.targets,'Export a static model without morph targets.');
        requireTrue(p.mode===undefined||p.mode===4,'Triangulate the model before exporting.');
        object(p.attributes,'A mesh is missing its vertex attributes.');
        const position=reference(p.attributes.POSITION,descriptors,'A mesh is missing its positions.');
        requireTrue(position.component===5126&&position.size===3,'Use floating-point XYZ positions.');
        for(const key of Object.keys(p.attributes)) {
          requireTrue(['POSITION','NORMAL','TANGENT','COLOR_0'].includes(key)||/^TEXCOORD_[0-7]$/.test(key),'Unsupported vertex attribute '+key+'. Export a static triangle mesh.');
          const a=reference(p.attributes[key],descriptors,'Invalid vertex attribute reference.');
          requireTrue(a.count===position.count,'Vertex attribute lengths do not match.');
          if(['POSITION','NORMAL'].includes(key))requireTrue(a.component===5126&&a.size===3,'Invalid XYZ vertex attribute.');
          if(key==='TANGENT')requireTrue(a.component===5126&&a.size===4,'Invalid tangent vertex attribute.');
          if(key.startsWith('TEXCOORD')) {
            requireTrue(a.size===2&&[5121,5123,5126].includes(a.component),'Invalid UV vertex attribute.');
            requireTrue(a.component===5126||a.normalized,'Normalize 8-bit or 16-bit integer UV coordinates before exporting.');
          }
          if(key==='COLOR_0')requireTrue([3,4].includes(a.size)&&[5121,5123,5126].includes(a.component),'Invalid vertex colour attribute.');
        }
        let count=position.count;
        if(p.indices!==undefined) {
          const index=reference(p.indices,descriptors,'Invalid triangle index reference.');
          requireTrue(index.size===1&&[5121,5123,5125].includes(index.component),'Use unsigned triangle indices.');
          count=index.count;
          // Scan each index accessor once, even if many instances reuse it.
          if(!indexMaximums.has(p.indices)){
            let maximum=0;
            for(let i=0;i<count;i++)maximum=Math.max(maximum,view[index.read](index.start+i*index.stride,true));
            indexMaximums.set(p.indices,maximum);
          }
          requireTrue(indexMaximums.get(p.indices)<position.count,'A triangle index exceeds the mesh positions.');
        }
        requireTrue(count%3===0,'Triangulate the model before exporting.');
        if(p.material!==undefined) {
          const material=reference(p.material,materials,'Invalid mesh material reference.'),pbr=material.pbrMetallicRoughness||{};
          const textured=['baseColorTexture','metallicRoughnessTexture'].some(key=>pbr[key]!==undefined)||
            ['normalTexture','occlusionTexture','emissiveTexture'].some(key=>material[key]!==undefined);
          requireTrue(!textured||p.attributes.TEXCOORD_0!==undefined,'A textured mesh needs UV coordinates in TEXCOORD_0. UV-map it before exporting.');
        }
        vertices+=position.count;triangles+=count/3;
      }
      return {vertices,triangles,meshes:mesh.primitives.length};
    });
    const parents=new Array(nodes.length).fill(0);
    nodes.forEach(n=>{
      object(n,'Invalid model node.');requireTrue(n.skin===undefined&&!n.weights&&n.camera===undefined,'Export static meshes without bones, morphs or cameras.');
      if(n.mesh!==undefined)reference(n.mesh,meshes,'Invalid node mesh reference.');
      if(n.matrix!==undefined) {
        requireTrue(n.translation===undefined&&n.rotation===undefined&&n.scale===undefined,'Use either a matrix or separate node transforms.');
        vector(n.matrix,16,-1e6,1e6,'Invalid node transform.');
        requireTrue(n.matrix[3]===0&&n.matrix[7]===0&&n.matrix[11]===0&&n.matrix[15]===1,'Node transforms must be affine.');
      }
      if(n.translation!==undefined)vector(n.translation,3,-1e6,1e6,'Invalid node position.');
      if(n.scale!==undefined)vector(n.scale,3,-10000,10000,'Invalid node scale.');
      if(n.rotation!==undefined) {
        vector(n.rotation,4,-1,1,'Invalid node rotation.');
        requireTrue(Math.abs(n.rotation.reduce((s,v)=>s+v*v,0)-1)<.01,'Normalize the model node rotations.');
      }
      if(n.children!==undefined) {
        requireTrue(Array.isArray(n.children)&&n.children.length<=limits.nodes,'Invalid node children.');
        n.children.forEach(index=>{reference(index,nodes,'Invalid child node reference.');requireTrue(++parents[index]<=1,'A node has multiple parents.');});
      }
    });
    let vertices=0,triangles=0,drawnMeshes=0;
    function walk(index,visiting,depth,count) {
      requireTrue(depth<=limits.depth&&!visiting.has(index),'The model contains a cycle or is nested too deeply.');
      const n=nodes[index];visiting.add(index);
      if(count&&n.mesh!==undefined) {const s=meshStats[n.mesh];vertices+=s.vertices;triangles+=s.triangles;drawnMeshes+=s.meshes;}
      for(const child of n.children||[])walk(child,visiting,depth+1,count);
      visiting.delete(index);
    }
    // Validate even unused nodes; unused malicious cycles must not reach a loader.
    nodes.forEach((n,i)=>walk(i,new Set(),0,false));
    scenes.forEach(s=>{
      object(s,'Invalid model scene.');requireTrue(Array.isArray(s.nodes)&&s.nodes.length>0&&s.nodes.length<=limits.nodes,'The model scene is empty or too large.');
      const unique=new Set();s.nodes.forEach(index=>{reference(index,nodes,'Invalid scene node.');requireTrue(parents[index]===0&&!unique.has(index),'Scene roots must be unique parentless nodes.');unique.add(index);walk(index,new Set(),0,true);});
    });
    requireTrue(drawnMeshes>0&&vertices<=limits.vertices&&triangles<=limits.triangles,'Reduce the model to 150,000 drawn vertices and triangles or fewer.');
    return {json,stats:Object.freeze({bytes:input.byteLength,vertices,triangles,meshes:drawnMeshes,textures:textures.length,materials:materials.length,imagePixels:pixels})};
  }
  function resources(scene) {
    const geometries=new Set(),materials=new Set(),textures=new Set();
    scene.traverse(o=>{
      if(o.geometry)geometries.add(o.geometry);
      const mats=Array.isArray(o.material)?o.material:[o.material];
      mats.filter(Boolean).forEach(m=>{materials.add(m);Object.keys(m).forEach(k=>{if(m[k]&&m[k].isTexture)textures.add(m[k]);});});
    });
    return {geometries,materials,textures,dispose:function(){
      textures.forEach(t=>t.dispose());materials.forEach(m=>m.dispose());geometries.forEach(g=>g.dispose());
    }};
  }
  // Exact owner-supplied file fingerprints identify these reviewed scene axes
  // and forehead-band anchors. Filenames and uploaded metadata are not trusted.
  const capProfiles=Object.freeze({
    '2ef37cee54fb735323c7813632ea955d5ee586757c8cad7ff6860d4fb6d773f2':Object.freeze({name:'White tennis visor',front:1,bandY:.3106876611709595,bandZ:1.061342716217041}),
    '0cfc418e8bfda81fab7c59d4b065b7ba12cfdd83bdb4d3376655daf914caf4ba':Object.freeze({name:'Pokemon Masters cap',front:-1,bandY:526.87109375,bandZ:-867.6121826171875}),
    '2a155da0da40320b4237912316b2394b604319df892a32368c233a2e877fbf6e':Object.freeze({name:'Pokemon Masters cap',front:-1,bandY:526.87109375,bandZ:-867.6121826171875})
  });
  async function fingerprint(buffer) {
    if(typeof crypto==='undefined'||!crypto.subtle)return '';
    const bytes=new Uint8Array(await crypto.subtle.digest('SHA-256',buffer));
    return Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('');
  }
  function createAsset(THREE,gltf,name,info,hash) {
    const owned=[];const seen=new Set();
    for(const scene of gltf.scenes||[gltf.scene])if(scene&&!seen.has(scene)){seen.add(scene);owned.push(resources(scene));}
    const all={geometries:new Set(),materials:new Set(),textures:new Set()};
    owned.forEach(r=>Object.keys(all).forEach(key=>r[key].forEach(v=>all[key].add(v))));
    let disposed=false;const active=new Set();
    function disposeResources() {Object.keys(all).reverse().forEach(key=>all[key].forEach(v=>v.dispose()));}
    try {
      const scene=gltf.scene;requireTrue(scene&&scene.isObject3D,'The GLB has no renderable scene.');
      scene.updateMatrixWorld(true);
      scene.traverse(o=>{
        if(o.geometry) {
          const p=o.geometry.attributes.position;requireTrue(p&&p.count>0,'The model has an empty mesh.');
          for(let i=0;i<p.array.length;i++)requireTrue(Number.isFinite(p.array[i]),'The model contains invalid geometry.');
          // Do not trust uploaded accessor min/max as normalization bounds.
          o.geometry.computeBoundingBox();
          if(!o.geometry.attributes.normal)o.geometry.computeVertexNormals();
          o.frustumCulled=false;
        }
        requireTrue(o.matrixWorld.elements.every(Number.isFinite),'The model transforms are invalid.');
      });
      const box=new THREE.Box3().setFromObject(scene),size=box.getSize(new THREE.Vector3()),center=box.getCenter(new THREE.Vector3());
      requireTrue([size.x,size.y,size.z,center.x,center.y,center.z].every(Number.isFinite)&&size.x>1e-8&&Math.max(size.x,size.y,size.z)<=1e8,'The model has empty or invalid bounds.');
      requireTrue(size.y/size.x<=20&&size.z/size.x<=20,'Rotate the model to face +Z with a usable horizontal width before exporting.');
      const normalize=2/size.x;
      const stats=Object.freeze(Object.assign({},info.stats,{width:size.x,height:size.y,depth:size.z}));
      const profile=capProfiles[hash];
      // One uniform scale preserves the source geometry. Reviewed caps seat the
      // front band above the eyes; other models start at the face, then use Fit.
      const initialFit=Object.freeze(profile?{
        scale:.7,x:0,y:.55-(profile.bandY-center.y)*normalize*.7,
        z:.25-profile.front*(profile.bandZ-center.z)*normalize*.7,
        rx:0,ry:profile.front===-1?180:0,rz:0,occlusion:true
      }:{scale:.6,x:0,y:-.05,z:.5,rx:0,ry:0,rz:0,occlusion:true});
      const fitName=profile?profile.name+' forehead-band fit':'Face-centred starting fit';
      function createRig(fit) {
        requireTrue(!disposed,'This uploaded model was removed. Upload it again.');
        const group=new THREE.Group();group.name='Uploaded local GLB';
        const fitted=new THREE.Group(),normalized=new THREE.Group(),copy=scene.clone(true);
        normalized.scale.setScalar(normalize);normalized.position.copy(center).multiplyScalar(-normalize);
        normalized.add(copy);fitted.add(normalized);group.add(fitted);
        let released=false;
        function updateFit(value) {
          requireTrue(!released&&!disposed,'This uploaded model is no longer active.');value=value||{};
          const scale=value.scale===undefined?1:value.scale;finite(scale,.1,5,'Model size must be between 10% and 500%.');
          const xyz=['x','y','z'].map(key=>value[key]===undefined?0:finite(value[key],-5,5,'Model offsets must stay between -5 and 5.'));
          const angles=['rx','ry','rz'].map(key=>value[key]===undefined?0:finite(value[key],-180,180,'Model rotation must stay between -180 and 180 degrees.'));
          fitted.scale.setScalar(scale);fitted.position.fromArray(xyz);fitted.rotation.set(angles[0]*Math.PI/180,angles[1]*Math.PI/180,angles[2]*Math.PI/180,'XYZ');
        }
        function dispose() {if(released)return;released=true;active.delete(dispose);if(group.parent)group.parent.remove(group);group.remove(fitted);}
        try {updateFit(fit);}catch(error){dispose();throw error;}
        active.add(dispose);
        return {group,hasMouth:false,setMouth:function(){},updateFit,dispose};
      }
      return {name,stats,initialFit,fitName,createRig,dispose:function(){if(disposed)return;active.forEach(fn=>fn());disposed=true;disposeResources();}};
    } catch(error) {disposeResources();throw error;}
  }
  async function loadFile(THREE,file) {
    requireTrue(file&&typeof file.name==='string'&&/\.glb$/i.test(file.name),'Choose a binary .glb model.');
    requireTrue(file.size!==0,'The selected GLB is empty. Choose the actual downloaded file.');
    requireTrue(Number.isSafeInteger(file.size)&&file.size>0&&file.size<=limits.bytes,'Choose a GLB smaller than 25 MiB.');
    requireTrue(typeof file.arrayBuffer==='function','This browser cannot read that local file.');
    requireTrue(THREE&&typeof THREE.GLTFLoader==='function','The local GLB loader is unavailable. Reload this page.');
    const buffer=await file.arrayBuffer();requireTrue(buffer.byteLength===file.size,'The local file changed while it was being read.');
    const info=preflight(buffer),hash=await fingerprint(buffer),blobURLs=new Set();
    const manager=new THREE.LoadingManager();
    manager.setURLModifier(function(url){requireTrue(typeof url==='string'&&url.startsWith('blob:'),'Only embedded local model textures are allowed.');blobURLs.add(url);return url;});
    function revoke() {if(typeof URL!=='undefined'&&URL.revokeObjectURL)blobURLs.forEach(url=>URL.revokeObjectURL(url));blobURLs.clear();}
    const name=file.name.replace(/[\x00-\x1f\x7f]/g,'').slice(0,120)||'Local model.glb';
    try {
      const gltf=await new Promise((resolve,rejectLoad)=>{
        try {new THREE.GLTFLoader(manager).parse(buffer,'',resolve,rejectLoad);}catch(error){rejectLoad(error);}
      });
      return createAsset(THREE,gltf,name,info,hash);
    } catch(error) {
      throw new Error('Could not read this GLB. '+(error&&error.message?error.message:'Check its embedded textures and export it again.'));
    } finally {revoke();}
  }
  return Object.freeze({loadFile,preflight,limits});
});
