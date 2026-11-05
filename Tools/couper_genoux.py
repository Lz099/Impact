# Coupe Chute_Genoux_1_NON_COUPEE (Praying recible) aux images 12 a 45 : il tombe sur les deux genoux, sans la priere qui suit.
# Le controleur Python n'a pas de "retirer des images" dans cette version : chaque os est relu image par image sur la fenetre
# gardee, la longueur est reglee, puis les cles sont reecrites. Rapport : Saved/_diag/couper_genoux.txt
# UnrealEditor-Cmd.exe Impact.uproject -run=pythonscript -script=D:/UEProjets/Impact/Tools/couper_genoux.py -unattended -nosplash -nullrhi
import unreal, os, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/couper_genoux.txt"
SRC = "/Game/Impact/Anims/Chutes/Chute_Genoux_1_NON_COUPEE"
DEST = "/Game/Impact/Anims/Chutes/Chute_Genoux_1"
FIRST, LAST = 12, 45

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

try:
    if unreal.EditorAssetLibrary.does_asset_exist(DEST):
        unreal.EditorAssetLibrary.delete_asset(DEST)
    if not unreal.EditorAssetLibrary.duplicate_asset(SRC, DEST):
        raise RuntimeError("copie impossible")
    anim = unreal.load_asset(DEST)
    ctrl = anim.get_editor_property("controller")
    bones = list(unreal.AnimationLibrary.get_animation_track_names(anim))
    keys = {}
    for bone in bones:
        pos, rot, sca = [], [], []
        for f in range(FIRST, LAST + 1):
            t = unreal.AnimationLibrary.get_bone_pose_for_frame(anim, bone, f, False)
            pos.append(t.translation)
            rot.append(t.rotation)
            sca.append(t.scale3d)
        keys[bone] = (pos, rot, sca)
    ctrl.set_number_of_frames(unreal.FrameNumber(LAST - FIRST))
    for bone, (pos, rot, sca) in keys.items():
        ctrl.set_bone_track_keys(bone, pos, rot, sca)
    unreal.EditorAssetLibrary.save_asset(DEST, False)
    n = unreal.AnimationLibrary.get_num_frames(anim)
    z = [unreal.AnimationLibrary.get_bone_pose_for_frame(anim, "pelvis", f, False).translation.z for f in range(0, n + 1)]
    say("%d os, %d images, %.2f s, bassin %.0f -> %.0f cm" % (len(bones), n, anim.get_play_length(), z[0], z[-1]))
    say("bassin par image : " + " ".join("%.0f" % v for v in z))
    unreal.EditorAssetLibrary.delete_asset(SRC)
    say("FIN")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")
