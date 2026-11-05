# La carte a une zone de navigation (NavMeshBoundsVolume) mais en jeu aucun maillage n'est cree (aucun message LogNavigation,
# "Unable to find RecastNavMesh instance"). On regarde ce qu'il y a, on fait construire le maillage par l'editeur et on enregistre
# la carte avec lui. Copie de la carte avant : Saved/_preuves_reprise/carte_avant_navmesh/.
#   UnrealEditor.exe Impact.uproject -ExecutePythonScript=Tools/construire_navmesh.py
# Rapport : Saved/_diag/construire_navmesh.txt
import unreal, os, traceback

OUT = "D:/UEProjets/Impact/Saved/_diag/construire_navmesh.txt"
MAP = "/ALS/ALSExtras/Levels/L_Als_Playground"

out = []
def say(t):
    out.append(str(t))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8").write("\n".join(out))

def nav_actors():
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    return [a for a in actors if "Nav" in a.get_class().get_name() or "Recast" in a.get_class().get_name()]

try:
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not levels.load_level(MAP):
        raise RuntimeError("carte non chargee")
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    for a in nav_actors():
        origin, extent = a.get_actor_bounds(False)
        say("avant : %s (%s) centre (%.0f %.0f %.0f) demi-taille (%.0f %.0f %.0f)" % (a.get_name(), a.get_class().get_name(),
            origin.x, origin.y, origin.z, extent.x, extent.y, extent.z))
    # The volume covered a 20 m square in one corner: stretched over the whole of the decor.
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    lo = [1e9, 1e9, 1e9]
    hi = [-1e9, -1e9, -1e9]
    counted = 0
    for a in actors:
        if not isinstance(a, unreal.StaticMeshActor):
            continue
        origin, extent = a.get_actor_bounds(False)
        if extent.x > 50000 or extent.y > 50000:
            continue
        counted += 1
        for i, (o, e) in enumerate(((origin.x, extent.x), (origin.y, extent.y), (origin.z, extent.z))):
            lo[i] = min(lo[i], o - e)
            hi[i] = max(hi[i], o + e)
    volumes = [a for a in actors if isinstance(a, unreal.NavMeshBoundsVolume)]
    if volumes and counted:
        volume = volumes[0]
        center = unreal.Vector((lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5)
        # The brush is what the scale applies to: measured from the current bounds and scale.
        o, e = volume.get_actor_bounds(False)
        s = volume.get_actor_scale3d()
        unit = unreal.Vector(e.x / max(s.x, 1e-3), e.y / max(s.y, 1e-3), e.z / max(s.z, 1e-3))
        want = unreal.Vector((hi[0] - lo[0]) * 0.5 + 300.0, (hi[1] - lo[1]) * 0.5 + 300.0, (hi[2] - lo[2]) * 0.5 + 300.0)
        volume.set_actor_location(center, False, False)
        volume.set_actor_scale3d(unreal.Vector(want.x / unit.x, want.y / unit.y, want.z / unit.z))
        o, e = volume.get_actor_bounds(False)
        say("decor : %d maillages ; zone etiree a centre (%.0f %.0f %.0f) demi-taille (%.0f %.0f %.0f)" % (counted, o.x, o.y, o.z, e.x, e.y, e.z))
    # The navigation data actor keeps its own generation mode: Static would leave the mesh empty in game once it is recreated.
    for a in nav_actors():
        if isinstance(a, unreal.RecastNavMesh):
            say("generation avant : %s" % a.get_editor_property("runtime_generation"))
            a.set_editor_property("runtime_generation", unreal.RuntimeGenerationType.DYNAMIC)
            say("generation apres : %s" % a.get_editor_property("runtime_generation"))
    unreal.SystemLibrary.execute_console_command(world, "RebuildNavigation")
    for a in nav_actors():
        say("apres : %s (%s)" % (a.get_name(), a.get_class().get_name()))
    # A path across the middle of the playground, to see whether the mesh answers.
    nav = unreal.NavigationSystemV1.get_navigation_system(world)
    say("systeme de navigation : %s" % (nav.get_name() if nav else "aucun"))
    if any("Recast" in a.get_class().get_name() for a in nav_actors()):
        if levels.save_current_level():
            say("carte enregistree avec le maillage")
        else:
            say("carte NON enregistree")
    else:
        say("toujours pas de RecastNavMesh apres RebuildNavigation")
    say("FIN")
except Exception:
    say(traceback.format_exc()[:2000])
    say("ECHEC")

try:
    unreal.SystemLibrary.quit_editor()
except Exception:
    pass
