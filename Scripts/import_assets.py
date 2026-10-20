"""Import the downloaded (non-UE-format) third-party assets and build their materials, headless.

    UnrealEditor-Cmd.exe TollSimUE.uproject -run=pythonscript -script=Scripts/import_assets.py

Reads from the Fab library cache (or TOLL_ASSETS, a folder laid out the same way) and
writes to /Game/ThirdParty, which is not committed. Re-running rebuilds the materials and
reuses textures and meshes already imported; delete /Game/ThirdParty to import afresh.
See assets-manifest.json for what each item is and ATTRIBUTION.md for its licence.
"""
import os
import unreal

SRC = os.environ.get("TOLL_ASSETS") or os.path.expandvars(r"%LOCALAPPDATA%\com.epicgames.eglv2\FabLibrary")
DST = "/Game/ThirdParty"
MAT = "/Game/Toll/Materials"

mel = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary

# Megascans 3D plants: (cache folder, name); one mesh per variation, LOD1 (the tiers Quixel ship for games)
PLANTS = [
    ("Dry Grass-472c99c7", "DryGrass"),
    ("Thatching Grass-0311f445", "ThatchingGrass"),
    ("Dead Shrubs-d8fbec55", "DeadShrubs"),
]
# Megascans surfaces
SURFACES = [
    ("Dry Cracked Mud-bdf4f488", "DryCrackedMud"),
    ("Gravel Ground-5f67fdd6", "GravelGround"),
    ("Rippled Sand-642bd724", "RippledSand"),
    ("Road Asphalt-37df99dd", "RoadAsphalt"),
]


def find_dir(cache):
    root = os.path.join(SRC, cache)
    for d in os.listdir(root):
        p = os.path.join(root, d)
        if os.path.isdir(p):
            return p
    raise RuntimeError(f"nothing in {root}")


def import_files(files, dest, mesh=False):
    tasks = []
    for f in files:
        t = unreal.AssetImportTask()
        t.filename = f
        t.destination_path = dest
        t.automated = True
        t.replace_existing = True
        t.save = True
        tasks.append(t)
    tools.import_asset_tasks(tasks)
    out = []
    for t in tasks:
        for p in t.imported_object_paths:
            out.append(eal.load_asset(p))
    return out


def existing(path, dest):
    """The asset an earlier run imported from this file, if any."""
    full = f"{dest}/{os.path.splitext(os.path.basename(path))[0].replace(' ', '_').replace('-', '_')}"
    return eal.load_asset(full) if eal.does_asset_exist(full) else None


def texture(path, dest, kind):
    """kind: color | normal | mask"""
    tex = existing(path, dest)
    if tex:
        return tex
    (tex,) = [a for a in import_files([path], dest) if isinstance(a, unreal.Texture2D)][:1]
    if kind == "normal":
        tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
        tex.set_editor_property("srgb", False)
        tex.set_editor_property("flip_green_channel", False)
    elif kind == "mask":
        tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_MASKS)
        tex.set_editor_property("srgb", False)
    tex.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_WORLD)
    tex.set_editor_property("max_texture_size", 2048)  # 4K sources; 2K keeps us inside 6 GB of VRAM
    eal.save_asset(tex.get_path_name())
    return tex


# ---------------------------------------------------------------- base materials

def fresh(name):
    full = f"{MAT}/{name}"
    if eal.does_asset_exist(full):
        eal.delete_asset(full)
    return tools.create_asset(name, MAT, unreal.Material, unreal.MaterialFactoryNew())


DEFAULTS = {}  # color | normal | mask -> a texture of that kind, for parameter defaults


def tex_param(m, name, x, y, normal=False, mask=False):
    e = mel.create_material_expression(m, unreal.MaterialExpressionTextureSampleParameter2D, x, y)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("texture", DEFAULTS["normal" if normal else "mask" if mask else "color"])
    if normal:
        e.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    elif mask:
        e.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)
    return e


def scalar(m, name, v, x, y):
    e = mel.create_material_expression(m, unreal.MaterialExpressionScalarParameter, x, y)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("default_value", v)
    return e


def node(m, cls, x, y, **props):
    e = mel.create_material_expression(m, cls, x, y)
    for k, v in props.items():
        e.set_editor_property(k, v)
    return e


def link(a, b, pin="", out=""):
    mel.connect_material_expressions(a, out, b, pin)


def save(m):
    mel.layout_material_expressions(m)
    mel.recompile_material(m)
    eal.save_asset(m.get_path_name())


def make_foliage():
    m = fresh("M_Foliage")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    m.set_editor_property("two_sided", True)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    m.set_editor_property("used_with_instanced_static_meshes", True)
    m.set_editor_property("used_with_nanite", True)
    base = tex_param(m, "Base", -900, 0)
    nrm = tex_param(m, "Normal", -900, 300, normal=True)
    opa = tex_param(m, "Opacity", -900, 600, mask=True)
    rough = tex_param(m, "Roughness", -900, 900, mask=True)
    tint = node(m, unreal.MaterialExpressionVectorParameter, -900, -300, parameter_name="Tint", default_value=unreal.LinearColor(1, 1, 1, 1))
    mul = node(m, unreal.MaterialExpressionMultiply, -500, 0)
    link(base, mul, "A", "RGB")
    link(tint, mul, "B")
    mel.connect_material_property(mul, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(nrm, "RGB", unreal.MaterialProperty.MP_NORMAL)
    mel.connect_material_property(opa, "R", unreal.MaterialProperty.MP_OPACITY_MASK)
    mel.connect_material_property(rough, "R", unreal.MaterialProperty.MP_ROUGHNESS)
    sss = node(m, unreal.MaterialExpressionMultiply, -500, 300)
    link(base, sss, "A", "RGB")
    link(scalar(m, "Subsurface", 0.5, -700, 400), sss, "B")
    mel.connect_material_property(sss, "", unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
    m.set_editor_property("opacity_mask_clip_value", 0.4)
    save(m)
    return m


def world_uv(m, tile_name, tile, x, y):
    wp = node(m, unreal.MaterialExpressionWorldPosition, x - 600, y)
    xy = node(m, unreal.MaterialExpressionComponentMask, x - 400, y, r=True, g=True, b=False, a=False)
    link(wp, xy)
    div = node(m, unreal.MaterialExpressionDivide, x - 200, y)
    link(xy, div, "A")
    link(scalar(m, tile_name, tile, x - 400, y + 120), div, "B")
    return div


def make_surface():
    """A tiling Megascans surface, world-aligned, tinted by vertex colour (the terrain's macro colour)."""
    m = fresh("M_Surface")
    uv = world_uv(m, "Tile", 400.0, -900, 0)
    base = tex_param(m, "Base", -600, 0)
    nrm = tex_param(m, "Normal", -600, 300, normal=True)
    rough = tex_param(m, "Roughness", -600, 600, mask=True)
    for t in (base, nrm, rough):
        link(uv, t, "UVs")
    vc = node(m, unreal.MaterialExpressionVertexColor, -600, -300)
    tint = node(m, unreal.MaterialExpressionMultiply, -350, -200)
    link(vc, tint, "A")
    link(scalar(m, "VertexTint", 2.2, -600, -150), tint, "B")
    blend = node(m, unreal.MaterialExpressionLinearInterpolate, -200, -100)  # VertexTintAmount 0 = texture only
    link(node(m, unreal.MaterialExpressionConstant3Vector, -400, -400, constant=unreal.LinearColor(1, 1, 1, 1)), blend, "A")
    link(tint, blend, "B")
    link(scalar(m, "VertexTintAmount", 1.0, -400, -50), blend, "Alpha")
    mul = node(m, unreal.MaterialExpressionMultiply, 0, 0)
    link(base, mul, "A", "RGB")
    link(blend, mul, "B")
    tinted = node(m, unreal.MaterialExpressionMultiply, 200, 0)
    link(mul, tinted, "A")
    link(node(m, unreal.MaterialExpressionVectorParameter, 0, -300, parameter_name="Tint", default_value=unreal.LinearColor(1, 1, 1, 1)), tinted, "B")
    mel.connect_material_property(tinted, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(nrm, "RGB", unreal.MaterialProperty.MP_NORMAL)
    mel.connect_material_property(rough, "R", unreal.MaterialProperty.MP_ROUGHNESS)
    save(m)
    return m


def make_ground():
    """Terrain: two world-aligned surfaces blended by vertex alpha, tinted by vertex colour."""
    m = fresh("M_Ground")
    uv = world_uv(m, "Tile", 350.0, -1200, 0)
    a = tex_param(m, "BaseA", -800, -200)
    b = tex_param(m, "BaseB", -800, 100)
    na = tex_param(m, "NormalA", -800, 400, normal=True)
    nb = tex_param(m, "NormalB", -800, 700, normal=True)
    for t in (a, b, na, nb):
        link(uv, t, "UVs")
    vc = node(m, unreal.MaterialExpressionVertexColor, -800, -500)
    lb = node(m, unreal.MaterialExpressionLinearInterpolate, -450, -100)
    link(a, lb, "A", "RGB")
    link(b, lb, "B", "RGB")
    link(vc, lb, "Alpha", "A")
    ln = node(m, unreal.MaterialExpressionLinearInterpolate, -450, 500)
    link(na, ln, "A", "RGB")
    link(nb, ln, "B", "RGB")
    link(vc, ln, "Alpha", "A")
    tint = node(m, unreal.MaterialExpressionMultiply, -450, -400)
    link(vc, tint, "A")  # VertexColor has no RGB pin; its default output is the colour
    link(scalar(m, "VertexTint", 2.2, -700, -350), tint, "B")
    mul = node(m, unreal.MaterialExpressionMultiply, -200, -200)
    link(lb, mul, "A")
    link(tint, mul, "B")
    warm = node(m, unreal.MaterialExpressionMultiply, 0, -200)
    link(mul, warm, "A")
    link(node(m, unreal.MaterialExpressionVectorParameter, -200, -450, parameter_name="Tint", default_value=unreal.LinearColor(1, 1, 1, 1)), warm, "B")
    mel.connect_material_property(warm, "", unreal.MaterialProperty.MP_BASE_COLOR)
    mel.connect_material_property(ln, "", unreal.MaterialProperty.MP_NORMAL)
    mel.connect_material_property(scalar(m, "Roughness", 0.92, -450, 800), "", unreal.MaterialProperty.MP_ROUGHNESS)
    save(m)
    return m


def instance(parent, name, dest, textures, scalars=None, vectors=None):
    full = f"{dest}/{name}"
    if eal.does_asset_exist(full):
        eal.delete_asset(full)
    mi = tools.create_asset(name, dest, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    mel.set_material_instance_parent(mi, parent)
    for k, t in textures.items():
        mel.set_material_instance_texture_parameter_value(mi, k, t)
    for k, v in (scalars or {}).items():
        mel.set_material_instance_scalar_parameter_value(mi, k, v)
    for k, v in (vectors or {}).items():
        mel.set_material_instance_vector_parameter_value(mi, k, v)
    mel.update_material_instance(mi)
    eal.save_asset(mi.get_path_name())
    return mi


def maps(folder):
    got = {}
    for f in os.listdir(folder):
        if not f.lower().endswith(".jpg") or "Billboard" in f:
            continue
        key = f.rsplit("_", 1)[-1][:-4]
        got[key] = os.path.join(folder, f)
    return got


def main():
    made = {}
    for cache, name in SURFACES:
        folder = find_dir(cache)
        dest = f"{DST}/Megascans/{name}"
        m = maps(folder)
        made[name] = {"Base": texture(m["BaseColor"], dest, "color"), "Normal": texture(m["Normal"], dest, "normal"), "Roughness": texture(m["Roughness"], dest, "mask")}
    g = made["GravelGround"]
    DEFAULTS.update(color=g["Base"], normal=g["Normal"], mask=g["Roughness"])
    foliage, surface, ground = make_foliage(), make_surface(), make_ground()

    for cache, name in SURFACES:
        instance(surface, f"MI_{name}", f"{DST}/Megascans/{name}", made[name])
        unreal.log(f"toll import: surface {name}")

    gm = made["DryCrackedMud"]  # bushveld soil; the rippled sand reads as dunes
    gg = made["GravelGround"]
    instance(ground, "MI_Ground", f"{DST}/Megascans", {"BaseA": gm["Base"], "NormalA": gm["Normal"], "BaseB": gg["Base"], "NormalB": gg["Normal"]})

    for cache, name in PLANTS:
        folder = find_dir(cache)
        dest = f"{DST}/Megascans/{name}"
        m = maps(folder)
        t = {"Base": texture(m["BaseColor"], dest, "color"), "Normal": texture(m["Normal"], dest, "normal"),
             "Opacity": texture(m["Opacity"], dest, "mask"), "Roughness": texture(m["Roughness"], dest, "mask")}
        mi = instance(foliage, f"MI_{name}", dest, t)
        fbx = sorted(os.path.join(folder, f) for f in os.listdir(folder) if f.endswith("_LOD1.fbx"))
        have = [existing(f, dest) for f in fbx]
        meshes = have if all(have) else import_files(fbx, dest, mesh=True)
        for sm in meshes:
            if isinstance(sm, unreal.StaticMesh):
                for i in range(sm.get_num_sections(0)):
                    sm.set_material(i, mi)
                eal.save_asset(sm.get_path_name())
        unreal.log(f"toll import: plant {name}, {len(fbx)} variations")

    acacia = os.path.join(SRC, "Acacia Tree-08ad3005", "acacia-tree", "source", "1.fbx")
    meshes = [a for a in eal.list_assets(f"{DST}/AcaciaTree", recursive=False) if a] if eal.does_directory_exist(f"{DST}/AcaciaTree") else []
    meshes = [m for m in (eal.load_asset(a) for a in meshes) if isinstance(m, unreal.StaticMesh)]
    if not meshes:
        meshes = [a for a in import_files([acacia], f"{DST}/AcaciaTree", mesh=True) if isinstance(a, unreal.StaticMesh)]
    # the FBX brings its own materials; the instanced, Nanite trees need both usage flags
    for path in eal.list_assets(f"{DST}/AcaciaTree", recursive=True):
        a = eal.load_asset(path)
        if isinstance(a, unreal.Material):
            a.set_editor_property("used_with_instanced_static_meshes", True)
            a.set_editor_property("used_with_nanite", True)
            mel.recompile_material(a)
            eal.save_asset(a.get_path_name())
    for sm in meshes:
        s = sm.get_editor_property("nanite_settings")
        s.enabled = True
        sm.set_editor_property("nanite_settings", s)
        eal.save_asset(sm.get_path_name())
    unreal.log(f"toll import: acacia, {len(meshes)} meshes")
    unreal.log("toll import done")


main()
