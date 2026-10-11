"""Stage or verify the pinned native face-lighting runtime and model assets."""
import argparse, hashlib, json, shutil
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('directory', type=Path)
p.add_argument('--dependencies', type=Path, default=ROOT / 'build/face-native-deps')
p.add_argument('--verify-only', action='store_true')
a = p.parse_args()
LICENSE_SHA = '8707EEF0533987EFC5B155D64761EEB6E20793F50B9BD1A68DAD1CF4719D0ED8'
rows = [
 ('mediapipe-0.10.32/mediapipe/tasks/c/libmediapipe.dll', 'libmediapipe.dll', '6DBB21A391CF79BEAD3D82EFB00D02B3638435340C950545042B83194A5B87E7'),
 ('face-landmarker-v1/face_landmarker.task', 'models/face-landmarker/face_landmarker.task', '64184E229B263107BC2B804C6625DB1341FF2BB731874B0BCC2FE6544E0BC9FF'),
 ('face-landmarker-v1/canonical_face_model.obj', 'models/face-landmarker/canonical_face_model.obj', '8BAC80443397E113F41A8B565EA72C59390BC031D9DEFAB289DBA7BC0C54E618'),
 ('mediapipe-0.10.32/mediapipe-0.10.32.dist-info/licenses/LICENSE', 'licenses/mediapipe/LICENSE', LICENSE_SHA),
 ('face-landmarker-v1/LICENSE', 'models/face-landmarker/LICENSE', LICENSE_SHA),
]
notice = (ROOT / 'tools/face-deps/NOTICE.txt').read_bytes()
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest().upper()
root = a.directory.resolve()
checks=[]
# Verify all inputs before staging any file. No model download or global install.
for source, target, expected in rows:
 path = root / target if a.verify_only else a.dependencies / source
 if not path.is_file() or digest(path) != expected:
  raise ValueError(f'Pinned face asset mismatch: {path}')
if not a.verify_only:
 root.mkdir(parents=True, exist_ok=True)
 for source, target, expected in rows:
  dest=root / target; dest.parent.mkdir(parents=True, exist_ok=True)
  shutil.copy2(a.dependencies / source, dest)
 (root / 'models/face-landmarker/NOTICE.txt').write_bytes(notice)
for _, target, expected in rows:
 dest=root / target
 if digest(dest) != expected: raise ValueError(f'Staged face asset mismatch: {dest}')
 checks.append({'path':target,'sha256':expected,'bytes':dest.stat().st_size})
notice_path=root / 'models/face-landmarker/NOTICE.txt'
if not notice_path.is_file() or notice_path.read_bytes()!=notice:raise ValueError('Face asset NOTICE mismatch')
checks.append({'path':notice_path.relative_to(root).as_posix(),'sha256':digest(notice_path),'bytes':notice_path.stat().st_size})
print(json.dumps({'verifiedFaceAssets':checks,'directory':str(root)},indent=2))
