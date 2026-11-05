import unreal, traceback
out = []
def say(t): out.append(str(t))
mesh = unreal.load_asset("/ALS/ALS/Character/SKM_Als")
f = unreal.IKRigDefinitionFactory()
say("--- proprietes de la fabrique ---")
for p in sorted(dir(f)):
    if not p.startswith("_") and "skelet" in p.lower() or "mesh" in p.lower() or "target" in p.lower():
        say("  " + p)
say("--- essai 1 : propriete sur la fabrique ---")
for prop in ["target_skeletal_mesh","skeletal_mesh","preview_skeletal_mesh"]:
    try:
        f.set_editor_property(prop, mesh); say("  %s : accepte" % prop)
    except Exception as e:
        say("  %s : %s" % (prop, str(e)[:90]))
say("--- essai 2 : creation ---")
try:
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    rig = tools.create_asset("IK_Als_T2", "/Game/Impact/Retarget", unreal.IKRigDefinition, f)
    say("  create_asset -> %s" % rig)
    if rig is None:
        say("--- essai 3 : sans fabrique ---")
        rig = tools.create_asset("IK_Als_T3", "/Game/Impact/Retarget", unreal.IKRigDefinition, None)
        say("  sans fabrique -> %s" % rig)
    if rig is not None:
        c = unreal.IKRigController.get_controller(rig)
        c.set_skeletal_mesh(mesh)
        c.add_retarget_chain("arm_l","upperarm_l","hand_l","None")
        say("  RESULTAT : OUI, chaines = %d" % len(c.get_retarget_chains()))
        unreal.EditorAssetLibrary.save_loaded_asset(rig)
    else:
        say("  RESULTAT : NON - creation impossible sans interface")
except Exception:
    say(traceback.format_exc()[:700])
open("D:/UEProjets/Impact/Saved/ikrig_result.txt","w",encoding="utf-8").write("\n".join(out))
