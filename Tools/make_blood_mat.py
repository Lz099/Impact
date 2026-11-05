import unreal, traceback
out=[]
try:
    FOLDER="/Game/Impact/Materials"
    lib=unreal.MaterialEditingLibrary
    tools=unreal.AssetToolsHelpers.get_asset_tools()
    assets=unreal.EditorAssetLibrary
    assets.make_directory(FOLDER)
    path=FOLDER+"/M_Impact_BloodSkin"
    if assets.does_asset_exist(path): assets.delete_asset(path)
    m=tools.create_asset("M_Impact_BloodSkin", FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("two_sided", False)
    # Laid over a skinned body (the mesh's overlay): without this flag a -game run drops it with a warning.
    m.set_editor_property("used_with_skeletal_mesh", True)
    try:
        # Lit like a surface, so the wet shine (Rugosite, Brillance) shows; the default volumetric mode ignores both.
        m.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    except Exception:
        out.append("translucency_lighting_mode non regle : " + traceback.format_exc()[-300:])

    def node(cls,x,y,**kw):
        e=lib.create_material_expression(m,cls,x,y)
        for k,v in kw.items(): e.set_editor_property(k,v)
        return e

    world=node(unreal.MaterialExpressionWorldPosition,-900,0)
    custom=node(unreal.MaterialExpressionCustom,-500,0)
    def mk(name):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", name)
        return ci
    ins=[mk("P")] + [mk("W%d"%i) for i in range(8)]
    # Opaque out to 0.6 of the radius, fading to nothing at the radius. Below the hole the blood has run: the
    # stain reaches twice as far downwards (world down, so on a man lying on his side it runs towards the floor).
    code=["float A = 0;", "float3 D;"]
    for i in range(8):
        code.append("D = P - W%d.xyz; D.z = D.z < 0 ? D.z * 0.5 : D.z;" % i)
        code.append("A = max(A, 1 - saturate((length(D) - W%d.w*0.6) / max(W%d.w*0.4, 0.5)));" % (i,i))
    code.append("return saturate(A);")
    custom.set_editor_property("code","\n".join(code))
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    custom.set_editor_property("inputs", ins)
    custom.set_editor_property("description","Taches de sang : masques spheriques autour des plaies")
    lib.connect_material_expressions(world,"",custom,"P")
    for i in range(8):
        v=node(unreal.MaterialExpressionVectorParameter,-900,160+i*90,parameter_name="Plaie%d"%i,
               default_value=unreal.LinearColor(0,0,-100000,0))
        # "RGBA", not "": the empty name is output 0, RGB, and the shader then has no .w (the radius) - the
        # material failed to compile with "vector swizzle 'w' is out of bounds" and the game used the default one.
        lib.connect_material_expressions(v,"RGBA",custom,"W%d"%i)

    colour=node(unreal.MaterialExpressionVectorParameter,-500,-250,parameter_name="Couleur",
                default_value=unreal.LinearColor(0.22,0.012,0.008,1.0))
    lib.connect_material_property(colour,"",unreal.MaterialProperty.MP_BASE_COLOR)
    lib.connect_material_property(custom,"",unreal.MaterialProperty.MP_OPACITY)
    rough=node(unreal.MaterialExpressionScalarParameter,-500,-120,parameter_name="Rugosite",default_value=0.15)
    lib.connect_material_property(rough,"",unreal.MaterialProperty.MP_ROUGHNESS)
    spec=node(unreal.MaterialExpressionScalarParameter,-500,-60,parameter_name="Brillance",default_value=0.8)
    lib.connect_material_property(spec,"",unreal.MaterialProperty.MP_SPECULAR)

    lib.recompile_material(m)
    assets.save_loaded_asset(m)
    out.append("cree : %s" % path)
except Exception:
    out.append(traceback.format_exc()[:900])
open("D:/UEProjets/Impact/Saved/mat_result.txt","w",encoding="utf-8").write("\n".join(out))
