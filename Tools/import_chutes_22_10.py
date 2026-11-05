# Les sept animations Mixamo recues le 22 octobre au soir, triees a la main d'apres leur analyse dans Blender
# (trajet du bassin, sens de la chute, face finale) plutot que par le classement automatique d'import_chutes.py :
#   Dying              tombe en avant, finit sur le ventre (1,5 m, 4,3 s)   -> Chute_Avant_1
#   Dying(1)           tombe en avant, finit sur le ventre (1,0 m, 2,6 s)   -> Chute_Avant_2
#   Rifle Hit To Back  renverse en arriere, finit sur le dos (1,35 m)       -> Chute_Arriere_3
#   Fall Over          tombe de cote et en arriere, finit sur le dos        -> Chute_Cote_1
#   Knocked Down       projete de cote, tourne, finit sur le ventre         -> Chute_Cote_2
#   Praying            0,4 a 1,5 s seulement : tombe sur les deux genoux    -> Chute_Genoux_1 (la priere est coupee)
#   Prone Roll         roule sur le cote a plat ventre, pas branche          -> Roule_Ventre_1
# Meme reciblage qu'import_chutes.py (descente du bassin recopiee de la source). A lancer dans l'editeur complet :
#   UnrealEditor.exe Impact.uproject -ExecutePythonScript=Tools/import_chutes_22_10.py
# Rapport : Saved/_diag/import_chutes_22_10.txt
import unreal, os, math, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/import_chutes_22_10.txt"
SRC_DIR = "D:/UEProjets/Impact/Tools/Mixamo_Chutes/recu_22_10"
MIX_MESH = "/Game/Impact/Mixamo/Ch36_nonPBR"
ALS_MESH = "/ALS/ALS/Character/SKM_Als"
RTG = "/Game/Impact/Retarget/RTG_Mixamo_Als_Chutes"
IMPORT_DIR = "/Game/Impact/Mixamo/Chutes_src_22_10"
DEST_DIR = "/Game/Impact/Anims/Chutes"

PLAN = [
    ("Dying.fbx", "Chute_Avant_1", None),
    ("Dying(1).fbx", "Chute_Avant_2", None),
    ("Rifle Hit To Back.fbx", "Chute_Arriere_3", None),
    ("Fall Over.fbx", "Chute_Cote_1", None),
    ("Knocked Down.fbx", "Chute_Cote_2", None),
    ("Praying.fbx", "Chute_Genoux_1", (12, 45)),  # frames kept, 30 per second
    ("Prone Roll.fbx", "Roule_Ventre_1", None),
]

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

def pelvis_z(anim, bone, frame):
    return unreal.AnimationLibrary.get_bone_pose_for_frame(anim, bone, frame, False).translation.z

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
    # The retargeter keeps only the pelvis rotations: its path is copied from the source, scaled from Mixamo to ALS. A take
    # that starts lying down (the roll) cannot give the scale from its first frame: it is given from the standing ones.
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

def crop(anim, first, last):
    # Keeps frames [first, last): the end is removed first, so the start does not move under it.
    ctrl = anim.get_editor_property("controller")
    n = unreal.AnimationLibrary.get_num_frames(anim)
    if last < n:
        ctrl.remove_number_of_frames(unreal.FrameNumber(last), unreal.FrameNumber(n - last))
    if first > 0:
        ctrl.remove_number_of_frames(unreal.FrameNumber(0), unreal.FrameNumber(first))

try:
    mix = unreal.load_asset(MIX_MESH)
    als = unreal.load_asset(ALS_MESH)
    rtg = unreal.load_asset(RTG)
    if mix is None or als is None or rtg is None:
        raise RuntimeError("maillage Mixamo, ALS ou retargeter des chutes absent")
    skel = mix.get_editor_property("skeleton")
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    unreal.EditorAssetLibrary.make_directory(IMPORT_DIR)
    unreal.EditorAssetLibrary.make_directory(DEST_DIR)
    standing_k = None
    for file_name, final, keep in PLAN:
        path = os.path.join(SRC_DIR, file_name).replace("\\", "/")
        if not os.path.exists(path):
            say("%-24s ABSENT" % file_name)
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
            say("%-24s IMPORT RATE" % file_name)
            continue
        lying = final.startswith("Roule")
        made, k = retarget_with_pelvis(src, "%s/%s" % (DEST_DIR, final), mix, als, rtg, standing_k if lying else None)
        if made is None:
            say("%-24s RECIBLAGE RATE" % file_name)
            continue
        if not lying and standing_k is None:
            standing_k = k
        note = ""
        if keep:
            try:
                crop(made, keep[0], keep[1])
                unreal.EditorAssetLibrary.save_asset("%s/%s" % (DEST_DIR, final), False)
                note = "  coupee aux images %d-%d" % keep
            except Exception as e:
                # Uncut, the prayer would play for five seconds after the knees: kept out of the falls.
                unreal.EditorAssetLibrary.rename_asset("%s/%s" % (DEST_DIR, final), "%s/%s_NON_COUPEE" % (DEST_DIR, final))
                note = "  COUPE RATEE (%s), renommee _NON_COUPEE" % str(e)[:120]
        n = unreal.AnimationLibrary.get_num_frames(made)
        say("%-24s -> %-16s %5.2f s  bassin %4.0f -> min %4.0f -> %4.0f cm  echelle %.3f%s" % (
            file_name, final, made.get_play_length(), pelvis_z(made, "pelvis", 0),
            min(pelvis_z(made, "pelvis", f) for f in range(0, n + 1, 2)), pelvis_z(made, "pelvis", n), k, note))
    unreal.EditorAssetLibrary.save_directory(DEST_DIR, False, True)
    say("FIN")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")

try:
    unreal.SystemLibrary.quit_editor()
except Exception:
    pass
