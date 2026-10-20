"""Create the project's base materials headlessly.

    UnrealEditor-Cmd.exe TollSimUE.uproject -run=pythonscript -script=Scripts/make_materials.py

Re-running replaces them. They are deliberately generic: the scene code feeds
them vertex colours (plaza and road geometry exported from the browser) or a
texture loaded at runtime (sign atlases, fare boards).
"""
import unreal

PATH = "/Game/Toll/Materials"
mel = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()
eal = unreal.EditorAssetLibrary


def fresh(name):
    full = f"{PATH}/{name}"
    if eal.does_asset_exist(full):
        eal.delete_asset(full)
    return tools.create_asset(name, PATH, unreal.Material, unreal.MaterialFactoryNew())


def scalar(mat, name, value, x, y):
    e = mel.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("default_value", value)
    return e


def multiply(mat, a, b, x, y):
    m = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, x, y)
    mel.connect_material_expressions(a, "", m, "A")
    mel.connect_material_expressions(b, "", m, "B")
    return m


def save(mat):
    mel.layout_material_expressions(mat)
    mel.recompile_material(mat)
    eal.save_asset(mat.get_path_name())


# vertex colours: plaza body, apron, markings, road
m = fresh("M_VertexColor")
vc = mel.create_material_expression(m, unreal.MaterialExpressionVertexColor, -600, 0)
mel.connect_material_property(vc, "", unreal.MaterialProperty.MP_BASE_COLOR)
mel.connect_material_property(scalar(m, "Roughness", 0.7, -600, 200), "", unreal.MaterialProperty.MP_ROUGHNESS)
mel.connect_material_property(scalar(m, "Metallic", 0.0, -600, 300), "", unreal.MaterialProperty.MP_METALLIC)
mel.connect_material_property(multiply(m, vc, scalar(m, "Emissive", 0.0, -600, 400), -300, 400), "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
save(m)

# a texture with a cut-out alpha: signs, fare boards, chevrons, lane signals
m = fresh("M_Textured")
m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
m.set_editor_property("two_sided", True)
tex = mel.create_material_expression(m, unreal.MaterialExpressionTextureSampleParameter2D, -600, 0)
tex.set_editor_property("parameter_name", "Tex")
mel.connect_material_property(tex, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
mel.connect_material_property(tex, "A", unreal.MaterialProperty.MP_OPACITY_MASK)
mel.connect_material_property(scalar(m, "Roughness", 0.5, -600, 250), "", unreal.MaterialProperty.MP_ROUGHNESS)
mel.connect_material_property(multiply(m, tex, scalar(m, "Emissive", 0.1, -600, 350), -300, 350), "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
save(m)

# a flat colour for stand-ins (vehicle boxes, ground)
m = fresh("M_Color")
c = mel.create_material_expression(m, unreal.MaterialExpressionVectorParameter, -600, 0)
c.set_editor_property("parameter_name", "Color")
mel.connect_material_property(c, "", unreal.MaterialProperty.MP_BASE_COLOR)
mel.connect_material_property(scalar(m, "Roughness", 0.6, -600, 200), "", unreal.MaterialProperty.MP_ROUGHNESS)
mel.connect_material_property(multiply(m, c, scalar(m, "Emissive", 0.0, -600, 300), -300, 300), "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
save(m)

unreal.log("toll materials written")
