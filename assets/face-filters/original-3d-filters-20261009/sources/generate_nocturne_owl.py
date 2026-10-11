"""
generate_nocturne_owl.py
Constructs the original Nocturne Owl Mask:
- Layered feather-shaped facial discs radiating from eyes
- Curved raptor brow ridge sweeping into tall horned plumicorns (ear tufts)
- Prominent hooked raptor beak distinctly separated from eyes
- Modest open eye openings aligned to reference pupils
- Preserved depth and silhouette with 3.5mm solid shell thickness
- Core glTF metallic-roughness PBR with embedded 2048x2048 textures
- Calibrated origin at nose bridge (0, 0, 0).
"""
import os
import math
import bpy
import bmesh
from mathutils import Vector, Matrix

def create_nocturne_owl_mask(output_blend, output_glb, textures_dir):
    bpy.ops.wm.read_factory_settings(use_empty=True)

    mesh = bpy.data.meshes.new("NocturneOwlMaskMesh")
    obj = bpy.data.objects.new("NocturneOwlMask", mesh)
    bpy.context.collection.objects.link(obj)

    bm = bmesh.new()

    # 1. Twin Facial Discs (Left & Right) with layered radiating plumage
    for disc_sign in [-1.0, 1.0]:
        disc_cx = disc_sign * 0.038
        disc_cy = -0.008
        disc_cz = 0.008

        # Radiating rings: from inner eye rim to outer cheek flare
        ring_radii = [
            (0.012, 0.009, -0.002), # Inner eye rim (hole border)
            (0.018, 0.014, 0.000),  # Tier 1 fine plumage
            (0.026, 0.021, 0.003),  # Tier 2 radiating feathers
            (0.035, 0.029, 0.006),  # Tier 3 contour feathers
            (0.046, 0.039, 0.009),  # Tier 4 outer disc
            (0.058, 0.050, 0.012),  # Tier 5 disc perimeter
            (0.070, 0.062, 0.016),  # Tier 6 outer cheek flare
        ]
        num_angles = 36
        disc_vert_rings = []

        for r_idx, (rx, rz, depth_offset) in enumerate(ring_radii):
            ring = []
            for a_idx in range(num_angles):
                theta = (a_idx / num_angles) * 2 * math.pi
                # Stepped scalloped feather edge on outer rings
                scallop = 1.0 + 0.08 * math.sin(theta * 8) if r_idx >= 2 else 1.0

                # Face curvature (Y curves backward as X or Z increases)
                dx = rx * math.cos(theta) * scallop
                dz = rz * math.sin(theta) * scallop

                # Asymmetrical cheek flare (more width laterally than medially)
                if disc_sign * dx > 0: # lateral side
                    dx *= 1.25

                vx = disc_cx + dx
                vz = disc_cz + dz

                # Conforming facial depth (ellipsoidal curvature)
                dist_from_bridge = math.sqrt(vx * vx + (vz - 0.01) ** 2)
                vy = disc_cy + 0.035 * (1.0 - math.cos(min(math.pi * 0.45, dist_from_bridge / 0.09)))
                vy += depth_offset

                # Outer feathers step slightly forward for shingled look
                if r_idx > 0:
                    vy -= 0.0025 * r_idx

                v = bm.verts.new((vx, vy, vz))
                ring.append(v)
            disc_vert_rings.append(ring)

        # Build faces between rings
        for r_idx in range(len(ring_radii) - 1):
            for a_idx in range(num_angles):
                a_next = (a_idx + 1) % num_angles
                v00 = disc_vert_rings[r_idx][a_idx]
                v01 = disc_vert_rings[r_idx][a_next]
                v11 = disc_vert_rings[r_idx + 1][a_next]
                v10 = disc_vert_rings[r_idx + 1][a_idx]
                bm.faces.new((v00, v01, v11, v10))

    # 2. Prominent Hooked Raptor Beak (Centrally anchored, distinct from eyes)
    beak_layers = 24
    beak_rings = []

    # Beak curves from nose bridge forward (-Y) and hooked sharply down (-Z)
    for b_idx in range(beak_layers):
        t = b_idx / (beak_layers - 1)

        # Trajectory of the beak central spine
        # Starts at bridge (y = -0.018, z = -0.005)
        # Projects forward to y = -0.046, then curves downward to z = -0.065
        by = -0.018 - 0.028 * math.sin(t * math.pi * 0.55)
        bz = -0.005 - 0.060 * (t ** 1.8)

        # Width and height of beak cross section
        bw = 0.014 * (1.0 - t ** 1.2) + 0.001
        bh = 0.018 * (1.0 - t ** 1.2) + 0.001

        ring_pts = 12
        ring = []
        for p in range(ring_pts):
            phi = (p / ring_pts) * 2 * math.pi
            # Tapered teardrop / wedge profile
            px = bw * math.cos(phi)
            pz = bh * math.sin(phi)
            if phi > -math.pi * 0.5 and phi < math.pi * 0.5:
                # Top ridge is sharper
                pz *= 1.2
            v = bm.verts.new((px, by, bz + pz))
            ring.append(v)
        beak_rings.append(ring)

    beak_tip = bm.verts.new((0.0, -0.046, -0.068)) # sharp hooked beak apex

    for b_idx in range(beak_layers - 1):
        for p in range(12):
            p_next = (p + 1) % 12
            v00 = beak_rings[b_idx][p]
            v01 = beak_rings[b_idx][p_next]
            v11 = beak_rings[b_idx + 1][p_next]
            v10 = beak_rings[b_idx + 1][p]
            bm.faces.new((v00, v01, v11, v10))

    for p in range(12):
        p_next = (p + 1) % 12
        bm.faces.new((beak_rings[-1][p], beak_rings[-1][p_next], beak_tip))

    # Upper Beak Cere / Bridge connector
    cere_top = bm.verts.new((0.0, -0.015, 0.005))
    for p in range(12):
        p_next = (p + 1) % 12
        bm.faces.new((cere_top, beak_rings[0][p_next], beak_rings[0][p]))

    # 3. Curved Raptor Brow Ridge & Plumicorns (Horned Ear Tufts)
    for horn_sign in [-1.0, 1.0]:
        horn_layers = 22
        horn_rings = []

        base_x = horn_sign * 0.046
        base_y = 0.008
        base_z = 0.042

        tip_x = horn_sign * 0.076
        tip_y = 0.035
        tip_z = 0.160

        for h_idx in range(horn_layers):
            t = h_idx / (horn_layers - 1)
            # Sweeping curve (s-curve flare)
            hx = base_x + (tip_x - base_x) * (t ** 0.9)
            hy = base_y + (tip_y - base_y) * (t ** 1.1)
            hz = base_z + (tip_z - base_z) * t

            hw = 0.024 * (1.0 - t ** 1.4) + 0.001
            hd = 0.012 * (1.0 - t ** 1.4) + 0.001

            h_pts = 12
            ring = []
            for p in range(h_pts):
                phi = (p / h_pts) * 2 * math.pi
                # Flattened blade-like feather cross section
                px = hw * math.cos(phi)
                py = hd * math.sin(phi)
                v = bm.verts.new((hx + px, hy + py, hz))
                ring.append(v)
            horn_rings.append(ring)

        horn_tip = bm.verts.new((tip_x, tip_y, tip_z + 0.005))

        for h_idx in range(horn_layers - 1):
            for p in range(12):
                p_next = (p + 1) % 12
                v00 = horn_rings[h_idx][p]
                v01 = horn_rings[h_idx][p_next]
                v11 = horn_rings[h_idx + 1][p_next]
                v10 = horn_rings[h_idx + 1][p]
                bm.faces.new((v00, v01, v11, v10))

        for p in range(12):
            p_next = (p + 1) % 12
            bm.faces.new((horn_rings[-1][p], horn_rings[-1][p_next], horn_tip))

        # Layered feather quill shingles extending along brow
        for shingle_idx in range(4):
            sz = base_z - 0.012 + shingle_idx * 0.010
            sy = base_y - 0.004 + shingle_idx * 0.002
            sx = base_x - horn_sign * (0.012 - shingle_idx * 0.004)
            s_base1 = bm.verts.new((sx - horn_sign * 0.010, sy, sz))
            s_base2 = bm.verts.new((sx + horn_sign * 0.012, sy, sz))
            s_tip = bm.verts.new((sx + horn_sign * 0.005, sy - 0.006, sz + 0.018))
            bm.faces.new((s_base1, s_base2, s_tip))

    # 4. Brow Bridge Connecting Plates
    bridge_center = bm.verts.new((0.0, -0.012, 0.028))
    bridge_left = bm.verts.new((0.024, -0.008, 0.034))
    bridge_right = bm.verts.new((-0.024, -0.008, 0.034))
    bridge_top = bm.verts.new((0.0, 0.002, 0.048))

    bm.faces.new((bridge_center, bridge_left, bridge_top))
    bm.faces.new((bridge_center, bridge_top, bridge_right))

    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(mesh)
    bm.free()

    # Apply Solidify Modifier for 3.5mm thick solid structure
    solid_mod = obj.modifiers.new("Solidify", "SOLIDIFY")
    solid_mod.thickness = 0.0035
    solid_mod.offset = 1.0 # inward
    solid_mod.use_rim = True

    # Subdivision Surface for smooth organic feather curves
    sub_mod = obj.modifiers.new("Subdivision", "SUBSURF")
    sub_mod.levels = 1
    sub_mod.render_levels = 1

    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.convert(target='MESH')

    # Triangulate
    tri_mod = obj.modifiers.new("Triangulate", "TRIANGULATE")
    tri_mod.quad_method = 'BEAUTY'
    tri_mod.ngon_method = 'BEAUTY'
    bpy.ops.object.convert(target='MESH')

    mesh.shade_smooth()

    # Generate UV0 mapping
    uv_layer = mesh.uv_layers.new(name="UVMap")
    mesh.uv_layers.active = uv_layer

    min_x = min(v.co.x for v in mesh.vertices)
    max_x = max(v.co.x for v in mesh.vertices)
    min_z = min(v.co.z for v in mesh.vertices)
    max_z = max(v.co.z for v in mesh.vertices)
    span_x = max_x - min_x
    span_z = max_z - min_z

    for poly in mesh.polygons:
        for loop_idx in poly.loop_indices:
            loop = mesh.loops[loop_idx]
            vert = mesh.vertices[loop.vertex_index]
            u = 0.5 + (vert.co.x / (span_x * 1.06))
            v = (vert.co.z - min_z) / span_z
            u = max(0.01, min(0.99, u))
            v = max(0.01, min(0.99, v))
            uv_layer.data[loop_idx].uv = (u, v)

    # Set up PBR Material with embedded textures
    mat = bpy.data.materials.new("NocturneOwlMaterial")
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    bsdf = nodes.get("Principled BSDF")

    albedo_img_path = os.path.join(textures_dir, "nocturne_owl_albedo.png")
    tex_albedo = nodes.new("ShaderNodeTexImage")
    tex_albedo.image = bpy.data.images.load(albedo_img_path)
    links.new(bsdf.inputs["Base Color"], tex_albedo.outputs["Color"])

    rm_img_path = os.path.join(textures_dir, "nocturne_owl_roughness_metal.png")
    tex_rm = nodes.new("ShaderNodeTexImage")
    tex_rm.image = bpy.data.images.load(rm_img_path)
    sep_color = nodes.new("ShaderNodeSeparateColor")
    links.new(sep_color.inputs["Color"], tex_rm.outputs["Color"])
    links.new(bsdf.inputs["Roughness"], sep_color.outputs["Green"])
    links.new(bsdf.inputs["Metallic"], sep_color.outputs["Blue"])

    obj.data.materials.append(mat)

    num_verts = len(mesh.vertices)
    num_tris = len(mesh.polygons)
    print(f"Nocturne Owl Mask: {num_verts} vertices, {num_tris} triangles")

    os.makedirs(os.path.dirname(output_blend), exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=output_blend)
    print(f"Saved source blend: {output_blend}")

    os.makedirs(os.path.dirname(output_glb), exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=output_glb,
        export_format='GLB',
        export_yup=True,
        export_apply=True,
        export_cameras=False,
        export_lights=False,
        export_texcoords=True,
        export_normals=True,
        export_materials='EXPORT',
        export_draco_mesh_compression_enable=False,
        export_meshopt_compression_enable=False
    )
    print(f"Exported GLB: {output_glb} ({os.path.getsize(output_glb)} bytes)")

if __name__ == "__main__":
    blend_path = str((__import__("pathlib").Path(__file__).resolve().parent / "../sources/nocturne-owl-mask.blend").resolve())
    glb_path = str((__import__("pathlib").Path(__file__).resolve().parent / "../models/nocturne-owl-mask.glb").resolve())
    tex_dir = str((__import__("pathlib").Path(__file__).resolve().parent / "../sources/textures").resolve())
    create_nocturne_owl_mask(blend_path, glb_path, tex_dir)
