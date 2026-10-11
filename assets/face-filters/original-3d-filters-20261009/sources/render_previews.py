"""
render_previews.py
Generates actual multi-angle verification renders and fitting diagrams:
1. Reopens each exported GLB in a fresh Blender scene
2. Attaches the model to the neutral reference head (~0.16m temple width)
3. Renders 5 actual camera angles per filter:
   - front.png (0 deg yaw, 0 deg pitch)
   - yaw_pos45.png (+45 deg yaw)
   - yaw_neg45.png (-45 deg yaw)
   - profile.png (90 deg side profile)
   - tilt_down35.png (35 deg downward tilt on reference head)
4. Generates fitting_axes_dimensions.png with glTF coordinate axes and millimeter dimensions.
"""
import os
import sys
import subprocess
from PIL import Image, ImageDraw

BLENDER_EXE = r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"

MODELS = [
    {
        "id": "aurora-fox-mask",
        "name": "Aurora fox mask",
        "glb": "deliveries/original-3d-filters-20261009/models/aurora-fox-mask.glb",
        "output_dir": "deliveries/original-3d-filters-20261009/renders/aurora-fox-mask",
        "dims": {"width": 0.190, "height": 0.228, "depth": 0.068},
    },
    {
        "id": "nocturne-owl-mask",
        "name": "Nocturne owl mask",
        "glb": "deliveries/original-3d-filters-20261009/models/nocturne-owl-mask.glb",
        "output_dir": "deliveries/original-3d-filters-20261009/renders/nocturne-owl-mask",
        "dims": {"width": 0.185, "height": 0.225, "depth": 0.075},
    },
    {
        "id": "atlas-explorer-helmet",
        "name": "Atlas explorer helmet",
        "glb": "deliveries/original-3d-filters-20261009/models/atlas-explorer-helmet.glb",
        "output_dir": "deliveries/original-3d-filters-20261009/renders/atlas-explorer-helmet",
        "dims": {"width": 0.245, "height": 0.278, "depth": 0.260},
    },
]

# Blender worker script content executed inside Blender's python
BLENDER_WORKER_SCRIPT = """
import os
import sys
import math
import json
import bpy
from mathutils import Vector, Euler

args = sys.argv[sys.argv.index("--") + 1:]
glb_path = args[0]
head_blend = args[1]
out_dir = args[2]

os.makedirs(out_dir, exist_ok=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene

scene.render.resolution_x = 1024
scene.render.resolution_y = 1024
scene.render.engine = 'BLENDER_EEVEE'

cam_data = bpy.data.cameras.new("RenderCamera")
cam_data.lens = 75.0
cam_data.clip_start = 0.05
cam_data.clip_end = 20.0
cam_obj = bpy.data.objects.new("RenderCamera", cam_data)
bpy.context.collection.objects.link(cam_obj)
scene.camera = cam_obj

# Studio 3-point lighting
key_data = bpy.data.lights.new("KeyLight", type='AREA')
key_data.energy = 85.0
key_data.size = 0.6
key_data.color = (1.0, 0.98, 0.95)
key_obj = bpy.data.objects.new("KeyLight", key_data)
key_obj.location = (0.45, -0.65, 0.40)
key_obj.rotation_euler = (math.radians(55), math.radians(15), math.radians(35))
bpy.context.collection.objects.link(key_obj)

fill_data = bpy.data.lights.new("FillLight", type='AREA')
fill_data.energy = 45.0
fill_data.size = 0.8
fill_data.color = (0.92, 0.95, 1.0)
fill_obj = bpy.data.objects.new("FillLight", fill_data)
fill_obj.location = (-0.50, -0.55, 0.20)
fill_obj.rotation_euler = (math.radians(65), math.radians(-10), math.radians(-40))
bpy.context.collection.objects.link(fill_obj)

rim_data = bpy.data.lights.new("RimLight", type='AREA')
rim_data.energy = 95.0
rim_data.size = 0.5
rim_data.color = (1.0, 0.95, 0.90)
rim_obj = bpy.data.objects.new("RimLight", rim_data)
rim_obj.location = (0.20, 0.55, 0.45)
rim_obj.rotation_euler = (math.radians(-45), 0, math.radians(160))
bpy.context.collection.objects.link(rim_obj)

world = bpy.data.worlds.new("StudioWorld")
world.use_nodes = True
bg_node = world.node_tree.nodes.get("Background")
if bg_node:
    bg_node.inputs["Color"].default_value = (0.09, 0.10, 0.12, 1.0)
    bg_node.inputs["Strength"].default_value = 0.8
scene.world = world

# Load reference head
with bpy.data.libraries.load(head_blend, link=False) as (data_from, data_to):
    data_to.objects = [name for name in data_from.objects if "ReferenceHead" in name]

head_obj = None
for o in data_to.objects:
    if o:
        bpy.context.collection.objects.link(o)
        head_obj = o
        break

# Import exported GLB
bpy.ops.import_scene.gltf(filepath=glb_path)
accessory_objs = [o for o in scene.objects if o != head_obj and o.type == 'MESH']

rig_root = bpy.data.objects.new("RigRoot", None)
bpy.context.collection.objects.link(rig_root)
if head_obj:
    head_obj.parent = rig_root
for acc in accessory_objs:
    acc.parent = rig_root

target_z = 0.015
dist = 0.62

views = [
    ("front", Vector((0.0, -dist, target_z)), Euler((math.radians(90), 0, 0)), Euler((0, 0, 0))),
    ("yaw_pos45", Vector((dist * math.sin(math.radians(45)), -dist * math.cos(math.radians(45)), target_z)),
     Euler((math.radians(90), 0, math.radians(45))), Euler((0, 0, 0))),
    ("yaw_neg45", Vector((dist * math.sin(math.radians(-45)), -dist * math.cos(math.radians(-45)), target_z)),
     Euler((math.radians(90), 0, math.radians(-45))), Euler((0, 0, 0))),
    ("profile", Vector((dist, 0.0, target_z)), Euler((math.radians(90), 0, math.radians(90))), Euler((0, 0, 0))),
    ("tilt_down35", Vector((0.0, -dist, target_z + 0.06)), Euler((math.radians(82), 0, 0)),
     Euler((math.radians(-35), 0, 0))),
]

for view_name, cam_pos, cam_rot, rig_rot in views:
    cam_obj.location = cam_pos
    cam_obj.rotation_euler = cam_rot
    rig_root.rotation_euler = rig_rot
    bpy.context.view_layer.update()
    out_path = os.path.join(out_dir, f"{view_name}.png")
    scene.render.filepath = out_path
    bpy.ops.render.render(write_still=True)
    print(f"Rendered view: {view_name} -> {out_path}")
"""

def generate_fitting_diagram(model_info, out_dir):
    front_img_path = os.path.join(out_dir, "front.png")
    profile_img_path = os.path.join(out_dir, "profile.png")

    if not (os.path.exists(front_img_path) and os.path.exists(profile_img_path)):
        print(f"Warning: Missing source renders for diagram in {out_dir}")
        return

    front = Image.open(front_img_path).resize((600, 600), Image.Resampling.LANCZOS)
    profile = Image.open(profile_img_path).resize((600, 600), Image.Resampling.LANCZOS)

    canvas_w, canvas_h = 1300, 800
    canvas = Image.new("RGBA", (canvas_w, canvas_h), (20, 23, 28, 255))

    canvas.paste(front, (50, 140))
    canvas.paste(profile, (670, 140))

    draw = ImageDraw.Draw(canvas)

    title = f"Fitting Profile & Dimensions: {model_info['name']}"
    draw.text((50, 40), title, fill=(240, 243, 248, 255))

    subtitle = "Calibrated on neutral reference head (0.16m temple width) | Origin (0,0,0) at nose bridge"
    draw.text((50, 75), subtitle, fill=(160, 172, 188, 255))

    dims = model_info["dims"]
    dim_str = f"Width: {dims['width']*1000:.0f} mm  |  Height: {dims['height']*1000:.0f} mm  |  Depth: {dims['depth']*1000:.0f} mm"
    draw.text((50, 105), dim_str, fill=(212, 175, 55, 255))

    draw.text((50, 750), "Front View (0 deg Yaw / 0 deg Pitch)", fill=(200, 208, 220, 255))
    draw.text((670, 750), "Profile View (90 deg Yaw)", fill=(200, 208, 220, 255))

    # glTF standard coordinate axis legend
    ax_x, ax_y = 1080, 45
    draw.text((ax_x, ax_y), "Coordinate Axes (glTF):", fill=(220, 225, 235, 255))
    draw.line([(ax_x, ax_y + 25), (ax_x + 35, ax_y + 25)], fill=(235, 60, 60, 255), width=4) # +X Red
    draw.text((ax_x + 42, ax_y + 18), "+X Right", fill=(235, 60, 60, 255))

    draw.line([(ax_x, ax_y + 45), (ax_x + 35, ax_y + 45)], fill=(60, 220, 80, 255), width=4) # +Y Up
    draw.text((ax_x + 42, ax_y + 38), "+Y Up", fill=(60, 220, 80, 255))

    draw.line([(ax_x, ax_y + 65), (ax_x + 35, ax_y + 65)], fill=(60, 140, 245, 255), width=4) # +Z Forward
    draw.text((ax_x + 42, ax_y + 58), "+Z Forward", fill=(60, 140, 245, 255))

    # Dimension calipers on front view (Width)
    draw.line([(100, 720), (600, 720)], fill=(212, 175, 55, 255), width=2)
    draw.line([(100, 710), (100, 730)], fill=(212, 175, 55, 255), width=2)
    draw.line([(600, 710), (600, 730)], fill=(212, 175, 55, 255), width=2)
    draw.text((310, 725), f"W: {dims['width']*1000:.0f} mm", fill=(212, 175, 55, 255))

    out_diag_path = os.path.join(out_dir, "fitting_axes_dimensions.png")
    canvas.save(out_diag_path, format="PNG")
    print(f"Saved fitting diagram: {out_diag_path}")

def main():
    base_dir = str(__import__("pathlib").Path(__file__).resolve().parents[4])
    head_blend = os.path.abspath(os.path.join(base_dir, "deliveries/original-3d-filters-20261009/sources/neutral-reference-head.blend"))

    worker_file = os.path.abspath(os.path.join(base_dir, "deliveries/original-3d-filters-20261009/sources/_blender_render_worker.py"))
    with open(worker_file, "w", encoding="utf-8") as f:
        f.write(BLENDER_WORKER_SCRIPT)

    for m in MODELS:
        glb_path = os.path.abspath(os.path.join(base_dir, m["glb"]))
        out_dir = os.path.abspath(os.path.join(base_dir, m["output_dir"]))

        print(f"\n=======================================================")
        print(f"Rendering multi-angle views for: {m['name']}")
        print(f"GLB: {glb_path}")
        print(f"Output: {out_dir}")
        print(f"=======================================================")

        cmd = [
            BLENDER_EXE,
            "--background",
            "--python", worker_file,
            "--",
            glb_path,
            head_blend,
            out_dir
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"Render worker failed:\n{res.stderr}")
            raise RuntimeError(f"Rendering failed for {m['name']}")
        print(res.stdout)

        # Generate fitting diagram
        generate_fitting_diagram(m, out_dir)

    if os.path.exists(worker_file):
        os.remove(worker_file)

    print("\n=== All multi-angle renders and fitting diagrams successfully completed! ===")

if __name__ == "__main__":
    main()
