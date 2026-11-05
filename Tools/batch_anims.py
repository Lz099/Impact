# PERIME - NE PAS RELANCER. Ce script construit son retargeter sans appeler
# assign_ik_rig_to_all_ops(), donc les operations du retargeter gardent une table de chaines vide
# et le reciblage sort une pose figee, sans rien dire. C'est ce qui a rendu les quatorze prises
# Mixamo immobiles. Utiliser Tools/retarget_setup.py puis Tools/retarget_batch.py, et controler
# avec Tools/check_anims.py. Les assets qu'il laisse (IK_Mixamo, IK_Mixamo_2,
# RTG_Mixamo_vers_Als, RTG_Mixamo_vers_Als_2) sont morts : le bon est RTG_Mixamo_Als_v3.
import unreal, os, traceback, re
out=[]
def say(t): out.append(str(t)); open("D:/UEProjets/Impact/Saved/batch_result.txt","w",encoding="utf-8").write("\n".join(out))
SRC="D:/UEProjets/Impact/Saved/_animations_a_importer"
try:
    mix_mesh=unreal.load_asset("/Game/Impact/Mixamo/Ch36_nonPBR")
    als_mesh=unreal.load_asset("/ALS/ALS/Character/SKM_Als")
    rtg=unreal.load_asset("/Game/Impact/Retarget/RTG_Mixamo_vers_Als_2")
    mix_skel=mix_mesh.get_editor_property("skeleton")
    tools=unreal.AssetToolsHelpers.get_asset_tools()
    files=[f for f in os.listdir(SRC) if f.lower().endswith(".fbx")]
    say("%d fichier(s) a traiter" % len(files))
    for f in files:
        base=re.sub(r"[^A-Za-z0-9]+","_",os.path.splitext(f)[0]).strip("_")
        ui=unreal.FbxImportUI()
        ui.set_editor_property("import_mesh", False)
        ui.set_editor_property("import_as_skeletal", True)
        ui.set_editor_property("import_animations", True)
        ui.set_editor_property("skeleton", mix_skel)
        ui.set_editor_property("mesh_type_to_import", unreal.FBXImportType.FBXIT_ANIMATION)
        d=ui.anim_sequence_import_data
        d.set_editor_property("animation_length", unreal.FBXAnimationLengthImportType.FBXALIT_EXPORTED_TIME)
        d.set_editor_property("remove_redundant_keys", False)
        d.set_editor_property("convert_scene", True)
        t=unreal.AssetImportTask()
        t.set_editor_property("filename", SRC+"/"+f)
        t.set_editor_property("destination_path","/Game/Impact/Mixamo")
        t.set_editor_property("automated",True); t.set_editor_property("replace_existing",True)
        t.set_editor_property("save",True); t.set_editor_property("options",ui)
        tools.import_asset_tasks([t])
        src_name=None
        for p in t.get_editor_property("imported_object_paths"):
            a=unreal.load_asset(p.split(".")[0])
            if isinstance(a, unreal.AnimSequence): src_name=a.get_name()
        if src_name is None:
            say("%s : IMPORT ECHOUE" % f); continue
        data=unreal.EditorAssetLibrary.find_asset_data("/Game/Impact/Mixamo/"+src_name)
        done=unreal.IKRetargetBatchOperation().duplicate_and_retarget(
            assets_to_retarget=[data], source_mesh=mix_mesh, target_mesh=als_mesh,
            ik_retarget_asset=rtg, search=src_name, replace=base, prefix="", suffix="")
        for dd in (done or []):
            p=str(dd.package_name)
            unreal.EditorAssetLibrary.save_asset(p, False)
            dest="/Game/Impact/Anims/"+str(dd.asset_name)
            if not unreal.EditorAssetLibrary.does_asset_exist(dest):
                unreal.EditorAssetLibrary.rename_asset(p, dest)
            a=unreal.load_asset(dest)
            say("%s -> %s (%.2f s)" % (f, dest, a.get_play_length() if a else 0))
    unreal.EditorAssetLibrary.save_directory("/Game/Impact", False, True)
    say("termine")
except Exception:
    say(traceback.format_exc()[:1200])
unreal.SystemLibrary.quit_editor()
