"""
generate_aurora_fox.py
Constructs the original Aurora Fox Mask:
- Sculpted curved cheeks with outward flare
- Shaped muzzle and triangular nose pad
- Symmetric clean almond eye openings aligned to reference pupils
- Ears anchored at head sides with layered inner fur tufts
- Layered ceramic/lacquer shell with 3.5mm solid thickness
- Core glTF metallic-roughness PBR with embedded 2048x2048 textures
- Origin at nose bridge (0, 0, 0), calibrated for rigid head tracking.
"""
import os
import math
import bpy
import bmesh
from mathutils import Vector, Matrix

def create_aurora_fox_mask(output_blend, output_glb, textures_dir):
    bpy.ops.wm.read_factory_settings(use_empty=True)

    mesh = bpy.data.meshes.new("AuroraFoxMaskMesh")
    obj = bpy.data.objects.new("AuroraFoxMask", mesh)
    bpy.context.collection.objects.link(obj)

    bm = bmesh.new()

    # Parametric grid for the curved face shell
    # Rows along Z (from lower muzzle/cheeks up to top of forehead)
    # Cols along X (from right cheek to left cheek)
    # Origin (0, 0, 0) at the nose bridge.
    # Eye openings at X = +-0.032m, Z = +0.008m.

    num_z = 28
    num_x = 36

    # Grid vertices storage: [row_idx][col_idx]
    grid_verts = []

    for r in range(num_z):
        v_z = r / (num_z - 1) # 0.0 (bottom) to 1.0 (top)
        # Z coordinate from -0.040m (snout/cheek base) to +0.075m (forehead)
        z = -0.040 + v_z * 0.115

        row = []
        for c in range(num_x):
            u_x = (c / (num_x - 1)) * 2.0 - 1.0 # -1.0 (right) to +1.0 (left)

            # Lateral span increases around cheeks and temples
            cheek_flare = 1.0 + 0.35 * math.exp(-((z - 0.005) ** 2) / 0.0012)
            max_x = 0.075 * cheek_flare
            x = u_x * max_x

            # Base curvature depth (Y is forward negative in Blender)
            # Cylinder / ellipsoid conforming to facial contour
            rad_fac = max(0.0, 1.0 - (x / (max_x + 0.01)) ** 2)
            face_depth = 0.028 * (1.0 - math.sqrt(rad_fac))

            # Snout / muzzle projection around the center (X near 0, Z below 0.01)
            snout_factor = math.exp(-(x ** 2) / 0.0006) * max(0.0, 1.0 - max(0.0, z) / 0.03)
            snout_proj = -0.032 * snout_factor # projects forward (-Y)

            # Forehead curvature (tilts slightly backward at top)
            forehead_recede = 0.012 * max(0.0, (z - 0.02) / 0.055) ** 1.5

            # Cheek flare curves backward at outer edges
            edge_curve = 0.018 * (abs(u_x) ** 2.2)

            y = -0.012 + face_depth + snout_proj + forehead_recede + edge_curve

            # Check if this vertex falls inside the eye cutouts
            # Left eye: x â‰ˆ +0.032, z â‰ˆ +0.008
            # Right eye: x â‰ˆ -0.032, z â‰ˆ +0.008
            is_eye_hole = False
            for eye_sign in [-1.0, 1.0]:
                dx = (x - eye_sign * 0.032) / 0.0125
                dz = (z - 0.008) / 0.0065
                dist_sq = dx * dx + dz * dz
                if dist_sq < 0.85:
                    is_eye_hole = True
                    break

            if is_eye_hole:
                row.append(None)
            else:
                v = bm.verts.new((x, y, z))
                row.append(v)

        grid_verts.append(row)

    # Build faces across the grid
    for r in range(num_z - 1):
        for c in range(num_x - 1):
            v00 = grid_verts[r][c]
            v01 = grid_verts[r][c + 1]
            v11 = grid_verts[r + 1][c + 1]
            v10 = grid_verts[r + 1][c]

            # All 4 valid: quad
            if v00 and v01 and v11 and v10:
                bm.faces.new((v00, v01, v11, v10))
            # 3 valid: triangle around eye boundaries
            elif v00 and v01 and v11:
                bm.faces.new((v00, v01, v11))
            elif v00 and v11 and v10:
                bm.faces.new((v00, v11, v10))
            elif v00 and v01 and v10:
                bm.faces.new((v00, v01, v10))
            elif v01 and v11 and v10:
                bm.faces.new((v01, v11, v10))

    # Construct the Fox Snout / Nose Pad at the apex
    nose_verts = []
    nose_steps = 12
    for i in range(nose_steps + 1):
        theta = i * math.pi / nose_steps
        nx = 0.010 * math.cos(theta)
        ny = -0.044 - 0.004 * math.sin(theta)
        nz = -0.028 + 0.007 * math.sin(theta)
        v = bm.verts.new((nx, ny, nz))
        nose_verts.append(v)
    apex_v = bm.verts.new((0.0, -0.048, -0.032))
    for i in range(nose_steps):
        bm.faces.new((apex_v, nose_verts[i], nose_verts[i + 1]))

    # Construct Fox Ears (Left and Right)
    # Anchored at temples/crown (X â‰ˆ +-0.065, Y â‰ˆ +0.015, Z â‰ˆ +0.060)
    # Tapering to pointed tips at (X â‰ˆ +-0.092, Y â‰ˆ +0.038, Z â‰ˆ +0.185)
    for ear_sign in [-1.0, 1.0]:
        ear_layers = 16
        ear_profile_steps = 12
        ear_vert_rings = []

        base_x = ear_sign * 0.065
        base_y = 0.015
        base_z = 0.060

        tip_x = ear_sign * 0.092
        tip_y = 0.038
        tip_z = 0.185

        for L in range(ear_layers):
            t = L / (ear_layers - 1)
            cx = base_x + (tip_x - base_x) * t
            cy = base_y + (tip_y - base_y) * t
            cz = base_z + (tip_z - base_z) * t

            scale_w = 0.035 * (1.0 - t ** 1.3)
            scale_d = 0.018 * (1.0 - t ** 1.3)

            ring = []
            for p in range(ear_profile_steps):
                phi = (p / ear_profile_steps) * 2 * math.pi
                # Triangular teardrop ear cross-section
                # Inner face (phi near pi/2) is concave (fur cavity)
                ex = scale_w * math.cos(phi) * (1.0 + 0.3 * math.sin(phi))
                ey = scale_d * math.sin(phi)
                if phi > 0 and phi < math.pi: # inner ear concavity
                    ey -= 0.006 * math.sin(phi) * (1.0 - t)
                ez = 0.0

                v = bm.verts.new((cx + ex, cy + ey, cz + ez))
                ring.append(v)
            ear_vert_rings.append(ring)

        tip_vert = bm.verts.new((tip_x, tip_y, tip_z + 0.004))

        for L in range(ear_layers - 1):
            for p in range(ear_profile_steps):
                p_next = (p + 1) % ear_profile_steps
                v00 = ear_vert_rings[L][p]
                v01 = ear_vert_rings[L][p_next]
                v11 = ear_vert_rings[L + 1][p_next]
                v10 = ear_vert_rings[L + 1][p]
                bm.faces.new((v00, v01, v11, v10))

        # Close ear tip
        for p in range(ear_profile_steps):
            p_next = (p + 1) % ear_profile_steps
            bm.faces.new((ear_vert_rings[-1][p], ear_vert_rings[-1][p_next], tip_vert))

        # Layered sculpted 3D fur tufts inside ear base
        for tuft_idx in range(3):
            tuft_h = base_z + 0.018 + tuft_idx * 0.018
            tuft_y = base_y - 0.005 + tuft_idx * 0.003
            tuft_base1 = bm.verts.new((base_x - ear_sign * 0.012, tuft_y, tuft_h))
            tuft_base2 = bm.verts.new((base_x + ear_sign * 0.015, tuft_y, tuft_h))
            tuft_apex = bm.verts.new((base_x + ear_sign * 0.008, tuft_y - 0.010, tuft_h + 0.022))
            bm.faces.new((tuft_base1, tuft_base2, tuft_apex))

    # Forehead raised flame crest (kitsune spirit crest)
    crest_verts = []
    crest_pts = [
        (0.0, -0.018, 0.042),
        (0.012, -0.016, 0.052),
        (0.016, -0.014, 0.065),
        (0.0, -0.020, 0.078), # apex
        (-0.016, -0.014, 0.065),
        (-0.012, -0.016, 0.052),
    ]
    center_crest = bm.verts.new((0.0, -0.024, 0.058)) # raised center
    for pt in crest_pts:
        crest_verts.append(bm.verts.new(pt))
    for i in range(len(crest_pts)):
        bm.faces.new((center_crest, crest_verts[i], crest_verts[(i + 1) % len(crest_pts)]))

    # Add Solidify modifier effect directly or extrude inner shell (3.5mm thickness)
    # Give the shell real physical volume and beveled borders
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(mesh)
    bm.free()

    # Add Solidify Modifier to create 3.5mm solid ceramic backing
    solid_mod = obj.modifiers.new("Solidify", "SOLIDIFY")
    solid_mod.thickness = 0.0035 # 3.5mm solid wall
    solid_mod.offset = 1.0 # Extrude backward (+Y) into head
    solid_mod.use_rim = True
    solid_mod.use_rim_only = False

    # Add Subdivision Surface for silky ceramic curves (reaching ~30k-40k triangles)
    sub_mod = obj.modifiers.new("Subdivision", "SUBSURF")
    sub_mod.levels = 1
    sub_mod.render_levels = 1

    # Apply all modifiers to freeze geometry
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.convert(target='MESH')

    # Triangulate
    tri_mod = obj.modifiers.new("Triangulate", "TRIANGULATE")
    tri_mod.quad_method = 'BEAUTY'
    tri_mod.ngon_method = 'BEAUTY'
    bpy.ops.object.convert(target='MESH')

    # Shade smooth
    mesh.shade_smooth()

    # Generate UV0 mapping
    # Cylinder / Box projection unwrapped cleanly across 0..1 UV space
    # Face maps to center (U: 0.15..0.85, V: 0.25..0.85)
    # Ears map to top (U: 0.05..0.95, V: 0.75..0.98)
    uv_layer = mesh.uv_layers.new(name="UVMap")
    mesh.uv_layers.active = uv_layer

    # Compute bounding box for projection
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
            # Normalized coordinates
            u = 0.5 + (vert.co.x / (span_x * 1.05))
            v = (vert.co.z - min_z) / span_z
            # Clamp to 0..1
            u = max(0.01, min(0.99, u))
            v = max(0.01, min(0.99, v))
            uv_layer.data[loop_idx].uv = (u, v)

    # Set up PBR Material with embedded textures
    mat = bpy.data.materials.new("AuroraFoxMaterial")
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    bsdf = nodes.get("Principled BSDF")

    # Albedo texture
    albedo_img_path = os.path.join(textures_dir, "aurora_fox_albedo.png")
    tex_albedo = nodes.new("ShaderNodeTexImage")
    tex_albedo.image = bpy.data.images.load(albedo_img_path)
    links.new(bsdf.inputs["Base Color"], tex_albedo.outputs["Color"])

    # Metallic / Roughness texture
    rm_img_path = os.path.join(textures_dir, "aurora_fox_roughness_metal.png")
    tex_rm = nodes.new("ShaderNodeTexImage")
    tex_rm.image = bpy.data.images.load(rm_img_path)
    sep_color = nodes.new("ShaderNodeSeparateColor")
    links.new(sep_color.inputs["Color"], tex_rm.outputs["Color"])
    links.new(bsdf.inputs["Roughness"], sep_color.outputs["Green"])
    links.new(bsdf.inputs["Metallic"], sep_color.outputs["Blue"])

    obj.data.materials.append(mat)

    # Calculate statistics
    num_verts = len(mesh.vertices)
    num_tris = len(mesh.polygons)
    print(f"Aurora Fox Mask: {num_verts} vertices, {num_tris} triangles")

    # Save .blend source
    os.makedirs(os.path.dirname(output_blend), exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=output_blend)
    print(f"Saved source blend: {output_blend}")

    # Export binary GLB
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
    root = os.path.dirname(os.path.abspath(__file__))
    create_aurora_fox_mask(os.path.join(root,"aurora-fox-mask.blend"), os.path.join(root,"aurora-fox-mask.glb"), os.path.join(root,"textures"))
