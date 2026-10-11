
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

scene.render.resolution_x = 512
scene.render.resolution_y = 512
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

for view_name, cam_pos, cam_rot, rig_rot in views[:1]:
    cam_obj.location = cam_pos
    cam_obj.rotation_euler = cam_rot
    rig_root.rotation_euler = rig_rot
    bpy.context.view_layer.update()
    out_path = os.path.join(out_dir, f"{view_name}.png")
    scene.render.filepath = out_path
    bpy.ops.render.render(write_still=True)
    print(f"Rendered view: {view_name} -> {out_path}")
