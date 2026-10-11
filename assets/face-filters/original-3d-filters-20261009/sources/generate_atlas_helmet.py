"""
generate_atlas_helmet.py
Constructs the original Atlas Explorer Helmet:
- Full rounded outer crown enclosing cranium and occiput
- Solid side and rear volume preserved at 35-degree downward tilt
- Panoramic viewport with heavy opaque pressure gasket
- Tinted transparent polycarbonate bubble visor (alphaMode: BLEND)
- Lateral comms pods, cheek rebreather canisters, and ribbed conduits
- Heavy mechanical collar neck ring with locking lugs
- Core glTF metallic-roughness PBR with embedded 2048x2048 textures
- Calibrated origin at nose bridge (0, 0, 0).
"""
import os
import math
import bpy
import bmesh
from mathutils import Vector, Matrix

def create_atlas_explorer_helmet(output_blend, output_glb, textures_dir):
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # We will create two objects:
    # 1. Helmet Main Shell & Hardware (Opaque PBR with embedded texture)
    # 2. Visor Bubble (Transparent PBR, alphaMode: BLEND)
    # Both will be in the exported GLB scene.

    # --- 1. HELMET MAIN SHELL ---
    shell_mesh = bpy.data.meshes.new("AtlasHelmetShellMesh")
    shell_obj = bpy.data.objects.new("AtlasExplorerHelmetShell", shell_mesh)
    bpy.context.collection.objects.link(shell_obj)

    bm = bmesh.new()

    # Outer spherical/ellipsoid crown shell
    # Center around cranium: cx=0.0, cy=0.050, cz=0.025
    center = Vector((0.0, 0.050, 0.025))
    rx = 0.115 # lateral radius (temples/ears)
    ry = 0.125 # front-back radius (crown to occiput)
    rz = 0.120 # vertical radius (crown to jaw)

    lat_steps = 30
    lon_steps = 40
    shell_verts = []

    # Latitude: from top pole (theta=0) down to neck aperture (theta = 0.82*pi)
    for i in range(lat_steps + 1):
        theta = (i / lat_steps) * (math.pi * 0.84)
        z = center.z + rz * math.cos(theta)
        r_scale = math.sin(theta)
        row = []
        for j in range(lon_steps):
            phi = (j / lon_steps) * 2 * math.pi
            x = center.x + rx * r_scale * math.sin(phi)
            # In Blender, forward is -Y, back is +Y
            y = center.y + ry * r_scale * math.cos(phi)

            # Visor viewport window cutout check
            # Front viewport: x between -0.078 and +0.078, z between -0.045 and +0.065, y < 0.015
            is_visor_hole = False
            if y < 0.015:
                if abs(x) < 0.078 and z > -0.045 and z < 0.065:
                    is_visor_hole = True

            if is_visor_hole:
                row.append(None)
            else:
                v = bm.verts.new((x, y, z))
                row.append(v)
        shell_verts.append(row)

    # Build faces across the shell grid
    for i in range(lat_steps):
        for j in range(lon_steps):
            j_next = (j + 1) % lon_steps
            v00 = shell_verts[i][j]
            v01 = shell_verts[i][j_next]
            v11 = shell_verts[i + 1][j_next]
            v10 = shell_verts[i + 1][j]

            if v00 and v01 and v11 and v10:
                bm.faces.new((v00, v01, v11, v10))
            elif v00 and v01 and v11:
                bm.faces.new((v00, v01, v11))
            elif v00 and v11 and v10:
                bm.faces.new((v00, v11, v10))
            elif v00 and v01 and v10:
                bm.faces.new((v00, v01, v10))
            elif v01 and v11 and v10:
                bm.faces.new((v01, v11, v10))

    # Visor Hermetic Pressure Gasket Frame (Opaque heavy structural bezel)
    gasket_pts = 28
    gasket_outer = []
    gasket_inner = []
    for g in range(gasket_pts):
        angle = (g / gasket_pts) * 2 * math.pi
        gx = 0.082 * math.cos(angle)
        gz = 0.010 + 0.055 * math.sin(angle)
        # Forward protrusion on visor rim
        gy_out = -0.038 - 0.018 * math.cos(angle * 0.5)
        gy_in = gy_out + 0.008

        v_out = bm.verts.new((gx, gy_out, gz))
        v_in = bm.verts.new((gx * 0.92, gy_in, gz * 0.92))
        gasket_outer.append(v_out)
        gasket_inner.append(v_in)

    for g in range(gasket_pts):
        g_next = (g + 1) % gasket_pts
        bm.faces.new((gasket_outer[g], gasket_outer[g_next], gasket_inner[g_next], gasket_inner[g]))

    # Clamp Bolts on Gasket Rim
    for bolt_idx in range(12):
        b_angle = (bolt_idx / 12) * 2 * math.pi
        bx = 0.084 * math.cos(b_angle)
        bz = 0.010 + 0.057 * math.sin(b_angle)
        by = -0.040
        b_center = bm.verts.new((bx, by - 0.004, bz))
        b_rim = []
        for bp in range(6):
            b_phi = (bp / 6) * 2 * math.pi
            b_rim.append(bm.verts.new((bx + 0.0035 * math.cos(b_phi), by, bz + 0.0035 * math.sin(b_phi))))
        for bp in range(6):
            bm.faces.new((b_center, b_rim[bp], b_rim[(bp + 1) % 6]))

    # Lateral Comms Pods (Left & Right)
    for pod_sign in [-1.0, 1.0]:
        pod_cx = pod_sign * 0.118
        pod_cy = 0.045
        pod_cz = 0.015

        pod_layers = 10
        pod_steps = 16
        pod_rings = []
        for pl in range(pod_layers):
            t = pl / (pod_layers - 1)
            px = pod_cx + pod_sign * (t * 0.022)
            ring = []
            for ps in range(pod_steps):
                phi = (ps / pod_steps) * 2 * math.pi
                pr = 0.028 * (1.0 - 0.25 * t)
                py = pod_cy + pr * math.cos(phi)
                pz = pod_cz + pr * math.sin(phi)
                ring.append(bm.verts.new((px, py, pz)))
            pod_rings.append(ring)

        pod_cap = bm.verts.new((pod_cx + pod_sign * 0.025, pod_cy, pod_cz))

        for pl in range(pod_layers - 1):
            for ps in range(pod_steps):
                ps_next = (ps + 1) % pod_steps
                v00 = pod_rings[pl][ps]
                v01 = pod_rings[pl][ps_next]
                v11 = pod_rings[pl + 1][ps_next]
                v10 = pod_rings[pl + 1][ps]
                bm.faces.new((v00, v01, v11, v10))

        for ps in range(pod_steps):
            ps_next = (ps + 1) % pod_steps
            bm.faces.new((pod_rings[-1][ps], pod_rings[-1][ps_next], pod_cap))

    # Lower Cheek Rebreather Regulators (Dual Canisters)
    for reg_sign in [-1.0, 1.0]:
        rcx = reg_sign * 0.082
        rcy = -0.005
        rcz = -0.075

        reg_steps = 14
        reg_h_steps = 8
        reg_rings = []
        for rh in range(reg_h_steps):
            t = rh / (reg_h_steps - 1)
            rz_cur = rcz - 0.015 + t * 0.035
            ring = []
            for rs in range(reg_steps):
                phi = (rs / reg_steps) * 2 * math.pi
                rx_cur = rcx + 0.018 * math.cos(phi)
                ry_cur = rcy + 0.018 * math.sin(phi)
                ring.append(bm.verts.new((rx_cur, ry_cur, rz_cur)))
            reg_rings.append(ring)

        reg_top = bm.verts.new((rcx, rcy, rcz + 0.022))
        reg_bot = bm.verts.new((rcx, rcy, rcz - 0.018))

        for rh in range(reg_h_steps - 1):
            for rs in range(reg_steps):
                rs_next = (rs + 1) % reg_steps
                bm.faces.new((reg_rings[rh][rs], reg_rings[rh][rs_next], reg_rings[rh + 1][rs_next], reg_rings[rh + 1][rs]))

        for rs in range(reg_steps):
            rs_next = (rs + 1) % reg_steps
            bm.faces.new((reg_rings[-1][rs], reg_rings[-1][rs_next], reg_top))
            bm.faces.new((reg_rings[0][rs_next], reg_rings[0][rs], reg_bot))

        # Ribbed flexible conduit sweeping from canister to rear collar
        conduit_pts = 16
        conduit_rings = []
        for cp in range(conduit_pts):
            t = cp / (conduit_pts - 1)
            hx = rcx + reg_sign * (0.015 * math.sin(t * math.pi))
            hy = rcy + 0.120 * t
            hz = rcz - 0.020 - 0.015 * t

            rib_radius = 0.007 + 0.0025 * math.sin(cp * math.pi * 1.5)
            c_ring = []
            for cs in range(8):
                c_phi = (cs / 8) * 2 * math.pi
                cx_cur = hx + rib_radius * math.cos(c_phi)
                cz_cur = hz + rib_radius * math.sin(c_phi)
                c_ring.append(bm.verts.new((cx_cur, hy, cz_cur)))
            conduit_rings.append(c_ring)

        for cp in range(conduit_pts - 1):
            for cs in range(8):
                cs_next = (cs + 1) % 8
                bm.faces.new((conduit_rings[cp][cs], conduit_rings[cp][cs_next], conduit_rings[cp + 1][cs_next], conduit_rings[cp + 1][cs]))

    # Neck Collar Flange Ring at Base (z ≈ -0.115m to -0.130m)
    collar_steps = 32
    collar_out = []
    collar_in = []
    collar_bot = []
    for c in range(collar_steps):
        phi = (c / collar_steps) * 2 * math.pi
        cx = center.x + 0.118 * math.sin(phi)
        cy = center.y + 0.128 * math.cos(phi)
        collar_out.append(bm.verts.new((cx * 1.04, cy * 1.04, -0.115)))
        collar_in.append(bm.verts.new((cx * 0.95, cy * 0.95, -0.115)))
        collar_bot.append(bm.verts.new((cx * 1.02, cy * 1.02, -0.132)))

    for c in range(collar_steps):
        c_next = (c + 1) % collar_steps
        bm.faces.new((collar_out[c], collar_out[c_next], collar_in[c_next], collar_in[c]))
        bm.faces.new((collar_out[c], collar_bot[c], collar_bot[c_next], collar_out[c_next]))

    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(shell_mesh)
    bm.free()

    # Solidify modifier for solid 4mm composite hull thickness
    solid_mod = shell_obj.modifiers.new("Solidify", "SOLIDIFY")
    solid_mod.thickness = 0.0040 # 4mm thickness
    solid_mod.offset = 1.0 # Inward
    solid_mod.use_rim = True

    # Subdivision for smooth aerospace fillets
    sub_mod = shell_obj.modifiers.new("Subdivision", "SUBSURF")
    sub_mod.levels = 1
    sub_mod.render_levels = 1

    bpy.context.view_layer.objects.active = shell_obj
    shell_obj.select_set(True)
    bpy.ops.object.convert(target='MESH')

    # Triangulate
    tri_mod = shell_obj.modifiers.new("Triangulate", "TRIANGULATE")
    tri_mod.quad_method = 'BEAUTY'
    tri_mod.ngon_method = 'BEAUTY'
    bpy.ops.object.convert(target='MESH')

    shell_mesh.shade_smooth()

    # UV0 mapping for shell
    uv_layer = shell_mesh.uv_layers.new(name="UVMap")
    shell_mesh.uv_layers.active = uv_layer

    min_x = min(v.co.x for v in shell_mesh.vertices)
    max_x = max(v.co.x for v in shell_mesh.vertices)
    min_z = min(v.co.z for v in shell_mesh.vertices)
    max_z = max(v.co.z for v in shell_mesh.vertices)
    span_x = max_x - min_x
    span_z = max_z - min_z

    for poly in shell_mesh.polygons:
        for loop_idx in poly.loop_indices:
            loop = shell_mesh.loops[loop_idx]
            vert = shell_mesh.vertices[loop.vertex_index]
            u = 0.5 + (vert.co.x / (span_x * 1.04))
            v = (vert.co.z - min_z) / span_z
            u = max(0.01, min(0.99, u))
            v = max(0.01, min(0.99, v))
            uv_layer.data[loop_idx].uv = (u, v)

    # Shell PBR Material
    shell_mat = bpy.data.materials.new("AtlasHelmetShellMaterial")
    shell_mat.use_nodes = True
    s_nodes = shell_mat.node_tree.nodes
    s_links = shell_mat.node_tree.links
    s_bsdf = s_nodes.get("Principled BSDF")

    albedo_img_path = os.path.join(textures_dir, "atlas_helmet_albedo.png")
    tex_albedo = s_nodes.new("ShaderNodeTexImage")
    tex_albedo.image = bpy.data.images.load(albedo_img_path)
    s_links.new(s_bsdf.inputs["Base Color"], tex_albedo.outputs["Color"])

    rm_img_path = os.path.join(textures_dir, "atlas_helmet_roughness_metal.png")
    tex_rm = s_nodes.new("ShaderNodeTexImage")
    tex_rm.image = bpy.data.images.load(rm_img_path)
    sep_color = s_nodes.new("ShaderNodeSeparateColor")
    s_links.new(sep_color.inputs["Color"], tex_rm.outputs["Color"])
    s_links.new(s_bsdf.inputs["Roughness"], sep_color.outputs["Green"])
    s_links.new(s_bsdf.inputs["Metallic"], sep_color.outputs["Blue"])

    shell_obj.data.materials.append(shell_mat)

    # --- 2. TRANSPARENT PANORAMIC BUBBLE VISOR ---
    visor_mesh = bpy.data.meshes.new("AtlasVisorMesh")
    visor_obj = bpy.data.objects.new("AtlasVisorBubble", visor_mesh)
    bpy.context.collection.objects.link(visor_obj)

    v_bm = bmesh.new()

    # Curved bubble visor patch fitted inside the gasket opening
    v_rows = 18
    v_cols = 24
    v_verts = []

    for vr in range(v_rows):
        v_frac = vr / (v_rows - 1)
        vz = -0.040 + v_frac * 0.100

        row = []
        for vc in range(v_cols):
            u_frac = (vc / (v_cols - 1)) * 2.0 - 1.0 # -1 to +1
            max_vx = 0.076 * math.sqrt(max(0.01, 1.0 - ((vz - 0.010) / 0.055) ** 2))
            vx = u_frac * max_vx

            bubble_sag = math.sqrt(max(0.01, 1.0 - (vx / 0.082) ** 2 - ((vz - 0.010) / 0.062) ** 2))
            vy = -0.014 - 0.028 * bubble_sag

            v = v_bm.verts.new((vx, vy, vz))
            row.append(v)
        v_verts.append(row)

    for vr in range(v_rows - 1):
        for vc in range(v_cols - 1):
            v00 = v_verts[vr][vc]
            v01 = v_verts[vr][vc + 1]
            v11 = v_verts[vr + 1][vc + 1]
            v10 = v_verts[vr + 1][vc]
            v_bm.faces.new((v00, v01, v11, v10))

    bmesh.ops.recalc_face_normals(v_bm, faces=v_bm.faces)
    v_bm.to_mesh(visor_mesh)
    v_bm.free()

    # Subdivision and triangulate
    v_sub = visor_obj.modifiers.new("Subdivision", "SUBSURF")
    v_sub.levels = 1
    v_sub.render_levels = 1

    bpy.context.view_layer.objects.active = visor_obj
    visor_obj.select_set(True)
    bpy.ops.object.convert(target='MESH')

    v_tri = visor_obj.modifiers.new("Triangulate", "TRIANGULATE")
    v_tri.quad_method = 'BEAUTY'
    v_tri.ngon_method = 'BEAUTY'
    bpy.ops.object.convert(target='MESH')

    visor_mesh.shade_smooth()

    # UV0 for visor
    v_uv = visor_mesh.uv_layers.new(name="UVMap")
    visor_mesh.uv_layers.active = v_uv
    for poly in visor_mesh.polygons:
        for loop_idx in poly.loop_indices:
            loop = visor_mesh.loops[loop_idx]
            vert = visor_mesh.vertices[loop.vertex_index]
            u = 0.5 + (vert.co.x / 0.16)
            v = 0.5 + ((vert.co.z - 0.010) / 0.12)
            v_uv.data[loop_idx].uv = (max(0.01, min(0.99, u)), max(0.01, min(0.99, v)))

    # Visor Transparent PBR Material (core glTF alphaMode: BLEND)
    visor_mat = bpy.data.materials.new("AtlasVisorMaterial")
    visor_mat.use_nodes = True
    visor_mat.blend_method = 'BLEND'

    vm_nodes = visor_mat.node_tree.nodes
    vm_bsdf = vm_nodes.get("Principled BSDF")

    vm_bsdf.inputs["Base Color"].default_value = (0.72, 0.88, 0.95, 0.35)
    if "Alpha" in vm_bsdf.inputs:
        vm_bsdf.inputs["Alpha"].default_value = 0.35
    vm_bsdf.inputs["Roughness"].default_value = 0.06
    vm_bsdf.inputs["Metallic"].default_value = 0.10
    visor_obj.data.materials.append(visor_mat)

    # Statistics
    total_verts = len(shell_mesh.vertices) + len(visor_mesh.vertices)
    total_tris = len(shell_mesh.polygons) + len(visor_mesh.polygons)
    print(f"Atlas Explorer Helmet: {total_verts} vertices, {total_tris} triangles (Shell: {len(shell_mesh.polygons)}, Visor: {len(visor_mesh.polygons)})")

    # Select both objects for scene
    shell_obj.select_set(True)
    visor_obj.select_set(True)
    bpy.context.view_layer.objects.active = shell_obj

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
    blend_path = str((__import__("pathlib").Path(__file__).resolve().parent / "../sources/atlas-explorer-helmet.blend").resolve())
    glb_path = str((__import__("pathlib").Path(__file__).resolve().parent / "../models/atlas-explorer-helmet.glb").resolve())
    tex_dir = str((__import__("pathlib").Path(__file__).resolve().parent / "../sources/textures").resolve())
    create_atlas_explorer_helmet(blend_path, glb_path, tex_dir)
