# Importe les chutes et reptations Mixamo deposees dans Tools/Mixamo_Chutes, les classe, les recible sur
# SKM_Als en gardant la descente du bassin, et les range sous les noms que le C++ cherche :
#   /Game/Impact/Anims/Chutes/Chute_<Genre>_<n>   Genre = Avant, Arriere, Genoux, Cote
#   /Game/Impact/Anims/Chutes/Rampe_<n>
# Rapport : Saved/_diag/import_chutes.txt. A lancer dans l'editeur complet (le reciblage par lot a besoin de
# Slate) : UnrealEditor.exe Impact.uproject -ExecutePythonScript=Tools/import_chutes.py
#
# Le classement se fait sur le trajet du bassin dans la source, pas sur le nom (\"Death From Front\" veut dire
# touche de face, donc tombe en ARRIERE) : la chute de reference Shoulder_Hit_And_Fall, qui part en arriere,
# donne le sens \"arriere\" ; un bassin qui se deplace peu est une chute sur place (a genoux). Le nom ne sert
# que pour \"crawl\" (reptation) et \"knee\"/\"kneel\" (genoux), sans ambiguite.
import unreal, os, glob, math, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/import_chutes.txt"
SRC_DIR = "D:/UEProjets/Impact/Tools/Mixamo_Chutes"
MIX_MESH = "/Game/Impact/Mixamo/Ch36_nonPBR"
ALS_MESH = "/ALS/ALS/Character/SKM_Als"
RTG = "/Game/Impact/Retarget/RTG_Mixamo_Als_Chutes"
IMPORT_DIR = "/Game/Impact/Mixamo/Chutes_src"
DEST_DIR = "/Game/Impact/Anims/Chutes"
REFERENCE = "/Game/Impact/Mixamo/Shoulder_Hit_And_Fall"

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

def hips_shift(anim):
    a = unreal.AnimationLibrary.get_bone_pose_for_time(anim, "hips", 0.0, False).translation
    b = unreal.AnimationLibrary.get_bone_pose_for_time(anim, "hips", anim.get_play_length(), False).translation
    return b.x - a.x, b.y - a.y, a.z, b.z

def retarget_with_pelvis(src_asset, dest, mix, als, rtg, in_place=False):
    if unreal.EditorAssetLibrary.does_asset_exist(dest):
        unreal.EditorAssetLibrary.delete_asset(dest)
    data = unreal.EditorAssetLibrary.find_asset_data(src_asset.get_path_name().split(".")[0])
    made = unreal.IKRetargetBatchOperation().duplicate_and_retarget(
        assets_to_retarget=[data], source_mesh=mix, target_mesh=als, ik_retarget_asset=rtg,
        search="", replace="", prefix="", suffix="_rtgc", include_referenced_assets=False, overwrite_existing_files=True)
    if not made:
        return None
    pkg = str(made[0].package_name)
    unreal.EditorAssetLibrary.save_asset(pkg, False)
    if not unreal.EditorAssetLibrary.rename_asset(pkg, dest):
        return None
    a = unreal.load_asset(dest)
    # Pelvis path copied from the source and scaled: the retargeter keeps only its rotations (see retarget_chutes.py).
    n = unreal.AnimationLibrary.get_num_frames(a)
    n_src = unreal.AnimationLibrary.get_num_frames(src_asset)
    ref_src = unreal.AnimationLibrary.get_bone_pose_for_frame(src_asset, "hips", 0, False).translation
    ref_dst = unreal.AnimationLibrary.get_bone_pose_for_frame(a, "pelvis", 0, False).translation
    k = ref_dst.z / max(ref_src.z, 1.0)
    end = unreal.AnimationLibrary.get_bone_pose_for_frame(src_asset, "hips", n_src, False).translation
    drift_x = (end.x - ref_src.x) * k
    drift_y = (end.y - ref_src.y) * k
    pos, rot, sca = [], [], []
    for f in range(n + 1):
        hs = unreal.AnimationLibrary.get_bone_pose_for_frame(src_asset, "hips", min(f, n_src), False).translation
        pd = unreal.AnimationLibrary.get_bone_pose_for_frame(a, "pelvis", f, False)
        x = ref_dst.x + (hs.x - ref_src.x) * k
        y = ref_dst.y + (hs.y - ref_src.y) * k
        if in_place:
            # A crawl loop advances: taken out of the pelvis here (or the body would jump back at every loop) and
            # handed to the code as a speed, which moves the whole man instead.
            x -= drift_x * f / max(n, 1)
            y -= drift_y * f / max(n, 1)
        pos.append(unreal.Vector(x, y, hs.z * k))
        rot.append(pd.rotation)
        sca.append(unreal.Vector(1.0, 1.0, 1.0))
    a.get_editor_property("controller").set_bone_track_keys("pelvis", pos, rot, sca)
    if in_place:
        speed = math.hypot(drift_x, drift_y) / max(a.get_play_length(), 0.01)
        unreal.AnimationLibrary.add_curve(a, "RampeVitesse", unreal.RawCurveTrackTypes.RCT_FLOAT, False)
        unreal.AnimationLibrary.add_float_curve_key(a, "RampeVitesse", 0.0, speed)
        unreal.AnimationLibrary.add_float_curve_key(a, "RampeVitesse", a.get_play_length(), speed)
        say("   reptation mise sur place, vitesse %.0f cm/s" % speed)
    unreal.EditorAssetLibrary.save_asset(dest, False)
    return a

try:
    files = sorted(glob.glob(os.path.join(SRC_DIR, "*.fbx")))
    say("%d fichiers FBX dans %s" % (len(files), SRC_DIR))
    mix = unreal.load_asset(MIX_MESH)
    als = unreal.load_asset(ALS_MESH)
    rtg = unreal.load_asset(RTG)
    if mix is None or als is None or rtg is None:
        raise RuntimeError("maillage Mixamo, ALS ou retargeter des chutes absent (lancer d'abord retarget_chutes.py)")
    skel = mix.get_editor_property("skeleton")
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    unreal.EditorAssetLibrary.make_directory(IMPORT_DIR)
    unreal.EditorAssetLibrary.make_directory(DEST_DIR)

    # The backward direction, from the reference fall.
    ref = unreal.load_asset(REFERENCE)
    rx, ry, _, _ = hips_shift(ref)
    rn = math.hypot(rx, ry) or 1.0
    back = (rx / rn, ry / rn)
    say("sens arriere de reference : (%.2f, %.2f)" % back)

    imported = []
    for path in files:
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
        t.set_editor_property("filename", path.replace("\\", "/"))
        t.set_editor_property("destination_path", IMPORT_DIR)
        t.set_editor_property("automated", True)
        t.set_editor_property("replace_existing", True)
        t.set_editor_property("save", True)
        t.set_editor_property("options", ui)
        tools.import_asset_tasks([t])
        for p in t.get_editor_property("imported_object_paths"):
            a = unreal.load_asset(p.split(".")[0])
            if isinstance(a, unreal.AnimSequence):
                imported.append((os.path.basename(path), a))

    counts = {}
    for name, a in imported:
        low = name.lower()
        dx, dy, z0, z1 = hips_shift(a)
        dist = math.hypot(dx, dy)
        if "crawl" in low:
            kind = "Rampe"
        elif "kneel" in low or "knee" in low:
            kind = "Genoux"
        elif dist < 25.0:
            kind = "Genoux"
        else:
            c = (dx * back[0] + dy * back[1]) / dist
            kind = "Arriere" if c > 0.5 else "Avant" if c < -0.5 else "Cote"
        counts[kind] = counts.get(kind, 0) + 1
        final = "Rampe_%d" % counts[kind] if kind == "Rampe" else "Chute_%s_%d" % (kind, counts[kind])
        # The two shoulder-hit falls already hold Chute_Arriere_1 and _2.
        if kind == "Arriere":
            final = "Chute_Arriere_%d" % (counts[kind] + 2)
        made = retarget_with_pelvis(a, "%s/%s" % (DEST_DIR, final), mix, als, rtg, in_place=(kind == "Rampe"))
        if made is None:
            say("%-40s RECIBLAGE RATE" % name)
            continue
        p0 = unreal.AnimationLibrary.get_bone_pose_for_time(made, "pelvis", 0.0, False).translation.z
        p1 = unreal.AnimationLibrary.get_bone_pose_for_time(made, "pelvis", made.get_play_length(), False).translation.z
        say("%-40s -> %-18s %5.2f s  deplacement bassin %4.0f cm  bassin %4.0f -> %4.0f cm" % (name, final, made.get_play_length(), dist, p0, p1))
    unreal.EditorAssetLibrary.save_directory(DEST_DIR, False, True)
    say("FIN : %s" % counts)
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")

try:
    unreal.SystemLibrary.quit_editor()
except Exception:
    pass
