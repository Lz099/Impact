import unreal, traceback
out=[]
try:
    anim=unreal.load_asset("/Game/Impact/Mixamo/Writhing_In_Pain")
    names=[]
    for fn in ["get_animation_track_names","get_bone_animation_track_names"]:
        if hasattr(unreal.AnimationLibrary, fn):
            try:
                names=[str(n) for n in getattr(unreal.AnimationLibrary, fn)(anim)]
                out.append("via %s : %d os" % (fn, len(names)))
                break
            except Exception as e:
                out.append("%s : %s" % (fn, str(e)[:80]))
    if not names:
        out.append("methodes AnimationLibrary dispo : " + ", ".join([m for m in dir(unreal.AnimationLibrary) if "track" in m.lower() or "bone" in m.lower()][:12]))
    for n in names[:30]:
        out.append("   " + n)
except Exception:
    out.append(traceback.format_exc()[:800])
open("D:/UEProjets/Impact/Saved/bones.txt","w",encoding="utf-8").write("\n".join(out))
