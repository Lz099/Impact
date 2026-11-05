# Les relevers Mixamo emmenent le bassin a 30-80 cm de son depart (mesure_sol_mixamo.txt). Joues tels quels, l'acteur reste ou il
# etait et, a la fin, la marche d'ALS remet le corps sur lui : il se teleporte ("il reprend la pose d'avant"). Le trajet horizontal
# du bassin passe ici a la racine (mouvement de racine) : c'est alors l'acteur qui avance avec lui. Rapporte aussi de combien le
# bassin a tourne a la fin par rapport a une pose debout (une prise qui finit de biais ferait pivoter le corps a la fin).
#   UnrealEditor.exe Impact.uproject -ExecutePythonScript=Tools/releve_mixamo_racine.py
# Rapport : Saved/_diag/releve_mixamo_racine.txt
import unreal, os, math, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/releve_mixamo_racine.txt"
DIR = "/Game/Impact/Anims/Sol_Mixamo"
NAMES = ["Releve_Dos", "Releve_Dos_Blesse", "Releve_Ventre"]
STANDING = "/Game/Impact/Anims/Chutes/Chute_Avant_3"

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

def pose(a, bone, f):
    return unreal.AnimationLibrary.get_bone_pose_for_frame(a, bone, f, False)

def yaw_of(q):
    # The pelvis's forward (+Y of the mesh in the standing pose) carried by q, flattened.
    v = q.rotate_vector(unreal.Vector(0.0, 1.0, 0.0))
    return math.degrees(math.atan2(v.y, v.x))

try:
    stand = unreal.load_asset(STANDING)
    q_stand = pose(stand, "pelvis", 0).rotation
    for name in NAMES:
        path = "%s/%s" % (DIR, name)
        a = unreal.load_asset(path)
        n = unreal.AnimationLibrary.get_num_frames(a)
        ctrl = a.get_editor_property("controller")
        pel = [pose(a, "pelvis", f) for f in range(n + 1)]
        root = [pose(a, "root", f) for f in range(n + 1)]
        x0, y0 = pel[0].translation.x, pel[0].translation.y
        rpos, rrot, rsca, ppos, prot, psca = [], [], [], [], [], []
        for f in range(n + 1):
            t = pel[f].translation
            rpos.append(unreal.Vector(t.x - x0, t.y - y0, 0.0))
            rrot.append(root[f].rotation)
            rsca.append(unreal.Vector(1.0, 1.0, 1.0))
            ppos.append(unreal.Vector(x0, y0, t.z))
            prot.append(pel[f].rotation)
            psca.append(unreal.Vector(1.0, 1.0, 1.0))
        ctrl.set_bone_track_keys("root", rpos, rrot, rsca)
        ctrl.set_bone_track_keys("pelvis", ppos, prot, psca)
        a.set_editor_property("enable_root_motion", True)
        a.set_editor_property("root_motion_root_lock", unreal.RootMotionRootLock.ANIM_FIRST_FRAME)
        unreal.EditorAssetLibrary.save_asset(path, False)
        # How the standing end is turned compared to a standing pose: the root carries no turn.
        q_end = pel[n].rotation
        turn = yaw_of(q_end) - yaw_of(q_stand)
        turn = (turn + 180.0) % 360.0 - 180.0
        say("%-18s racine %4.0f %4.0f cm sur %.2f s, bassin debout tourne de %4.0f deg a la fin" % (
            name, rpos[-1].x, rpos[-1].y, a.get_play_length(), turn))
    say("FIN")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")

try:
    unreal.SystemLibrary.quit_editor()
except Exception:
    pass
