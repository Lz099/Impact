import unreal, os, traceback
out=[]
try:
    OUT = "D:/UEProjets/Impact/Saved/_preuves_reprise/mixamo"
    os.makedirs(OUT, exist_ok=True)
    mesh = unreal.load_asset("/ALS/ALS/Character/SKM_Als")
    task = unreal.AssetExportTask()
    task.set_editor_property("object", mesh)
    task.set_editor_property("filename", OUT + "/SKM_Als.fbx")
    task.set_editor_property("automated", True)
    task.set_editor_property("prompt", False)
    task.set_editor_property("replace_identical", True)
    task.set_editor_property("exporter", unreal.SkeletalMeshExporterFBX())
    o = unreal.FbxExportOption()
    o.set_editor_property("ascii", False)
    o.set_editor_property("collision", False)
    o.set_editor_property("level_of_detail", False)
    task.set_editor_property("options", o)
    ok = unreal.Exporter.run_asset_export_task(task)
    out.append("export SKM_Als : %s" % ok)
    out.append("fichier : %s" % (os.path.getsize(OUT+"/SKM_Als.fbx") if os.path.exists(OUT+"/SKM_Als.fbx") else "absent"))
except Exception:
    out.append(traceback.format_exc()[:800])
open("D:/UEProjets/Impact/Saved/export_result.txt","w",encoding="utf-8").write("\n".join(out))
unreal.SystemLibrary.quit_editor()
