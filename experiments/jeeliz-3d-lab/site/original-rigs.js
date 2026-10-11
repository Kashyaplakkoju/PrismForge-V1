(function(root, factory) {
  'use strict';
  const api = factory();
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.OriginalRigs = api;
})(typeof window !== 'undefined' ? window : globalThis, function() {
  'use strict';
  // Original, mathematically authored geometry. Coordinates use the same local
  // head frame as HeadAttachment: +Z toward the camera and +Y toward the crown.
  const HEAD_PATH = 'assets/helmet/face.json';
  function supports(type) { return type === 'astronaut' || type === 'cyber'; }
  function requireType(type) {
    if (!supports(type)) throw new Error('Unknown original filter: ' + String(type));
  }
  async function load(THREE, type) {
    requireType(type);
    const response = await fetch(HEAD_PATH);
    if (!response.ok) throw new Error('Missing model: ' + HEAD_PATH);
    return {data:[await response.json()], maps:[]};
  }
  function create(THREE, type, data, maps, showReferenceHead) {
    requireType(type);
    if (!data || !data[0]) throw new Error('The head occlusion geometry is missing.');
    const attachment = typeof window !== 'undefined' ? window.HeadAttachment : require('./head-attachment.js');
    if (!attachment) throw new Error('The head attachment helper is missing.');
    const group = attachment.anchor(THREE, 'Original ' + type + ' head attachment',
      {position:[0,0,0],scale:1});
    const geometries = new Set(), materials = new Set();
    const ownGeometry = function(value) { geometries.add(value); return value; };
    const ownMaterial = function(value) { materials.add(value); return value; };
    const phong = function(settings) {
      return ownMaterial(new THREE.MeshPhongMaterial(Object.assign({
        color:0xbac9d3, specular:0xbecbd5, shininess:75, side:THREE.FrontSide
      },settings)));
    };
    const standard = function(settings) {
      return ownMaterial(new THREE.MeshStandardMaterial(Object.assign({
        color:0x657480, metalness:0.5, roughness:0.3, side:THREE.FrontSide
      },settings)));
    };
    const mesh = function(name, geometry, material, parent) {
      const value = new THREE.Mesh(ownGeometry(geometry),material);
      value.name=name;value.frustumCulled=false;(parent || group).add(value);
      return value;
    };
    function geometry(positions, indices) {
      const result = new THREE.BufferGeometry();
      result.addAttribute('position',new THREE.Float32BufferAttribute(positions,3));
      result.setIndex(indices);result.computeVertexNormals();
      result.computeBoundingBox();result.computeBoundingSphere();
      return result;
    }
    function tube(name, points, radius, material, closed) {
      const curve = new THREE.CatmullRomCurve3(points.map(function(point) {
        return new THREE.Vector3().fromArray(point);
      }));
      curve.closed=!!closed;curve.curveType='centripetal';
      return mesh(name,new THREE.TubeBufferGeometry(curve,Math.max(24,points.length*5),
        radius,8,!!closed),material);
    }
    function ellipse(name, center, rx, ry, radius, material, horizontal) {
      const points=[];
      for (let i=0;i<64;i++) {
        const a=i*Math.PI/32;
        points.push(horizontal ? [center[0]+rx*Math.cos(a),center[1],center[2]+ry*Math.sin(a)] :
          [center[0]+rx*Math.cos(a),center[1]+ry*Math.sin(a),center[2]]);
      }
      return tube(name,points,radius,material,true);
    }
    // Closed, faceted plate with a raised centre and a genuinely thick edge.
    // The front polygon must be counterclockwise when viewed from +Z.
    function plate(name, polygon, peak, thickness, faceMaterial, edgeMaterial) {
      const positions=peak.slice(), indices=[];
      polygon.forEach(function(point) { positions.push.apply(positions,point); });
      const backCentre=polygon.reduce(function(sum,p) {
        return [sum[0]+p[0]/polygon.length,sum[1]+p[1]/polygon.length,sum[2]+p[2]/polygon.length];
      },[0,0,0]);
      backCentre[2]-=thickness;
      positions.push.apply(positions,backCentre);
      polygon.forEach(function(p) { positions.push(p[0],p[1],p[2]-thickness); });
      const n=polygon.length,back=n+1;
      for(let i=0;i<n;i++) {
        const a=1+i,b=1+(i+1)%n;
        indices.push(0,a,b);
      }
      const frontCount=indices.length;
      for(let i=0;i<n;i++) {
        const a=1+i,b=1+(i+1)%n,c=back+a,d=back+b;
        indices.push(back,d,c,a,c,b,b,c,d);
      }
      const result=geometry(positions,indices);
      result.clearGroups();result.addGroup(0,frontCount,0);
      result.addGroup(frontCount,indices.length-frontCount,1);
      return mesh(name,result,[faceMaterial,edgeMaterial || faceMaterial]);
    }
    function cylinder(name, radius, depth, location, material, axis, sides) {
      const value=mesh(name,new THREE.CylinderBufferGeometry(radius,radius,depth,sides || 32),material);
      value.position.fromArray(location);
      if(axis==='x')value.rotation.z=Math.PI/2;
      else if(axis==='z')value.rotation.x=Math.PI/2;
      return value;
    }
    function bolt(name, position, radius, material, axis) {
      const value=cylinder(name,radius || .026,.015,position,material,axis || 'z',6);
      return value;
    }
    const head=attachment.occluder(THREE,data[0],attachment.profiles.dogHead);
    head.mesh.geometry.computeVertexNormals();
    ownGeometry(head.mesh.geometry);ownMaterial(head.mesh.material);group.add(head.group);
    head.mesh.name='Original rig depth-only head';
    let reference=null;
    if(showReferenceHead) {
      reference=mesh('Model-preview head reference',head.mesh.geometry.clone(),
        phong({color:0x59646c,specular:0x777777,shininess:12}),head.group);
      reference.renderOrder=-999;
    }
    let shell=null,visor=null,mask=null;
    if(type==='astronaut') {
      const pearl=phong({color:0xdde6e7,specular:0xe7f8ff,shininess:88});
      const pearlShade=phong({color:0xaebfc7,shininess:68});
      const graphite=phong({color:0x172431,specular:0x64819a,shininess:28});
      const rubber=phong({color:0x101920,specular:0x233442,shininess:9});
      const titanium=standard({color:0xb3c5cb,metalness:.62,roughness:.26});
      const cyan=phong({color:0x51dce2,emissive:0x146974,shininess:90});
      const rivet=phong({color:0xc6d5da,specular:0xffffff,shininess:110});
      const cx=0,cy=.07,cz=-.22,rx=1.08,ry=1.33,rz=1.12,opening=.91;
      function shellPoint(phi,theta,offset) {
        return [cx+(rx+offset)*Math.sin(phi)*Math.cos(theta),
          cy+(ry+offset)*Math.sin(phi)*Math.sin(theta),
          cz+(rz+offset)*Math.cos(phi)];
      }
      function shellBand(first,last,aroundFirst,aroundLast,offset,rings,segments) {
        const positions=[],indices=[];
        for(let j=0;j<=rings;j++)for(let i=0;i<=segments;i++)
          positions.push.apply(positions,shellPoint(first+(last-first)*j/rings,
            aroundFirst+(aroundLast-aroundFirst)*i/segments,offset));
        for(let j=0;j<rings;j++)for(let i=0;i<segments;i++) {
          const a=j*(segments+1)+i,b=a+segments+1;
          indices.push(a,b,a+1,a+1,b,b+1);
        }
        return geometry(positions,indices);
      }
      shell=mesh('Pearl helmet shell with an actual open face aperture',
        shellBand(opening,Math.PI-.045,0,Math.PI*2,0,40,96),pearl);
      shell.userData.faceOpening={width:2*rx*Math.sin(opening),height:2*ry*Math.sin(opening)};
      // Back cap closes the tiny rear ring, while the front stays completely open.
      const rear=[],rearIndices=[];
      rear.push(cx,cy,cz-rz);
      for(let i=0;i<=96;i++)rear.push.apply(rear,shellPoint(Math.PI-.045,i*Math.PI/48,0));
      for(let i=1;i<=96;i++)rearIndices.push(0,i+1,i);
      mesh('Closed rear helmet crown',geometry(rear,rearIndices),pearl);
      mesh('Raised crown service panel',
        shellBand(.96,2.2,1.16,1.98,.014,25,24),pearlShade);
      for(const theta of [0.72,2.42,3.86,5.56]) {
        const points=[];
        for(let i=0;i<=30;i++)points.push(shellPoint(.98+(2.78-.98)*i/30,theta,.013));
        tube('Recessed helmet panel seam',points,.008,graphite);
      }
      const rim=[];
      for(let i=0;i<96;i++)rim.push(shellPoint(opening,i*Math.PI/48,.006));
      tube('Thick pressure-window rubber seal',rim,.061,rubber,true);
      tube('Raised titanium window rim',rim.map(function(p) {
        return [p[0]*1.012,cy+(p[1]-cy)*1.012,p[2]+.034];
      }),.027,titanium,true);
      tube('Fine cyan window witness line',rim.map(function(p) {
        return [p[0]*.973,cy+(p[1]-cy)*.973,p[2]+.012];
      }),.008,cyan,true);
      // A convex clear cap spans the opening; there is no opaque face-covering sphere.
      const visorPositions=shellPoint(0,0,.018),visorIndices=[],visorRings=18,visorSegments=96;
      for(let j=1;j<=visorRings;j++)for(let i=0;i<=visorSegments;i++)
        visorPositions.push.apply(visorPositions,shellPoint(opening*j/visorRings,i*Math.PI/48,.018));
      for(let i=0;i<visorSegments;i++)visorIndices.push(0,1+i,2+i);
      for(let j=0;j<visorRings-1;j++)for(let i=0;i<visorSegments;i++) {
        const a=1+j*(visorSegments+1)+i,b=a+visorSegments+1;
        visorIndices.push(a,b,a+1,a+1,b,b+1);
      }
      visor=mesh('Convex clear pressure visor',geometry(visorPositions,visorIndices),
        phong({color:0xa7d9e6,specular:0xffffff,shininess:130,transparent:true,
          opacity:.10,depthWrite:false}));
      visor.renderOrder=20;
      const glare=ownMaterial(new THREE.MeshBasicMaterial({
        color:0xe3faff,transparent:true,opacity:.20,depthWrite:false,side:THREE.FrontSide
      }));
      const highlight=[];
      for(let i=0;i<=15;i++)highlight.push(shellPoint(.56+.16*i/15,2.05+.15*i/15,.026));
      tube('Visor edge reflection',highlight,.009,glare).renderOrder=21;
      for(const side of [-1,1]) {
        cylinder('Rubber communication pod mounting',.30,.12,[side*1.015,.03,-.22],rubber,'x');
        cylinder('Titanium communication pod outer bezel',.265,.16,[side*1.085,.03,-.22],titanium,'x');
        cylinder('Pearl communication pod cap',.218,.018,[side*1.174,.03,-.22],pearl,'x');
        cylinder('Communication pod recessed centre',.137,.024,[side*1.19,.03,-.22],graphite,'x');
        cylinder('Communication status lens',.066,.025,[side*1.207,.03,-.22],cyan,'x');
        for(let i=0;i<6;i++) {
          const angle=i*Math.PI/3;
          bolt('Helmet captive pod fastener',[side*1.195,.03+.181*Math.cos(angle),
            -.22+.181*Math.sin(angle)],.019,rivet,'x');
        }
        tube('Communication cable',[[side*.84,-.20,-.58],[side*.95,-.48,-.59],
          [side*.90,-.83,-.43],[side*.67,-1.07,-.12]],.045,graphite);
      }
      ellipse('Lower locking collar', [0,-1.115,-.22],.79,.71,.10,graphite,true);
      ellipse('Collar metal lip',[0,-1.07,-.22],.80,.72,.025,titanium,true);
      const bumper=plate('Contoured chin pressure latch',[
        [-.57,-1.02,.53],[-.44,-1.18,.64],[.44,-1.18,.64],
        [.57,-1.02,.53],[.38,-.91,.53],[-.38,-.91,.53]
      ],[0,-1.055,.75],.13,pearl,pearlShade);
      bumper.userData.structuralPart=true;
      for(const x of [-.17,-.085,0,.085,.17]) {
        tube('Chin latch ventilation slot',[[x,-1.11,.752],[x,-1.02,.752]],.012,graphite);
      }
      plate('Original mission shield',[[.30,.99,.56],[.43,1.01,.53],[.42,1.12,.45],
        [.30,1.1,.49]],[.36,1.055,.557],.012,cyan,titanium);
    } else {
      const graphite=phong({color:0x243542,specular:0x7e99a9,shininess:64});
      const shadow=phong({color:0x0d161e,specular:0x455560,shininess:20});
      const titanium=standard({color:0x92aab5,metalness:.58,roughness:.28});
      const ceramic=phong({color:0xaec4cc,specular:0xe8faff,shininess:95});
      const cyan=phong({color:0x6df1ef,emissive:0x156d73,shininess:100});
      const bronze=standard({color:0xb18c57,metalness:.65,roughness:.27});
      const rows=[
        {y:.14,w:.12,z:.74},{y:-.04,w:.46,z:.71},{y:-.25,w:.65,z:.71},
        {y:-.53,w:.59,z:.67},{y:-.79,w:.36,z:.56},{y:-.9,w:.14,z:.46}
      ];
      const positions=[],indices=[],columns=24;
      for(let layer=0;layer<2;layer++)for(let r=0;r<rows.length;r++)for(let i=0;i<=columns;i++) {
        const u=2*i/columns-1,row=rows[r];
        positions.push(row.w*u,row.y+.035*Math.pow(Math.abs(u),1.4),
          row.z-.27*Math.pow(Math.abs(u),1.6)-layer*.07);
      }
      const layerSize=rows.length*(columns+1);
      for(let r=0;r<rows.length-1;r++)for(let i=0;i<columns;i++) {
        const a=r*(columns+1)+i,b=a+columns+1,c=a+layerSize,d=b+layerSize;
        indices.push(a,b,a+1,a+1,b,b+1,c,c+1,d,c+1,d+1,d);
      }
      function closeEdge(a,b) { indices.push(a,a+layerSize,b,b,a+layerSize,b+layerSize); }
      for(let i=0;i<columns;i++) {
        closeEdge(i+1,i);
        closeEdge((rows.length-1)*(columns+1)+i,(rows.length-1)*(columns+1)+i+1);
      }
      for(let r=0;r<rows.length-1;r++) {
        closeEdge(r*(columns+1),(r+1)*(columns+1));
        closeEdge((r+1)*(columns+1)+columns,r*(columns+1)+columns);
      }
      mask=mesh('Sculpted continuous lower-face respirator shell',geometry(positions,indices),graphite);
      mask.userData.eyeOpening=true;
      for(const side of [-1,1]) {
        function points(values) {
          let result=values.map(function(p) { return [p[0]*side,p[1],p[2]]; });
          if(side<0)result.reverse();return result;
        }
        plate('Raised faceted titanium cheek armor',points([
          [.16,-.44,.73],[.34,-.66,.64],[.56,-.49,.48],[.64,-.16,.40],
          [.46,-.10,.59],[.18,-.19,.78]
        ]),[side*.36,-.34,.80],.065,titanium,shadow);
        plate('Floating ceramic cheek ridge',points([
          [.24,-.24,.82],[.40,-.30,.77],[.60,-.21,.49],[.60,-.13,.48],
          [.43,-.15,.69]
        ]),[side*.42,-.21,.79],.025,ceramic,bronze);
        tube('Inset cheek light pipe',[[side*.24,-.28,.827],[side*.40,-.35,.78],
          [side*.56,-.27,.565]],.012,cyan);
        tube('Cheek lower structural seam',[[side*.18,-.46,.75],
          [side*.35,-.68,.66],[side*.55,-.51,.51]],.016,shadow);
        plate('Open eye brow armor',points([
          [.10,.49,.55],[.34,.48,.55],[.63,.37,.27],[.65,.49,.26],
          [.34,.65,.48],[.12,.61,.56]
        ]),[side*.34,.555,.62],.055,ceramic,titanium);
        tube('Brow cyan bevel inlay',[[side*.14,.60,.59],[side*.34,.62,.54],
          [side*.59,.48,.34]],.010,cyan);
        tube('Open temple bridge',[[side*.64,.43,.24],[side*.71,.22,.16],
          [side*.68,-.04,.19],[side*.62,-.18,.34]],.048,graphite);
        cylinder('Temple circular armor mounting',.154,.10,[side*.678,-.20,.14],shadow,'x');
        cylinder('Temple bronze bezel',.127,.12,[side*.704,-.20,.14],bronze,'x');
        cylinder('Temple cyan concentric insert',.068,.128,[side*.714,-.20,.14],cyan,'x');
        for(const y of [-.22,-.36])bolt('Cheek recessed hex fastener',[side*.52,y,.63],.021,bronze,'z');
      }
      plate('Central nose armor',[
        [-.13,-.25,.81],[.13,-.25,.81],[.125,.025,.77],
        [0,.16,.77],[-.125,.025,.77]
      ],[0,-.055,.9],.07,graphite,bronze);
      plate('Central bronze nose ridge',[
        [-.023,-.20,.91],[.023,-.20,.91],[.025,.023,.855],
        [0,.10,.824],[-.025,.023,.855]
      ],[0,-.05,.944],.015,bronze,shadow);
      cylinder('Octagonal recessed respirator body',.22,.095,[0,-.53,.72],shadow,'z',8);
      cylinder('Raised titanium respirator perimeter',.195,.105,[0,-.53,.735],titanium,'z',8);
      cylinder('Recessed dark respirator grille',.16,.115,[0,-.53,.74],shadow,'z',8);
      for(let i=-2;i<=2;i++) {
        const x=i*.049,half=.12*Math.sqrt(1-(x/.16)*(x/.16));
        tube('Separate respirator grille fin',[[x,-.53-half,.803],[x,-.53+half,.803]],.012,titanium);
      }
      for(const side of [-1,1])
        tube('Lower breathing conduit',[[side*.15,-.72,.64],[side*.30,-.78,.51],
          [side*.44,-.64,.34],[side*.56,-.52,.25]],.036,shadow);
      tube('Articulated chin edge',[[-.30,-.81,.58],[0,-.93,.51],[.30,-.81,.58]],.025,titanium);
      // Translucent brow lip sits above the eyes; the eye openings stay empty.
      visor=plate('Transparent raised brow shield',[
        [-.15,.65,.60],[.15,.65,.60],[.13,.77,.52],[-.13,.77,.52]
      ],[0,.71,.63],.012,phong({color:0x69d7de,specular:0xffffff,shininess:120,
        transparent:true,opacity:.28,depthWrite:false}),titanium);
      visor.renderOrder=20;
    }
    // Geometry/material ownership is independent of scene attachment and deduped.
    // Cached/shared maps belong to the caller and are deliberately never disposed.
    let disposed=false;
    function dispose() {
      if(disposed)return;disposed=true;
      geometries.forEach(function(value) { value.dispose(); });
      materials.forEach(function(value) { value.dispose(); });
    }
    group.userData.originalAuthorship='PrismForge mathematically authored geometry';
    group.userData.headLocalProfile='HeadAttachment dogHead depth proxy; uniform scale 1';
    return {group:group,shell:shell,helmet:shell,visor:visor,mask:mask,occluder:head.mesh,
      referenceHead:reference,hasMouth:false,setMouth:function(){},dispose:dispose};
  }
  return Object.freeze({supports:supports,load:load,create:create});
});
