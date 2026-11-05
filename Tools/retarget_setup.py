# Construit l'IK Rig source (Mixamo), l'IK Rig cible (ALS) et le retargeter qui les relie.
#
# Le defaut repare ici : depuis UE 5.6 le retargeter n'est plus un bloc unique, c'est une PILE
# D'OPERATIONS (Pelvis Motion, FK Chains, IK Chains, Run IK Rig, Root Motion, Remap Curves) et
# c'est chaque operation qui porte sa propre table de correspondance des chaines. Appeler
# set_ik_rig(SOURCE/TARGET) ne renseigne que les rigs globaux : les operations gardent
# ik_rig_asset = None et chains_to_retarget vide. auto_map_chains n'a alors rien a associer et ne
# dit rien. Le retargeter sort la pose de reciblage de la cible, identique a tous les instants.
# C'est exactement ce qui a ete mesure : 79 pistes, une seule valeur par os.
#
# Il manquait assign_ik_rig_to_all_ops(...), qui pousse le rig dans chaque operation et reconstruit
# leurs tables. On verifie ensuite que chains_to_retarget n'est plus vide : c'est la seule preuve
# que le reciblage a de quoi travailler.
#
# A lancer en -Cmd : ce script ne touche pas au FBX, il ne lui faut ni rendu ni Slate.
import unreal, os, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/retarget_setup.txt"
SRC = unreal.RetargetSourceOrTarget.SOURCE
TGT = unreal.RetargetSourceOrTarget.TARGET

MIX_MESH = "/Game/Impact/Mixamo/Ch36_nonPBR"
ALS_MESH = "/ALS/ALS/Character/SKM_Als"
DIR      = "/Game/Impact/Retarget"
IK_MIX   = "IK_Mixamo_src"
IK_ALS   = "IK_Als_tgt"
RTG      = "RTG_Mixamo_Als_v3"

# Les noms d'os cote Mixamo sont ceux qu'Unreal ecrit a l'import : prefixe mixamorig: retire et
# tout en minuscules. Les noms d'origine (mixamorig:Hips) ne correspondent a rien.
CHAINS_MIX = [("Root", "hips", "hips"), ("Spine", "spine", "spine2"), ("Head", "neck", "head"),
              ("LeftClavicle", "leftshoulder", "leftshoulder"), ("RightClavicle", "rightshoulder", "rightshoulder"),
              ("LeftArm", "leftarm", "lefthand"), ("RightArm", "rightarm", "righthand"),
              ("LeftLeg", "leftupleg", "leftfoot"), ("RightLeg", "rightupleg", "rightfoot")]

CHAINS_ALS = [("Root", "pelvis", "pelvis"), ("Spine", "spine_01", "spine_03"), ("Head", "neck_01", "head"),
              ("LeftClavicle", "clavicle_l", "clavicle_l"), ("RightClavicle", "clavicle_r", "clavicle_r"),
              ("LeftArm", "upperarm_l", "hand_l"), ("RightArm", "upperarm_r", "hand_r"),
              ("LeftLeg", "thigh_l", "foot_l"), ("RightLeg", "thigh_r", "foot_r")]

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

def fresh(name, cls, factory):
    # create_asset rend None si un asset du meme nom a deja existe dans la session : on prend un
    # nom neuf plutot que de supprimer puis recreer.
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = "%s/%s" % (DIR, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        a = unreal.load_asset(path)
        if a is not None:
            say("reutilise : %s" % name)
            return a
    for i in range(1, 20):
        n = name if i == 1 else "%s_%d" % (name, i)
        if unreal.EditorAssetLibrary.does_asset_exist("%s/%s" % (DIR, n)):
            continue
        a = tools.create_asset(n, DIR, cls, factory)
        if a is not None:
            say("cree : %s" % n)
            return a
    raise RuntimeError("impossible de creer %s" % name)

def build_rig(asset, mesh_path, chains, root):
    c = unreal.IKRigController.get_controller(asset)
    mesh = unreal.load_asset(mesh_path)
    if mesh is None:
        raise RuntimeError("maillage absent : %s" % mesh_path)
    c.set_skeletal_mesh(mesh)
    have = set()
    for ch in c.get_retarget_chains():
        have.add(str(ch.get_editor_property("chain_name")))
    for n, a, b in chains:
        if n in have:
            continue
        c.add_retarget_chain(n, a, b, "None")
    c.set_retarget_root(root)
    # Preuve : chaque chaine doit porter ses deux os. Une chaine dont les os n'existent pas dans le
    # squelette est acceptee en silence par add_retarget_chain mais reste creuse.
    ok = 0
    for ch in c.get_retarget_chains():
        sb = str(ch.get_editor_property("start_bone").get_editor_property("bone_name"))
        eb = str(ch.get_editor_property("end_bone").get_editor_property("bone_name"))
        bad = (sb in ("None", "")) or (eb in ("None", ""))
        say("   %-14s %s -> %s%s" % (ch.get_editor_property("chain_name"), sb, eb, "   CREUSE" if bad else ""))
        if not bad:
            ok += 1
    say("   %d/%d chaines completes, racine = %s" % (ok, len(chains), c.get_retarget_root()))
    unreal.EditorAssetLibrary.save_loaded_asset(asset)
    return ok

try:
    say("=== IK Rig source (Mixamo) ===")
    rig_mix = fresh(IK_MIX, unreal.IKRigDefinition, unreal.IKRigDefinitionFactory())
    n_mix = build_rig(rig_mix, MIX_MESH, CHAINS_MIX, "hips")

    say("")
    say("=== IK Rig cible (ALS) ===")
    rig_als = fresh(IK_ALS, unreal.IKRigDefinition, unreal.IKRigDefinitionFactory())
    n_als = build_rig(rig_als, ALS_MESH, CHAINS_ALS, "pelvis")

    if n_mix < len(CHAINS_MIX) or n_als < len(CHAINS_ALS):
        raise RuntimeError("des chaines sont creuses : les noms d'os ne correspondent pas")

    say("")
    say("=== Retargeter ===")
    rtg = fresh(RTG, unreal.IKRetargeter, unreal.IKRetargetFactory())
    rc = unreal.IKRetargeterController.get_controller(rtg)

    rc.set_ik_rig(SRC, rig_mix)
    rc.set_ik_rig(TGT, rig_als)
    try:
        rc.set_preview_mesh(SRC, unreal.load_asset(MIX_MESH))
        rc.set_preview_mesh(TGT, unreal.load_asset(ALS_MESH))
    except Exception as e:
        say("apercu : %s" % str(e)[:80])

    # La fabrique pose deja les operations par defaut. add_default_ops() par-dessus en empile un
    # SECOND jeu complet : deux Pelvis Motion, deux FK Chains, deux Root Motion dont une active, et
    # le resultat devient imprevisible. On repart d'une pile vide a chaque fois.
    rc.remove_all_ops()
    rc.add_default_ops()
    say("operations : %d" % rc.get_num_retarget_ops())

    # LE POINT QUI MANQUAIT. set_ik_rig ne renseigne que les rigs globaux ; ceci pousse le rig dans
    # chaque operation et reconstruit leurs tables de correspondance.
    rc.assign_ik_rig_to_all_ops(SRC, rig_mix)
    rc.assign_ik_rig_to_all_ops(TGT, rig_als)
    rc.auto_map_chains(unreal.AutoMapChainType.EXACT, True)

    # Les os de bassin, explicitement : c'est l'operation Pelvis Motion qui les porte maintenant,
    # plus la racine de reciblage du rig.
    for i in range(rc.get_num_retarget_ops()):
        name = str(rc.get_op_name(i))
        oc = rc.get_op_controller(i)
        if name == "Pelvis Motion":
            try:
                oc.set_source_pelvis_bone("hips")
                oc.set_target_pelvis_bone("pelvis")
                say("bassin : %s -> %s" % (oc.get_source_pelvis_bone(), oc.get_target_pelvis_bone()))
            except Exception as e:
                say("bassin : %s" % str(e)[:100])
        if name == "Root Motion":
            # Mixamo n'a pas d'os racine : hips EST la racine. Copier un mouvement racine depuis un
            # os qui n'existe pas n'a pas de sens, et ces prises sont jouees sur place dans un
            # emplacement d'animation. L'operation est desactivee.
            rc.set_retarget_op_enabled(i, False)
            say("Root Motion desactivee (Mixamo n'a pas d'os racine)")

    # Preuve que le mappage existe vraiment, operation par operation.
    say("")
    total = 0
    for i in range(rc.get_num_retarget_ops()):
        name = str(rc.get_op_name(i))
        oc = rc.get_op_controller(i)
        s = None
        try:
            s = oc.get_settings()
        except Exception:
            pass
        n = 0
        if s is not None:
            try:
                n = len(s.get_editor_property("chains_to_retarget"))
            except Exception:
                n = -1
        say("op %d %-18s actif=%-5s chaines=%s" % (i, name, rc.get_retarget_op_enabled(i), n))
        if n > 0:
            total += n

    say("")
    for tc in [c[0] for c in CHAINS_ALS]:
        say("   %-14s <- %s" % (tc, rc.get_source_chain(tc)))

    if total == 0:
        raise RuntimeError("aucune chaine associee : le reciblage sortirait encore une pose figee")

    # Les deux squelettes n'ont pas la meme pose de repos (Mixamo en T, ALS en A). Le reciblage FK
    # copie l'ecart a la pose de reciblage : si les deux poses ne se ressemblent pas, le resultat
    # est tordu. auto_align_all_bones aligne la cible sur la source, chaine par chaine.
    try:
        pose = "AligneeSurMixamo"
        if pose not in [str(p) for p in rc.get_retarget_poses(TGT)]:
            pose = str(rc.create_retarget_pose(pose, TGT))
        rc.set_current_retarget_pose(pose, TGT)
        rc.reset_retarget_pose(pose, [], TGT)
        rc.auto_align_all_bones(TGT, unreal.RetargetAutoAlignMethod.CHAIN_TO_CHAIN)
        say("")
        say("pose de reciblage cible : %s (alignee chaine par chaine)" % rc.get_current_retarget_pose_name(TGT))
    except Exception:
        say("alignement : %s" % traceback.format_exc()[-300:])

    unreal.EditorAssetLibrary.save_loaded_asset(rtg)
    say("")
    say("%d associations de chaines au total. %s enregistre." % (total, RTG))
    say("OK")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")
