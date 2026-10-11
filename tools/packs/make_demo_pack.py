from pathlib import Path
import argparse, hashlib, json, struct, zlib, zipfile

def digest(data):
    return hashlib.sha256(data).hexdigest()

def main():
    parser=argparse.ArgumentParser(description='Create an original Warm Studio LUT demo ZIP for the real pack importer.')
    parser.add_argument('output',type=Path,help='New output directory; existing directories are never overwritten')
    args=parser.parse_args()
    repo=Path(__file__).resolve().parents[2]
    source=repo/'windows/src/effects/packs/samples/pack_warm_studio'
    out=args.output.resolve()
    if out.exists():
        raise SystemExit('Output already exists; choose a new directory')
    original=json.loads((source/'manifest.json').read_text(encoding='utf-8'))
    payloads={}
    assets=[]
    for asset in original['assets']:
        if asset['type'] not in ('CubeLut','PreviewImage'):
            continue
        data=(source/asset['path']).read_bytes()
        if len(data)!=asset['size'] or digest(data)!=asset['sha256']:
            raise SystemExit('Sample payload differs from its manifest')
        asset=dict(asset)
        if asset['type']=='PreviewImage':
            if len(data)!=64*64*4:
                raise SystemExit('Sample raw dimensions differ')
            def chunk(kind,body):
                return struct.pack('>I',len(body))+kind+body+struct.pack('>I',zlib.crc32(kind+body)&0xffffffff)
            rows=b''.join(b'\0'+data[y*64*4:(y+1)*64*4] for y in range(64))
            data=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',64,64,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(rows,9))+chunk(b'IEND',b'')
            asset.update(path='preview.png',sha256=digest(data),size=len(data))
        payloads[asset['path']]=data
        assets.append(asset)
    manifest=dict(schemaVersion=1,packId='com.prismforge.packs.warmstudio.demo',version='1.0.0',displayName='Warm Studio Demo',description='Original warm colour grade. Preview is an illustrative colour swatch.',author='PrismForge Studio',previewImagePath='preview.png',assets=assets)
    payloads['manifest.json']=(json.dumps(manifest,indent=2,ensure_ascii=False)+'\n').encode('utf-8')
    out.mkdir(parents=True,exist_ok=False)
    folder=out/'warm-studio-demo'
    for name,data in payloads.items():
        target=folder/name
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes(data)
    archive=out/'warm-studio-demo-1.0.0.zip'
    with zipfile.ZipFile(archive,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for name in sorted(payloads):
            entry=zipfile.ZipInfo(name,date_time=(2026,10,9,0,0,0))
            entry.compress_type=zipfile.ZIP_DEFLATED
            entry.create_system=0
            z.writestr(entry,payloads[name])
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None and sorted(z.namelist())==sorted(payloads)
        for name,data in payloads.items():
            assert z.read(name)==data
    receipt=dict(kind='Original LUT demo delivery, not a published catalogue',packId=manifest['packId'],version=manifest['version'],archive=archive.name,archiveSha256=digest(archive.read_bytes()),archiveBytes=archive.stat().st_size,files=[dict(path=name,sha256=digest(data),bytes=len(data)) for name,data in sorted(payloads.items())],sourceManifestSha256=digest((source/'manifest.json').read_bytes()),generatorSha256=digest(Path(__file__).read_bytes()))
    (out/'inventory.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
    (out/'README.txt').write_text('In PrismForge Studio choose Colour look > Effects packs > Import ZIP, select this ZIP, then select the LUT and Apply effect. The same pack works offline after import. This is an original LUT demonstration; it adds no accessory models. The preview is a synthetic colour swatch, not a photograph. Distribution is local development only pending commercial release review.\n',encoding='utf-8')
    print(json.dumps(receipt,indent=2))

if __name__=='__main__':
    main()