"""Import the authored loops for evaluation; do not overwrite the running game's clips."""
import unreal
import json
import traceback
from pathlib import Path

ROOT = Path("D:/UEProjets/Impact/Saved/AnimationsSol")
DEST = "/Game/Impact/Anims/Sol_Creation_v01"
NAMES = ("Recroqueville_Au_Sol","Halete_Au_Sol","Tient_Jambe_Au_Sol")
report = {"destination":DEST, "clips":[], "passed":False}
try:
    skeleton = unreal.load_asset("/ALS/ALS/Character/SK_Als")
    assert skeleton is not None
    for name in NAMES:
        ui = unreal.FbxImportUI()
        ui.set_editor_property("import_mesh",False)
        ui.set_editor_property("import_as_skeletal",True)
        ui.set_editor_property("import_animations",True)
        ui.set_editor_property("skeleton",skeleton)
        ui.set_editor_property("mesh_type_to_import",unreal.FBXImportType.FBXIT_ANIMATION)
        d = ui.anim_sequence_import_data
        d.set_editor_property("animation_length",unreal.FBXAnimationLengthImportType.FBXALIT_EXPORTED_TIME)
        d.set_editor_property("use_default_sample_rate",False)
        d.set_editor_property("custom_sample_rate",30)
        d.set_editor_property("remove_redundant_keys",False)
        d.set_editor_property("convert_scene",True)
        task = unreal.AssetImportTask()
        task.set_editor_property("filename",str(ROOT/name/(name+".fbx")))
        task.set_editor_property("destination_path",DEST)
        task.set_editor_property("destination_name",name)
        task.set_editor_property("automated",True)
        task.set_editor_property("replace_existing",True)
        task.set_editor_property("save",True)
        task.set_editor_property("options",ui)
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        anim = unreal.load_asset(DEST+"/"+name)
        assert isinstance(anim,unreal.AnimSequence),name
        tracks = list(unreal.AnimationLibrary.get_animation_track_names(anim))
        entry = {"name":name,"duration_s":anim.get_play_length(),"tracks":len(tracks),
                 "skeleton":anim.get_editor_property("skeleton").get_path_name(),"motion_deg":{}}
        import math
        for bone in ("pelvis","spine_01","spine_02","spine_03","head","upperarm_l","thigh_r","foot_r"):
            first = unreal.AnimationLibrary.get_bone_pose_for_time(anim,bone,0.,False)
            samples = [unreal.AnimationLibrary.get_bone_pose_for_time(anim,bone,i/30,False)
                       for i in range(151)]
            entry["motion_deg"][bone] = max(math.degrees(abs(
                unreal.MathLibrary.quat_angular_distance(first.rotation,p.rotation))) for p in samples)
            last = samples[-1]
            entry.setdefault("seam_deg",{})[bone] = math.degrees(abs(
                unreal.MathLibrary.quat_angular_distance(first.rotation,last.rotation)))
        assert abs(entry["duration_s"]-5.0)<.002,entry
        assert entry["skeleton"]==skeleton.get_path_name(),entry
        assert entry["tracks"]>=60,entry
        assert max(entry["motion_deg"].values())>1,entry
        assert max(entry["seam_deg"].values())<.1,entry
        report["clips"].append(entry)
    report["passed"] = True
except Exception:
    report["error"] = traceback.format_exc()
finally:
    (ROOT/"unreal_import.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    unreal.log("GROUND_ANIMATION_REPORT "+json.dumps(report))
    unreal.SystemLibrary.quit_editor()
