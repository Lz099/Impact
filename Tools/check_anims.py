# Controle des quatorze prises d'impact : est-ce qu'elles portent vraiment du mouvement sur
# SK_Als, ou seulement une pose figee ?
#
# Pour chaque prise, cinq os sont echantillonnes a six instants repartis sur la duree :
#   spine_01, spine_02, spine_03  la colonne, ce que la reaction doit bouger
#   upperarm_r                    l'epaule, qui suit la colonne par l'emplacement Spine d'ALS
#   thigh_r                       la cuisse, qui doit rester a la locomotion (masquee)
# et l'ecart maximal en degres entre l'instant zero et les cinq autres est imprime.
#
# Le verdict porte sur la colonne : l'ecart doit depasser 5 degres pour chacune des quatorze. C'est
# le meme seuil, sur les memes os, que TakeMovesTheTrunk dans ImpactReactionComponent.cpp, qui
# refuse en jeu une prise dont le torse est immobile.
#
# A lancer en -Cmd, il ne lit que des assets :
#   UnrealEditor-Cmd.exe Impact.uproject -run=pythonscript ^
#       -script=D:/UEProjets/Impact/Tools/check_anims.py -unattended -nosplash -nopause
# Resultat dans Saved/_diag/check_anims.txt
import unreal, os, math, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/check_anims.txt"
DIR = "/Game/Impact/Anims"
OS_COLONNE = ("spine_01", "spine_02", "spine_03")
OS_TOUS    = ("spine_01", "spine_02", "spine_03", "upperarm_r", "thigh_r")
INSTANTS   = 6
SEUIL      = 5.0

# Les noms que la table de ImpactReactionComponent.cpp charge, dans l'ordre des zones du corps.
PRISES = ["Big_Hit_To_Head", "Big_Hit_To_Head_1", "Big_Hit_To_Head_2", "Head_Hit",
          "Hit_On_Side_Of_Head", "Big_Stomach_Hit", "Stomach_Hit", "Rib_Hit", "Hit_To_Body",
          "Hit_To_Body_1", "Hit_On_Side_Of_Body", "Shoulder_Hit_And_Fall",
          "Shoulder_Hit_And_Fall_1", "Douleur_Au_Sol"]

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

def ecarts(anim):
    # Pour chaque os : le plus grand angle entre la pose a l'instant zero et la pose aux cinq
    # instants suivants. Un reciblage rate rend exactement 0 partout.
    L = anim.get_play_length()
    res = {}
    for bone in OS_TOUS:
        try:
            base = unreal.AnimationLibrary.get_bone_pose_for_time(anim, bone, 0.0, False).rotation
        except Exception:
            res[bone] = None
            continue
        pire = 0.0
        for i in range(1, INSTANTS):
            q = unreal.AnimationLibrary.get_bone_pose_for_time(anim, bone, L * i / (INSTANTS - 1), False).rotation
            d = math.degrees(abs(unreal.MathLibrary.quat_angular_distance(base, q)))
            if d > pire:
                pire = d
        res[bone] = pire
    return res

try:
    say("Controle des prises d'impact sur SK_Als")
    say("%d os echantillonnes a %d instants repartis sur la duree, ecart maximal en degres." % (len(OS_TOUS), INSTANTS))
    say("Verdict : la colonne doit depasser %.0f deg. Sous ce seuil, le jeu refuse la prise." % SEUIL)
    say("")
    say("%-26s %6s %4s %8s %8s %8s %10s %8s   %s"
        % ("prise", "duree", "pist", "spine_01", "spine_02", "spine_03", "upperarm_r", "thigh_r", "verdict"))
    say("-" * 108)

    bonnes = 0
    manquantes = []
    for nom in PRISES:
        path = "%s/%s" % (DIR, nom)
        anim = unreal.load_asset(path)
        if anim is None or not isinstance(anim, unreal.AnimSequence):
            say("%-26s ABSENTE de %s" % (nom, DIR))
            manquantes.append(nom)
            continue
        e = ecarts(anim)
        colonne = max([e[b] for b in OS_COLONNE if e[b] is not None] or [0.0])
        def f(b):
            return "     ---" if e.get(b) is None else "%8.1f" % e[b]
        say("%-26s %5.2fs %4d %s %s %s %s%s   %s"
            % (nom, anim.get_play_length(), len(unreal.AnimationLibrary.get_animation_track_names(anim)),
               f("spine_01"), f("spine_02"), f("spine_03"),
               ("%10.1f" % e["upperarm_r"]) if e.get("upperarm_r") is not None else "       ---",
               f("thigh_r"),
               "bouge" if colonne > SEUIL else "FIGEE"))
        if colonne > SEUIL:
            bonnes += 1

    say("-" * 108)
    say("")
    say("%d / %d prises bougent la colonne de plus de %.0f deg." % (bonnes, len(PRISES), SEUIL))
    if manquantes:
        say("Absentes : %s" % ", ".join(manquantes))
    say("OK" if bonnes == len(PRISES) else "INCOMPLET")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")
