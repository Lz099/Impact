# De quel cote chaque prise Sol_Mixamo est couchee a sa premiere image : la face avant de la poitrine, calculee comme
# UImpactReactionComponent::UpdateCrawl la lit sur le corps (poitrine vers le haut = sur le dos), dans l'espace du maillage.
#   UnrealEditor.exe Impact.uproject -ExecutePythonScript=Tools/mesure_face_sol_mixamo.py
# Rapport : Saved/_diag/mesure_face_sol_mixamo.txt
import unreal, os, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/mesure_face_sol_mixamo.txt"
DIR = "/Game/Impact/Anims/Sol_Mixamo"
NAMES = ["Sol_Tord_Dos", "Sol_Gemit_Dos", "Sol_Souffle_Dos", "Sol_Meurt_Dos", "Sol_Meurt_Ventre", "Rampe_Bras", "Rampe_Ventre",
         "Rampe_Dos_Recule", "Releve_Dos", "Releve_Dos_Blesse", "Releve_Ventre"]
CHAINS = {
    "pelvis": ["root", "pelvis"],
    "spine_03": ["root", "pelvis", "spine_01", "spine_02", "spine_03"],
    "upperarm_l": ["root", "pelvis", "spine_01", "spine_02", "spine_03", "clavicle_l", "upperarm_l"],
    "upperarm_r": ["root", "pelvis", "spine_01", "spine_02", "spine_03", "clavicle_r", "upperarm_r"],
    "head": ["root", "pelvis", "spine_01", "spine_02", "spine_03", "neck_01", "head"],
}

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

def where(a, bone, f):
    t = unreal.Transform()
    for b in CHAINS[bone]:
        local = unreal.AnimationLibrary.get_bone_pose_for_frame(a, b, f, False)
        t = local.multiply(t)
    return t.translation

def norm(v):
    l = (v.x * v.x + v.y * v.y + v.z * v.z) ** 0.5
    return unreal.Vector(v.x / l, v.y / l, v.z / l) if l > 1e-6 else v

try:
    for name in NAMES:
        a = unreal.load_asset("%s/%s" % (DIR, name))
        for f in (0,):
            p, c, l, r, h = (where(a, b, f) for b in ("pelvis", "spine_03", "upperarm_l", "upperarm_r", "head"))
            chest = norm(c - p)
            across = norm(r - l)
            front = chest.cross(across) * -1.0
            say("%-18s image %d : poitrine vers %s (front.z %+.2f), tete a (%4.0f %4.0f %4.0f) du bassin" % (
                name, f, "le haut (sur le dos)" if front.z > 0.25 else "le bas (sur le ventre)" if front.z < -0.3 else "le cote",
                front.z, h.x - p.x, h.y - p.y, h.z - p.z))
    say("FIN")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")

try:
    unreal.SystemLibrary.quit_editor()
except Exception:
    pass
