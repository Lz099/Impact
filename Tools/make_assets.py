"""Creates the materials the Impact code loads by path (/Game/Impact/Materials).

Run without the editor open:
  UnrealEditor-Cmd.exe Impact.uproject -run=pythonscript -script=<this file> -unattended -nosplash
Running it again rebuilds every material from scratch.
"""
import unreal

FOLDER = "/Game/Impact/Materials"
tools = unreal.AssetToolsHelpers.get_asset_tools()
lib = unreal.MaterialEditingLibrary
assets = unreal.EditorAssetLibrary


def new_material(name):
    path = f"{FOLDER}/{name}"
    if assets.does_asset_exist(path):
        assets.delete_asset(path)
    return tools.create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew())


def node(material, cls, x, y, **properties):
    expression = lib.create_material_expression(material, cls, x, y)
    for key, value in properties.items():
        expression.set_editor_property(key, value)
    return expression


def scalar(material, name, value, x, y):
    return node(material, unreal.MaterialExpressionScalarParameter, x, y, parameter_name=name, default_value=value)


def vector(material, name, color, x, y):
    return node(material, unreal.MaterialExpressionVectorParameter, x, y, parameter_name=name,
                default_value=unreal.LinearColor(*color, 1.0))


def constant(material, value, x, y):
    return node(material, unreal.MaterialExpressionConstant, x, y, r=value)


def binary(material, cls, a, b, x, y):
    expression = node(material, cls, x, y)
    lib.connect_material_expressions(a, "", expression, "A")
    lib.connect_material_expressions(b, "", expression, "B")
    return expression


def finish(material):
    lib.recompile_material(material)
    assets.save_loaded_asset(material)
    unreal.log(f"[make_assets] saved {material.get_path_name()}")


assets.make_directory(FOLDER)

# Bright additive colour for muzzle flashes and tracers.
m = new_material("M_Impact_Emissive")
m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
m.set_editor_property("two_sided", True)
color = vector(m, "Color", (1.0, 0.7, 0.3), -600, 0)
intensity = scalar(m, "Intensity", 20.0, -600, 220)
lib.connect_material_property(binary(m, unreal.MaterialExpressionMultiply, color, intensity, -300, 100), "",
                              unreal.MaterialProperty.MP_EMISSIVE_COLOR)
finish(m)

# Wet dark red for flying blood droplets (drawn as instanced spheres).
m = new_material("M_Impact_Blood")
m.set_editor_property("used_with_instanced_static_meshes", True)
lib.connect_material_property(vector(m, "Color", (0.11, 0.004, 0.003), -400, 0), "", unreal.MaterialProperty.MP_BASE_COLOR)
lib.connect_material_property(constant(m, 0.18, -400, 200), "", unreal.MaterialProperty.MP_ROUGHNESS)
lib.connect_material_property(constant(m, 0.7, -400, 300), "", unreal.MaterialProperty.MP_SPECULAR)
finish(m)

# Irregular round stain projected on surfaces: blood splats and bullet holes (colour and noise set per decal).
m = new_material("M_Impact_DecalSplat")
m.set_editor_property("material_domain", unreal.MaterialDomain.MD_DEFERRED_DECAL)
m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
uv = node(m, unreal.MaterialExpressionTextureCoordinate, -1400, 0)
center = node(m, unreal.MaterialExpressionConstant2Vector, -1400, 150, r=0.5, g=0.5)
distance = binary(m, unreal.MaterialExpressionDistance, uv, center, -1150, 50)
half = constant(m, 0.5, -1150, 250)
radial = binary(m, unreal.MaterialExpressionMultiply,
                binary(m, unreal.MaterialExpressionSubtract, half, distance, -950, 100),
                constant(m, 2.0, -950, 250), -750, 100)

world = node(m, unreal.MaterialExpressionWorldPosition, -1400, 400)
noise = node(m, unreal.MaterialExpressionNoise, -1150, 400, scale=0.12, levels=4, output_min=0.0, output_max=1.0)
lib.connect_material_expressions(world, "", noise, "Position")
wobble = binary(m, unreal.MaterialExpressionMultiply,
                binary(m, unreal.MaterialExpressionSubtract, noise, half, -950, 400),
                scalar(m, "NoiseAmount", 0.7, -950, 550), -750, 450)

shape = binary(m, unreal.MaterialExpressionMultiply,
               binary(m, unreal.MaterialExpressionAdd, radial, wobble, -550, 250),
               scalar(m, "Hardness", 8.0, -550, 450), -350, 300)
saturate = node(m, unreal.MaterialExpressionSaturate, -150, 300)
lib.connect_material_expressions(shape, "", saturate, "")
lifetime = node(m, unreal.MaterialExpressionDecalLifetimeOpacity, -150, 450)
opacity = binary(m, unreal.MaterialExpressionMultiply,
                 binary(m, unreal.MaterialExpressionMultiply, saturate, lifetime, 50, 350),
                 scalar(m, "Opacity", 1.0, 50, 500), 250, 400)
lib.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)
lib.connect_material_property(vector(m, "Color", (0.11, 0.004, 0.003), 0, -200), "", unreal.MaterialProperty.MP_BASE_COLOR)
lib.connect_material_property(scalar(m, "Roughness", 0.2, 0, 0), "", unreal.MaterialProperty.MP_ROUGHNESS)
finish(m)

unreal.log("[make_assets] done")
