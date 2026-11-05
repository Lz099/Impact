# Recible les quatorze prises Mixamo sur le squelette SK_Als, en place, sous les noms que la table
# de ImpactReactionComponent.cpp attend.
#
# A lancer APRES Tools/retarget_setup.py, qui construit RTG_Mixamo_Als_v3 et verifie que ses
# chaines sont bien associees. Sans cela le reciblage sort une pose figee sans rien dire.
#
# Trois pieges payes ici :
#  - duplicate_and_retarget veut des AssetData, pas des objets, et ses arguments positionnels se
#    decalent : ils sont passes en mots-cles.
#  - l'asset produit atterrit a cote de la source et n'est pas enregistre par save_directory :
#    il est enregistre explicitement avant d'etre deplace.
#  - un suffixe temporaire (_rtg) evite que le nom du produit heurte celui de sa propre source
#    dans le meme dossier.
import unreal, os, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/retarget_batch.txt"
MIX_MESH = "/Game/Impact/Mixamo/Ch36_nonPBR"
ALS_MESH = "/ALS/ALS/Character/SKM_Als"
RTG      = "/Game/Impact/Retarget/RTG_Mixamo_Als_v3"
MIX_DIR  = "/Game/Impact/Mixamo"
DEST_DIR = "/Game/Impact/Anims"
TMP      = "_rtg"

# source dans /Game/Impact/Mixamo  ->  nom final attendu par le C++
PRISES = [
    ("Big_Hit_To_Head",            "Big_Hit_To_Head"),
    ("Big_Hit_To_Head__1_",        "Big_Hit_To_Head_1"),
    ("Big_Hit_To_Head__2_",        "Big_Hit_To_Head_2"),
    ("Big_Stomach_Hit",            "Big_Stomach_Hit"),
    ("Head_Hit",                   "Head_Hit"),
    ("Hit_On_Side_Of_Body",        "Hit_On_Side_Of_Body"),
    ("Hit_On_Side_Of_Head",        "Hit_On_Side_Of_Head"),
    ("Hit_To_Body",                "Hit_To_Body"),
    ("Hit_To_Body__1_",            "Hit_To_Body_1"),
    ("Rib_Hit",                    "Rib_Hit"),
    ("Shoulder_Hit_And_Fall",      "Shoulder_Hit_And_Fall"),
    ("Shoulder_Hit_And_Fall__1_",  "Shoulder_Hit_And_Fall_1"),
    ("Stomach_Hit",                "Stomach_Hit"),
    ("Writhing_In_Pain",           "Douleur_Au_Sol"),
]

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

def ecart_colonne(anim):
    # Combien de degres la colonne parcourt du debut a la fin. Un reciblage rate rend 0.
    L = anim.get_play_length()
    pire = 0.0
    for bone in ("spine_01", "spine_02", "spine_03"):
        try:
            base = unreal.AnimationLibrary.get_bone_pose_for_time(anim, bone, 0.0, False).rotation
        except Exception:
            continue
        for i in range(1, 7):
            try:
                q = unreal.AnimationLibrary.get_bone_pose_for_time(anim, bone, L * i / 6.0, False).rotation
            except Exception:
                continue
            d = abs(unreal.MathLibrary.quat_angular_distance(base, q)) * 180.0 / 3.14159265
            if d > pire:
                pire = d
    return pire

try:
    mix = unreal.load_asset(MIX_MESH)
    als = unreal.load_asset(ALS_MESH)
    rtg = unreal.load_asset(RTG)
    if mix is None or als is None or rtg is None:
        raise RuntimeError("maillage ou retargeter absent")

    # Garde-fou : refuser de travailler avec un retargeter dont les chaines ne sont pas associees.
    rc = unreal.IKRetargeterController.get_controller(rtg)
    liees = 0
    for i in range(rc.get_num_retarget_ops()):
        try:
            liees += len(rc.get_op_controller(i).get_settings().get_editor_property("chains_to_retarget"))
        except Exception:
            pass
    say("%s : %d chaines associees" % (rtg.get_name(), liees))
    if liees == 0:
        raise RuntimeError("retargeter sans chaines : lance d'abord Tools/retarget_setup.py")

    # Premiere passe : vider les destinations. Supprimer et renommer sur le MEME chemin dans la
    # meme boucle echoue : la suppression laisse le paquet en memoire et rename_asset refuse en
    # rendant False, sans erreur. Les destinations sont donc toutes liberees d'abord, et celle qui
    # resiste est mise de cote au lieu d'etre supprimee.
    for src, final in PRISES:
        dest = "%s/%s" % (DEST_DIR, final)
        if not unreal.EditorAssetLibrary.does_asset_exist(dest):
            continue
        if unreal.EditorAssetLibrary.delete_asset(dest):
            continue
        for i in range(1, 30):
            aside = "%s/_mortes/%s_%d" % (DEST_DIR, final, i)
            if not unreal.EditorAssetLibrary.does_asset_exist(aside):
                say("%-26s suppression refusee, mise de cote dans _mortes : %s"
                    % (final, unreal.EditorAssetLibrary.rename_asset(dest, aside)))
                break

    ok = 0
    for src, final in PRISES:
        src_path = "%s/%s" % (MIX_DIR, src)
        if not unreal.EditorAssetLibrary.does_asset_exist(src_path):
            say("%-26s SOURCE ABSENTE" % src)
            continue
        data = unreal.EditorAssetLibrary.find_asset_data(src_path)
        produits = unreal.IKRetargetBatchOperation().duplicate_and_retarget(
            assets_to_retarget=[data], source_mesh=mix, target_mesh=als,
            ik_retarget_asset=rtg, search="", replace="", prefix="", suffix=TMP,
            include_referenced_assets=False, overwrite_existing_files=True)
        if not produits:
            say("%-26s RECIBLAGE SANS SORTIE" % src)
            continue
        for d in produits:
            pkg = str(d.package_name)
            # L'asset produit n'est pas enregistre par save_directory : explicitement, et tout de suite.
            unreal.EditorAssetLibrary.save_asset(pkg, False)
            dest = "%s/%s" % (DEST_DIR, final)
            if not unreal.EditorAssetLibrary.rename_asset(pkg, dest):
                say("%-26s DEPLACEMENT REFUSE vers %s" % (src, dest))
                continue
            unreal.EditorAssetLibrary.save_asset(dest, False)
            a = unreal.load_asset(dest)
            n = len(unreal.AnimationLibrary.get_animation_track_names(a))
            e = ecart_colonne(a)
            say("%-26s -> %-24s %5.2f s  %3d pistes  colonne %6.1f deg%s"
                % (src, final, a.get_play_length(), n, e, "" if e > 5.0 else "   FIGEE"))
            if e > 5.0:
                ok += 1

    unreal.EditorAssetLibrary.save_directory(DEST_DIR, False, True)
    say("")
    say("%d / %d prises portent du mouvement sur la colonne" % (ok, len(PRISES)))
    say("OK" if ok == len(PRISES) else "INCOMPLET")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")

# Le reciblage passe par Slate : il faut UnrealEditor.exe -ExecutePythonScript=, pas
# UnrealEditor-Cmd.exe -run=pythonscript, qui echoue sur une assertion CurrentApplication.IsValid().
# D'ou la fermeture explicite de l'editeur.
unreal.SystemLibrary.quit_editor()
