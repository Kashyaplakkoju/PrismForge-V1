from pathlib import Path
import copy,io,struct,sys,math,json
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools/accessories'))
from export_face_filters import FilterGlb,encode,ANCHORS,NAMES,verify_manifest
from export_reference_caps import node_matrix,normal_matrix,point,normal
groups=0;assertions=0
def need(v,message):
 global assertions
 assertions+=1
 if not v:raise AssertionError(message)
def rejects(fn,label):
 try:fn()
 except (ValueError,KeyError,TypeError,IndexError):return
 raise AssertionError('Accepted '+label)
delivery=Path(sys.argv[1]);manifest,verified=verify_manifest(delivery);need(verified==53,'exact53delivery hashes');groups+=1
variant=Path(__file__).resolve().parents[2]/'assets/face-filters/native-repaired-20261009'
for key in NAMES:
 path=variant/(key+'.glb') if key in NAMES[:2] else delivery/'models'/(key+'.glb')
 glb=FilterGlb(path);raw,report=encode(glb,ANCHORS[key]);raw2,report2=encode(glb,ANCHORS[key])
 need(raw==raw2 and report==report2,'byte deterministic conversion '+key)
 current=Path(__file__).resolve().parents[2]/'windows/res/face-filters'/(key+'.pfmodel')
 need(current.read_bytes()==raw,'checked native resource source-bound '+key)
 h=struct.unpack_from('<9I3f',raw,4);need(len(raw)==52+h[8],'exact format extent')
 need(h[1]==report['vertices'] and h[2]==report['triangles'],'geometry counts')
 need(h[9:]==tuple(struct.unpack('<3f',struct.pack('<3f',*ANCHORS[key]))),'metric anchor preserved')
 v,t,p=glb.geometry();need(report['boundsMeters']['min'][0]<0<report['boundsMeters']['max'][0],'not AABB recentered')
 mats,images,src=glb.materials()
 for role,img in images.items():need(img[0:2]==(2048,2048) and len(img[2])==16777216,'two complete image planes')
 groups+=1
 # Fail closed instead of silently dropping features.
 for mutation in (
  lambda d:d['materials'][0].update({'alphaMode':'MASK'}),
  lambda d:d['materials'][0].update({'normalTexture':{'index':0}}),
  lambda d:d['materials'][0]['pbrMetallicRoughness'].update({'baseColorFactor':[float('nan'),1,1,1]}),
  lambda d:d['materials'][0]['pbrMetallicRoughness']['baseColorTexture'].update({'texCoord':1}),
  lambda d:d['samplers'][0].update({'wrapS':33071}),
 ):
  test=FilterGlb(path);mutation(test.doc);rejects(lambda:encode(test,ANCHORS[key]),'unsupported material '+key);assertions+=1
 for mutation in (
  lambda d:d['nodes'][0].update({'children':[0]}),
  lambda d:d['nodes'][0].update({'scale':[1,0,1]}),
  lambda d:d['accessors'][0].update({'count':60001}),
  lambda d:d['meshes'][0]['primitives'][0].update({'mode':5}),
  lambda d:d['nodes'][0].update({'translation':[3,0,0]}),
 ):
  test=FilterGlb(path);mutation(test.doc);rejects(lambda:encode(test,ANCHORS[key]),'unsupported geometry '+key);assertions+=1
 groups+=1
# Independent simple transform oracle checks reflections and inverse transpose.
matrix=node_matrix({'translation':[.1,.2,.3],'scale':[-2,3,4]});nm,det=normal_matrix(matrix)
need(point((1,2,3),matrix)==(-1.9,6.2,12.3),'fullworldposition transform')
need(det==-24,'reflection handedness')
a=normal((1,1,0),nm);length=math.sqrt((-.5)**2+(1/3)**2)
need(abs(a[0]+.5/length)<1e-12 and abs(a[1]-(1/3)/length)<1e-12,'inverse transpose normal oracle')
groups+=1
print(f'NATIVE_FILTER_CONVERTER {groups} groups {assertions} assertions')
