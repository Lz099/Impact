import bpy, json
from pathlib import Path

root = Path("D:/UEProjets/Impact/Saved/AnimationsSol")
result = {}
for folder in root.iterdir():
    if not folder.is_dir() or not (folder/(folder.name+".blend")).exists():
        continue
    bpy.ops.wm.open_mainfile(filepath=str(folder/(folder.name+".blend")))
    mesh = next(o for o in bpy.context.scene.objects if o.type=="MESH" and o.name!="Preview_floor")
    groups = {g.index:g.name for g in mesh.vertex_groups}
    assigned = {v.index:groups[max(v.groups, key=lambda g:g.weight).group] for v in mesh.data.vertices if v.groups}
    frames = []
    for frame in range(151):
        bpy.context.scene.frame_set(frame)
        ev = mesh.evaluated_get(bpy.context.evaluated_depsgraph_get())
        geo = ev.to_mesh()
        minima = {}
        for v in geo.vertices:
            group = assigned.get(v.index,"")
            z = (ev.matrix_world@v.co).z*100
            minima[group] = min(minima.get(group, 1e6),z)
        ev.to_mesh_clear()
        frames.append({"frame":frame,"all_min_cm":round(min(minima.values()),3),
                      "lowest_group":min(minima,key=minima.get),"minimum_cm":{k:round(v,2) for k,v in minima.items()
                      if k in ("pelvis","spine_01","spine_03","head","upperarm_r","lowerarm_r",
                               "upperarm_l","lowerarm_l","hand_r","hand_l","thigh_r","thigh_l","calf_r","foot_r","foot_l")}})
    result[folder.name] = frames
(root/"contacts.json").write_text(json.dumps(result,indent=2))
print(json.dumps(result,indent=2))
