# PERIME - NE PAS RELANCER. Ce script construit son retargeter sans appeler
# assign_ik_rig_to_all_ops(), donc les operations du retargeter gardent une table de chaines vide
# et le reciblage sort une pose figee, sans rien dire. C'est ce qui a rendu les quatorze prises
# Mixamo immobiles. Utiliser Tools/retarget_setup.py puis Tools/retarget_batch.py, et controler
# avec Tools/check_anims.py. Les assets qu'il laisse (IK_Mixamo, IK_Mixamo_2,
# RTG_Mixamo_vers_Als, RTG_Mixamo_vers_Als_2) sont morts : le bon est RTG_Mixamo_Als_v3.
import unreal, traceback
out=[]
def say(t): out.append(str(t)); open("D:/UEProjets/Impact/Saved/retarget_result.txt","w",encoding="utf-8").write("\n".join(out))
try:
    mix_mesh=unreal.load_asset("/Game/Impact/Mixamo/Ch36_nonPBR")
    als_mesh=unreal.load_asset("/ALS/ALS/Character/SKM_Als")
    rtg=unreal.load_asset("/Game/Impact/Retarget/RTG_Mixamo_vers_Als_2")
    data=unreal.EditorAssetLibrary.find_asset_data("/Game/Impact/Mixamo/Writhing_In_Pain")
    op=unreal.IKRetargetBatchOperation()
    done=op.duplicate_and_retarget(assets_to_retarget=[data], source_mesh=mix_mesh, target_mesh=als_mesh,
                                   ik_retarget_asset=rtg, search="Writhing_In_Pain", replace="Douleur_Au_Sol",
                                   prefix="", suffix="")
    for d in (done or []):
        p = str(d.package_name)
        say("produit : %s" % p)
        unreal.EditorAssetLibrary.save_asset(p, False)
        a = unreal.load_asset(p)
        if isinstance(a, unreal.AnimSequence):
            say("  duree %.2f s, squelette %s" % (a.get_play_length(), a.get_editor_property("skeleton").get_name()))
            say("  os animes : %d" % len(unreal.AnimationLibrary.get_animation_track_names(a)))
        dest = "/Game/Impact/Anims/" + str(d.asset_name)
        if not unreal.EditorAssetLibrary.does_asset_exist(dest):
            say("  range : %s" % unreal.EditorAssetLibrary.rename_asset(p, dest))
        unreal.EditorAssetLibrary.save_directory("/Game/Impact/Anims", False, True)
except Exception:
    say(traceback.format_exc()[:1200])
unreal.SystemLibrary.quit_editor()
