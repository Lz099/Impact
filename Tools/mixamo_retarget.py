# PERIME - NE PAS RELANCER. Ce script construit son retargeter sans appeler
# assign_ik_rig_to_all_ops(), donc les operations du retargeter gardent une table de chaines vide
# et le reciblage sort une pose figee, sans rien dire. C'est ce qui a rendu les quatorze prises
# Mixamo immobiles. Utiliser Tools/retarget_setup.py puis Tools/retarget_batch.py, et controler
# avec Tools/check_anims.py. Les assets qu'il laisse (IK_Mixamo, IK_Mixamo_2,
# RTG_Mixamo_vers_Als, RTG_Mixamo_vers_Als_2) sont morts : le bon est RTG_Mixamo_Als_v3.
import unreal, traceback
out=[]
def say(t): out.append(str(t)); open("D:/UEProjets/Impact/Saved/retarget_result.txt","w",encoding="utf-8").write("\n".join(out))
def make(name, cls, factory):
    tools=unreal.AssetToolsHelpers.get_asset_tools()
    for i in range(1,12):
        n = name if i==1 else "%s_%d"%(name,i)
        if unreal.EditorAssetLibrary.does_asset_exist("/Game/Impact/Retarget/"+n): continue
        a=tools.create_asset(n,"/Game/Impact/Retarget",cls,factory)
        if a is not None:
            say("cree : %s" % n); return a
    return None
try:
    assets=unreal.EditorAssetLibrary
    mix_mesh=unreal.load_asset("/Game/Impact/Mixamo/Ch36_nonPBR")
    als_mesh=unreal.load_asset("/ALS/ALS/Character/SKM_Als")
    anim=unreal.load_asset("/Game/Impact/Mixamo/Writhing_In_Pain")

    rig=make("IK_Mixamo", unreal.IKRigDefinition, unreal.IKRigDefinitionFactory())
    c=unreal.IKRigController.get_controller(rig); c.set_skeletal_mesh(mix_mesh)
    for n,a,b in [("Root","hips","hips"),("Spine","spine","spine2"),("Head","neck","head"),
                  ("LeftClavicle","leftshoulder","leftshoulder"),("RightClavicle","rightshoulder","rightshoulder"),
                  ("LeftArm","leftarm","lefthand"),("RightArm","rightarm","righthand"),
                  ("LeftLeg","leftupleg","leftfoot"),("RightLeg","rightupleg","rightfoot")]:
        c.add_retarget_chain(n,a,b,"None")
    c.set_retarget_root("hips")
    say("IK Rig Mixamo : %d chaines" % len(c.get_retarget_chains()))
    assets.save_loaded_asset(rig)

    rtg=make("RTG_Mixamo_vers_Als", unreal.IKRetargeter, unreal.IKRetargetFactory())
    rc=unreal.IKRetargeterController.get_controller(rtg)
    rc.set_ik_rig(unreal.RetargetSourceOrTarget.SOURCE, rig)
    rc.set_ik_rig(unreal.RetargetSourceOrTarget.TARGET, unreal.load_asset("/Game/Impact/Retarget/IK_Als"))
    try:
        rc.set_preview_mesh(unreal.RetargetSourceOrTarget.SOURCE, mix_mesh)
        rc.set_preview_mesh(unreal.RetargetSourceOrTarget.TARGET, als_mesh)
    except Exception: pass
    try: rc.auto_map_chains(unreal.AutoMapChainType.EXACT, True)
    except Exception as e: say("auto_map : %s" % str(e)[:100])
    assets.save_loaded_asset(rtg)

    op=unreal.IKRetargetBatchOperation()
    done=op.duplicate_and_retarget([anim], mix_mesh, als_mesh, rtg, "Writhing_In_Pain", "Douleur_Au_Sol", "", False, True)
    say("recible : %s" % done)
    for a in (done or []): say("   -> %s" % a.get_path_name())
except Exception:
    say(traceback.format_exc()[:1400])
unreal.SystemLibrary.quit_editor()
