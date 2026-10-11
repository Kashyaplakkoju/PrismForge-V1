"""Verify exact bundled native starter models without loading untrusted geometry."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

EXPECTED = {"aurora-fox-mask", "nocturne-owl-mask", "atlas-explorer-helmet"}

def verify(directory):
    assets = directory / "res" / "face-filters"
    manifest = json.loads((assets / "manifest.json").read_text(encoding="utf-8-sig"))
    models = manifest.get("models", [])
    if manifest.get("schema") != "PFM1" or len(models) != 3 or {m.get("id") for m in models} != EXPECTED:
        raise ValueError("Native collection manifest mismatch")
    results = []
    for record in models:
        name = record.get("nativeFile")
        if name != record["id"] + ".pfmodel":
            raise ValueError("Invalid native asset filename")
        raw = (assets / name).read_bytes()
        if len(raw) > 64 * 1024 * 1024 or len(raw) < 52 or len(raw) != record["nativeBytes"] or hashlib.sha256(raw).hexdigest().upper() != record["nativeSha256"].upper():
            raise ValueError("Native asset bytes/hash drift: " + name)
        version, vertices, triangles, materials, bw, bh, mw, mh, payload, ipd, ey, ez = struct.unpack_from("<9I3f", raw, 4)
        if raw[:4] != b"PFM1" or version != 1 or not 0 < vertices <= 60000 or not 0 < triangles <= 60000 or not 0 < materials <= 8:
            raise ValueError("Invalid native asset header: " + name)
        if not all(math.isfinite(v) for v in (ipd, ey, ez)) or not .035 <= ipd <= .12 or abs(ey) > .2 or abs(ez) > .2:
            raise ValueError("Invalid native metric fit: " + name)
        for w, h in ((bw, bh), (mw, mh)):
            if not ((w == h == 0) or (0 < w <= 2048 and 0 < h <= 2048)):
                raise ValueError("Invalid bounded texture: " + name)
        expected = vertices * 32 + triangles * 16 + materials * 32 + bw * bh * 4 + mw * mh * 4
        if payload != expected or len(raw) != 52 + expected or vertices != record["vertices"] or triangles != record["triangles"] or materials != record["materials"]:
            raise ValueError("Invalid exact native payload extent: " + name)
        results.append({"id": record["id"], "bytes": len(raw), "sha256": record["nativeSha256"]})
    if not (assets / "NOTICE.txt").is_file():
        raise ValueError("Native asset provenance notice missing")
    return results

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    results = verify(args.directory)
    print("Native collection verified: " + str(len(results)) + " exact bounded models")
