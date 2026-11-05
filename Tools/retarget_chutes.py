# Recible des chutes Mixamo sur SKM_Als en GARDANT la descente du bassin.
#
# Le reciblage des reactions aux coups (RTG_Mixamo_Als_v3) ne recopie que les rotations de la chaine
# Root (hips -> pelvis) : le bassin d'une chute restait a 97 cm du debut a la fin, le corps finissait
# allonge en l'air. Mesure : source Mixamo hips 94 -> 10 cm, prise reciblee pelvis 97 -> 97 cm.
# Ce script fait une copie du retargeter, RTG_Mixamo_Als_Chutes, ou la chaine Root recopie aussi la
# translation (mise a l'echelle globale), et l'utilise pour les chutes et les reptations.
#
# Entree : la liste PRISES ci-dessous (assets Mixamo deja importes dans /Game/Impact/Mixamo).
# Sortie : /Game/Impact/Anims/Chutes/<nom final>, et un rapport avec la hauteur du bassin au debut et a la fin.
import unreal, os, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/retarget_chutes.txt"
MIX_MESH = "/Game/Impact/Mixamo/Ch36_nonPBR"
ALS_MESH = "/ALS/ALS/Character/SKM_Als"
RTG_SRC = "/Game/Impact/Retarget/RTG_Mixamo_Als_v3"
RTG = "/Game/Impact/Retarget/RTG_Mixamo_Als_Chutes"
MIX_DIR = "/Game/Impact/Mixamo"
DEST_DIR = "/Game/Impact/Anims/Chutes"
TMP = "_rtgc"

# source dans /Game/Impact/Mixamo -> nom final que le C++ cherche (Chute_<Genre>_<n>, Rampe_<n>)
PRISES = [
    ("Shoulder_Hit_And_Fall", "Chute_Arriere_1"),
    ("Shoulder_Hit_And_Fall__1_", "Chute_Arriere_2"),
]

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

def bassin(anim):
    L = anim.get_play_length()
    z = []
    for t in (0.0, L):
        try:
            z.append(unreal.AnimationLibrary.get_bone_pose_for_time(anim, "pelvis", t, False).translation.z)
        except Exception:
            z.append(float("nan"))
    return z

try:
    if not unreal.EditorAssetLibrary.does_asset_exist(RTG):
        unreal.EditorAssetLibrary.duplicate_asset(RTG_SRC, RTG)
        say("retargeter copie : %s" % RTG)
    rtg = unreal.load_asset(RTG)
    rc = unreal.IKRetargeterController.get_controller(rtg)
    for i in range(rc.get_num_retarget_ops()):
        oc = rc.get_op_controller(i)
        settings = oc.get_settings()
        if settings.__class__.__name__ == "IKRetargetPelvisMotionOpSettings":
            # The op that carries the pelvis translation. Without its two bones it moves nothing, and a fall keeps
            # the pelvis at standing height.
            say("bassin, fonctions : %s" % [m for m in dir(oc) if "pelvis" in m.lower()])
            for getter in ("get_source_pelvis_bone", "get_target_pelvis_bone"):
                if hasattr(oc, getter):
                    say("   avant %s = %s" % (getter, getattr(oc, getter)()))
            if hasattr(oc, "set_source_pelvis_bone"):
                oc.set_source_pelvis_bone("hips")
            if hasattr(oc, "set_target_pelvis_bone"):
                oc.set_target_pelvis_bone("pelvis")
            for getter in ("get_source_pelvis_bone", "get_target_pelvis_bone"):
                if hasattr(oc, getter):
                    say("   apres %s = %s" % (getter, getattr(oc, getter)()))
            for prop in ("source_pelvis_bone", "target_pelvis_bone"):
                try:
                    say("   reglage %s = %s" % (prop, settings.get_editor_property(prop)))
                except Exception as e:
                    say("   reglage %s illisible" % prop)
            continue
        if settings.__class__.__name__ != "IKRetargetFKChainsOpSettings":
            continue
        chains = settings.get_editor_property("chains_to_retarget")
        for chain in chains:
            if str(chain.get_editor_property("target_chain_name")) == "Root":
                # The pelvis belongs to the Pelvis Motion op, which runs first. Left in the FK op, the Root chain wrote the
                # pelvis again after it, without translation, and the fall stayed at standing height.
                chain.set_editor_property("enable_fk", False)
                say("chaine Root : FK -> %s" % chain.get_editor_property("enable_fk"))
        settings.set_editor_property("chains_to_retarget", chains)
        oc.set_settings(settings)
    unreal.EditorAssetLibrary.save_asset(RTG, False)

    mix = unreal.load_asset(MIX_MESH)
    als = unreal.load_asset(ALS_MESH)
    unreal.EditorAssetLibrary.make_directory(DEST_DIR)
    for src, final in PRISES:
        src_path = "%s/%s" % (MIX_DIR, src)
        if not unreal.EditorAssetLibrary.does_asset_exist(src_path):
            say("%-28s SOURCE ABSENTE" % src)
            continue
        dest = "%s/%s" % (DEST_DIR, final)
        if unreal.EditorAssetLibrary.does_asset_exist(dest):
            unreal.EditorAssetLibrary.delete_asset(dest)
        data = unreal.EditorAssetLibrary.find_asset_data(src_path)
        produits = unreal.IKRetargetBatchOperation().duplicate_and_retarget(
            assets_to_retarget=[data], source_mesh=mix, target_mesh=als,
            ik_retarget_asset=rtg, search="", replace="", prefix="", suffix=TMP,
            include_referenced_assets=False, overwrite_existing_files=True)
        if not produits:
            say("%-28s RECIBLAGE SANS SORTIE" % src)
            continue
        for d in produits:
            pkg = str(d.package_name)
            unreal.EditorAssetLibrary.save_asset(pkg, False)
            if not unreal.EditorAssetLibrary.rename_asset(pkg, dest):
                say("%-28s DEPLACEMENT REFUSE vers %s" % (src, dest))
                continue
            unreal.EditorAssetLibrary.save_asset(dest, False)
            a = unreal.load_asset(dest)
            # The pelvis path, copied from the source frame by frame and scaled to this body: the retargeter keeps
            # the rotations but not the translation of the pelvis whatever its settings (tried: Root chain globally
            # scaled, Root chain without FK, pelvis bones set on the Pelvis Motion op), and a fall without its drop
            # ends lying in the air.
            source = unreal.load_asset(src_path)
            n = unreal.AnimationLibrary.get_num_frames(a)
            ref_src = unreal.AnimationLibrary.get_bone_pose_for_frame(source, "hips", 0, False).translation
            ref_dst = unreal.AnimationLibrary.get_bone_pose_for_frame(a, "pelvis", 0, False).translation
            k = ref_dst.z / max(ref_src.z, 1.0)
            pos, rot, sca = [], [], []
            for f in range(n + 1):
                hs = unreal.AnimationLibrary.get_bone_pose_for_frame(source, "hips", min(f, unreal.AnimationLibrary.get_num_frames(source)), False).translation
                pd = unreal.AnimationLibrary.get_bone_pose_for_frame(a, "pelvis", f, False)
                pos.append(unreal.Vector(ref_dst.x + (hs.x - ref_src.x) * k, ref_dst.y + (hs.y - ref_src.y) * k, hs.z * k))
                rot.append(pd.rotation)
                sca.append(unreal.Vector(1.0, 1.0, 1.0))
            ctrl = a.get_editor_property("controller")
            ctrl.set_bone_track_keys("pelvis", pos, rot, sca)
            unreal.EditorAssetLibrary.save_asset(dest, False)
            say("   bassin recopie de la source : %d images, echelle %.2f" % (n + 1, k))
            z0, z1 = bassin(a)
            say("%-28s -> %-18s %5.2f s  bassin %4.0f -> %4.0f cm%s" % (src, final, a.get_play_length(), z0, z1,
                "" if z0 - z1 > 40 else "   PAS DE DESCENTE"))
    unreal.EditorAssetLibrary.save_directory(DEST_DIR, False, True)
    say("FIN")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")

# Le reciblage par lot a besoin de Slate : il plante en commandlet (-run=pythonscript, assertion
# CurrentApplication.IsValid()). Le script est donc lance dans l'editeur complet
# (UnrealEditor.exe -ExecutePythonScript=...), et ferme l'editeur lui-meme a la fin.
try:
    unreal.SystemLibrary.quit_editor()
except Exception:
    pass
