# Diagnostic : la chute Mixamo perd sa descente de bassin au reciblage. Lit la hauteur du bassin dans
# la source Mixamo et dans la prise reciblee, et liste les reglages de l'operation Pelvis Motion.
# Lance par : UnrealEditor-Cmd.exe Impact.uproject -run=pythonscript -script=Tools/diag_bassin_reciblage.py
import unreal, os, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/bassin_reciblage.txt"
out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

def hauteur(anim, bone):
    L = anim.get_play_length()
    vals = []
    for i in range(0, 7):
        try:
            p = unreal.AnimationLibrary.get_bone_pose_for_time(anim, bone, L * i / 6.0, False)
            vals.append("%.0f" % p.translation.z)
        except Exception as e:
            vals.append("?")
    return " ".join(vals)

try:
    src = unreal.load_asset("/Game/Impact/Mixamo/Shoulder_Hit_And_Fall")
    dst = unreal.load_asset("/Game/Impact/Anims/Shoulder_Hit_And_Fall")
    for name, a in (("source Mixamo", src), ("reciblee ALS", dst)):
        if a is None:
            say("%s : absente" % name)
            continue
        say("%s : %s, %.2f s" % (name, a.get_name(), a.get_play_length()))
        for bone in ("hips", "mixamorig:Hips", "root", "pelvis"):
            say("   %-16s z : %s" % (bone, hauteur(a, bone)))

    rtg = unreal.load_asset("/Game/Impact/Retarget/RTG_Mixamo_Als_v3")
    rc = unreal.IKRetargeterController.get_controller(rtg)
    for i in range(rc.get_num_retarget_ops()):
        oc = rc.get_op_controller(i)
        settings = oc.get_settings()
        say("op %d %s actif=%s" % (i, rc.get_retarget_op_name(i) if hasattr(rc, "get_retarget_op_name") else "?", rc.get_retarget_op_enabled(i)))
        say("   classe reglages : %s" % settings.__class__.__name__)
        for prop in dir(settings):
            if prop.startswith("_"):
                continue
            try:
                v = settings.get_editor_property(prop)
            except Exception:
                continue
            if callable(v):
                continue
            say("   %s = %s" % (prop, str(v)[:160]))
    say("FIN")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")
