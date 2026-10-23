"""Import a live MP6 QA scene snapshot into Blender.

The JSON is produced by ``run_widescreen_scenes.py --export-scene``.  This
module intentionally has no top-level ``bpy`` import: validation and geometry
conversion remain usable from the normal Python test runner, while Blender is
only touched by :func:`build_blend` or the command-line entry point.

Usage inside Blender::

    blender --background --python tools/blender_scene.py -- \
        --snapshot build/widescreen-qa/runs/.../scene.json \
        --output build/widescreen-qa/runs/.../scene.blend

Textures are not copied.  Native UVs, material colors/alpha metadata, live
transforms, face winding and vertex colors are retained.  A snapshot is a
single live frame; it does not claim to contain animation or skinning data.
Unsupported/deformed objects remain in the JSON as explicit metadata and are
represented by empty Blender markers instead of being silently treated as
static meshes.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any


SCHEMA = "mp6-native-scene-v1"


def _finite(value: Any) -> bool:
    return isinstance(value, (int, float)) and math.isfinite(float(value))


def _vec(value: Any, size: int, label: str) -> tuple[float, ...]:
    if not isinstance(value, list) or len(value) != size or not all(_finite(x) for x in value):
        raise ValueError(f"{label} must contain {size} finite numbers")
    return tuple(float(x) for x in value)


def _matrix(value: Any, label: str) -> list[list[float]]:
    if not isinstance(value, list) or len(value) != 3:
        raise ValueError(f"{label} must be a 3x4 matrix")
    rows = []
    for row in value:
        rows.append(list(_vec(row, 4, label)))
    return rows


def _validate_face(face: Any, vertex_count: int, label: str) -> None:
    if not isinstance(face, dict):
        raise ValueError(f"{label} must be an object")
    primitive = face.get("primitive")
    if primitive not in {"tri", "quad", "strip"}:
        raise ValueError(f"{label}.primitive is unsupported")
    loops = face.get("loops")
    if not isinstance(loops, list) or (primitive == "tri" and len(loops) != 3) or (
            primitive == "quad" and len(loops) != 4) or (primitive == "strip" and len(loops) < 3):
        raise ValueError(f"{label}.loops has the wrong corner count")
    for index, loop in enumerate(loops):
        if not isinstance(loop, dict):
            raise ValueError(f"{label}.loops[{index}] must be an object")
        vertex = loop.get("vertex")
        if not isinstance(vertex, int) or vertex < 0 or vertex >= vertex_count:
            raise ValueError(f"{label}.loops[{index}].vertex is out of range")
        if loop.get("uv") is not None:
            _vec(loop["uv"], 2, f"{label}.loops[{index}].uv")
        if loop.get("rgba") is not None:
            _vec(loop["rgba"], 4, f"{label}.loops[{index}].rgba")
        if loop.get("normal_value") is not None:
            _vec(loop["normal_value"], 3, f"{label}.loops[{index}].normal_value")


def _validate_camera(camera: Any, label: str) -> None:
    if not isinstance(camera, dict):
        raise ValueError(f"{label} must be an object")
    _vec(camera.get("position"), 3, f"{label}.position")
    _vec(camera.get("target"), 3, f"{label}.target")
    if camera.get("up") is not None:
        _vec(camera["up"], 3, f"{label}.up")
    for field in ("fov", "near", "far", "aspect"):
        if camera.get(field) is not None and not _finite(camera[field]):
            raise ValueError(f"{label}.{field} must be finite")


def validate_snapshot(snapshot: Any) -> dict[str, Any]:
    """Validate and return a snapshot suitable for import.

    Unsupported meshes are intentionally allowed: the exporter reports them
    explicitly so a reviewer can distinguish missing support from an empty
    authored mesh.  Malformed supported geometry is rejected.
    """

    if not isinstance(snapshot, dict) or snapshot.get("schema") != SCHEMA:
        raise ValueError(f"snapshot schema must be {SCHEMA}")
    if not isinstance(snapshot.get("models"), list):
        raise ValueError("snapshot.models must be a list")
    if not isinstance(snapshot.get("cameras"), list):
        raise ValueError("snapshot.cameras must be a list")
    for model_index, model in enumerate(snapshot["models"]):
        label = f"models[{model_index}]"
        if not isinstance(model, dict):
            raise ValueError(f"{label} must be an object")
        if not isinstance(model.get("model_id"), int):
            raise ValueError(f"{label}.model_id must be an integer")
        if not model.get("supported", True) and not model.get("unsupported_reason"):
            raise ValueError(f"{label} must explain why it is unsupported")
        _matrix(model.get("model_matrix"), f"{label}.model_matrix")
        if not isinstance(model.get("materials", []), list):
            raise ValueError(f"{label}.materials must be a list")
        for material_index, material in enumerate(model.get("materials", [])):
            if not isinstance(material, dict):
                raise ValueError(f"{label}.materials[{material_index}] must be an object")
            if material.get("rgba") is not None:
                _vec(material["rgba"], 4, f"{label}.materials[{material_index}].rgba")
        objects = model.get("objects")
        if not isinstance(objects, list):
            raise ValueError(f"{label}.objects must be a list")
        for object_index, obj in enumerate(objects):
            object_label = f"{label}.objects[{object_index}]"
            if not isinstance(obj, dict):
                raise ValueError(f"{object_label} must be an object")
            _matrix(obj.get("transform"), f"{object_label}.transform")
            if not obj.get("supported", False):
                if not obj.get("unsupported_reason"):
                    raise ValueError(f"{object_label} must explain why it is unsupported")
                continue
            vertices = obj.get("vertices")
            faces = obj.get("faces")
            if not isinstance(vertices, list) or not isinstance(faces, list):
                raise ValueError(f"{object_label} must contain vertices and faces")
            for vertex_index, vertex in enumerate(vertices):
                _vec(vertex, 3, f"{object_label}.vertices[{vertex_index}]")
            for face_index, face in enumerate(faces):
                _validate_face(face, len(vertices), f"{object_label}.faces[{face_index}]")
        hsf_cameras = model.get("hsf_cameras", [])
        if not isinstance(hsf_cameras, list):
            raise ValueError(f"{label}.hsf_cameras must be a list")
        for camera_index, camera in enumerate(hsf_cameras):
            _validate_camera(camera, f"{label}.hsf_cameras[{camera_index}]")
    for camera_index, camera in enumerate(snapshot["cameras"]):
        _validate_camera(camera, f"cameras[{camera_index}]")
    return snapshot


def load_snapshot(path: str | Path) -> dict[str, Any]:
    """Read and validate a live scene snapshot."""

    with Path(path).open(encoding="utf-8") as stream:
        return validate_snapshot(json.load(stream))


def native_to_blender(vector: list[float] | tuple[float, ...]) -> tuple[float, float, float]:
    """Convert MP6's right-handed Y-up vector to Blender's Z-up basis."""

    return float(vector[0]), -float(vector[2]), float(vector[1])


def matrix_to_blender(matrix: list[list[float]]) -> list[list[float]]:
    """Convert a native 3x4 affine matrix into Blender's 4x4 basis."""

    native = [list(row) + ([0.0] if len(row) == 3 else []) for row in matrix]
    native.append([0.0, 0.0, 0.0, 1.0])
    # B maps (x,y,z) to (x,-z,y). Native up stays positive Blender Z.
    # For an affine transform, convert the
    # basis on both sides: B * M * B^-1 (B is orthogonal, so B^-1 == B^T).
    basis = (
        (1.0, 0.0, 0.0, 0.0),
        (0.0, 0.0, -1.0, 0.0),
        (0.0, 1.0, 0.0, 0.0),
        (0.0, 0.0, 0.0, 1.0),
    )
    inverse_basis = (
        (1.0, 0.0, 0.0, 0.0),
        (0.0, 0.0, 1.0, 0.0),
        (0.0, -1.0, 0.0, 0.0),
        (0.0, 0.0, 0.0, 1.0),
    )

    def mul(left: list[list[float]], right: list[list[float]]) -> list[list[float]]:
        return [[sum(left[row][k] * right[k][column] for k in range(4))
                 for column in range(4)] for row in range(4)]

    return mul(mul([list(row) for row in basis], native),
               [list(row) for row in inverse_basis])


def _strip_triangles(loop_count: int) -> list[tuple[int, int, int]]:
    """Triangulate the already-emitted native GX triangle-strip stream."""

    triangles = []
    for index in range(loop_count - 2):
        if index & 1:
            triangles.append((index + 1, index, index + 2))
        else:
            triangles.append((index, index + 1, index + 2))
    return triangles


def blender_faces(face: dict[str, Any]) -> list[tuple[int, ...]]:
    """Return Blender polygon indices while retaining native face winding."""

    return [tuple(int(face["loops"][index]["vertex"]) for index in triangle)
            for triangle in _face_loop_triangles(face)]


def _face_loop_triangles(face: dict[str, Any]) -> list[tuple[int, ...]]:
    """Return source-loop indices for each Blender polygon.

    A native strip is one face record but several Blender triangles.  Keeping
    the source-loop expansion alongside the polygon expansion ensures every
    generated corner receives the corresponding native UV/color record.
    """

    if face["primitive"] == "strip":
        return _strip_triangles(len(face["loops"]))
    return [tuple(range(len(face["loops"])))]


def _expanded_face_records(face: dict[str, Any]) -> list[dict[str, Any]]:
    return [{"loops": [face["loops"][index] for index in triangle]}
            for triangle in _face_loop_triangles(face)]


def _set_material(material: Any, record: dict[str, Any]) -> None:
    rgba = record.get("rgba") or [255, 255, 255, 255]
    color = tuple(max(0.0, min(1.0, float(value) / 255.0)) for value in rgba)
    material.diffuse_color = color
    if hasattr(material, "use_nodes"):
        material.use_nodes = True
        nodes = material.node_tree.nodes if material.node_tree else []
        shader = next((node for node in nodes if node.type == "BSDF_PRINCIPLED"), None)
        if shader:
            if "Base Color" in shader.inputs:
                shader.inputs["Base Color"].default_value = color
            if "Alpha" in shader.inputs:
                shader.inputs["Alpha"].default_value = color[3]
    material["mp6_native_rgba"] = list(rgba)
    material["mp6_native_inv_alpha"] = record.get("inv_alpha")
    material["mp6_native_ref_alpha"] = record.get("ref_alpha")
    material["mp6_native_attributes"] = record.get("attribute_records", [])


def _assign_corner_data(mesh: Any, faces: list[dict[str, Any]]) -> None:
    uv_layer = mesh.uv_layers.new(name="NativeUV") if mesh.uv_layers is not None else None
    color_layer = None
    if hasattr(mesh, "color_attributes"):
        color_layer = mesh.color_attributes.new(name="NativeColor", type="BYTE_COLOR", domain="CORNER")
    elif hasattr(mesh, "vertex_colors"):
        color_layer = mesh.vertex_colors.new(name="NativeColor")
    # Adding a CustomData layer can relocate the existing layer storage in
    # Blender. Do not retain the RNA wrapper returned before that allocation.
    # Real 4.3 imports otherwise crash inside uv_layer.data, not in Python.
    if uv_layer is not None:
        uv_layer = mesh.uv_layers["NativeUV"]
    if color_layer is not None:
        collection = mesh.color_attributes if hasattr(mesh, "color_attributes") else mesh.vertex_colors
        color_layer = collection["NativeColor"]
    loop_index = 0
    for face, polygon in zip(faces, mesh.polygons):
        for source_loop in face["loops"]:
            if uv_layer is not None and source_loop.get("uv") is not None:
                uv_layer.data[loop_index].uv = tuple(source_loop["uv"])
            if color_layer is not None and source_loop.get("rgba") is not None:
                color = tuple(float(value) / 255.0 for value in source_loop["rgba"])
                color_layer.data[loop_index].color = color
            loop_index += 1
        polygon.use_smooth = False


def _camera_from_record(bpy: Any, record: dict[str, Any], prefix: str) -> Any:
    from mathutils import Matrix, Quaternion, Vector

    camera_data = bpy.data.cameras.new(prefix + "Data")
    camera_object = bpy.data.objects.new(prefix, camera_data)
    bpy.context.collection.objects.link(camera_object)
    position = Vector(native_to_blender(record["position"]))
    target = Vector(native_to_blender(record["target"]))
    direction = target - position
    if direction.length > 1.0e-6:
        direction.normalize()
        up = Vector(native_to_blender(record.get("up", [0.0, 1.0, 0.0])))
        if "up" not in record and _finite(record.get("up_rot")):
            up.rotate(Quaternion(direction, math.radians(float(record["up_rot"]))))
        right = direction.cross(up)
        if right.length > 1.0e-6:
            right.normalize()
            up = right.cross(direction).normalized()
            camera_object.rotation_euler = Matrix((right, up, -direction)).transposed().to_euler()
    camera_object.location = position
    if _finite(record.get("fov")) and 0.0 < float(record["fov"]) < 180.0:
        camera_data.sensor_fit = "VERTICAL"
        camera_data.lens = camera_data.sensor_height / (
            2.0 * math.tan(math.radians(float(record["fov"])) / 2.0))
    if _finite(record.get("near")) and float(record["near"]) > 0.0:
        camera_data.clip_start = float(record["near"])
    if _finite(record.get("far")) and float(record["far"]) > float(camera_data.clip_start):
        camera_data.clip_end = float(record["far"])
    camera_object["mp6_native_camera"] = record.get("camera_id", record.get("object_index"))
    camera_object["mp6_native_aspect"] = record.get("aspect")
    return camera_object


def build_blend(snapshot: dict[str, Any], output: str | Path) -> Path:
    """Build and save a Blender scene; only callable inside Blender."""

    validate_snapshot(snapshot)
    import bpy  # type: ignore  # Imported only inside Blender.
    from mathutils import Matrix

    for object_ in list(bpy.data.objects):
        bpy.data.objects.remove(object_, do_unlink=True)
    for model_index, model in enumerate(snapshot["models"]):
        model_id = model["model_id"]
        materials = []
        for material_index, record in enumerate(model.get("materials", [])):
            material = bpy.data.materials.new(f"MP6_M{model_id}_Material{material_index}")
            _set_material(material, record)
            materials.append(material)
        for object_index, record in enumerate(model["objects"]):
            object_name = (f"MP6_M{model_id}_O{record.get('object_index', object_index)}"
                           f"_I{record.get('instance', 0)}_{record.get('name') or 'unnamed'}")
            if not record.get("supported", False):
                marker = bpy.data.objects.new(object_name + "_UNSUPPORTED", None)
                bpy.context.collection.objects.link(marker)
                marker["mp6_unsupported_reason"] = record.get("unsupported_reason", "unknown")
                marker.hide_render = True
                continue
            vertices = [native_to_blender(vertex) for vertex in record["vertices"]]
            polygons = []
            polygon_materials = []
            expanded_faces = []
            for face in record["faces"]:
                for expanded_face, polygon in zip(_expanded_face_records(face), blender_faces(face)):
                    expanded_faces.append(expanded_face)
                    polygons.append(polygon)
                    polygon_materials.append(int(face.get("material", -1)))
            mesh = bpy.data.meshes.new(object_name + "Mesh")
            mesh.from_pydata(vertices, [], polygons)
            mesh.update()
            obj = bpy.data.objects.new(object_name, mesh)
            bpy.context.collection.objects.link(obj)
            obj.matrix_world = Matrix(matrix_to_blender(record["transform"]))
            obj.hide_render = bool(record.get("hidden"))
            obj.hide_viewport = bool(record.get("hidden"))
            obj["mp6_model_id"] = model_id
            obj["mp6_object_index"] = record.get("object_index", object_index)
            obj["mp6_native_name"] = record.get("name") or ""
            for material in materials:
                mesh.materials.append(material)
            for polygon, material_index in zip(mesh.polygons, polygon_materials):
                if 0 <= material_index < len(materials):
                    polygon.material_index = material_index
            _assign_corner_data(mesh, expanded_faces)
        for camera_index, record in enumerate(model.get("hsf_cameras", [])):
            _camera_from_record(bpy, record,
                                f"MP6_M{model_id}_HSFCamera{record.get('object_index', camera_index)}")
    for camera_index, record in enumerate(snapshot["cameras"]):
        _camera_from_record(bpy, record, f"MP6_RuntimeCamera{record.get('camera_id', camera_index)}")
    scene = bpy.context.scene
    scene["mp6_snapshot_schema"] = snapshot["schema"]
    scene["mp6_snapshot_tick"] = snapshot.get("tick")
    scene["mp6_snapshot_overlay"] = snapshot.get("overlay")
    output_path = Path(output).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(output_path))
    return output_path


def main(argv: list[str] | None = None) -> int:
    if argv is None:
        argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    snapshot = load_snapshot(args.snapshot)
    build_blend(snapshot, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

