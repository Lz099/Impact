import unreal, traceback
out=[]
def say(t): out.append(str(t)); open("D:/UEProjets/Impact/Saved/retarget_result.txt","w",encoding="utf-8").write("\n".join(out))
try:
    a=unreal.load_asset("/Game/Douleur_Au_Sol")
    say("anim : %s" % a)
    say("duree %.2f s, squelette : %s" % (a.get_play_length(), a.get_editor_property("skeleton").get_name()))
    ok=unreal.EditorAssetLibrary.rename_asset("/Game/Douleur_Au_Sol", "/Game/Impact/Anims/Douleur_Au_Sol")
    say("range dans /Game/Impact/Anims : %s" % ok)
    unreal.EditorAssetLibrary.save_directory("/Game/Impact", False, True)
    say("os animes : %d" % len(unreal.AnimationLibrary.get_animation_track_names(unreal.load_asset("/Game/Impact/Anims/Douleur_Au_Sol"))))
except Exception:
    say(traceback.format_exc()[:900])
unreal.SystemLibrary.quit_editor()
