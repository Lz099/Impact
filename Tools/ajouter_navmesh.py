# La carte L_Als_Playground n'a pas de zone de navigation : measure dans les journaux, 209 trajets des PNJ sur 213 "sans
# navigation" (MoveToLocation echoue, ils marchent en ligne droite). On y pose un NavMeshBoundsVolume qui couvre tout le decor ;
# la generation est dynamique (Config/DefaultEngine.ini, RecastNavMesh RuntimeGeneration=Dynamic), le maillage est donc construit
# au lancement. Copie de la carte avant : Saved/_preuves_reprise/carte_avant_navmesh/.
#   UnrealEditor.exe Impact.uproject -ExecutePythonScript=Tools/ajouter_navmesh.py
# Rapport : Saved/_diag/ajouter_navmesh.txt
import unreal, os, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/ajouter_navmesh.txt"
MAP = "/ALS/ALSExtras/Levels/L_Als_Playground"

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

try:
    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not subsystem.load_level(MAP):
        raise RuntimeError("carte non chargee")
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    existing = [a for a in actors if isinstance(a, unreal.NavMeshBoundsVolume)]
    if existing:
        say("deja %d zone(s) de navigation, rien a faire" % len(existing))
    else:
        lo = unreal.Vector(1e9, 1e9, 1e9)
        hi = unreal.Vector(-1e9, -1e9, -1e9)
        counted = 0
        for a in actors:
            if not isinstance(a, unreal.StaticMeshActor):
                continue
            origin, extent = a.get_actor_bounds(False)
            if extent.x > 50000 or extent.y > 50000:
                continue  # sky spheres and the like
            counted += 1
            lo = unreal.Vector(min(lo.x, origin.x - extent.x), min(lo.y, origin.y - extent.y), min(lo.z, origin.z - extent.z))
            hi = unreal.Vector(max(hi.x, origin.x + extent.x), max(hi.y, origin.y + extent.y), max(hi.z, origin.z + extent.z))
        center = unreal.Vector((lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5)
        size = unreal.Vector(hi.x - lo.x + 400.0, hi.y - lo.y + 400.0, hi.z - lo.z + 400.0)
        volume = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(unreal.NavMeshBoundsVolume, center)
        # The default brush is a 200 cm cube.
        volume.set_actor_scale3d(unreal.Vector(size.x / 200.0, size.y / 200.0, size.z / 200.0))
        say("decor : %d maillages, de (%.0f %.0f %.0f) a (%.0f %.0f %.0f)" % (counted, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z))
        say("zone de navigation posee au centre (%.0f %.0f %.0f), taille (%.0f %.0f %.0f) cm" % (center.x, center.y, center.z, size.x, size.y, size.z))
        if not subsystem.save_current_level():
            raise RuntimeError("carte non enregistree")
        say("carte enregistree")
    say("FIN")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")

try:
    unreal.SystemLibrary.quit_editor()
except Exception:
    pass
