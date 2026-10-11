"""Acquire pinned official native face assets into an isolated development folder."""
from pathlib import Path
import argparse,hashlib,json,urllib.request,zipfile
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,default=ROOT/'build/face-native-deps');p.add_argument('--offline',action='store_true');args=p.parse_args()
root=args.output.resolve();root.mkdir(parents=True,exist_ok=True)
def acquire(url,path,sha):
    if path.exists():
        if hashlib.sha256(path.read_bytes()).hexdigest().upper()!=sha.upper():raise RuntimeError('Existing asset hash mismatch: '+str(path))
        return
    if args.offline:raise RuntimeError('Pinned asset unavailable in offline mode: '+str(path))
    with urllib.request.urlopen(url,timeout=60) as response:data=response.read(40*1024*1024)
    if hashlib.sha256(data).hexdigest().upper()!=sha.upper():raise RuntimeError('Downloaded asset hash mismatch')
    path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
expected='438d98977a4e1b0f88070ad16537a328912f847ce9debf236da047b0b483cbfd'
filename='mediapipe-0.10.32-py3-none-win_amd64.whl'
wpath=root/'mediapipe-0.10.32'/filename
url='https://files.pythonhosted.org/packages/fe/63/c9172142521fc334fa891be9ef6259a3790502041b395127d63755cb3f0b/'+filename
acquire(url,wpath,expected)
with zipfile.ZipFile(wpath) as z:
    members=['mediapipe/tasks/c/libmediapipe.dll','mediapipe-0.10.32.dist-info/licenses/LICENSE']
    for name in members:
        dest=root/'mediapipe-0.10.32'/name;dest.parent.mkdir(parents=True,exist_ok=True);data=z.read(name)
        if dest.exists() and dest.read_bytes()!=data:raise RuntimeError('Extracted file differs: '+name)
        dest.write_bytes(data)
assets=root/'face-landmarker-v1'
base='https://raw.githubusercontent.com/google-ai-edge/mediapipe/8317ba78778738ba90a521e7e4580a2ba0129c81/'
rows=[('face_landmarker.task','https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task','64184E229B263107BC2B804C6625DB1341FF2BB731874B0BCC2FE6544E0BC9FF'),('canonical_face_model.obj',base+'mediapipe/modules/face_geometry/data/canonical_face_model.obj','8BAC80443397E113F41A8B565EA72C59390BC031D9DEFAB289DBA7BC0C54E618'),('LICENSE',base+'LICENSE','8707EEF0533987EFC5B155D64761EEB6E20793F50B9BD1A68DAD1CF4719D0ED8')]
for name,url,sha in rows:acquire(url,assets/name,sha)
print('Pinned MediaPipe0.10.32 native runtime, FaceLandmarker v1, topology and licenses ready:',root)
