import unreal, traceback
out = []
def say(t): out.append(str(t))
try:
    for n in ["IK_Als_Test","IK_Als_T2","IK_Als_T3"]:
        p = "/Game/Impact/Retarget/" + n
        if unreal.EditorAssetLibrary.does_asset_exist(p):
            unreal.EditorAssetLibrary.delete_asset(p)
    mesh = unreal.load_asset("/ALS/ALS/Character/SKM_Als")
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    rig = tools.create_asset("IK_Als", "/Game/Impact/Retarget", unreal.IKRigDefinition, unreal.IKRigDefinitionFactory())
    c = unreal.IKRigController.get_controller(rig)
    c.set_skeletal_mesh(mesh)
    chains = [("Root","pelvis","pelvis"),("Spine","spine_01","spine_03"),("Head","neck_01","head"),
              ("LeftClavicle","clavicle_l","clavicle_l"),("RightClavicle","clavicle_r","clavicle_r"),
              ("LeftArm","upperarm_l","hand_l"),("RightArm","upperarm_r","hand_r"),
              ("LeftLeg","thigh_l","foot_l"),("RightLeg","thigh_r","foot_r")]
    for n,a,b in chains:
        c.add_retarget_chain(n, a, b, "None")
    try:
        c.set_retarget_root("pelvis"); say("racine de reciblage : pelvis")
    except Exception as e:
        say("racine : %s" % str(e)[:80])
    say("IK Rig cree : %d chaines" % len(c.get_retarget_chains()))
    for ch in c.get_retarget_chains():
        say("   %s" % ch)
    unreal.EditorAssetLibrary.save_loaded_asset(rig)
    say("sauvegarde OK -> /Game/Impact/Retarget/IK_Als")
except Exception:
    say(traceback.format_exc()[:900])
open("D:/UEProjets/Impact/Saved/ikrig_result.txt","w",encoding="utf-8").write("\n".join(out))
