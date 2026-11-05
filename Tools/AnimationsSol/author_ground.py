"""Author ALS ground loops from the original rig. Blender --background --python ... -- [clip].

Coordinates inside the imported armature are centimetres; the original parent converts to metres.
Only bone poses are keyed. Rest transforms, weights, names and hierarchy are left intact.
"""
import bpy
import math
import json
import sys
from pathlib import Path
from mathutils import Vector, Matrix, Quaternion

PROJECT = Path("D:/UEProjets/Impact")
OUT = PROJECT / "Saved/AnimationsSol"
SOURCE = PROJECT / "ArtSource/Animations/Reference/SKM_Als.fbx"
CLIPS = ["Recroqueville_Au_Sol", "Halete_Au_Sol", "Tient_Jambe_Au_Sol"]
N = 150
V = Vector


def rotation(x=0, y=0, z=0):
    return (Quaternion((0, 0, 1), math.radians(z))
            @ Quaternion((0, 1, 0), math.radians(y))
            @ Quaternion((1, 0, 0), math.radians(x)))


def pulse(t, start, peak, finish):
    """Zero velocity at both ends, unequal attack/recovery durations."""
    if t <= start or t >= finish:
        return 0.
    u = (t-start)/(peak-start) if t < peak else (finish-t)/(finish-peak)
    return u*u*u*(10 + u*(-15 + 6*u))


def build(name):
    folder = OUT / name
    folder.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(SOURCE), automatic_bone_orientation=False, use_anim=False)
    scene = bpy.context.scene
    rig = next(o for o in scene.objects if o.type == "ARMATURE")
    mesh = next(o for o in scene.objects if o.type == "MESH")
    rest = {b.name: b.matrix_local.copy() for b in rig.data.bones}
    scene.frame_start = 0
    scene.frame_end = N
    scene.render.fps = 30
    scene.render.fps_base = 1
    rig.animation_data_clear()

    def update():
        bpy.context.view_layer.update()

    def orient(bone, q):
        b = rig.pose.bones[bone]
        b.matrix = Matrix.LocRotScale(b.head, q, V((1, 1, 1)))
        update()

    def rotate(bone, x=0, y=0, z=0):
        r = rest[bone].to_quaternion()
        rig.pose.bones[bone].rotation_quaternion = r.inverted() @ rotation(x, y, z) @ r

    def aim(bone, child, target):
        b = rig.pose.bones[bone]
        current = rig.pose.bones[child].head - b.head
        orient(bone, current.normalized().rotation_difference((target-b.head).normalized())
               @ b.matrix.to_quaternion())

    def solve(first, second, last, target, pole):
        start = rig.pose.bones[first].head.copy()
        a = (rest[second].translation-rest[first].translation).length
        b = (rest[last].translation-rest[second].translation).length
        vec = V(target)-start
        d = min(max(vec.length, abs(a-b)+.02), a+b-.02)
        direction = vec.normalized()
        bend = V(pole)-start
        bend -= direction*bend.dot(direction)
        if bend.length < .001:
            bend = direction.cross(V((0, 1, 0)))
        bend.normalize()
        along = (a*a-b*b+d*d)/(2*d)
        elbow = start+direction*along+bend*math.sqrt(max(0, a*a-along*along))
        aim(first, second, elbow)
        aim(second, last, start+direction*d)

    hand_axes = {}
    for side in ("l", "r"):
        h = rest["hand_"+side].translation
        f = (rest["middle_01_"+side].translation-h).normalized()
        n = ((rest["index_01_"+side].translation-h)
             .cross(rest["pinky_01_"+side].translation-h)).normalized()
        n = (n-f*n.dot(f)).normalized()
        hand_axes[side] = (Matrix((f, n.cross(f), n)).transposed(), n.cross(f))

    def hand(side, forward, normal, curl=12):
        f = V(forward).normalized()
        n = V(normal)
        n = (n-f*n.dot(f)).normalized()
        change = Matrix((f, n.cross(f), n)).transposed() @ hand_axes[side][0].inverted()
        orient("hand_"+side, (change @ rest["hand_"+side].to_3x3()).to_quaternion())
        for digit in ("index", "middle", "ring", "pinky"):
            for index, weight in ((1, 1), (2, .7), (3, .4)):
                bone = f"{digit}_{index:02d}_{side}"
                r = rest[bone].to_quaternion()
                rig.pose.bones[bone].rotation_quaternion = (
                    r.inverted() @ Quaternion(hand_axes[side][1],
                    math.radians(curl*weight*(1 if side=="l" else -1))) @ r)
        # The mannequin's rest thumb is opposed well below the palm. Spread it into the
        # palm plane for the supporting hand so it cannot spear the floor.
        if side == "l" and curl == 0:
            update()
            thumb_dir = change @ (rest["thumb_02_l"].translation-rest["thumb_01_l"].translation)
            thumb_dir = (thumb_dir-n*thumb_dir.dot(n)).normalized()
            for index in (1,2,3):
                bone = rig.pose.bones[f"thumb_{index:02d}_l"]
                old_axis = bone.matrix.to_quaternion() @ V((0,1,0))
                orient(bone.name,old_axis.rotation_difference(thumb_dir) @ bone.matrix.to_quaternion())

    def anchor(bone, offset):
        """Anatomical XYZ offset, following the current bone rather than the world."""
        b = rig.pose.bones[bone]
        q = b.matrix.to_quaternion() @ rest[bone].to_quaternion().inverted()
        return b.head + q @ V(offset)

    groups = {g.index:g.name for g in mesh.vertex_groups}
    vertex_groups = {v.index:groups[max(v.groups,key=lambda g:g.weight).group]
                     for v in mesh.data.vertices if v.groups}
    support_body = {"pelvis","spine_01","spine_02","spine_03","head",
                    "thigh_l","thigh_r","calf_l","calf_r"}
    metrics = []
    for frame in range(N+1):
        scene.frame_set(frame)
        for b in rig.pose.bones:
            b.rotation_mode = "QUATERNION"
            b.matrix_basis = Matrix.Identity(4)
        t = frame / 30
        contraction = pulse(t, .3, 1.0, 2.4) + .67*pulse(t, 2.9, 3.45, 4.65)
        delayed = pulse(t, .4, 1.12, 2.5) + .67*pulse(t, 3.02, 3.57, 4.75)
        breath = (pulse(t, .12, .48, 1.1) + .8*pulse(t, 1.3, 1.65, 2.18)
                  + 1.1*pulse(t, 2.45, 2.80, 3.4) + .65*pulse(t, 3.8, 4.17, 4.85))
        pelvis = rig.pose.bones["pelvis"]
        if name == CLIPS[0]:
            base = rotation(0, -90, 0)
            origin = V((0, 0, 16))
            pelvis.matrix = Matrix.LocRotScale(origin, base @ rest["pelvis"].to_quaternion(), V((1,1,1)))
            rotate("spine_01", 12+6*contraction, 9)
            rotate("spine_02", 13+6*contraction, 4)
            rotate("spine_03", 10+3*delayed, -12)
            rotate("neck_01", 9+3*delayed, -30)
            rotate("head", 5+2*delayed, -22, -3)
            update()
            # Femurs forward and down in the body's anatomical frame; calves fold back.
            for side, sign in (("l", 1), ("r", -1)):
                hip = rig.pose.bones["thigh_"+side].head.copy()
                forward = base @ V((0, -1, 0))
                down = base @ V((0, 0, -1))
                lateral = base @ V((sign, 0, 0))
                knee = hip + forward*(36+contraction*1.2) + down*19 + lateral*1.5
                foot = hip + forward*13 + down*51 + lateral*2
                solve("thigh_"+side, "calf_"+side, "foot_"+side, foot, knee)
                orient("foot_"+side, base @ rotation(15) @ rest["foot_"+side].to_quaternion())
            for side, sign in (("l", 1), ("r", -1)):
                target = anchor("spine_01", (sign*10, -18, 3+(3 if side=="l" else -4)))
                pole = anchor("spine_01", (28, -20, 10) if side=="l" else (-5, -38, 10))
                solve("upperarm_"+side, "lowerarm_"+side, "hand_"+side, target, pole)
                q = rig.pose.bones["spine_01"].matrix.to_quaternion() @ rest["spine_01"].to_quaternion().inverted()
                hand(side, q @ V((-sign*.85, -.1, -.4)), q @ V((0, -sign, 0)), 10+4*delayed)
        elif name == CLIPS[1]:
            base = rotation(-90, 0, -4)
            origin = V((0, 0, 14))
            pelvis.matrix = Matrix.LocRotScale(origin, base @ rest["pelvis"].to_quaternion(), V((1,1,1)))
            rotate("spine_01", 9+.7*breath)
            rotate("spine_02", -2+1.8*breath)
            rotate("spine_03", 2+1.2*breath)
            rotate("neck_01", -18-1.6*breath, 0, -7)
            rotate("head", -12-1.3*breath, 0, -5)
            rotate("clavicle_l", -3*breath, 0, -2.5*breath)
            rotate("clavicle_r", -2.3*breath, 0, 2.1*breath)
            update()
            for side, sign in (("l", 1), ("r", -1)):
                solve("thigh_"+side, "calf_"+side, "foot_"+side,
                      (sign*15, -69 if side=="l" else -77, 15),
                      (sign*20, -31, 47 if side=="l" else 25))
                orient("foot_"+side, rotation(-35, sign*8) @ rest["foot_"+side].to_quaternion())
            target = anchor("spine_03", (-9, -18, -3))
            solve("upperarm_r", "lowerarm_r", "hand_r", target, (-36, 22, 8))
            hand("r", (.75, .55, -.12), (0, 0, -1), 9+3*breath)
            solve("upperarm_l", "lowerarm_l", "hand_l", (37, 16, 4), (45, 35, 18))
            hand("l", (.25, -.95, 0), (0, 0, 1), 0)
        else:
            attempt = pulse(t, .7, 1.35, 2.5)
            wince = pulse(t, 1.1, 1.55, 3.0) + .35*pulse(t, 3.4, 3.9, 4.8)
            base = rotation(-25, 18, -3)
            origin = V((0, 0, 11))
            pelvis.matrix = Matrix.LocRotScale(origin, base @ rest["pelvis"].to_quaternion(), V((1,1,1)))
            rotate("spine_01", 6+4*wince, 0, -2)
            rotate("spine_02", 9+5*wince)
            rotate("spine_03", 6+3*wince)
            rotate("neck_01", 18+3*wince, 0, -6)
            rotate("head", 13+2*wince)
            update()
            for side, sign in (("l", 1), ("r", -1)):
                foot = V((sign*16, -74 if side=="r" else -79, 12))
                if side == "r":
                    foot.y += 1.5*attempt
                    foot.z += .7*attempt
                solve("thigh_"+side, "calf_"+side, "foot_"+side, foot,
                      (sign*17, -36, 37 if side=="r" else 26))
                orient("foot_"+side, rotation(-60, sign*4) @ rest["foot_"+side].to_quaternion())
            thigh = rig.pose.bones["thigh_r"].head
            knee = rig.pose.bones["calf_r"].head
            target = thigh.lerp(knee, .47) + V((-3, 0, 12))
            solve("upperarm_r", "lowerarm_r", "hand_r", target, (-39, -7, 38))
            hand("r", (knee-thigh).normalized(), (0, 0, -1), 10+4*wince)
            solve("upperarm_l", "lowerarm_l", "hand_l", (33, 13, 3), (38, 3, 30))
            hand("l", (.55, -.85, 0), (0, 0, 1), 0)
        update()
        for side in ("l", "r"):
            rig.pose.bones["ik_foot_"+side].matrix = rig.pose.bones["foot_"+side].matrix.copy()
        rig.pose.bones["ik_hand_gun"].matrix = rig.pose.bones["hand_r"].matrix.copy()
        update()
        for side in ("l", "r"):
            rig.pose.bones["ik_hand_"+side].matrix = rig.pose.bones["hand_"+side].matrix.copy()
        update()
        for b in rig.pose.bones:
            for path in ("location", "rotation_quaternion", "scale"):
                b.keyframe_insert(data_path=path, frame=frame, group=b.name)
        ev = mesh.evaluated_get(bpy.context.evaluated_depsgraph_get())
        geometry = ev.to_mesh()
        minimum = min((ev.matrix_world @ v.co).z for v in geometry.vertices)
        body_minimum = min((ev.matrix_world @ v.co).z for v in geometry.vertices
                           if vertex_groups.get(v.index) in support_body)
        ev.to_mesh_clear()
        metrics.append({"frame": frame, "min_z_cm": minimum*100,
                        "body_min_z_cm":body_minimum*100,
                        "pelvis_cm": list(pelvis.head)})

    action = rig.animation_data.action
    action.name = name
    for layer in action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                for fc in bag.fcurves:
                    for k in fc.keyframe_points:
                        k.interpolation = "LINEAR"
    # One fixed height correction for the whole clip, not a per-frame floor follower.
    # Position the trunk on the ground; distal extremities get their own contact solves below.
    # A low heel or an opposed thumb must not lift the entire torso into a bridge.
    lift_cm = -min(m["body_min_z_cm"] for m in metrics)+.1

    def clearance(names):
        ev = mesh.evaluated_get(bpy.context.evaluated_depsgraph_get())
        geo = ev.to_mesh()
        result = min((ev.matrix_world @ v.co).z*100 for v in geo.vertices
                     if vertex_groups.get(v.index) in names)
        ev.to_mesh_clear()
        return result

    for frame in range(N+1):
        scene.frame_set(frame)
        mat = rig.pose.bones["pelvis"].matrix.copy()
        mat.translation.z += lift_cm
        rig.pose.bones["pelvis"].matrix = mat
        rig.pose.bones["pelvis"].keyframe_insert(data_path="location", frame=frame, group="pelvis")
        update()
        # Anchor the supporting heel/sole, not the ankle centre. Rotation and leg length stay intact.
        contact_sides = ("r",) if name == CLIPS[0] else ("l","r")
        for side in contact_sides:
            target = rig.pose.bones["foot_"+side].head.copy()
            target.z -= clearance({"foot_"+side,"ball_"+side})-.15
            pole = rig.pose.bones["calf_"+side].head.copy()
            q = rig.pose.bones["foot_"+side].matrix.to_quaternion().copy()
            solve("thigh_"+side,"calf_"+side,"foot_"+side,target,pole)
            orient("foot_"+side,q)
        if name != CLIPS[0]:
            # Palm support remains planted through the contractions.
            target = rig.pose.bones["hand_l"].head.copy()
            palm_groups = {g for g in groups.values() if g.endswith("_l") and
                           g.startswith(("hand_","thumb_","index_","middle_","ring_","pinky_"))}
            target.z -= clearance(palm_groups)-.15
            pole = rig.pose.bones["lowerarm_l"].head.copy()
            q = rig.pose.bones["hand_l"].matrix.to_quaternion().copy()
            solve("upperarm_l","lowerarm_l","hand_l",target,pole)
            orient("hand_l",q)
        if name == CLIPS[2]:
            thigh = rig.pose.bones["thigh_r"].head.copy()
            knee = rig.pose.bones["calf_r"].head.copy()
            solve("upperarm_r","lowerarm_r","hand_r",thigh.lerp(knee,.47)+V((-3,0,12)),
                  (-39,-7,38+lift_cm))
            hand("r",(knee-thigh).normalized(),(0,0,-1),10+4*pulse(frame/30,1.1,1.55,3.0))
        update()
        for side in ("l", "r"):
            rig.pose.bones["ik_foot_"+side].matrix = rig.pose.bones["foot_"+side].matrix.copy()
        rig.pose.bones["ik_hand_gun"].matrix = rig.pose.bones["hand_r"].matrix.copy()
        update()
        for side in ("l", "r"):
            rig.pose.bones["ik_hand_"+side].matrix = rig.pose.bones["hand_"+side].matrix.copy()
        update()
        for b in rig.pose.bones:
            for prop in ("location", "rotation_quaternion", "scale"):
                b.keyframe_insert(data_path=prop, frame=frame, group=b.name)
    (folder/"author_metrics.json").write_text(json.dumps({"clip":name, "ground_lift_cm":lift_cm,
          "frames_before_lift":metrics}, indent=2))
    scene.frame_set(0)

    mat = bpy.data.materials.new("Mannequin")
    mat.diffuse_color = (.35, .53, .59, 1)
    mesh.data.materials.clear()
    mesh.data.materials.append(mat)
    for face in mesh.data.polygons:
        face.material_index = 0
    bpy.ops.mesh.primitive_plane_add(size=200, location=(0,0,-.003))
    floor = bpy.context.object
    floor.name = "Preview_floor"
    material = bpy.data.materials.new("Ground")
    material.diffuse_color = (.18, .21, .25, 1)
    floor.data.materials.append(material)
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x = 800
    scene.render.resolution_y = 600
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    shading = scene.display.shading
    shading.light = "STUDIO"
    shading.color_type = "MATERIAL"
    shading.show_shadows = True
    shading.show_cavity = True
    shading.cavity_type = "BOTH"
    shading.show_specular_highlight = True
    scene.world = bpy.data.worlds.new("PreviewWorld")
    scene.world.color = (.055, .065, .085)
    scene.view_settings.view_transform = "Standard"
    positions = [("three_quarter", (2.5,-3.4,2.1)), ("profile",(3.3, .1, 1.1))]
    if name == CLIPS[0]:
        positions = [("three_quarter",(-2.1,-3.5,2.4)), ("profile",(.1,-3.8,.8))]
    for label, loc in positions:
        data = bpy.data.cameras.new(label)
        cam = bpy.data.objects.new(label, data)
        scene.collection.objects.link(cam)
        cam.location = loc
        target = V((0,-.1,.3))
        cam.rotation_euler = (target-cam.location).to_track_quat("-Z","Y").to_euler()
        data.type = "ORTHO"
        data.ortho_scale = 2.15
    scene.camera = bpy.data.objects["three_quarter"]
    bpy.ops.wm.save_as_mainfile(filepath=str(folder/(name+".blend")))
    for view in ("three_quarter", "profile"):
        scene.camera = bpy.data.objects[view]
        for f in (0, 30, 45, 105):
            scene.frame_set(f)
            scene.render.filepath = str(folder/f"{view}_{f:03d}.png")
            bpy.ops.render.render(write_still=True)
    print("AUTHORED", name, "lift_cm", lift_cm, flush=True)


if __name__ == "__main__":
    requested = sys.argv[sys.argv.index("--")+1:] if "--" in sys.argv else CLIPS
    for clip in requested:
        assert clip in CLIPS
        build(clip)
