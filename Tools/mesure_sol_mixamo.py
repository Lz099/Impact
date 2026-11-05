# Mesures des prises Sol_Mixamo avant de les brancher : la couture de boucle (ecart de pose entre la derniere et la premiere
# image : grande, la boucle saute - "il va super vite au debut de l'anim"), le trajet du bassin (un relever qui finit a 60 cm de
# son depart, joue sans mouvement de racine, se teleporte a la fin - "il reprend la pose d'avant") et le cote vers le haut.
#   UnrealEditor.exe Impact.uproject -ExecutePythonScript=Tools/mesure_sol_mixamo.py
# Rapport : Saved/_diag/mesure_sol_mixamo.txt
import unreal, os, math, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/mesure_sol_mixamo.txt"
DIR = "/Game/Impact/Anims/Sol_Mixamo"
NAMES = ["Sol_Tord_Dos", "Sol_Gemit_Dos", "Sol_Souffle_Dos", "Sol_Meurt_Dos", "Sol_Meurt_Ventre", "Rampe_Bras", "Rampe_Ventre",
         "Rampe_Dos_Recule", "Releve_Dos", "Releve_Dos_Blesse", "Releve_Ventre"]
BONES = ["pelvis", "spine_01", "spine_02", "spine_03", "neck_01", "head", "clavicle_l", "upperarm_l", "lowerarm_l", "clavicle_r",
         "upperarm_r", "lowerarm_r", "thigh_l", "calf_l", "thigh_r", "calf_r"]

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

def pose(a, bone, f):
    return unreal.AnimationLibrary.get_bone_pose_for_frame(a, bone, f, False)

def angle(q1, q2):
    d = abs(q1.x * q2.x + q1.y * q2.y + q1.z * q2.z + q1.w * q2.w)
    return math.degrees(2.0 * math.acos(min(1.0, d)))

try:
    for name in NAMES:
        a = unreal.load_asset("%s/%s" % (DIR, name))
        if a is None:
            say("%-18s ABSENT" % name)
            continue
        n = unreal.AnimationLibrary.get_num_frames(a)
        seam = max(angle(pose(a, b, 0).rotation, pose(a, b, n).rotation) for b in BONES)
        worst = max(BONES, key=lambda b: angle(pose(a, b, 0).rotation, pose(a, b, n).rotation))
        p0, p1 = pose(a, "pelvis", 0).translation, pose(a, "pelvis", n).translation
        # The pelvis's own up axis in mesh space at the start: lying on the back the belly faces up.
        q = pose(a, "pelvis", 0).rotation
        say("%-18s %5.2f s %4d im  couture %5.1f deg (%s)  bassin (%4.0f %4.0f %4.0f) -> (%4.0f %4.0f %4.0f)  deplacement %4.0f cm" % (
            name, a.get_play_length(), n, seam, worst, p0.x, p0.y, p0.z, p1.x, p1.y, p1.z, math.hypot(p1.x - p0.x, p1.y - p0.y)))
    say("FIN")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")

try:
    unreal.SystemLibrary.quit_editor()
except Exception:
    pass
