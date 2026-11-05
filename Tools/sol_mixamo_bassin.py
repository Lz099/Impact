# Laying Moaning et Laying Breathless sont faites sur un lit : bassin a 108 cm. Redescendues au sol (bassin a 11 cm, comme les
# autres prises sur le dos), le reste du trajet garde. A lancer dans l'editeur complet :
#   UnrealEditor.exe Impact.uproject -ExecutePythonScript=Tools/sol_mixamo_bassin.py
# Rapport : Saved/_diag/sol_mixamo_bassin.txt
import unreal, os, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/sol_mixamo_bassin.txt"
ASSETS = ["/Game/Impact/Anims/Sol_Mixamo/Sol_Gemit_Dos", "/Game/Impact/Anims/Sol_Mixamo/Sol_Souffle_Dos"]
FLOOR = 11.0

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

try:
    for path in ASSETS:
        a = unreal.load_asset(path)
        n = unreal.AnimationLibrary.get_num_frames(a)
        poses = [unreal.AnimationLibrary.get_bone_pose_for_frame(a, "pelvis", f, False) for f in range(n + 1)]
        low = min(p.translation.z for p in poses)
        shift = FLOOR - low
        pos = [unreal.Vector(p.translation.x, p.translation.y, p.translation.z + shift) for p in poses]
        rot = [p.rotation for p in poses]
        sca = [unreal.Vector(1.0, 1.0, 1.0) for p in poses]
        a.get_editor_property("controller").set_bone_track_keys("pelvis", pos, rot, sca)
        unreal.EditorAssetLibrary.save_asset(path, False)
        z = unreal.AnimationLibrary.get_bone_pose_for_frame(a, "pelvis", 0, False).translation.z
        say("%s : bassin %.0f -> %.0f cm" % (path.split("/")[-1], low, z))
    say("FIN")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")

try:
    unreal.SystemLibrary.quit_editor()
except Exception:
    pass
