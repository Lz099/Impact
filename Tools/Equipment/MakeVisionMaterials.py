"""Create/update the two helmet post-process materials in UE 5.7's editor.

Run manually with Tools > Execute Python Script. This file is not a commandlet
launcher and does not change project settings or any other assets. Re-running
rebuilds the graphs of these two materials in place, preserving asset references.
Thermal requires Custom Depth-Stencil Pass = Enabled with Stencil (r.CustomDepth=3).
Safe in a shared -ExecutePythonScript wrapper: use runpy.run_path with
run_name="__main__", or import this module and call main(). No editor quit/exit.
"""

import unreal


ASSET_ROOT = "/Game/Impact/Equipment"
MATERIAL_NAMES = ("M_HelmetNight", "M_HelmetThermal")
MEL = unreal.MaterialEditingLibrary


class Graph:
    def __init__(self, material):
        self.material = material

    def node(self, expression_class, **properties):
        expression = MEL.create_material_expression(self.material, expression_class)
        if expression is None:
            raise RuntimeError(f"Cannot create {expression_class} in {self.material.get_path_name()}")
        for name, value in properties.items():
            expression.set_editor_property(name, value)
        return expression

    @staticmethod
    def connect(source, target, pin=""):
        # The first output is Color for SceneTexture, scalar/RGB for the other nodes.
        if not MEL.connect_material_expressions(source, "", target, pin):
            raise RuntimeError(f"Cannot connect {source.get_name()} -> {target.get_name()}.{pin}")

    def scalar(self, value):
        return self.node(unreal.MaterialExpressionConstant, r=float(value))

    def color(self, r, g, b):
        return self.node(
            unreal.MaterialExpressionConstant3Vector,
            constant=unreal.LinearColor(r, g, b, 1.0),
        )

    def unary(self, expression_class, source, **properties):
        result = self.node(expression_class, **properties)
        self.connect(source, result)
        return result

    def binary(self, expression_class, a, b):
        result = self.node(expression_class)
        self.connect(a, result, "A")
        self.connect(b, result, "B")
        return result

    def multiply(self, a, b):
        return self.binary(unreal.MaterialExpressionMultiply, a, b)

    def add(self, a, b):
        return self.binary(unreal.MaterialExpressionAdd, a, b)

    def saturate(self, source):
        return self.unary(unreal.MaterialExpressionSaturate, source)

    def lerp(self, a, b, alpha):
        result = self.binary(unreal.MaterialExpressionLinearInterpolate, a, b)
        self.connect(alpha, result, "Alpha")
        return result

    def compare(self, a, b, greater, equal, less):
        # These are UE 5.7 MaterialEditingLibrary's shortened pin names.
        # Zero equality threshold makes depth <= exact, without a wall-leaking bias.
        result = self.node(unreal.MaterialExpressionIf, equals_threshold=0.0)
        for pin, source in (("A", a), ("B", b), ("A > B", greater),
                            ("A == B", equal), ("A < B", less)):
            self.connect(source, result, pin)
        return result

    def scene(self, texture_id, rgb=False):
        source = self.node(
            unreal.MaterialExpressionSceneTexture,
            scene_texture_id=texture_id,
            filtered=False,
        )
        # Leave UVs unconnected: UE computes the correct default UV per texture,
        # including editor sub-viewports and differing scene/output resolutions.
        return self.unary(
            unreal.MaterialExpressionComponentMask, source,
            r=True, g=rgb, b=rgb, a=False,
        )

    def luminance(self):
        scene = self.scene(unreal.SceneTextureId.PPI_POST_PROCESS_INPUT0, rgb=True)
        return self.binary(
            unreal.MaterialExpressionDotProduct, scene, self.color(0.2126, 0.7152, 0.0722)
        )

    def finish(self, result):
        if not MEL.connect_material_property(result, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
            raise RuntimeError(f"Cannot connect Emissive Color on {self.material.get_path_name()}")
        MEL.layout_material_expressions(self.material)
        MEL.recompile_material(self.material)


def build_night(material):
    graph = Graph(material)
    luminance = graph.saturate(graph.luminance())
    # Gain plus gamma lifting reveals existing shadow detail; a tint alone cannot.
    lifted = graph.unary(unreal.MaterialExpressionPower, luminance, const_exponent=0.55)
    gain = graph.multiply(lifted, graph.scalar(2.0))
    # Compress bright regions smoothly instead of clipping the whole daylight view green.
    amplified = graph.binary(unreal.MaterialExpressionDivide,gain,graph.add(gain,graph.scalar(0.7)))
    graph.finish(graph.multiply(amplified, graph.color(0.12, 1.0, 0.20)))


def build_thermal(material):
    graph = Graph(material)
    luminance = graph.saturate(graph.luminance())
    background = graph.multiply(luminance, graph.color(0.14, 0.14, 0.14))

    # Characters remain warm even when their scene lighting is black.
    heat = graph.saturate(graph.add(
        graph.multiply(luminance, graph.scalar(2.0)), graph.scalar(0.25)
    ))
    warm = graph.lerp(graph.color(1.0, 0.15, 0.025), graph.color(1.0, 1.0, 0.95), heat)

    stencil = graph.scene(unreal.SceneTextureId.PPI_CUSTOM_STENCIL)
    custom_depth = graph.scene(unreal.SceneTextureId.PPI_CUSTOM_DEPTH)
    scene_depth = graph.scene(unreal.SceneTextureId.PPI_SCENE_DEPTH)
    zero, one = graph.scalar(0.0), graph.scalar(1.0)

    # CustomStencil is an integer value in [0, 255], not a normalized color.
    tagged = graph.compare(stencil, graph.scalar(253.0), zero, one, zero)
    # Both depth samples are linear view depths. Reject every sample behind the
    # visible scene surface: a wall in front of a tagged Character stays dark.
    visible = graph.compare(custom_depth, scene_depth, zero, one, one)
    mask = graph.multiply(tagged, visible)
    graph.finish(graph.lerp(background, warm, mask))


def main():
    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

    # Check both destinations before changing either graph; never replace a
    # non-Material asset that happens to occupy one of these exact paths.
    materials = []
    for name in MATERIAL_NAMES:
        path = f"{ASSET_ROOT}/{name}"
        material = assets.load_asset(path) if assets.does_asset_exist(path) else None
        if material is not None and not isinstance(material, unreal.Material):
            raise RuntimeError(f"Expected a Material at {path}; existing asset left untouched")
        if material is None and assets.does_asset_exist(path):
            raise RuntimeError(f"Existing asset could not be loaded: {path}")
        materials.append(material)

    for index, name in enumerate(MATERIAL_NAMES):
        if materials[index] is None:
            materials[index] = asset_tools.create_asset(
                name, ASSET_ROOT, unreal.Material, unreal.MaterialFactoryNew()
            )
        if materials[index] is None:
            raise RuntimeError(f"Could not create {ASSET_ROOT}/{name}")

    for material, builder in zip(materials, (build_night, build_thermal)):
        material.set_editor_property("material_domain", unreal.MaterialDomain.MD_POST_PROCESS)
        material.set_editor_property(
            "blendable_location", unreal.BlendableLocation.BL_SCENE_COLOR_AFTER_TONEMAPPING
        )
        material.set_editor_property("blendable_priority", 1000)
        material.set_editor_property("is_blendable", True)
        material.set_editor_property("enable_stencil_test", False)
        MEL.delete_all_material_expressions(material)
        builder(material)

    # Save only these two assets. Compilation is requested above; shader errors
    # must still be checked in the editor after asynchronous compilation finishes.
    for material in materials:
        if not assets.save_loaded_asset(material, False):
            raise RuntimeError(f"Failed to save {material.get_path_name()}")
        unreal.log(f"Helmet vision material saved: {material.get_path_name()}")


if __name__ == "__main__":
    main()
