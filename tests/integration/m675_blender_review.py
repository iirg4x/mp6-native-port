"""Camera-matched review of actual evaluated M675 scene geometry.

Input: --export-scene --evaluated-shapes capture. No fog or proxy water is
added. This is one evaluated pose; verify textures and animation in game.
"""
import argparse
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.blender_scene import build_blend, load_snapshot


def main():
    import bpy
    parser = argparse.ArgumentParser()
    parser.add_argument('--snapshot', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--water-texture', type=Path)
    options = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    snapshot = load_snapshot(options.snapshot)
    water = [o for m in snapshot['models'] for o in m['objects'] if o['name'] == 'umi']
    if not water or not all(o.get('supported') and o.get('evaluated_shape_pose') for o in water):
        raise ValueError('Capture the actual evaluated water pose, not a rigid proxy')
    build_blend(snapshot, options.output)
    scene = bpy.context.scene
    camera = bpy.data.objects['MP6_RuntimeCamera0']
    scene.camera = camera
    scene.render.engine = 'BLENDER_EEVEE_NEXT'
    scene.render.resolution_x = 1260
    scene.render.resolution_y = 540
    scene.render.resolution_percentage = 100
    ratio = snapshot['cameras'][0]['aspect'] / (1260 / 540)
    scene.render.pixel_aspect_x = max(1, ratio)
    scene.render.pixel_aspect_y = max(1, 1 / ratio)
    target_top = math.tan(math.radians(snapshot['cameras'][0]['fov']) / 2)
    frame = camera.data.view_frame(scene=scene)
    camera.data.lens *= max(abs(v.y / v.z) for v in frame) / target_top
    frame = camera.data.view_frame(scene=scene)
    measured_top = max(abs(v.y / v.z) for v in frame)
    aspect = max(abs(v.x / v.z) for v in frame) / measured_top
    assert abs(measured_top - target_top) < 1e-5
    assert abs(aspect - snapshot['cameras'][0]['aspect']) < 1e-5
    print('MP6_BLENDER_CAMERA_MATCH', measured_top, aspect)
    scene.view_settings.view_transform = 'Standard'
    palette = {'sora': (.25, .5, .9, 1), 'yama': (.35, .62, .3, 1),
               'umi': (.08, .52, .82, 1), 'sunahama': (.78, .72, .5, 1),
               'siba3': (.25, .6, .25, 1)}
    for obj in bpy.data.objects:
        if obj.type != 'MESH':
            continue
        color = palette.get(obj.get('mp6_native_name'), (.65, .65, .6, 1))
        mat = bpy.data.materials.new(obj.name + '_Diagnostic')
        mat.use_nodes = True
        nodes = mat.node_tree.nodes
        nodes.clear()
        emission = nodes.new('ShaderNodeEmission')
        emission.inputs['Color'].default_value = color
        output = nodes.new('ShaderNodeOutputMaterial')
        mat.node_tree.links.new(emission.outputs[0], output.inputs['Surface'])
        if obj.get('mp6_native_name') == 'umi' and options.water_texture:
            texture = nodes.new('ShaderNodeTexImage')
            texture.image = bpy.data.images.load(str(options.water_texture.resolve()), check_existing=True)
            texture.extension = 'EXTEND'
            transparent = nodes.new('ShaderNodeBsdfTransparent')
            mix = nodes.new('ShaderNodeMixShader')
            mat.node_tree.links.new(texture.outputs['Color'], emission.inputs['Color'])
            mat.node_tree.links.new(texture.outputs['Alpha'], mix.inputs[0])
            mat.node_tree.links.new(transparent.outputs[0], mix.inputs[1])
            mat.node_tree.links.new(emission.outputs[0], mix.inputs[2])
            mat.node_tree.links.new(mix.outputs[0], output.inputs['Surface'])
            mat.surface_render_method = 'DITHERED'
        obj.data.materials.clear()
        obj.data.materials.append(mat)
        for polygon in obj.data.polygons:
            polygon.material_index = 0
    scene['mp6_review_only'] = 'Actual evaluated geometry; one pose, no fog. Check textures/animation in game.'
    bpy.ops.wm.save_as_mainfile(filepath=str(options.output.resolve()))
    scene.render.filepath = str(options.output.with_suffix('.png').resolve())
    bpy.ops.render.render(write_still=True)


if __name__ == '__main__':
    main()
