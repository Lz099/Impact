# -ImpactAttitudeGang : les prises d'attitude Mixamo (personnage de Mixamo, sans peau, 30 images/s) rangees par role dans
# Tools/Mixamo_Gang sous Gang_<Role>_<n>.fbx (Repos, Geste, Provoc, Crie, Fete, Recharge, Course), importees puis reciblees
# sur le squelette d'ALS dans /Game/Impact/Anims/Gang sous le meme nom : UImpactGangAttitudeComponent les trouve par ce nom.
# Meme chaine que Tools/import_mixamo_23_10_couche.py (RTG_Mixamo_Als_Chutes, trajet du bassin recopie de la source).
# Le rapport donne pour chaque prise sa duree et l'angle de la ligne des epaules par rapport a une pose d'ALS : une prise qui
# regarde ailleurs que devant ferait tourner le buste.
# A lancer dans l'editeur complet (l'import FBX et le reciblage exigent Slate) :
#   UnrealEditor.exe Impact.uproject -ExecutePythonScript=D:/UEProjets/Impact/Tools/import_gang.py -RenderOffScreen
# Rapport : Saved/_diag/import_gang.txt
import unreal, os, math, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/import_gang.txt"
SRC_DIR = "D:/UEProjets/Impact/Tools/Mixamo_Gang"
MIX_MESH = "/Game/Impact/Mixamo/Ch36_nonPBR"
ALS_MESH = "/ALS/ALS/Character/SKM_Als"
RTG = "/Game/Impact/Retarget/RTG_Mixamo_Als_Chutes"
IMPORT_DIR = "/Game/Impact/Mixamo/Src_Gang"
GANG_DIR = "/Game/Impact/Anims/Gang"
ALS_REFERENCE = "/ALS/ALS/Animations/Grounded/WalkRun/A_Als_Walk_Forward_Pose"
CHAIN = ["root", "pelvis", "spine_01", "spine_02", "spine_03"]

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

def local(anim, bone, frame):
    return unreal.AnimationLibrary.get_bone_pose_for_frame(anim, bone, frame, False)

def mesh_space(anim, bones, frame):
    # Composes the local transforms down a chain (child * parent in Unreal's order).
    t = None
    for b in bones:
        l = local(anim, b, frame)
        t = l if t is None else l * t
    return t

def shoulder_yaw(anim, frame):
    # Yaw of the line from the left to the right clavicle, in mesh space.
    chest = mesh_space(anim, CHAIN, frame)
    l = local(anim, "clavicle_l", frame) * chest
    r = local(anim, "clavicle_r", frame) * chest
    d = r.translation - l.translation
    return math.degrees(math.atan2(d.y, d.x))

def retarget_with_pelvis(src_asset, dest, mix, als, rtg, k):
    if unreal.EditorAssetLibrary.does_asset_exist(dest):
        unreal.EditorAssetLibrary.delete_asset(dest)
    data = unreal.EditorAssetLibrary.find_asset_data(src_asset.get_path_name().split(".")[0])
    made = unreal.IKRetargetBatchOperation().duplicate_and_retarget(
        assets_to_retarget=[data], source_mesh=mix, target_mesh=als, ik_retarget_asset=rtg,
        search="", replace="", prefix="", suffix="_rtgg", include_referenced_assets=False, overwrite_existing_files=True)
    if not made:
        return None
    pkg = str(made[0].package_name)
    unreal.EditorAssetLibrary.save_asset(pkg, False)
    if not unreal.EditorAssetLibrary.rename_asset(pkg, dest):
        return None
    a = unreal.load_asset(dest)
    n = unreal.AnimationLibrary.get_num_frames(a)
    n_src = unreal.AnimationLibrary.get_num_frames(src_asset)
    ref_src = local(src_asset, "hips", 0).translation
    ref_dst = local(a, "pelvis", 0).translation
    # The retargeter keeps only the pelvis rotations: its path is copied from the source, scaled from Mixamo to ALS.
    pos, rot, sca = [], [], []
    for f in range(n + 1):
        hs = local(src_asset, "hips", min(f, n_src)).translation
        pd = local(a, "pelvis", f)
        pos.append(unreal.Vector(ref_dst.x + (hs.x - ref_src.x) * k, ref_dst.y + (hs.y - ref_src.y) * k, hs.z * k))
        rot.append(pd.rotation)
        sca.append(unreal.Vector(1.0, 1.0, 1.0))
    a.get_editor_property("controller").set_bone_track_keys("pelvis", pos, rot, sca)
    unreal.EditorAssetLibrary.save_asset(dest, False)
    return a

try:
    mix = unreal.load_asset(MIX_MESH)
    als = unreal.load_asset(ALS_MESH)
    rtg = unreal.load_asset(RTG)
    reference = unreal.load_asset(ALS_REFERENCE)
    if mix is None or als is None or rtg is None:
        raise RuntimeError("maillage Mixamo, ALS ou recibleur absent")
    skel = mix.get_editor_property("skeleton")
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    for d in (IMPORT_DIR, GANG_DIR):
        unreal.EditorAssetLibrary.make_directory(d)
    als_yaw = shoulder_yaw(reference, 0) if reference is not None else None
    say("pose ALS de reference : epaules a %s deg" % ("%.1f" % als_yaw if als_yaw is not None else "?"))
    files = sorted(f for f in os.listdir(SRC_DIR) if f.lower().endswith(".fbx")) if os.path.isdir(SRC_DIR) else []
    if not files:
        say("aucun FBX dans %s" % SRC_DIR)
    for file_name in files:
        name = file_name[:-4]
        path = os.path.join(SRC_DIR, file_name).replace("\\", "/")
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
        t.set_editor_property("destination_name", name + "_src")
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
            say("%-22s IMPORT RATE" % name)
            continue
        dest = "%s/%s" % (GANG_DIR, name)
        # Standing takes: the Mixamo -> ALS scale of the standing takes of 23 October.
        made = retarget_with_pelvis(src, dest, mix, als, rtg, 0.955)
        if made is None:
            say("%-22s RECIBLAGE RATE" % name)
            continue
        n = unreal.AnimationLibrary.get_num_frames(made)
        yaw0 = shoulder_yaw(made, 0)
        yaw_mid = shoulder_yaw(made, n // 2)
        off = (yaw0 - als_yaw + 180.0) % 360.0 - 180.0 if als_yaw is not None else 0.0
        say("%-22s %5.2f s  %3d images  epaules %6.1f deg (ecart a ALS %6.1f, milieu %6.1f)  bassin %4.0f cm" % (
            name, made.get_play_length(), n, yaw0, off, yaw_mid, local(made, "pelvis", 0).translation.z))
    unreal.EditorAssetLibrary.save_directory(GANG_DIR, True, True)
    say("FIN")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")

try:
    unreal.SystemLibrary.quit_editor()
except Exception:
    pass
