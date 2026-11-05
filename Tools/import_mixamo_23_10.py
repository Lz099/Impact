# Les dix-huit animations Mixamo telechargees le 23 octobre (personnage par defaut de Mixamo, sans peau, 30 images/s), dans
# Tools/Mixamo_Chutes/recu_23_10. Meme reciblage qu'import_chutes_22_10.py (descente du bassin recopiee de la source).
#   - les chutes et morts debout vont dans /Game/Impact/Anims/Chutes sous Chute_<Genre>_<n>, le genre lu sur le trajet du bassin
#     (fini haut : Genoux ; sinon vers l'avant, l'arriere ou le cote), n le premier numero libre : l'ancien systeme les prend seul ;
#   - au sol, ramper et se relever vont dans /Game/Impact/Anims/Sol_Mixamo.
# Une prise qui commence couchee ne donne pas l'echelle Mixamo -> ALS par sa premiere image : elle prend celle des prises debout.
# A lancer dans l'editeur complet :
#   UnrealEditor.exe Impact.uproject -ExecutePythonScript=Tools/import_mixamo_23_10.py
# Rapport : Saved/_diag/import_mixamo_23_10.txt
import unreal, os, math, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/import_mixamo_23_10.txt"
SRC_DIR = "D:/UEProjets/Impact/Tools/Mixamo_Chutes/recu_23_10"
MIX_MESH = "/Game/Impact/Mixamo/Ch36_nonPBR"
ALS_MESH = "/ALS/ALS/Character/SKM_Als"
RTG = "/Game/Impact/Retarget/RTG_Mixamo_Als_Chutes"
IMPORT_DIR = "/Game/Impact/Mixamo/Src_23_10"
FALL_DIR = "/Game/Impact/Anims/Chutes"
GROUND_DIR = "/Game/Impact/Anims/Sol_Mixamo"

# (fichier, nom final ou None pour une chute classee d'apres son trajet, couchee au debut)
PLAN = [
    ("Dying Backwards.fbx", None, False),
    ("Falling Forward Death.fbx", None, False),
    ("Falling Back Death.fbx", None, False),
    ("Death From Right.fbx", None, False),
    ("Death From The Front.fbx", None, False),
    ("Death From The Back.fbx", None, False),
    ("Dying Chest Two Knees.fbx", None, False),
    ("Writhing In Pain.fbx", "Sol_Tord_Dos", True),
    ("Laying Moaning.fbx", "Sol_Gemit_Dos", True),
    ("Laying Breathless.fbx", "Sol_Souffle_Dos", True),
    ("Dying On Ground.fbx", "Sol_Meurt_Dos", True),
    ("Prone Death.fbx", "Sol_Meurt_Ventre", True),
    ("Zombie Crawl InPlace.fbx", "Rampe_Bras", True),
    ("Low Crawl InPlace.fbx", "Rampe_Ventre", True),
    ("Crawl Backwards InPlace.fbx", "Rampe_Dos_Recule", True),
    ("Getting Up From Back.fbx", "Releve_Dos", True),
    ("Getting Up From Back Injured.fbx", "Releve_Dos_Blesse", True),
    ("Getting Up From Stomach.fbx", "Releve_Ventre", True),
]

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

def pelvis(anim, frame):
    return unreal.AnimationLibrary.get_bone_pose_for_frame(anim, "pelvis", frame, False).translation

def retarget_with_pelvis(src_asset, dest, mix, als, rtg, k_override=None):
    if unreal.EditorAssetLibrary.does_asset_exist(dest):
        unreal.EditorAssetLibrary.delete_asset(dest)
    data = unreal.EditorAssetLibrary.find_asset_data(src_asset.get_path_name().split(".")[0])
    made = unreal.IKRetargetBatchOperation().duplicate_and_retarget(
        assets_to_retarget=[data], source_mesh=mix, target_mesh=als, ik_retarget_asset=rtg,
        search="", replace="", prefix="", suffix="_rtgc", include_referenced_assets=False, overwrite_existing_files=True)
    if not made:
        return None, 0.0
    pkg = str(made[0].package_name)
    unreal.EditorAssetLibrary.save_asset(pkg, False)
    if not unreal.EditorAssetLibrary.rename_asset(pkg, dest):
        return None, 0.0
    a = unreal.load_asset(dest)
    n = unreal.AnimationLibrary.get_num_frames(a)
    n_src = unreal.AnimationLibrary.get_num_frames(src_asset)
    ref_src = unreal.AnimationLibrary.get_bone_pose_for_frame(src_asset, "hips", 0, False).translation
    ref_dst = unreal.AnimationLibrary.get_bone_pose_for_frame(a, "pelvis", 0, False).translation
    # The retargeter keeps only the pelvis rotations: its path is copied from the source, scaled from Mixamo to ALS.
    k = k_override if k_override else ref_dst.z / max(ref_src.z, 1.0)
    pos, rot, sca = [], [], []
    for f in range(n + 1):
        hs = unreal.AnimationLibrary.get_bone_pose_for_frame(src_asset, "hips", min(f, n_src), False).translation
        pd = unreal.AnimationLibrary.get_bone_pose_for_frame(a, "pelvis", f, False)
        pos.append(unreal.Vector(ref_dst.x + (hs.x - ref_src.x) * k, ref_dst.y + (hs.y - ref_src.y) * k, hs.z * k))
        rot.append(pd.rotation)
        sca.append(unreal.Vector(1.0, 1.0, 1.0))
    a.get_editor_property("controller").set_bone_track_keys("pelvis", pos, rot, sca)
    unreal.EditorAssetLibrary.save_asset(dest, False)
    return a, k

def fall_kind(anim):
    # Where the pelvis ends up relative to where it started, the mesh facing +Y: high at the end is a fall to the knees.
    n = unreal.AnimationLibrary.get_num_frames(anim)
    a, b = pelvis(anim, 0), pelvis(anim, n)
    dx, dy = b.x - a.x, b.y - a.y
    d = math.hypot(dx, dy)
    if b.z > 40.0:
        return "Genoux", dx, dy, b.z
    if d > 25.0 and dy > 0.6 * d:
        return "Avant", dx, dy, b.z
    if d > 25.0 and dy < -0.6 * d:
        return "Arriere", dx, dy, b.z
    return "Cote", dx, dy, b.z

def free_name(kind):
    for i in range(1, 9):
        name = "Chute_%s_%d" % (kind, i)
        if not unreal.EditorAssetLibrary.does_asset_exist("%s/%s" % (FALL_DIR, name)):
            return name
    return None

try:
    mix = unreal.load_asset(MIX_MESH)
    als = unreal.load_asset(ALS_MESH)
    rtg = unreal.load_asset(RTG)
    if mix is None or als is None or rtg is None:
        raise RuntimeError("maillage Mixamo, ALS ou retargeter des chutes absent")
    skel = mix.get_editor_property("skeleton")
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    for d in (IMPORT_DIR, FALL_DIR, GROUND_DIR):
        unreal.EditorAssetLibrary.make_directory(d)
    standing_k = None
    for file_name, final, lying in PLAN:
        path = os.path.join(SRC_DIR, file_name).replace("\\", "/")
        if not os.path.exists(path):
            say("%-34s ABSENT" % file_name)
            continue
        ui = unreal.FbxImportUI()
        ui.set_editor_property("import_mesh", False)
        ui.set_editor_property("import_as_skeletal", True)
        ui.set_editor_property("import_animations", True)
        ui.set_editor_property("skeleton", skel)
        ui.set_editor_property("mesh_type_to_import", unreal.FBXImportType.FBXIT_ANIMATION)
        d = ui.anim_sequence_import_data
        d.set_editor_property("animation_length", unreal.FBXAnimationLengthImportType.FBXALIT_EXPORTED_TIME)
        d.set_editor_property("remove_redundant_keys", False)
        d.set_editor_property("convert_scene", True)
        t = unreal.AssetImportTask()
        t.set_editor_property("filename", path)
        t.set_editor_property("destination_path", IMPORT_DIR)
        t.set_editor_property("destination_name", file_name[:-4].replace(" ", "_"))
        t.set_editor_property("automated", True)
        t.set_editor_property("replace_existing", True)
        t.set_editor_property("save", True)
        t.set_editor_property("options", ui)
        tools.import_asset_tasks([t])
        src = None
        for p in t.get_editor_property("imported_object_paths"):
            a = unreal.load_asset(p.split(".")[0])
            if isinstance(a, unreal.AnimSequence):
                src = a
        if src is None:
            say("%-34s IMPORT RATE" % file_name)
            continue
        if final is None:
            temp = "%s/_Tmp_%s" % (FALL_DIR, file_name[:-4].replace(" ", "_"))
            made, k = retarget_with_pelvis(src, temp, mix, als, rtg, None)
            if made is None:
                say("%-34s RECIBLAGE RATE" % file_name)
                continue
            if standing_k is None:
                standing_k = k
            kind, dx, dy, z_end = fall_kind(made)
            name = free_name(kind)
            if name is None or not unreal.EditorAssetLibrary.rename_asset(temp, "%s/%s" % (FALL_DIR, name)):
                say("%-34s RENOMMAGE RATE (%s)" % (file_name, kind))
                continue
            dest = "%s/%s" % (FALL_DIR, name)
            note = "trajet bassin dx %5.0f dy %5.0f, fini a %3.0f cm" % (dx, dy, z_end)
        else:
            dest = "%s/%s" % (GROUND_DIR, final)
            made, k = retarget_with_pelvis(src, dest, mix, als, rtg, standing_k if lying else None)
            if made is None:
                say("%-34s RECIBLAGE RATE" % file_name)
                continue
            name = final
            note = ""
        a = unreal.load_asset(dest)
        n = unreal.AnimationLibrary.get_num_frames(a)
        say("%-34s -> %-22s %5.2f s  bassin %4.0f -> min %4.0f -> %4.0f cm  echelle %.3f  %s" % (
            file_name, name, a.get_play_length(), pelvis(a, 0).z,
            min(pelvis(a, f).z for f in range(0, n + 1, 2)), pelvis(a, n).z, k, note))
    unreal.EditorAssetLibrary.save_directory(FALL_DIR, False, True)
    unreal.EditorAssetLibrary.save_directory(GROUND_DIR, False, True)
    say("FIN")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")

try:
    unreal.SystemLibrary.quit_editor()
except Exception:
    pass
