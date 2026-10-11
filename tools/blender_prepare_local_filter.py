"""Prepare private, static GLB copies with the installed Blender, never edit sources.
Run: blender.exe --background --factory-startup --python this.py -- --output-dir DIR INPUT.glb ...
The result still needs the native importer and a live wearer fitting check.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import time
import bpy
from mathutils import Vector


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def glb_chunks(path):
    raw = Path(path).read_bytes()
    if len(raw) < 20 or raw[:4] != b'glTF' or struct.unpack_from('<II', raw, 4) != (2, len(raw)):
        raise ValueError('Not a complete GLB2 file')
    chunks = []
    pos = 12
    while pos < len(raw):
        length, kind = struct.unpack_from('<II', raw, pos)
        chunks.append((kind, raw[pos + 8:pos + 8 + length]))
        pos += 8 + length
    return raw, chunks, json.loads(chunks[0][1])


def stats(path):
    raw, chunks, doc = glb_chunks(path)
    positions = []
    vertices = triangles = primitives = 0
    for mesh in doc.get('meshes', []):
        for prim in mesh['primitives']:
            primitives += 1
            ac = doc['accessors'][prim['attributes']['POSITION']]
            vertices += ac['count']
            positions.append((ac.get('min'), ac.get('max')))
            triangles += doc['accessors'][prim['indices']]['count'] // 3 if 'indices' in prim else ac['count'] // 3
    return {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest(), 'vertices': vertices,
            'triangles': triangles, 'primitives': primitives, 'nodes': len(doc.get('nodes', [])),
            'materials': len(doc.get('materials', [])), 'images': len(doc.get('images', [])),
            'textures': len(doc.get('textures', [])), 'extensionsUsed': doc.get('extensionsUsed', []),
            'extensionsRequired': doc.get('extensionsRequired', []), 'positionAccessorBounds': positions}


def baseline_only(path):
    raw, chunks, doc = glb_chunks(path)
    removed = []
    for i, mat in enumerate(doc.get('materials', [])):
        for name in mat.pop('extensions', {}):
            removed.append({'material': i, 'extension': name})
        for name in ('normalTexture', 'occlusionTexture', 'emissiveTexture', 'emissiveFactor'):
            if name in mat:
                mat.pop(name)
                removed.append({'material': i, 'omitted': name})
    if not doc.get('textures'):
        # Untextured author assets may contain tiled or negative unused UVs. Omit
        # those attributes rather than clamp UVs and silently damage a texture.
        for mesh in doc.get('meshes', []):
            for primitive in mesh['primitives']:
                for attribute in list(primitive['attributes']):
                    if attribute.startswith('TEXCOORD_'):
                        primitive['attributes'].pop(attribute)
                        removed.append({'omittedUnusedAttribute': attribute})
    doc.pop('extensionsUsed', None)
    doc.pop('extensionsRequired', None)
    if 'animations' in doc or 'skins' in doc:
        raise ValueError('Static export unexpectedly contains animation or skins')
    payload = json.dumps(doc, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
    payload += b' ' * (-len(payload) % 4)
    out = struct.pack('<II', len(payload), 0x4e4f534a) + payload
    for kind, data in chunks[1:]:
        out += struct.pack('<II', len(data), kind) + data
    Path(path).write_bytes(b'glTF' + struct.pack('<II', 2, len(out) + 12) + out)
    return removed


def bounds(obj):
    points = [obj.matrix_world @ v.co for v in obj.data.vertices]
    return {'min': [min(p[a] for p in points) for a in range(3)],
            'max': [max(p[a] for p in points) for a in range(3)]}


def bake_standard_atlas(obj, output_dir):
    """Bake fourteen authored materials into two baseline maps, without replacing texture detail by a flat colour."""
    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'CPU'
    scene.cycles.samples = 1
    scene.render.bake.use_clear = True
    scene.render.bake.margin = 8
    scene.render.bake.use_selected_to_active = False
    if not obj.data.uv_layers:
        raise ValueError('Atlas baking requires the authored UV coordinates')
    original_uv = obj.data.uv_layers[0]
    original_uv.name = 'AuthoredUV'
    original_uv_name = original_uv.name
    # Freeze existing image sampling to the authored UV before switching the active bake UV.
    for mat in obj.data.materials:
        if not mat or not mat.use_nodes:
            continue
        tree = mat.node_tree
        uv_node = tree.nodes.new('ShaderNodeUVMap')
        uv_node.uv_map = original_uv_name
        for node in list(tree.nodes):
            if node.type == 'TEX_IMAGE' and not node.inputs['Vector'].is_linked:
                tree.links.new(uv_node.outputs['UV'], node.inputs['Vector'])
    bake_uv = obj.data.uv_layers.new(name='NativeAtlasUV')
    bake_uv_name = bake_uv.name
    obj.data.uv_layers.active = obj.data.uv_layers[bake_uv_name]
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=0.015)
    bpy.ops.object.mode_set(mode='OBJECT')
    base = bpy.data.images.new('Native_BaseColor_Atlas', width=1024, height=1024, alpha=False)
    mr = bpy.data.images.new('Native_MetalRough_Atlas', width=1024, height=1024, alpha=False)
    mr.colorspace_settings.name = 'Non-Color'
    for mode, image in [('base', base), ('metalrough', mr)]:
        for mat in obj.data.materials:
            if not mat:
                continue
            tree = mat.node_tree
            output = next(n for n in tree.nodes if n.type == 'OUTPUT_MATERIAL')
            principled = next(n for n in tree.nodes if n.type == 'BSDF_PRINCIPLED')
            emission = tree.nodes.new('ShaderNodeEmission')
            if mode == 'base':
                src = principled.inputs['Base Color']
                if src.is_linked:
                    tree.links.new(src.links[0].from_socket, emission.inputs['Color'])
                else:
                    emission.inputs['Color'].default_value = src.default_value
            else:
                combine = tree.nodes.new('ShaderNodeCombineColor')
                combine.mode = 'RGB'
                combine.inputs['Red'].default_value = 1.0
                for target, source in [('Green', 'Roughness'), ('Blue', 'Metallic')]:
                    src = principled.inputs[source]
                    if src.is_linked:
                        tree.links.new(src.links[0].from_socket, combine.inputs[target])
                    else:
                        combine.inputs[target].default_value = src.default_value
                tree.links.new(combine.outputs['Color'], emission.inputs['Color'])
            tree.links.new(emission.outputs['Emission'], output.inputs['Surface'])
            target = tree.nodes.new('ShaderNodeTexImage')
            target.image = image
            tree.nodes.active = target
            target.select = True
        bpy.ops.object.bake(type='EMIT', uv_layer=bake_uv_name)
        image.pack()
    # Only the baked layer remains, so every exported image and the importer agree on UV0.
    for layer_name in [layer.name for layer in obj.data.uv_layers]:
        if layer_name != bake_uv_name:
            obj.data.uv_layers.remove(obj.data.uv_layers[layer_name])
    obj.data.uv_layers.active_index = 0
    atlas = bpy.data.materials.new('Native_Standard_MetallicRoughness_Atlas')
    atlas.use_nodes = True
    tree = atlas.node_tree
    bsdf = next(n for n in tree.nodes if n.type == 'BSDF_PRINCIPLED')
    colour = tree.nodes.new('ShaderNodeTexImage'); colour.image = base
    packed = tree.nodes.new('ShaderNodeTexImage'); packed.image = mr
    separate = tree.nodes.new('ShaderNodeSeparateColor'); separate.mode = 'RGB'
    tree.links.new(colour.outputs['Color'], bsdf.inputs['Base Color'])
    tree.links.new(packed.outputs['Color'], separate.inputs['Color'])
    tree.links.new(separate.outputs['Green'], bsdf.inputs['Roughness'])
    tree.links.new(separate.outputs['Blue'], bsdf.inputs['Metallic'])
    obj.data.materials.clear()
    obj.data.materials.append(atlas)
    for polygon in obj.data.polygons:
        polygon.material_index = 0
    return {'method': 'Emission baking of authored base colour and metallic/roughness into UV0',
            'imageSize': [1024, 1024], 'outputMaterials': 1,
            'limitations': ['Transparent/refractive gems approximated as opaque coloured baseline material.',
                            'Authored normal/occlusion maps omitted; small relief may be lost.',
                            '1024 atlas and aggressive geometric reduction require visual inspection.']}


def prepare(source, output_dir, target_triangles, allow_atlas=False):
    start = time.time()
    source = source.resolve(strict=True)
    before_hash = sha(source)
    original_stats = stats(source)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(source))
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    if not meshes:
        raise ValueError('No meshes imported')
    bpy.ops.object.select_all(action='DESELECT')
    for obj in meshes:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.object.join()
    obj = bpy.context.active_object
    obj.name = source.stem + '_native_copy'
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    original_bounds = bounds(obj)
    material_changes = []
    if len(obj.data.materials) > 8 and not allow_atlas:
        raise ValueError('More than eight materials require an explicit atlas bake; refusing silent material replacement')
    for mat in obj.data.materials:
        if not mat:
            continue
        mat.use_nodes = True
        for node in mat.node_tree.nodes:
            if node.type != 'BSDF_PRINCIPLED':
                continue
            changes = {'material': mat.name}
            for name in ('Coat Weight', 'Transmission Weight', 'Subsurface Weight', 'Sheen Weight', 'Anisotropic IOR Level'):
                if name in node.inputs:
                    socket = node.inputs[name]
                    for link in list(socket.links):
                        mat.node_tree.links.remove(link)
                    changes[name] = socket.default_value
                    socket.default_value = 0.0
            if 'IOR' in node.inputs:
                node.inputs['IOR'].default_value = 1.5
            if 'Emission Color' in node.inputs:
                socket = node.inputs['Emission Color']
                for link in list(socket.links):
                    mat.node_tree.links.remove(link)
                socket.default_value = (0, 0, 0, 1)
            if 'Emission Strength' in node.inputs:
                node.inputs['Emission Strength'].default_value = 0.0
            rough = node.inputs.get('Roughness')
            if rough and not rough.is_linked:
                changes['originalRoughness'] = rough.default_value
                rough.default_value = max(0.12, rough.default_value)
                changes['newRoughness'] = rough.default_value
            metal = node.inputs.get('Metallic')
            if 'gold' in mat.name.lower() and metal and not metal.is_linked:
                changes['originalMetallic'] = metal.default_value
                metal.default_value = max(0.85, metal.default_value)
                changes['newMetallic'] = metal.default_value
            alpha = node.inputs.get('Alpha')
            if alpha and not alpha.is_linked and alpha.default_value < 1:
                changes['originalAlpha'] = alpha.default_value
                alpha.default_value = max(0.8, alpha.default_value)
                changes['newAlpha'] = alpha.default_value
            material_changes.append(changes)
    obj.data.calc_loop_triangles()
    initial_triangles = len(obj.data.loop_triangles)
    if initial_triangles > target_triangles:
        mod = obj.modifiers.new('Bounded private-copy simplification', 'DECIMATE')
        mod.decimate_type = 'COLLAPSE'
        mod.ratio = max(0.001, (target_triangles - 500) / initial_triangles)
        mod.use_collapse_triangulate = True
        bpy.ops.object.modifier_apply(modifier=mod.name)
    triangulate = obj.modifiers.new('Explicit triangles', 'TRIANGULATE')
    bpy.ops.object.modifier_apply(modifier=triangulate.name)
    obj.data.update()
    final_bounds = bounds(obj)
    atlas_receipt = bake_standard_atlas(obj, output_dir) if len(obj.data.materials) > 8 else None
    # Keep original world geometry: no normalization, nonuniform scaling or auto reorientation.
    for image in bpy.data.images:
        if image.size[0] > 2048 or image.size[1] > 2048:
            factor = 2048 / max(image.size)
            image.scale(max(1, round(image.size[0] * factor)), max(1, round(image.size[1] * factor)))
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    output = output_dir / (source.stem.replace(' ', '_').replace('(', '').replace(')', '') + '_native.glb')
    if output.resolve() == source:
        raise ValueError('Output must not be the source')
    candidate = output.with_suffix('.candidate.glb')
    bpy.ops.export_scene.gltf(filepath=str(candidate), export_format='GLB', use_selection=True,
        export_animations=False, export_apply=True, export_yup=True, export_normals=True,
        export_texcoords=True, export_tangents=False, export_materials='EXPORT',
        export_extras=False, export_unused_images=False, export_unused_textures=False,
        export_draco_mesh_compression_enable=False)
    omissions = baseline_only(candidate)
    final_stats = stats(candidate)
    if final_stats['vertices'] > 60000 or final_stats['triangles'] > 50000:
        raise ValueError('Export split vertices/triangles exceed the safe native budget')
    if final_stats['nodes'] > 512 or final_stats['primitives'] > 256 or final_stats['materials'] > 8:
        raise ValueError('Export exceeds native scene budget')
    if final_stats['bytes'] > 32 * 1024 * 1024:
        raise ValueError('Export exceeds 32 MiB')
    after_hash = sha(source)
    if after_hash != before_hash:
        raise ValueError('Source bytes changed unexpectedly')
    candidate.replace(output)
    receipt = {'status': 'OptimizedCopyFormatPreflightPassedNativeImportAndLiveFitPending',
        'blender': bpy.app.version_string, 'source': str(source), 'sourceBeforeSha256': before_hash,
        'sourceAfterSha256': after_hash, 'sourceChanged': False, 'sourceStats': original_stats,
        'output': str(output), 'outputStats': final_stats, 'beforeBlenderWorldBounds': original_bounds,
        'afterBlenderWorldBounds': final_bounds, 'uniformProportionsPreserved': True,
        'materialApproximations': material_changes, 'atlas': atlas_receipt, 'removedExportFeatures': omissions,
        'limitations': ['Clearcoat and refraction are approximated by baseline metallic/roughness and alpha.',
                        'Decimation changes surface tessellation; silhouette needs visual inspection.',
                        'Native import and live wearer fit are separate checks.',
                        'Private user model: not approved for app/source redistribution.'],
        'elapsedSeconds': round(time.time() - start, 3)}
    output.with_suffix('.receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print('NATIVE_CROWN_READY ' + json.dumps({'output': str(output), 'stats': final_stats}), flush=True)
    return receipt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output-dir', type=Path, required=True)
    ap.add_argument('--target-triangles', type=int, default=48000)
    ap.add_argument('--bake-atlas', action='store_true')
    ap.add_argument('sources', type=Path, nargs='+')
    args = ap.parse_args(sys.argv[sys.argv.index('--') + 1:])
    if not 1000 <= args.target_triangles <= 50000:
        ap.error('Target triangles must be within 1000..50000')
    args.output_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for source in args.sources:
        try:
            results.append(prepare(source, args.output_dir, args.target_triangles, args.bake_atlas))
        except Exception as error:
            results.append({'status': 'Failed', 'source': str(source), 'error': str(error)})
            print('NATIVE_CROWN_FAILED ' + json.dumps(results[-1]), flush=True)
    (args.output_dir.parent / 'receipt.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
    if any(r['status'] == 'Failed' for r in results):
        raise SystemExit(1)

if __name__ == '__main__':
    main()
