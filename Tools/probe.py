import unreal
out=[]
for n in ["MaterialExpressionPreSkinnedLocalPosition","MaterialExpressionCustom","MaterialExpressionVectorParameter",
          "MaterialExpressionScalarParameter","MaterialExpressionLinearInterpolate","MaterialExpressionSaturate",
          "MaterialExpressionMaterialFunctionCall","MaterialEditingLibrary"]:
    out.append("%-42s %s" % (n, "OUI" if hasattr(unreal,n) else "NON"))
m = unreal.load_asset("/ALS/ALS/Character/MI_Als_Character")
out.append("materiau du mannequin : %s" % m)
if m is not None:
    try:
        out.append("  parent : %s" % m.get_editor_property("parent"))
    except Exception as e:
        out.append("  parent : %s" % str(e)[:60])
mesh = unreal.load_asset("/ALS/ALS/Character/SKM_Als")
if mesh is not None:
    mats = mesh.get_editor_property("materials")
    out.append("  emplacements materiaux sur SKM_Als : %d" % len(mats))
open("D:/UEProjets/Impact/Saved/probe.txt","w",encoding="utf-8").write("\n".join(out))
