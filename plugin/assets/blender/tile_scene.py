"""Blender scene for designing SynthMatch's background tile.

Written for Blender 4.2 and newer; tested with 5.2.

Sets up a square, transparent, top-down render that matches how the plugin
uses the image, adds a starter metal pentagon to reshape or replace, and saves
every render (F12) straight to plugin/assets/tile.png. See ../README.md.

Run it from Blender's Scripting tab (Open, then Run Script), or start Blender
with it from the repository folder:

    blender --python plugin/assets/blender/tile_scene.py

Render without opening Blender's window (for example from your saved .blend):

    blender -b my_tile.blend --python plugin/assets/blender/tile_scene.py -- --render

Options after "--": --render (render once and save), --output PATH (where the
PNG goes; the default is plugin/assets/tile.png, or tile.png next to the .blend
when this script isn't in the repository).

Running it again is safe. It re-applies the render settings and adds only
what is missing (camera, lights, starter shape); objects you made, and the
camera and lights once you have moved them, are left alone.
"""

import math
import sys
from pathlib import Path

import bmesh
import bpy
from bpy.app.handlers import persistent
from mathutils import Vector

SIZE = 512  # pixels, square; the plugin scales it down to about 60-130 px


def script_options():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    options = {"render": "--render" in args, "output": None}
    if "--output" in args and args.index("--output") + 1 < len(args):
        options["output"] = args[args.index("--output") + 1]
    return options


def default_output():
    # plugin/assets/tile.png when this script sits in the repository.
    try:
        here = Path(__file__).resolve().parent
        if here.name == "blender" and (here.parent / "README.md").exists():
            return str(here.parent / "tile.png")
    except (NameError, OSError):
        pass
    return "//tile.png"  # next to the .blend


def set_render_settings(scene, output):
    for engine in ("BLENDER_EEVEE", "BLENDER_EEVEE_NEXT"):
        try:
            scene.render.engine = engine
            break
        except TypeError:
            continue
    r = scene.render
    r.resolution_x = SIZE
    r.resolution_y = SIZE
    r.resolution_percentage = 100
    r.film_transparent = True  # the plugin's dark background shows through
    r.image_settings.file_format = "PNG"
    r.image_settings.color_mode = "RGBA"
    r.image_settings.color_depth = "8"
    r.filepath = output
    # "Standard" keeps colours as you see them in the plugin; Blender's default
    # (AgX) greys out highlights.
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0


def clear_startup_scene(scene):
    # Blender's default cube, light and camera, only in an untouched new file.
    names = {obj.name for obj in scene.objects}
    if not bpy.data.filepath and names == {"Cube", "Light", "Camera"}:
        for name in names:
            bpy.data.objects.remove(bpy.data.objects[name], do_unlink=True)


def aim(obj, target=(0.0, 0.0, 0.0)):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def add_camera(scene):
    cam = bpy.data.objects.get("Tile Camera")
    if cam is None:
        cam = bpy.data.objects.new("Tile Camera", bpy.data.cameras.new("Tile Camera"))
        scene.collection.objects.link(cam)
        cam.data.type = "ORTHO"
        cam.data.ortho_scale = 2.5  # the starter shape is 2 units across, with room for its bevels
        cam.data.clip_end = 100.0
        cam.location = (0.0, 0.0, 10.0)
        cam.rotation_euler = (0.0, 0.0, 0.0)  # straight down; up in the image is +Y
    scene.camera = cam


def add_light(scene, name, location, energy, size, color):
    if bpy.data.objects.get(name) is not None:
        return
    data = bpy.data.lights.new(name, "AREA")
    data.energy = energy
    data.size = size
    data.color = color
    obj = bpy.data.objects.new(name, data)
    scene.collection.objects.link(obj)
    obj.location = location
    aim(obj)


def add_lights(scene):
    # Like the plugin's own tiles: a key light from the upper left and a faint,
    # cooler one from the lower right.
    add_light(scene, "Tile Key", (-3.0, 3.0, 4.5), 900.0, 3.0, (1.0, 0.98, 0.95))
    add_light(scene, "Tile Fill", (3.5, -3.0, 2.0), 150.0, 2.0, (0.82, 0.88, 1.0))
    world = scene.world
    if world is None:
        world = bpy.data.worlds.new("World")
        scene.world = world
    if world.node_tree is None:
        world.use_nodes = True  # Blender 4 and older; newer ones always use nodes
    if world.node_tree is not None:
        background = next((n for n in world.node_tree.nodes if n.type == "BACKGROUND"), None)
        if background is not None and not world.get("synthmatch_tile"):
            background.inputs["Color"].default_value = (0.035, 0.036, 0.04, 1.0)
            background.inputs["Strength"].default_value = 1.0
            world["synthmatch_tile"] = True


def metal(name, colour, roughness):
    mat = bpy.data.materials.get(name)
    if mat is not None:
        return mat
    mat = bpy.data.materials.new(name)
    if mat.node_tree is None:
        mat.use_nodes = True  # Blender 4 and older; newer ones always use nodes
    bsdf = next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Metallic"].default_value = 1.0
    bsdf.inputs["Roughness"].default_value = roughness
    return mat


def add_starter_shape(scene):
    # Only into an empty scene: a pentagon with bevelled sides around a dark,
    # glossy top, like the plugin's drawn tile. Reshape it, or delete it and
    # model your own.
    if any(obj.type == "MESH" for obj in scene.objects):
        return
    mesh = bpy.data.meshes.new("Tile Shape")
    bm = bmesh.new()
    outer, inner = [], []
    for i in range(5):
        a = math.pi / 2 + i * 2 * math.pi / 5  # first corner points up
        outer.append(bm.verts.new((math.cos(a), math.sin(a), 0.0)))
        inner.append(bm.verts.new((0.5 * math.cos(a), 0.5 * math.sin(a), 0.3)))
    top = bm.faces.new(inner)
    top.material_index = 1
    for i in range(5):
        j = (i + 1) % 5
        bm.faces.new((outer[i], outer[j], inner[j], inner[i]))
    bm.faces.new(list(reversed(outer)))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    bm.to_mesh(mesh)
    bm.free()

    mesh.materials.append(metal("Tile Steel", (0.62, 0.64, 0.68), 0.3))
    mesh.materials.append(metal("Tile Top", (0.06, 0.06, 0.07), 0.26))
    obj = bpy.data.objects.new("Tile Shape", mesh)
    scene.collection.objects.link(obj)
    bevel = obj.modifiers.new("Bevel", "BEVEL")
    bevel.width = 0.025
    bevel.segments = 3


@persistent
def save_tile(scene, *_):
    # After every render of a tile scene, write it where the plugin looks.
    if not scene.get("synthmatch_tile"):
        return
    image = bpy.data.images.get("Render Result")
    path = bpy.path.abspath(scene.render.filepath)
    if image is None or not path.lower().endswith(".png"):
        return
    if path.startswith("//"):
        print("SynthMatch: save the .blend first (the tile goes next to it), or set an output path")
        return
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    image.save_render(path, scene=scene)
    print(f"SynthMatch: saved the tile to {path}")


def register_save_handler():
    handlers = bpy.app.handlers.render_complete
    handlers[:] = [h for h in handlers if getattr(h, "__name__", "") != "save_tile"]
    handlers.append(save_tile)


def look_through_camera():
    try:
        for area in bpy.context.screen.areas:
            if area.type == "VIEW_3D":
                area.spaces.active.region_3d.view_perspective = "CAMERA"
    except AttributeError:
        pass  # no window (running in the background)


def main():
    options = script_options()
    scene = bpy.context.scene
    output = options["output"] or (scene.render.filepath if scene.get("synthmatch_tile") else default_output())
    clear_startup_scene(scene)
    set_render_settings(scene, output)
    add_camera(scene)
    add_lights(scene)
    add_starter_shape(scene)
    scene["synthmatch_tile"] = True
    look_through_camera()

    if options["render"]:
        bpy.ops.render.render(write_still=True)
        print(f"SynthMatch: saved the tile to {bpy.path.abspath(scene.render.filepath)}")
    else:
        register_save_handler()
        print(f"SynthMatch: tile scene ready. Press F12 to render; each render is saved to {output}")


main()
