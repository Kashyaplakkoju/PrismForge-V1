#!/usr/bin/env python3
"""Convert reviewed embedded static GLB2 assets to bounded native PFM1, deterministically.

No runtime Python dependency. This converter shares the already bounded scene/
accessor math with the cap exporter while using separate material/geometry limits.
Transforms and inverse-transpose normals are flattened; source metre coordinates,
nose-bridge origin, UV0, PBR factors, alpha and image bytes remain attributable.
"""
from __future__ import annotations
import argparse, hashlib, io, json, math, pathlib, shutil, struct, sys
from PIL import Image
from export_reference_caps import Glb, IDENTITY, mul, node_matrix, normal_matrix, point, normal, f32, integer, values, sha
MAX_VERTICES=60000
MAX_TRIANGLES=60000
MAX_MATERIALS=8
MAX_TEXTURE=2048
NAMES=('aurora-fox-mask','nocturne-owl-mask','atlas-explorer-helmet')
# Source generators place mask aperture centres at y=.008m, not AABB centre.
# Owl generator actually uses +/- .038m despite its report claiming .064 common IPD.
ANCHORS={NAMES[0]:(.064,.008,.014),NAMES[1]:(.076,.008,.007),NAMES[2]:(.064,.008,0.)}

class FilterGlb(Glb):
    def geometry(self):
        vertices=[];triangles=[];visited=set();parts=[]
        scene=self.get('scenes',self.doc.get('scene',0))
        def visit(index,parent,active,depth):
            if depth>32 or index in active or index in visited:raise ValueError('Cyclic/shared/too-deep scene node')
            node=self.get('nodes',index);visited.add(index)
            if node.get('extensions') or node.get('skin') is not None or node.get('weights'):raise ValueError('Unsupported mesh node')
            world=mul(parent,node_matrix(node));nm,det=normal_matrix(world)
            if 'mesh' in node:
                mesh=self.get('meshes',node['mesh'])
                primitives=mesh.get('primitives',[])
                if not isinstance(primitives,list) or len(primitives)>256:raise ValueError('Unbounded primitive list')
                for prim in primitives:
                    if prim.get('mode',4)!=4 or prim.get('targets') or prim.get('extensions'):raise ValueError('Only static triangle lists supported')
                    attrs=prim.get('attributes',{})
                    if set(attrs)-{'POSITION','NORMAL','TEXCOORD_0'}:raise ValueError('Unsupported vertex attributes')
                    p=self.accessor(attrs.get('POSITION'),'VEC3',MAX_VERTICES)
                    n=self.accessor(attrs.get('NORMAL'),'VEC3',MAX_VERTICES)
                    uv=self.accessor(attrs.get('TEXCOORD_0'),'VEC2',MAX_VERTICES)
                    indices=self.accessor(prim.get('indices'),'SCALAR',MAX_TRIANGLES*3)
                    if len(p)!=len(n) or len(p)!=len(uv) or len(indices)%3:raise ValueError('Invalid triangle attributes')
                    material=integer(prim.get('material',0),'material',MAX_MATERIALS-1);self.get('materials',material)
                    base=len(vertices)
                    if base+len(p)>MAX_VERTICES or len(triangles)+len(indices)//3>MAX_TRIANGLES:raise ValueError('Unbounded filter scene')
                    for pos,nor,tex in zip(p,n,uv):
                        v=tuple(map(f32,(*point(pos,world),*normal(nor,nm),*tex)))
                        if any(abs(x)>2 for x in v[:3]) or any(abs(x)>10000 for x in v[6:]):raise ValueError('Unbounded metric filter vertex')
                        vertices.append(v)
                    for at in range(0,len(indices),3):
                        t=[indices[at+i][0] for i in range(3)]
                        if any(x>=len(p) for x in t):raise ValueError('Index outside primitive')
                        if det<0:t[1],t[2]=t[2],t[1]
                        triangles.append(tuple(base+x for x in t)+(material,))
                    parts.append({'node':node.get('name'),'material':material,'vertices':len(p),'triangles':len(indices)//3,'worldMatrixRowMajor':list(world)})
            children=node.get('children',[])
            if not isinstance(children,list) or len(children)>512:raise ValueError('Unbounded children')
            for child in children:visit(child,world,active|{index},depth+1)
        nodes=scene.get('nodes',[])
        if not isinstance(nodes,list) or len(nodes)>512:raise ValueError('Unbounded scene roots')
        for node in nodes:visit(node,IDENTITY,set(),0)
        if not vertices or not triangles:raise ValueError('No drawable filter geometry')
        return vertices,triangles,parts

    def materials(self):
        materials=self.doc.get('materials',[])
        if not materials or len(materials)>MAX_MATERIALS:raise ValueError('Unbounded material list')
        rows=[];images={};image_sources={}
        def image_for(info,role):
            if info is None:return False
            if info.get('texCoord',0)!=0 or info.get('extensions'):raise ValueError('Unsupported UV transform/set')
            texture=self.get('textures',info.get('index'));image=self.get('images',texture.get('source'))
            sampler=self.get('samplers',texture.get('sampler',0)) if self.doc.get('samplers') else {}
            if sampler.get('wrapS',10497)!=10497 or sampler.get('wrapT',10497)!=10497:raise ValueError('Collection converter supports repeat texture wrapping only')
            if sampler.get('magFilter',9729)!=9729 or sampler.get('minFilter',9987) not in (9729,9987):raise ValueError('Collection converter requires linear image sampling')
            raw=bytes(self.view(self.get('bufferViews',image.get('bufferView'))))
            if not raw or len(raw)>16*1024*1024:raise ValueError('Unbounded embedded image')
            if image['mimeType']=='image/png' and not raw.startswith(b'\x89PNG\r\n\x1a\n'):raise ValueError('Image signature mismatch')
            if image['mimeType']=='image/jpeg' and not raw.startswith(b'\xff\xd8\xff'):raise ValueError('Image signature mismatch')
            if role in image_sources and image_sources[role]!=sha(raw):raise ValueError('Only one shared image per material role supported')
            with Image.open(io.BytesIO(raw)) as im:
                if not im.width or not im.height or max(im.size)>MAX_TEXTURE or im.width*im.height>MAX_TEXTURE**2:raise ValueError('Unbounded decoded texture')
                if getattr(im,'n_frames',1)!=1:raise ValueError('Animated texture unsupported')
                decoded=im.convert('RGBA');pixels=decoded.tobytes()
            images[role]=(decoded.width,decoded.height,pixels);image_sources[role]=sha(raw)
            return True
        for material in materials:
            if material.get('extensions') or any(k in material for k in ('normalTexture','occlusionTexture','emissiveTexture')) or any(material.get('emissiveFactor',[0,0,0])):raise ValueError('Unsupported material feature')
            mode=material.get('alphaMode','OPAQUE')
            if mode not in ('OPAQUE','BLEND'):raise ValueError('Unsupported material alpha mode')
            pbr=material.get('pbrMetallicRoughness',{})
            color=values(pbr.get('baseColorFactor',[1,1,1,1]),4,'base colour')
            factors=[pbr.get('metallicFactor',1),pbr.get('roughnessFactor',1)]
            if any(type(x) not in (float,int) or not math.isfinite(x) or x<0 or x>1 for x in color+factors):raise ValueError('Invalid material factor')
            if mode=='OPAQUE' and color[3]!=1:raise ValueError('Opaque alpha factor unsupported')
            textured=image_for(pbr.get('baseColorTexture'),'base')
            mr=image_for(pbr.get('metallicRoughnessTexture'),'metalRough')
            if mr and not textured:raise ValueError('Metallic-roughness requires base image')
            rows.append(tuple(map(f32,color+factors))+(int(mode=='BLEND')|int(textured)*2,0))
        return rows,images,image_sources

def encode(glb,anchors):
    vertices,triangles,parts=glb.geometry();materials,images,sources=glb.materials()
    body=b''.join(struct.pack('<8f',*v) for v in vertices)+b''.join(struct.pack('<4I',*t) for t in triangles)+b''.join(struct.pack('<6f2I',*m) for m in materials)
    dims=[]
    for role in ('base','metalRough'):
        w,h,pixels=images.get(role,(0,0,b''));dims.extend((w,h));body+=pixels
    if len(body)+52>64*1024*1024:raise ValueError('Unbounded native payload')
    header=b'PFM1'+struct.pack('<9I3f',1,len(vertices),len(triangles),len(materials),*dims,len(body),*anchors)
    return header+body,{'vertices':len(vertices),'triangles':len(triangles),'materials':len(materials),'parts':parts,'textureDimensions':dims,'embeddedImageSha256':sources,'boundsMeters':{'min':[min(v[i] for v in vertices) for i in range(3)],'max':[max(v[i] for v in vertices) for i in range(3)]},'authoredPupilSpacingMeters':anchors[0],'authoredEyeHeightMeters':anchors[1],'authoredEyeDepthMeters':anchors[2]}

def verify_manifest(delivery):
    manifest=json.loads((delivery/'manifest.json').read_text(encoding='utf-8-sig'))
    # Payload paths and exact byte metadata are checked before any asset is imported.
    entries=manifest.get('payloads',manifest.get('files',[]))
    if isinstance(entries,dict):entries=[dict(value,path=key) for key,value in entries.items()]
    verified=0
    for record in entries:
        name=record.get('path',record.get('file'))
        if not isinstance(name,str) or pathlib.PurePosixPath(name).is_absolute() or '..' in pathlib.PurePosixPath(name).parts:raise ValueError('Unsafe manifest payload path')
        data=(delivery/name).read_bytes()
        size=record.get('bytes',record.get('size',record.get('byteLength')))
        if size is not None and len(data)!=size:raise ValueError('Payload size drift '+name)
        if sha(data).upper()!=record['sha256'].upper():raise ValueError('Payload hash drift '+name)
        verified+=1
    for key in NAMES:
        record=manifest['models'][key];data=(delivery/record['file']).read_bytes()
        if len(data)!=record['bytes'] or sha(data).upper()!=record['sha256'].upper():raise ValueError('Source model drift '+key)
    return manifest,verified

def export(delivery,output,variant=None):
    manifest,verified=verify_manifest(delivery);dest=output/'windows/res/face-filters';dest.mkdir(parents=True,exist_ok=True)
    source=output/'assets/face-filters/original-3d-filters-20261009';source.mkdir(parents=True,exist_ok=True)
    records=[]
    for key in NAMES:
        original=delivery/'models'/(key+'.glb')
        path=variant/(key+'.glb') if variant is not None and key in NAMES[:2] else original
        if path!=original and not path.is_file():raise ValueError('Native revised asset missing '+key)
        glb=FilterGlb(path);packed,record=encode(glb,ANCHORS[key])
        (dest/(key+'.pfmodel')).write_bytes(packed)
        modeldest=source/'models'/(key+'.glb');modeldest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(original,modeldest)
        records.append(dict(id=key,sourceSha256=sha(glb.raw),frozenOriginalSourceSha256=sha(original.read_bytes()),sourceRevision='native-repaired-20261009' if path!=original else 'original-3d-filters-20261009',nativeFile=key+'.pfmodel',nativeBytes=len(packed),nativeSha256=sha(packed),**record))
    # Preserve complete exact source generators and metadata; do not copy unrelated delivery/tool files.
    for name in ('manifest.json','fitting-profiles.json','README.md'):
        if (delivery/name).is_file():shutil.copyfile(delivery/name,source/name)
    if (delivery/'sources').is_dir():shutil.copytree(delivery/'sources',source/'sources',dirs_exist_ok=True)
    report={'schema':'PFM1','headerBytes':52,'manifestPayloadHashesVerified':verified,'conversion':'Full scene transforms and inverse-transpose normals; metres and authored bridge origin preserved; UV0 unchanged; RGBA sRGB base/linear metallic-roughness; one uniform fit scale. Runtime validates all bounds before rendering.','coordinateFrame':'+X right, +Y up, +Z viewer','nativeLimits':{'fileBytes':64*1024*1024,'vertices':MAX_VERTICES,'triangles':MAX_TRIANGLES,'materials':MAX_MATERIALS,'textureDimension':MAX_TEXTURE},'models':records,'provenance':{'author':manifest.get('author'),'claim':'Original procedural models according to supplied delivery; independent geometry/material/hash review completed, visual quality and commercial suitability remain separately reviewable.'},'knownAssetReview':'Owl authored aperture spacing is .076m, not the delivery common .064m. Native fox/owl revision aligns painted eye features to actual aperture UVs and closes owl horn bases; independent front renders reviewed. Artistic fit still needs actual native live review.'}
    (dest/'manifest.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    (dest/'NOTICE.txt').write_text('PrismForge original 3D filter starter collection\nSource: Antigravity IDE delivery original-3d-filters-20261009.\nDelivery licence claim: Proprietary - PrismForge Studio internal production asset collection.\nOriginal procedural sources and exact GLBs are retained in assets/face-filters/original-3d-filters-20261009.\nNative fox/owl artwork and owl horn-base corrections are preserved in assets/face-filters/native-repaired-20261009.\nThese assets are distinct from third-party Jeeliz demos and downloaded caps.\nPFM1 conversion preserves original metric geometry and material/texture metadata.\nArtwork originality is the supplier claim; no independent legal clearance or commercial quality certification is implied.\n',encoding='utf-8')
    print(json.dumps({'models':len(records),'manifestPayloadHashesVerified':verified,'nativeBytes':[r['nativeBytes'] for r in records]},indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--delivery',type=pathlib.Path,required=True);p.add_argument('--variant-root',type=pathlib.Path,default=pathlib.Path(__file__).resolve().parents[2]/'assets/face-filters/native-repaired-20261009');p.add_argument('--output-root',type=pathlib.Path,default=pathlib.Path(__file__).resolve().parents[2]);a=p.parse_args()
    try:export(a.delivery.resolve(),a.output_root.resolve(),a.variant_root.resolve())
    except Exception as e:print('Filter conversion failed: '+str(e),file=sys.stderr);sys.exit(1)
