import unreal, os
OUT = "D:/UEProjets/Impact/Saved/_preuves_reprise/mixamo"
os.makedirs(OUT, exist_ok=True)

# 1 - exporter le maillage pour Mixamo
mesh = unreal.load_asset("/ALS/ALS/Character/SKM_Als")
unreal.log("[mixamo] maillage charge : %s" % mesh)
task = unreal.AssetExportTask()
task.set_editor_property("object", mesh)
task.set_editor_property("filename", OUT + "/SKM_Als.fbx")
task.set_editor_property("automated", True)
task.set_editor_property("prompt", False)
task.set_editor_property("replace_identical", True)
task.set_editor_property("exporter", unreal.SkeletalMeshExporterFBX())
opt = unreal.FbxExportOption()
opt.set_editor_property("ascii", False)
opt.set_editor_property("collision", False)
opt.set_editor_property("level_of_detail", False)
task.set_editor_property("options", opt)
unreal.log("[mixamo] export FBX : %s" % unreal.Exporter.run_asset_export_task(task))

# 2 - le reciblage est-il scriptable ?
for name in ["IKRigDefinition", "IKRigController", "IKRetargeter", "IKRetargeterController", "IKRetargetBatchOperation"]:
    unreal.log("[api] %s : %s" % (name, "OUI" if hasattr(unreal, name) else "NON"))

try:
    factory = unreal.IKRigDefinitionFactory()
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    rig = tools.create_asset("IK_Als_Test", "/Game/Impact/Retarget", unreal.IKRigDefinition, factory)
    ctrl = unreal.IKRigController.get_controller(rig)
    ctrl.set_skeletal_mesh(mesh)
    ctrl.add_retarget_chain("spine", "spine_01", "spine_03", "None")
    ctrl.add_retarget_chain("arm_l", "upperarm_l", "hand_l", "None")
    ctrl.add_retarget_chain("leg_l", "thigh_l", "foot_l", "None")
    unreal.log("[api] IK Rig cree par script : OUI, chaines = %d" % len(ctrl.get_retarget_chains()))
    unreal.EditorAssetLibrary.save_loaded_asset(rig)
except Exception as e:
    unreal.log("[api] IK Rig par script : ECHEC -> %s" % e)
