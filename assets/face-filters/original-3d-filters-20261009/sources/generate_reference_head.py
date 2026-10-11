"""
generate_reference_head.py
Generates the neutral adult reference head (approx 0.16m temple width)
centered at the nose bridge origin (0, 0, 0).
Used as an authoring guide and for multi-angle render verification.
"""
import os
import json
import math
import bpy
import bmesh
from mathutils import Vector

def build_reference_head(output_blend, output_glb=None):
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # Path to face.json oracle proxy
    oracle_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "oracle"))
    # Codex fallback if running standalone
    face_json_path = os.path.join(oracle_dir, "face.json")
    if not os.path.exists(face_json_path):
        face_json_path = "LOCAL_REFERENCE_FACE.json"

    with open(face_json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    raw_pos = data["data"]["attributes"]["position"]["array"]
    raw_idx = data["data"]["index"]["array"]

    # Compute scale for temple width 0.16m
    xs = raw_pos[0::3]
    width_raw = max(xs) - min(xs)
    scale = 0.160 / width_raw

    # Convert coordinates: face +Y -> Blender +Z, face +Z -> Blender -Y, face +X -> Blender +X
    verts = []
    num_verts = len(raw_pos) // 3
    for i in range(num_verts):
        vx = raw_pos[i * 3] * scale
        vy = -raw_pos[i * 3 + 2] * scale
        vz = raw_pos[i * 3 + 1] * scale
        verts.append(Vector((vx, vy, vz)))

    # Shift so nose bridge (vert 68) is at origin (0, 0, 0)
    anchor_offset = Vector((verts[68].x, verts[68].y, verts[68].z))
    for i in range(len(verts)):
        verts[i] -= anchor_offset

    faces = []
    num_faces = len(raw_idx) // 3
    for i in range(num_faces):
        faces.append([raw_idx[i * 3], raw_idx[i * 3 + 1], raw_idx[i * 3 + 2]])

    mesh = bpy.data.meshes.new("ReferenceHeadMesh")
    mesh.from_pydata(verts, [], faces)
    mesh.validate()

    obj = bpy.data.objects.new("NeutralReferenceHead", mesh)
    bpy.context.collection.objects.link(obj)

    # Create cranial rear dome to give full 3D head volume
    bm = bmesh.new()
    bm.from_mesh(mesh)

    # Construct rear skull dome
    dome_center = Vector((0.0, 0.065, 0.020))
    dome_rx = 0.076
    dome_ry = 0.090
    dome_rz = 0.095

    dome_verts = []
    lat_steps = 14
    lon_steps = 20
    # Semi-ellipsoid covering the rear half of the head
    for i in range(lat_steps + 1):
        theta = (i / lat_steps) * (math.pi / 2) # from top (0) to equator (pi/2)
        z = dome_center.z + dome_rz * math.cos(theta)
        r_scale = math.sin(theta)
        row = []
        for j in range(lon_steps + 1):
            phi = math.pi * (0.0 + (j / lon_steps)) # back hemisphere (0 to pi in local space)
            # local coords: Y is positive (back), X is left/right
            x = dome_center.x + dome_rx * r_scale * math.cos(phi)
            y = dome_center.y + dome_ry * r_scale * math.sin(phi)
            v = bm.verts.new((x, y, z))
            row.append(v)
        dome_verts.append(row)

    for i in range(lat_steps):
        for j in range(lon_steps):
            v1 = dome_verts[i][j]
            v2 = dome_verts[i][j + 1]
            v3 = dome_verts[i + 1][j + 1]
            v4 = dome_verts[i + 1][j]
            try:
                bm.faces.new((v1, v2, v3, v4))
            except Exception:
                pass

    bm.to_mesh(mesh)
    bm.free()

    # Recalculate normals and smooth shading
    mesh.shade_smooth()

    # Material: neutral grey studio matte
    mat = bpy.data.materials.new("NeutralHeadMat")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = (0.553, 0.588, 0.573, 1.0) # #8D9692
        bsdf.inputs["Roughness"].default_value = 0.65
        bsdf.inputs["Metallic"].default_value = 0.0
    obj.data.materials.append(mat)

    # Save .blend
    os.makedirs(os.path.dirname(output_blend), exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=output_blend)
    print(f"Saved reference head to: {output_blend}")

    if output_glb:
        os.makedirs(os.path.dirname(output_glb), exist_ok=True)
        bpy.ops.export_scene.gltf(
            filepath=output_glb,
            export_format='GLB',
            export_yup=True,
            export_apply=True,
            export_cameras=False,
            export_lights=False
        )
        print(f"Exported reference head GLB to: {output_glb}")

if __name__ == "__main__":
    blend_path = str((__import__("pathlib").Path(__file__).resolve().parent / "../sources/neutral-reference-head.blend").resolve())
    build_reference_head(blend_path)
