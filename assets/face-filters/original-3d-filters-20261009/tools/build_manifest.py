"""
build_manifest.py
Inspects the delivery payloads and writes manifest.json.
Records relative paths, exact byte counts, SHA256 hashes, model specifications,
starting fits, texture sizes, and provenance.
"""
import os
import json
import hashlib
import struct

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().upper()

def get_glb_stats(glb_path):
    data = open(glb_path, "rb").read()
    magic, version, length = struct.unpack_from("<III", data, 0)
    json_len, json_type = struct.unpack_from("<II", data, 12)
    gltf = json.loads(data[20:20 + json_len].decode("utf-8"))

    min_xyz = [1e9, 1e9, 1e9]
    max_xyz = [-1e9, -1e9, -1e9]
    total_vertices = 0
    total_triangles = 0

    for mesh in gltf.get("meshes", []):
        for prim in mesh.get("primitives", []):
            pos_acc = gltf["accessors"][prim["attributes"]["POSITION"]]
            total_vertices += pos_acc["count"]
            for c in range(3):
                min_xyz[c] = min(min_xyz[c], pos_acc["min"][c])
                max_xyz[c] = max(max_xyz[c], pos_acc["max"][c])

            if "indices" in prim:
                idx_acc = gltf["accessors"][prim["indices"]]
                total_triangles += idx_acc["count"] // 3
            else:
                total_triangles += pos_acc["count"] // 3

    return {
        "vertices": total_vertices,
        "triangles": total_triangles,
        "primitives": len(gltf.get("meshes", [{}])[0].get("primitives", [])),
        "nodes": len(gltf.get("nodes", [])),
        "materials": len(gltf.get("materials", [])),
        "textures": len(gltf.get("textures", [])),
        "images": len(gltf.get("images", [])),
        "bounds": {
            "min": [round(v, 4) for v in min_xyz],
            "max": [round(v, 4) for v in max_xyz],
            "dimensions_m": [round(max_xyz[c] - min_xyz[c], 4) for c in range(3)]
        }
    }

def main():
    delivery_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

    # Load fitting profiles
    fitting_path = os.path.join(delivery_dir, "fitting-profiles.json")
    with open(fitting_path, "r", encoding="utf-8") as f:
        fitting_data = json.load(f)

    # Models metadata
    models_meta = {
        "aurora-fox-mask": {
            "name": "Aurora fox mask",
            "description": "Original sculpted kitsune / arctic fox ceramic mask with outward-flaring curved cheeks, shaped muzzle, black nose pad, clean almond eye cutouts, side-anchored ears with 3D inner fur tufts, vermilion lacquer ceremonial markings and gold leaf accents.",
            "provenance": "All-original procedural geometry and original textures authored by Antigravity IDE for PrismForgeStudio. No borrowed trademarks, copyrighted characters, or external scans.",
            "licence": "Proprietary - PrismForge Studio internal asset collection.",
            "textures": [
                {"name": "aurora_fox_albedo.png", "resolution": "2048x2048", "format": "PNG (RGBA)", "role": "Base Color"},
                {"name": "aurora_fox_roughness_metal.png", "resolution": "2048x2048", "format": "PNG (RGBA)", "role": "Metallic-Roughness (G=Roughness, B=Metallic)"}
            ]
        },
        "nocturne-owl-mask": {
            "name": "Nocturne owl mask",
            "description": "Original nocturnal raptor face mask featuring concentric layered feather discs radiating around modest eye openings, sweeping curved brow ridge with tall horned plumicorns (ear tufts), and a pronounced hooked beak distinctly separated from the eyes.",
            "provenance": "All-original procedural geometry and original textures authored by Antigravity IDE for PrismForgeStudio. No borrowed trademarks, copyrighted characters, or external scans.",
            "licence": "Proprietary - PrismForge Studio internal asset collection.",
            "textures": [
                {"name": "nocturne_owl_albedo.png", "resolution": "2048x2048", "format": "PNG (RGBA)", "role": "Base Color"},
                {"name": "nocturne_owl_roughness_metal.png", "resolution": "2048x2048", "format": "PNG (RGBA)", "role": "Metallic-Roughness (G=Roughness, B=Metallic)"}
            ]
        },
        "atlas-explorer-helmet": {
            "name": "Atlas explorer helmet",
            "description": "Original deep space EVA / planetary exploration helmet featuring a full rounded crown, solid occiput volume preserved at 35-degree downward tilt, panoramic bubble visor (glTF alphaMode: BLEND) framed by heavy opaque pressure gasket, lateral comms pods, cheek rebreather canisters, ribbed conduits, and docking neck ring.",
            "provenance": "All-original procedural geometry and original textures authored by Antigravity IDE for PrismForgeStudio. No borrowed trademarks, copyrighted characters, or external scans.",
            "licence": "Proprietary - PrismForge Studio internal asset collection.",
            "textures": [
                {"name": "atlas_helmet_albedo.png", "resolution": "2048x2048", "format": "PNG (RGBA)", "role": "Base Color"},
                {"name": "atlas_helmet_roughness_metal.png", "resolution": "2048x2048", "format": "PNG (RGBA)", "role": "Metallic-Roughness (G=Roughness, B=Metallic)"}
            ]
        }
    }

    # Inventory all files
    payloads = {}
    for root, dirs, filenames in os.walk(delivery_dir):
        for fn in filenames:
            if fn == "manifest.json":
                continue
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, delivery_dir).replace("\\", "/")
            payloads[rel] = {
                "bytes": os.path.getsize(full),
                "sha256": sha256_file(full)
            }

    # Build model summaries
    models_summary = {}
    for model_id, meta in models_meta.items():
        glb_rel = f"models/{model_id}.glb"
        glb_full = os.path.join(delivery_dir, glb_rel)
        stats = get_glb_stats(glb_full)
        prof = fitting_data["profiles"][model_id]

        models_summary[model_id] = {
            "name": meta["name"],
            "description": meta["description"],
            "file": glb_rel,
            "bytes": payloads[glb_rel]["bytes"],
            "sha256": payloads[glb_rel]["sha256"],
            "drawn_counts": {
                "vertices": stats["vertices"],
                "triangles": stats["triangles"],
                "primitives": stats["primitives"],
                "nodes": stats["nodes"],
                "materials": stats["materials"],
                "textures": stats["textures"],
                "images": stats["images"]
            },
            "bounds": stats["bounds"],
            "coordinate_axes": "glTF 2.0 (+X Right, +Y Up, +Z Forward)",
            "textures": meta["textures"],
            "starting_fit": prof["starting_fit"],
            "normalization": prof["lab_normalization"],
            "provenance": meta["provenance"],
            "licence": meta["licence"]
        }

    manifest = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "title": "PrismForge Original 3D Face Filters Manifest",
        "delivery": "original-3d-filters-20261009",
        "author": "Antigravity IDE",
        "date": "2026-10-09",
        "spec_version": "glTF 2.0 Binary (GLB)",
        "models": models_summary,
        "payloads": payloads
    }

    manifest_path = os.path.join(delivery_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    print(f"Manifest written to: {manifest_path}")
    print(f"Total tracked payloads: {len(payloads)}")

if __name__ == "__main__":
    main()
