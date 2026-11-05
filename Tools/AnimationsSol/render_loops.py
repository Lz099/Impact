"""Render both views of each authored loop, 15 preview frames/s; animation itself is 30 fps."""
import bpy
from pathlib import Path

root = Path("D:/UEProjets/Impact/Saved/AnimationsSol")
for name in ("Recroqueville_Au_Sol","Halete_Au_Sol","Tient_Jambe_Au_Sol"):
    folder = root/name
    bpy.ops.wm.open_mainfile(filepath=str(folder/(name+".blend")))
    scene = bpy.context.scene
    scene.render.resolution_x = 560
    scene.render.resolution_y = 420
    for view in ("three_quarter","profile"):
        scene.camera = bpy.data.objects[view]
        for frame in range(0,150,2):
            scene.frame_set(frame)
            scene.render.filepath = str(folder/"frames"/f"{view}_{frame:03d}.png")
            bpy.ops.render.render(write_still=True)
    print("LOOP_RENDERED",name,flush=True)
