"""Bake the owner's accepted crown fit into a private native PFM copy.

Input is the PFM produced by PrismForge's native GLB importer. This changes
only vertex positions and the pupil anchor; original model files stay intact.
The result is a private development asset, not redistribution permission.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error("Use a separate output copy")
    original = args.input.read_bytes()
    if len(original) < 52 or len(original) > 128 * 1024 * 1024:
        parser.error("Expected a bounded native PFM")
    if original[:4] != b"PFM1" or struct.unpack_from("<I", original, 4)[0] != 1:
        parser.error("Expected native PFM version 1")
    nv, nt, nm, bw, bh, mw, mh, payload = struct.unpack_from("<8I", original, 8)
    if not (0 < nv <= 1_000_000 and 0 < nt <= 1_000_000 and 0 < nm <= 16):
        parser.error("Invalid model counts")
    if payload != nv * 32 + nt * 16 + nm * 32 + (bw * bh + mw * mh) * 4 or len(original) != payload + 52:
        parser.error("Invalid exact payload extent")
    ipd, ey, ez = struct.unpack_from("<3f", original, 40)
    if not all(math.isfinite(q) for q in (ipd, ey, ez)) or not .035 <= ipd <= .12:
        parser.error("Invalid pupil anchor")
    units = 100 * .063 / ipd
    fitted = bytearray(original)
    for at in range(52, 52 + nv * 32, 32):
        x, y, z = struct.unpack_from("<3f", original, at)
        position = (1.26 * x, 1.26 * (y - ey) + 8.8 / units, 1.26 * (z - ez) - 5.8 / units)
        if not all(math.isfinite(q) and abs(q) <= 2 for q in position):
            parser.error("Fitted position exceeds native metric bounds")
        struct.pack_into("<3f", fitted, at, *position)
    struct.pack_into("<2f", fitted, 44, 0, 0)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(fitted)
    print(json.dumps({"inputSha256": hashlib.sha256(original).hexdigest(),
                      "outputSha256": hashlib.sha256(fitted).hexdigest(),
                      "vertices": nv, "triangles": nt, "size": 1.26,
                      "offsetCm": [0, 8.8, -5.8], "rotationDegrees": [0, 0, 0]}))


if __name__ == "__main__":
    main()
