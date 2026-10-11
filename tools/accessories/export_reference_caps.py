#!/usr/bin/env python3
"""Bounded GLB2-to-native cap export; standard-library parser and Pillow image decoding.

The original downloaded models are read only. Mesh vertices include complete
scene transforms and inverse-transpose normals. Embedded image bytes are copied
unchanged. Run with --output-root to a scratch checkout for deterministic checks.
"""
from __future__ import annotations
import argparse, hashlib, io, json, math, pathlib, struct
import PIL
from PIL import Image

MAX_FILE = 32 * 1024 * 1024
MAX_VERTICES = 20000
MAX_TRIANGLES = 20000
MAX_IMAGE = 16 * 1024 * 1024
IDENTITY = (1.,0.,0.,0., 0.,1.,0.,0., 0.,0.,1.,0., 0.,0.,0.,1.)
COMPONENT = {5121: ('B',1), 5123: ('H',2), 5125: ('I',4), 5126: ('f',4)}
TYPES = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}

def sha(data): return hashlib.sha256(data).hexdigest()
def integer(value, name, limit=1 << 30):
    if type(value) is not int or value < 0 or value > limit: raise ValueError('Invalid '+name)
    return value

def values(value, count, name):
    if not isinstance(value,list) or len(value)!=count: raise ValueError('Invalid '+name)
    out=[]
    for v in value:
        if type(v) not in (int,float) or not math.isfinite(v) or abs(v)>1e9: raise ValueError('Non-finite or unbounded '+name)
        out.append(float(v))
    return out

def mul(a,b):
    return tuple(sum(a[row*4+k]*b[k*4+col] for k in range(4)) for row in range(4) for col in range(4))

def node_matrix(node):
    if 'matrix' in node:
        if any(k in node for k in ('translation','rotation','scale')): raise ValueError('Matrix and TRS are mutually exclusive')
        m=values(node['matrix'],16,'node matrix')
        m=tuple(m[col*4+row] for row in range(4) for col in range(4))
    else:
        t=values(node.get('translation',[0,0,0]),3,'node translation')
        s=values(node.get('scale',[1,1,1]),3,'node scale')
        x,y,z,w=values(node.get('rotation',[0,0,0,1]),4,'node quaternion')
        q=math.sqrt(x*x+y*y+z*z+w*w)
        if q<1e-8 or abs(q-1)>1e-3: raise ValueError('Invalid node quaternion')
        x,y,z,w=x/q,y/q,z/q,w/q
        m=( (1-2*(y*y+z*z))*s[0], 2*(x*y-z*w)*s[1], 2*(x*z+y*w)*s[2],t[0],
            2*(x*y+z*w)*s[0], (1-2*(x*x+z*z))*s[1],2*(y*z-x*w)*s[2],t[1],
            2*(x*z-y*w)*s[0],2*(y*z+x*w)*s[1],(1-2*(x*x+y*y))*s[2],t[2],
            0.,0.,0.,1.)
    if any(abs(m[i]-v)>1e-6 for i,v in zip((12,13,14,15),(0,0,0,1))): raise ValueError('Non-affine scene matrix')
    return m

def normal_matrix(m):
    a,b,c,d,e,f,g,h,i=(m[0],m[1],m[2],m[4],m[5],m[6],m[8],m[9],m[10])
    det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g)
    if not math.isfinite(det) or abs(det)<1e-12: raise ValueError('Singular scene transform')
    return ((e*i-f*h)/det,(f*g-d*i)/det,(d*h-e*g)/det,
            (c*h-b*i)/det,(a*i-c*g)/det,(b*g-a*h)/det,
            (b*f-c*e)/det,(c*d-a*f)/det,(a*e-b*d)/det),det

def point(p,m):
    return tuple(sum(m[row*4+k]*p[k] for k in range(3))+m[row*4+3] for row in range(3))

def normal(n,m):
    r=tuple(sum(m[row*3+k]*n[k] for k in range(3)) for row in range(3))
    length=math.sqrt(sum(v*v for v in r))
    if length<1e-8 or not math.isfinite(length): raise ValueError('Missing valid source surface normal')
    return tuple(v/length for v in r)

def f32(v):
    if not math.isfinite(v) or abs(v)>1e8: raise ValueError('Non-finite or unbounded transformed value')
    return struct.unpack('<f',struct.pack('<f',v))[0]

class Glb:
    def __init__(self,path):
        self.path=pathlib.Path(path)
        if self.path.stat().st_size>MAX_FILE: raise ValueError('GLB exceeds bounded file limit')
        self.raw=self.path.read_bytes()
        if len(self.raw)<20: raise ValueError('Truncated GLB')
        magic,version,length=struct.unpack_from('<III',self.raw)
        if magic!=0x46546c67 or version!=2 or length!=len(self.raw): raise ValueError('Invalid GLB2 header')
        chunks=[];offset=12
        while offset<len(self.raw):
            if offset+8>len(self.raw): raise ValueError('Truncated GLB chunk')
            count,kind=struct.unpack_from('<II',self.raw,offset);offset+=8
            if count%4 or count>len(self.raw)-offset: raise ValueError('Unbounded GLB chunk')
            chunks.append((kind,self.raw[offset:offset+count]));offset+=count
        if len(chunks)!=2 or chunks[0][0]!=0x4e4f534a or chunks[1][0]!=0x004e4942: raise ValueError('Expected JSON then one embedded BIN chunk')
        if len(chunks[0][1])>1024*1024: raise ValueError('Oversized GLB JSON')
        self.doc=json.loads(chunks[0][1].decode('utf-8'))
        self.bin=chunks[1][1]
        if self.doc.get('asset',{}).get('version')!='2.0': raise ValueError('Unsupported glTF version')
        if self.doc.get('extensionsUsed') or self.doc.get('extensionsRequired'): raise ValueError('Unsupported GLB extension')
        if self.doc.get('skins') or self.doc.get('animations'): raise ValueError('Animated/skinned GLB unsupported')
        buffers=self.doc.get('buffers',[])
        if len(buffers)!=1 or 'uri' in buffers[0]: raise ValueError('External or multiple buffers unsupported')
        declared=integer(buffers[0].get('byteLength'),'buffer byteLength',MAX_FILE)
        if declared>len(self.bin) or len(self.bin)-declared>3: raise ValueError('BIN buffer does not match declared length')
        self.length=declared
        for key in ('nodes','meshes','accessors','bufferViews','images','textures','materials','samplers','scenes'):
            array=self.doc.get(key,[])
            if not isinstance(array,list) or len(array)>512: raise ValueError('Unbounded '+key)
        for view in self.doc.get('bufferViews',[]): self.view(view)
        for image in self.doc.get('images',[]):
            if 'uri' in image: raise ValueError('External images unsupported')
            if image.get('mimeType') not in ('image/jpeg','image/png'): raise ValueError('Unsupported image encoding')

    def get(self,key,index):
        integer(index,key+' index',511)
        array=self.doc.get(key,[])
        if index>=len(array): raise ValueError('Invalid '+key+' index')
        return array[index]

    def view(self,view):
        if integer(view.get('buffer',0),'view buffer')!=0: raise ValueError('View must use embedded buffer')
        offset=integer(view.get('byteOffset',0),'view byteOffset',MAX_FILE)
        length=integer(view.get('byteLength'),'view byteLength',MAX_FILE)
        if offset>self.length or length>self.length-offset: raise ValueError('Buffer view outside BIN')
        return memoryview(self.bin)[offset:offset+length]

    def accessor(self,index,expected,count_limit):
        a=self.get('accessors',index)
        if 'sparse' in a or a.get('normalized',False): raise ValueError('Sparse/normalized accessors unsupported')
        if a.get('type')!=expected or a.get('componentType') not in COMPONENT: raise ValueError('Unsupported accessor type')
        if expected!='SCALAR' and a['componentType']!=5126: raise ValueError('Mesh attribute must be float32')
        if expected=='SCALAR' and a['componentType']==5126: raise ValueError('Triangle indices must be unsigned integer')
        count=integer(a.get('count'),'accessor count',count_limit)
        if count==0: raise ValueError('Empty accessor')
        view=self.get('bufferViews',a.get('bufferView'));data=self.view(view)
        code,width=COMPONENT[a['componentType']];components=TYPES[expected];item=width*components
        stride=integer(view.get('byteStride',item),'accessor stride',252)
        if stride<item or stride%width: raise ValueError('Invalid accessor stride')
        offset=integer(a.get('byteOffset',0),'accessor byteOffset',MAX_FILE)
        span=(count-1)*stride+item
        if offset%width or offset>len(data) or span>len(data)-offset: raise ValueError('Accessor outside buffer view')
        fmt=struct.Struct('<'+code*components)
        result=[fmt.unpack_from(data,offset+i*stride) for i in range(count)]
        if code=='f' and any(not math.isfinite(v) or abs(v)>1e7 for row in result for v in row): raise ValueError('Non-finite or unbounded attribute')
        return result

    def geometry(self):
        vertices=[];triangles=[];components=[];visited=set()
        scene=self.get('scenes',self.doc.get('scene',0))
        def visit(index,parent,active,depth):
            if depth>32 or index in active or index in visited: raise ValueError('Cyclic/shared/too-deep scene node')
            node=self.get('nodes',index);visited.add(index)
            if node.get('extensions') or node.get('skin') is not None or node.get('weights'): raise ValueError('Unsupported mesh node')
            world=mul(parent,node_matrix(node));normal_transform,det=normal_matrix(world)
            if 'mesh' in node:
                mesh=self.get('meshes',node['mesh'])
                for primitive in mesh.get('primitives',[]):
                    if primitive.get('mode',4)!=4 or primitive.get('targets') or primitive.get('extensions'): raise ValueError('Only static triangle-list primitives supported')
                    attributes=primitive.get('attributes',{})
                    p=self.accessor(attributes.get('POSITION'),'VEC3',MAX_VERTICES)
                    n=self.accessor(attributes.get('NORMAL'),'VEC3',MAX_VERTICES)
                    uv=self.accessor(attributes.get('TEXCOORD_0'),'VEC2',MAX_VERTICES)
                    if len(p)!=len(n) or len(p)!=len(uv): raise ValueError('Mismatched vertex attributes')
                    idx=self.accessor(primitive.get('indices'),'SCALAR',MAX_TRIANGLES*3)
                    if len(idx)%3: raise ValueError('Incomplete triangle list')
                    material=self.get('materials',primitive.get('material',0))
                    if material.get('alphaMode','OPAQUE')!='OPAQUE' or material.get('extensions'): raise ValueError('Unsupported cap material')
                    base=len(vertices);first_triangle=len(triangles)
                    if base+len(p)>MAX_VERTICES or len(triangles)+len(idx)//3>MAX_TRIANGLES: raise ValueError('Mesh exceeds bounded scene geometry')
                    vertices.extend(tuple(map(f32,(*point(pos,world),*normal(nor,normal_transform),*tex))) for pos,nor,tex in zip(p,n,uv))
                    for i in range(0,len(idx),3):
                        t=[idx[i+j][0] for j in range(3)]
                        if any(v>=len(p) for v in t): raise ValueError('Triangle index outside primitive')
                        if det<0: t[1],t[2]=t[2],t[1]
                        triangles.append(tuple(base+v for v in t))
                    components.append({'node':node.get('name'),'mesh':mesh.get('name'),'vertices':len(p),'triangles':len(idx)//3,'firstVertex':base,'firstTriangle':first_triangle,'material':primitive.get('material',0),'worldMatrixRowMajor':list(world),'negativeDeterminantWindingReversed':det<0})
            children=node.get('children',[])
            if not isinstance(children,list) or len(children)>512: raise ValueError('Unbounded children')
            for child in children: visit(child,world,active|{index},depth+1)
        for node in scene.get('nodes',[]): visit(node,IDENTITY,set(),0)
        if not vertices or not triangles: raise ValueError('No scene mesh geometry')
        return vertices,triangles,components

    def images(self):
        result=[]
        material=self.get('materials',0);pbr=material.get('pbrMetallicRoughness',{})
        names=[('base',pbr.get('baseColorTexture')),('roughness_metallic',pbr.get('metallicRoughnessTexture')),('normal',material.get('normalTexture'))]
        for role,texture in names:
            if texture is None: continue
            if texture.get('texCoord',0)!=0 or texture.get('extensions'): raise ValueError('Texture requires unsupported UV transform/set')
            t=self.get('textures',texture.get('index'));im=self.get('images',t.get('source'))
            data=bytes(self.view(self.get('bufferViews',im.get('bufferView'))))
            if not data or len(data)>MAX_IMAGE: raise ValueError('Unbounded embedded image')
            mime=im['mimeType'];extension='.jpg' if mime=='image/jpeg' else '.png'
            if (extension=='.png' and not data.startswith(b'\x89PNG\r\n\x1a\n')) or (extension=='.jpg' and not data.startswith(b'\xff\xd8\xff')): raise ValueError('Image signature mismatch')
            result.append({'role':role,'mimeType':mime,'extension':extension,'bytes':data,'sha256':sha(data),'sampler':self.get('samplers',t.get('sampler',0)) if self.doc.get('samplers') else {},'textureProperties':texture})
        return result

def envelope(vertices):
    return {'min':[min(v[i] for v in vertices) for i in range(3)],'max':[max(v[i] for v in vertices) for i in range(3)]}

def mesh_digest(vertices,triangles):
    data=b''.join(struct.pack('<8f',*v) for v in vertices)+b''.join(struct.pack('<3I',*t) for t in triangles)
    return sha(data)

# Verified from transformed source-mesh three-view inspection. Values are source
# WORLD coordinates after complete scene transforms, never raw FBX attributes.
CALIBRATIONS={
    'tennis': {'frontSign':1,'frontBandVertex':493,'bandY':0.3106876611709595,'frontBandZ':1.061342716217041},
    'pokemon': {'frontSign':-1,'frontBandVertex':354,'bandY':526.87109375,'frontBandZ':-867.6121826171875},
}

def normalized(vertices,calibration):
    bounds=envelope(vertices);scale=1.1/(bounds['max'][0]-bounds['min'][0]);cx=(bounds['min'][0]+bounds['max'][0])*.5
    sign=calibration['frontSign'];out=[]
    # A 180-degree Y rotation keeps a proper handed frame if source front is -Z.
    for v in vertices:
        x=sign*(v[0]-cx)*scale;y=(v[1]-calibration['bandY'])*scale;z=sign*(v[2]-calibration['frontBandZ'])*scale+.5
        out.append(tuple(map(f32,(x,y,z,sign*v[3],v[4],sign*v[5],v[6],v[7]))))
    return out,{'worldEnvelope':bounds,'uniformScale':scale,'worldCenterX':cx,**calibration,'normalization':'proper Y rotation when frontSign=-1; width1.1; crown-front band at local(y0,z+.5); original UV0 unchanged'}

def literal(value):
    text=format(value,'.9g')
    if '.' not in text and 'e' not in text: text+='.0'
    return text+'f'

def export(source_root,output_root):
    names=[('tennis','white_tennis_cap.glb','WhiteTennisCap'),('pokemon','pokemon_masters_cap.glb','PokemonCap')]
    include=['// Generated by tools/accessories/export_reference_caps.py; do not edit.','// Scene transforms and inverse-transpose normals applied; embeddedUV0 unchanged.']
    records=[];source_hashes=[]
    for key,name,symbol in names:
        glb=Glb(source_root/name);v,t,c=glb.geometry();imgs=glb.images();nv,cal=normalized(v,CALIBRATIONS[key])
        include+=['static constexpr MeshVertex k'+symbol+'Vertices[] = {',*[' {'+','.join(literal(x) for x in row)+'},' for row in nv],'};','static constexpr MeshTriangle k'+symbol+'Triangles[] = {',*[' {%du,%du,%du,Material::CapFabric},'%row for row in t],'};']
        assets=[]
        for image in imgs:
            dest=output_root/'windows/res/accessories'/(key+'_'+image['role']+image['extension']);dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(image['bytes'])
            with Image.open(io.BytesIO(image['bytes'])) as image_info:
                source_size=image_info.size
            assets.append({k:v for k,v in image.items() if k not in ('bytes','extension')}|{'path':dest.relative_to(output_root).as_posix(),'byteLength':len(image['bytes']),'width':source_size[0],'height':source_size[1]})
            if image['role']=='base':
                if source_size[0]<=0 or source_size[1]<=0 or max(source_size)>8192 or source_size[0]*source_size[1]>16*1024*1024: raise ValueError('Unbounded source image dimensions')
                with Image.open(io.BytesIO(image['bytes'])) as source_image:
                    rgba=source_image.convert('RGBA')
                resized=max(source_size)>2048
                if resized:
                    factor=2048/max(source_size)
                    rgba=rgba.resize((max(1,round(source_size[0]*factor)),max(1,round(source_size[1]*factor))),Image.Resampling.LANCZOS)
                packed=rgba.tobytes()
                if len(packed)!=rgba.width*rgba.height*4 or len(packed)>16*1024*1024: raise ValueError('Unbounded runtime texture dimensions')
                runtime=output_root/'windows/res/accessories'/('white_tennis_cap.pfrgba' if key=='tennis' else 'pokemon_masters_cap.pfrgba')
                runtime_data=b'PFC1'+struct.pack('<II',rgba.width,rgba.height)+packed
                runtime.write_bytes(runtime_data)
                assets[-1]['runtime']={'path':runtime.relative_to(output_root).as_posix(),'format':'PFC1 + uint32LE width + uint32LE height + tightly packed RGBA8 rows; image row0 first; UV0 unchanged','width':rgba.width,'height':rgba.height,'byteLength':len(runtime_data),'sha256':sha(runtime_data),'sourceSha256':image['sha256'],'decodedRgbaSha256':sha(packed),'resized':resized,'conversion':'Pillow '+PIL.__version__+'; convertRGBA; '+('aspect-preserving LANCZOS to maximum2048axis' if resized else 'no resizing; exact decoded source pixels'),'baseColorFactor':glb.doc.get('materials',[{}])[0].get('pbrMetallicRoughness',{}).get('baseColorFactor',[1,1,1,1])}
        records.append({'id':key,'file':name,'sha256':sha(glb.raw),'byteLength':len(glb.raw),'assetMetadata':glb.doc['asset'],'components':c,'worldGeometrySha256':mesh_digest(v,t),'vertices':len(v),'triangles':len(t),'calibration':cal,'normalizedEnvelope':envelope(nv),'textures':assets,'materials':glb.doc.get('materials',[])})
        source_hashes.append({'file':name,'sha256':sha(glb.raw)})
    variant=Glb(source_root/'pokemon_masters_cap (1).glb');vv,vt,vc=variant.geometry();vi=variant.images()
    records[1]['alternativeDownload']={'file':variant.path.name,'sha256':sha(variant.raw),'byteLength':len(variant.raw),'worldGeometrySha256':mesh_digest(vv,vt),'geometryMatchesSelected':mesh_digest(vv,vt)==records[1]['worldGeometrySha256'],'textures':[{'role':i['role'],'sha256':i['sha256'],'byteLength':len(i['bytes'])} for i in vi],'selection':'original filename selected; alternate has same geometry but different embedded textures'}
    source_hashes.append({'file':variant.path.name,'sha256':sha(variant.raw)})
    content='\n'.join(include)+'\n';dest=output_root/'windows/src/effects/overlays/reference_caps_mesh.inc';dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(content,encoding='utf-8',newline='\n')
    manifest={'schemaVersion':1,'sourceRoot':source_root.as_posix(),'exportScript':'tools/accessories/export_reference_caps.py','exportScriptSha256':sha(pathlib.Path(__file__).read_bytes()),'scope':'User-supplied GLB geometry and original embedded image bytes. Metadata is recorded verbatim; no commercial rights are inferred.','sources':source_hashes,'models':records,'output':{'path':dest.relative_to(output_root).as_posix(),'sha256':sha(dest.read_bytes())}}
    md=output_root/'tools/accessories/reference_caps_provenance.json';md.parent.mkdir(parents=True,exist_ok=True);md.write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8',newline='\n')
    return manifest

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--source-root',type=pathlib.Path,default=pathlib.Path.home()/'Downloads');parser.add_argument('--output-root',type=pathlib.Path,default=pathlib.Path(__file__).resolve().parents[2]);args=parser.parse_args()
    result=export(args.source_root,args.output_root)
    print(json.dumps({'output':result['output'],'models':[{k:m[k] for k in ('id','vertices','triangles','calibration','normalizedEnvelope')} for m in result['models']]},indent=2))