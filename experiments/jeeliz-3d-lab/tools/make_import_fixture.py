"""Export an original static GLB visor fixture, using only Python's standard library.

Usage: python make_import_fixture.py OUTPUT.glb
The aqua frame is an upload/fit test, not a production accessory or tracking claim.
"""
from pathlib import Path
import json
import struct
import sys


def export(destination: Path) -> None:
    positions = []
    indices = []
    # Six independent solids form two open eye frames with a bridge and temples.
    def box(cx, cy, cz, sx, sy, sz):
        start = len(positions) // 3
        for x, y, z in [(-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1),
                        (-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1)]:
            positions.extend((cx+x*sx/2, cy+y*sy/2, cz+z*sz/2))
        for face in [(0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)]:
            a,b,c,d = [start + i for i in face]
            indices.extend((a,b,c,a,c,d))
    for x in (-.48,.48):
        box(x,.30,.25,.84,.10,.13)
        box(x,-.17,.25,.84,.10,.13)
        for side in (-1,1):
            box(x+side*.37,.065,.25,.10,.47,.13)
    box(0,.10,.25,.18,.10,.13)
    for side in (-1,1):
        box(side*.86,.10,-.03,.10,.12,.55)
    pos = struct.pack('<'+'f'*len(positions), *positions)
    ind = struct.pack('<'+'H'*len(indices), *indices)
    binary = pos + ind
    metadata = {'asset':{'version':'2.0','generator':'PrismForge original upload fixture'},
                'scene':0,'scenes':[{'nodes':[0]}],'nodes':[{'name':'Original aqua visor','mesh':0}],
                'buffers':[{'byteLength':len(binary)}],
                'bufferViews':[{'buffer':0,'byteOffset':0,'byteLength':len(pos)},
                               {'buffer':0,'byteOffset':len(pos),'byteLength':len(ind)}],
                'accessors':[{'bufferView':0,'componentType':5126,'count':len(positions)//3,'type':'VEC3'},
                             {'bufferView':1,'componentType':5123,'count':len(indices),'type':'SCALAR'}],
                'meshes':[{'primitives':[{'attributes':{'POSITION':0},'indices':1,'material':0}]}],
                'materials':[{'name':'Aqua polymer','pbrMetallicRoughness':{
                    'baseColorFactor':[.04,.68,.74,1],'metallicFactor':.25,'roughnessFactor':.35}}]}
    text = json.dumps(metadata,separators=(',',':')).encode('utf-8')
    text += b' ' * (-len(text) % 4)
    binary += b'\0' * (-len(binary) % 4)
    payload = struct.pack('<III',0x46546c67,2,28+len(text)+len(binary))
    payload += struct.pack('<II',len(text),0x4e4f534a) + text
    payload += struct.pack('<II',len(binary),0x004e4942) + binary
    destination.parent.mkdir(parents=True,exist_ok=True)
    destination.write_bytes(payload)
    print(json.dumps({'path':str(destination.resolve()),'bytes':len(payload),
                      'vertices':len(positions)//3,'triangles':len(indices)//3,
                      'authorship':'Original PrismForge test geometry'}))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('Usage: python make_import_fixture.py OUTPUT.glb')
    export(Path(sys.argv[1]))
