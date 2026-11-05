"""Original Impact equipment. Run in Blender 5.2 background mode.

blender --background --factory-startup --python Tools/Equipment/BuildEquipment.py
Only writes ArtSource/Equipment and Saved/Equipment (notes and renders).
Rebuilds archive their previous generated outputs; no other project files are read
or modified. All authored coordinates are the requested UE-local centimeters.
"""
from pathlib import Path
import datetime
import hashlib
import json
import math
import shutil
import sys

import bpy
import bmesh
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'ArtSource/Equipment'
QA = ROOT / 'Saved/Equipment'
DEST = '/Game/Impact/Equipment'
NAMES = ['SM_VestLight', 'SM_VestMedium', 'SM_VestHeavy', 'SM_Helmet',
         'SM_HelmetNight', 'SM_HelmetThermal', 'SM_HelmetLamp']
PALETTE = {
    'Olive': ((.095, .118, .064, 1), .08, .78),
    'Charcoal': ((.025, .033, .032, 1), .22, .61),
    'Webbing': ((.047, .057, .035, 1), 0, .92),
    'Edge': ((.145, .164, .101, 1), .05, .82),
    'Hardware': ((.075, .087, .082, 1), .72, .34),
    'Lens': ((.018, .15, .16, 1), .55, .16),
    'LampGlass': ((.58, .64, .48, 1), .35, .2),
}
MATERIALS = {}
MODELS = {}
RECORDS = []


def archive_outputs():
    OUT.mkdir(parents=True, exist_ok=True)
    QA.mkdir(parents=True, exist_ok=True)
    existing = [OUT / 'Equipment.blend', OUT / 'manifest.json']
    existing += [OUT / 'FBX' / (n + '.fbx') for n in NAMES]
    existing += [QA / n for n in ('models_notes.md', 'contact_sheet.png',
                                'helmet_cavity.png', 'vest_clearance.png')]
    existing = [p for p in existing if p.exists()]
    if existing:
        stamp = datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f')
        for source in existing:
            target = OUT / 'History' / stamp / source.relative_to(ROOT)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    (OUT / 'FBX').mkdir(exist_ok=True)


def create_materials():
    for key, (color, metallic, roughness) in PALETTE.items():
        mat = bpy.data.materials.new('M_Equipment_' + key)
        mat.diffuse_color = color
        mat.use_nodes = True
        shader = mat.node_tree.nodes.get('Principled BSDF')
        shader.inputs['Base Color'].default_value = color
        shader.inputs['Metallic'].default_value = metallic
        shader.inputs['Roughness'].default_value = roughness
        MATERIALS[key] = mat


class Builder:
    def __init__(self, name):
        self.name = name
        self.v, self.f, self.m, self.parts = [], [], [], []

    def solid(self, verts, faces, material, label):
        start = len(self.v)
        self.v.extend([tuple(p) for p in verts])
        self.f.extend([tuple(start + i for i in f) for f in faces])
        self.m.extend([material] * len(faces))
        self.parts.append({'name': label, 'first_vertex': start,
                           'vertex_count': len(verts), 'material': material})

    def box(self, center, size, material, label, bevel=.12):
        # BMesh bevel is baked; the delivered assets contain no modifiers.
        bm = bmesh.new()
        bmesh.ops.create_cube(bm, size=1)
        for v in bm.verts:
            v.co = Vector(tuple(v.co[i] * size[i] for i in range(3)))
        if bevel:
            bmesh.ops.bevel(bm, geom=list(bm.edges), offset=min(bevel, min(size)*.24),
                            segments=1, affect='EDGES')
        bm.verts.ensure_lookup_table()
        bm.verts.index_update()
        self.solid([v.co + Vector(center) for v in bm.verts],
                   [[v.index for v in f.verts] for f in bm.faces], material, label)
        bm.free()

    def panel(self, outline, x1, x2, material, label, bevel=.18):
        # A perimeter bevel retains large, deliberately low-poly planar faces.
        n = len(outline)
        cy = sum(p[0] for p in outline) / n
        cz = sum(p[1] for p in outline) / n
        verts = []
        for x, inset in [(x1, .025), (x1+bevel, 0),
                         (x2-bevel, 0), (x2, .025)]:
            verts += [(x, y+(cy-y)*inset, z+(cz-z)*inset) for y, z in outline]
        faces = [tuple(reversed(range(n))), tuple(range(3*n, 4*n))]
        for ring in range(3):
            for i in range(n):
                j = (i+1) % n
                faces.append((ring*n+i, ring*n+j, (ring+1)*n+j, (ring+1)*n+i))
        self.solid(verts, faces, material, label)

    def strap(self, path, width, thick, width_axis, material, label):
        path = [Vector(p) for p in path]
        width_axis = Vector(width_axis).normalized()
        verts = []
        for i, p in enumerate(path):
            tangent = path[min(i+1, len(path)-1)] - path[max(0, i-1)]
            normal = tangent.normalized().cross(width_axis).normalized()
            verts += [p + width_axis*(a*width/2) + normal*(b*thick/2)
                      for a, b in [(-1,-1),(1,-1),(1,1),(-1,1)]]
        faces = [(3,2,1,0), tuple(range(len(verts)-4, len(verts)))]
        for i in range(len(path)-1):
            for j in range(4):
                faces.append((4*i+j,4*i+(j+1)%4,4*(i+1)+(j+1)%4,4*(i+1)+j))
        self.solid(verts, faces, material, label)

    def tube(self, start, end, radius, material, label, sides=12, inner=0):
        a, b = Vector(start), Vector(end)
        axis = (b-a).normalized()
        t = Vector((0,0,1)) if abs(axis.z) < .9 else Vector((0,1,0))
        u = axis.cross(t).normalized()
        v = axis.cross(u).normalized()
        verts = []
        rings = [(a,radius),(b,radius)]
        if inner:
            rings += [(a,inner),(b,inner)]
        for c, r in rings:
            verts += [c + r*(math.cos(i*math.tau/sides)*u + math.sin(i*math.tau/sides)*v)
                      for i in range(sides)]
        faces = []
        for i in range(sides):
            j = (i+1) % sides
            faces.append((i,j,sides+j,sides+i))
            if inner:
                faces.extend([(2*sides+j,2*sides+i,3*sides+i,3*sides+j),
                              (j,i,2*sides+i,2*sides+j),
                              (sides+i,sides+j,3*sides+j,3*sides+i)])
        if not inner:
            faces.extend([tuple(reversed(range(sides))),tuple(range(sides,2*sides))])
        self.solid(verts, faces, material, label)

    def finish(self, collection, description, mount=None):
        mesh = bpy.data.meshes.new(self.name)
        mesh.from_pydata(self.v, [], self.f)
        used = list(dict.fromkeys(self.m))
        for key in used:
            mesh.materials.append(MATERIALS[key])
        for poly, material in zip(mesh.polygons, self.m):
            poly.material_index = used.index(material)
        bm = bmesh.new()
        bm.from_mesh(mesh)
        bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
        bmesh.ops.triangulate(bm, faces=list(bm.faces))
        assert all(e.is_manifold for e in bm.edges), self.name + ': open geometry'
        assert all(f.calc_area() > 1.e-8 for f in bm.faces), self.name + ': zero area'
        bm.to_mesh(mesh)
        bm.free()
        mesh.update()
        obj = bpy.data.objects.new(self.name, mesh)
        collection.objects.link(obj)
        bpy.ops.object.select_all(action='DESELECT')
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=.018)
        bpy.ops.object.mode_set(mode='OBJECT')
        obj['units'] = 'centimeters; X forward, Y right, Z up'
        obj['origin'] = 'midchest' if 'Vest' in self.name else 'head center'
        obj['description'] = description
        obj['collision'] = 'None'
        MODELS[self.name] = obj
        lo = [min(v.co[i] for v in mesh.vertices) for i in range(3)]
        hi = [max(v.co[i] for v in mesh.vertices) for i in range(3)]
        row = {'name': self.name, 'asset_path': DEST+'/'+self.name,
               'object_path': DEST+'/'+self.name+'.'+self.name,
               'fbx': 'ArtSource/Equipment/FBX/'+self.name+'.fbx',
               'description': description, 'origin': obj['origin'],
               'bounds_cm': [[round(v,5) for v in lo], [round(v,5) for v in hi]],
               'dimensions_xyz_cm': [round(hi[i]-lo[i],5) for i in range(3)],
               'vertices': len(mesh.vertices), 'triangles': len(mesh.polygons),
               'material_slots': [m.name for m in mesh.materials],
               'local_transform': {'translation':[0,0,0], 'rotation':[0,0,0], 'scale':[1,1,1]},
               'closed_components': True, 'parts': self.parts}
        if mount:
            row['mount_cm'] = mount
        RECORDS.append(row)
        return obj


def vest(level, collection):
    name = NAMES[level]
    b = Builder(name)
    half = [15.5,17.3,19.0][level]
    bottom = [-19.7,-20.4,-21][level]
    # Deep upper cut-outs leave the shoulder joint free of rigid armor.
    outline = [(-half*.8,bottom),(half*.8,bottom),(half,bottom+3),
               (half,1.5),(11.2,12),(9.4,15.0),(5.0,15.0),(3.2,12.8),
               (-3.2,12.8),(-5,15.0),(-9.4,15.0),(-11.2,12),(-half,1.5),
               (-half,bottom+3)]
    for front in (True,False):
        x1,x2 = (9.8,11.5) if front else (-12.1,-10.3)
        b.panel(outline,x1,x2,'Olive','front carrier' if front else 'back carrier')
        # Thin edge binding sits within the panel silhouette.
        x = x2+.1 if front else x1-.1
        pts = [(x,y*.973,z*.973) for y,z in outline]
        b.strap(pts+[pts[0]],.23,.18,(1,0,0),'Edge','stitched perimeter binding')
        if level:
            face = [(y*.86,z*.84) for y,z in outline]
            a,c = (11.55,12.15) if front else (-12.75,-12.15)
            b.panel(face,a,c,'Charcoal','segmented plate face',.1)
        # Back handle is a flexible webbing arch, not a rigid shoulder piece.
        if not front:
            b.strap([(-12.35,-4,10),(-13.15,-3.5,12),(-13.15,3.5,12),
                     (-12.35,4,10)],1.0,.35,(0,0,1),'Webbing','drag handle')
    for side in (-1,1):
        for z in ([-10,-3] if level == 0 else [-10]):
            width = [3,10,15][level]
            p = [(10.3,side*12,z),(8,side*18,z),(4,side*20.65,z),
                 (-4,side*20.65,z),(-8,side*18,z),(-11,side*12,z)]
            b.strap(p,width,.7,(0,0,1),'Webbing','flexible side cummerbund')
        # Width <= 3.2 cm; only webbing spans the top of the shoulder.
        b.strap([(10.2,side*8.2,13.4),(8,side*8.8,18),
                 (3,side*9.0,20.65),(-3,side*9.0,20.65),
                 (-8,side*8.8,18),(-10.8,side*8.2,13.4)],
                3.2,.7,(0,1,0),'Webbing','flexible shoulder strap')
        b.box((11.8,side*8.2,12.1),(.6,3.2,2.5),'Hardware','low front strap buckle',.15)
        b.box((12.15,side*8.2,12.1),(.2,1.7,1.2),'Webbing','buckle webbing insert',.02)
        if level == 2:
            # Side armor stays under the arm, ending below z=-2.5 cm.
            b.strap([(9,side*17,-11),(5,side*19.5,-11),
                     (-5,side*19.5,-11),(-9,side*17,-11)],
                    16.0,.8,(0,0,1),'Olive','lower lateral armor')
            for z in (-15,-10,-5):
                b.strap([(8,side*18,-0+z),(4,side*20.1,z),(-4,side*20.1,z),
                         (-8,side*18,z)],.8,.2,(0,0,1),'Edge','side attachment loop')
    # Tier markings and horizontal sewn webbing remain shallow enough for 27 cm depth.
    front_x = 11.8 if level == 0 else 12.45
    back_x = -12.5 if level == 0 else -13.1
    rows = [-8,-3] if level == 0 else [-11,-6,-1,4]
    for x in (front_x,back_x):
        for z in rows:
            b.box((x,0,z),(.32,half*1.45,1.15),'Webbing','horizontal attachment webbing',.06)
            for y in (-8,-4,0,4,8):
                b.box((x+(.2 if x>0 else -.2),y,z),(.12,.19,1.2),
                      'Edge','bar tack',.02)
    b.box((12.55 if level else 12.15,0,8.3),(.35,6.7,2.5),
          'Charcoal','blank identification patch')
    for i in range(level+1):
        b.box((12.8 if level else 12.4,(i-level/2)*1.3,8.3),(.16,.48,1.3),
              'Edge','tier mark',.02)
    if level >= 1:
        for y in (-7,0,7):
            b.box((12.85,y,-13.8),(.95,5.8,7.5),'Olive','flat utility pocket',.2)
            b.box((13.4,y,-10.6),(.2,5.6,1.5),'Edge','pocket flap',.06)
    if level == 2:
        # Broad lower abdominal coverage and a large upper plate distinguish heavy.
        lower=[(-13.5,-20.9),(13.5,-20.9),(16,-17.8),(16,-12),(-16,-12),(-16,-17.8)]
        b.panel(lower,12.5,13.4,'Charcoal','lower abdominal armor',.12)
        for side in (-1,1):
            facet=[(side*2,1.7),(side*12,1.7),(side*12,6),(side*8.5,11),(side*2,11)]
            b.panel(facet,12.25,13.1,'Olive','upper split breastplate',.12)
    desc = [
        'Light / open sides, twin webbing bands, slim front and back carriers.',
        'Medium / inset plates, broad soft cummerbund, three shallow utility pockets.',
        'Heavy / wider torso plates, lower lateral and abdominal armor, split breastplate; no pauldrons.',
    ][level]
    return b.finish(collection,desc)


def helmet(collection):
    b = Builder('SM_Helmet')
    # Elliptical head clearance at the rim: X radius 12.5, Y radius 10.5.
    # A cut that rises towards X+ opens the brow/face; nothing caps the bottom.
    n, rows = 24, 7
    verts=[]
    for inner in (False, True):
        rx,ry,rz = (13.3,11.3,14) if not inner else (12.5,10.5,13.2)
        for r in range(rows):
            t=r/rows
            for j in range(n):
                a=math.tau*j/n
                zrim=3.4*max(0,math.cos(a))**3
                p0=math.asin(zrim/rz)
                phi=p0+(math.pi/2-p0)*t
                verts.append((rx*math.cos(phi)*math.cos(a),
                              ry*math.cos(phi)*math.sin(a),rz*math.sin(phi)))
        verts.append((0,0,rz))
    faces=[]
    stride=n*rows+1
    for k in range(2):
        offset=k*stride
        for r in range(rows-1):
            for j in range(n):
                q=(j+1)%n
                faces.append((offset+r*n+j,offset+r*n+q,
                              offset+(r+1)*n+q,offset+(r+1)*n+j))
        for j in range(n):
            faces.append((offset+(rows-1)*n+j,offset+(rows-1)*n+(j+1)%n,offset+n*rows))
    for j in range(n):
        q=(j+1)%n
        faces.append((j,q,stride+q,stride+j))
    b.solid(verts,faces,'Olive','hollow bowl / 0.8 cm shell')
    # Edge binding follows only the open shell perimeter.
    rim=[]
    for j in range(n+1):
        a=math.tau*(j%n)/n
        z=3.4*max(0,math.cos(a))**3
        phi=math.asin(z/14)
        rim.append((13.32*math.cos(phi)*math.cos(a),11.32*math.cos(phi)*math.sin(a),z+.08))
    b.strap(rim,.34,.25,(0,0,1),'Charcoal','rubber rim')
    for side in (-1,1):
        b.strap([(3,side*10.55,3.2),(-1,side*11.05,3.2),(-6,side*9.8,3.2)],
                1.5,.6,(0,0,1),'Charcoal','side accessory rail')
        for x,y in [(2.2,10.95),(-3,10.9)]:
            b.tube((x,side*y,3.2),(x,side*(y+.35),3.2),.35,'Hardware','rail fastener',8)
        # Chin straps are lateral ribbons, leaving both eyes and mouth unobstructed.
        b.strap([(4,side*9.8,.9),(3,side*9.0,-3.5),(2,side*5.6,-10.4)],
                .85,.22,(1,0,0),'Webbing','front chin webbing')
        b.strap([(-7,side*8.1,.4),(-6,side*8.3,-3),
                 (2,side*5.6,-10.4)],.85,.22,(1,0,0),'Webbing','rear chin webbing')
    b.strap([(2,-5.6,-10.4),(3.5,-3,-11.0),(3.5,3,-11.0),(2,5.6,-10.4)],
            1.45,.32,(1,0,0),'Charcoal','chin cup strap')
    b.box((2.25,6.05,-9.5),(1.2,1.6,1.1),'Hardware','chin strap buckle',.12)
    # Front plate covers mount datum (12,0,5), bevelled to sit on the curved brow.
    b.panel([(-2,3.5),(2,3.5),(2,7.2),(1,8.4),(-1,8.4),(-2,7.2)],
            11.9,12.9,'Charcoal','front mount shroud',.12)
    b.box((13.05,0,5.6),(.4,1.65,2.1),'Hardware','front dovetail')
    for y in (-1.4,1.4):
        b.tube((12.85,y,6.8),(13.15,y,6.8),.24,'Hardware','mount screw',8)
    return b.finish(collection,'Open elliptical shell, high brow, open bottom; lateral chin straps, front mount and side rails.')


def front_bracket(b):
    b.box((13.4,0,5), (1.2,1.6,2.4),'Hardware','front mount tongue')
    b.strap([(13.6,0,5.2),(15.6,0,5.2),(16.5,0,1.4)],
            1.55,.7,(0,1,0),'Charcoal','folding bridge')
    b.tube((15.5,-1.2,4.5),(15.5,1.2,4.5),.7,'Hardware','bridge hinge',10)


def accessories(collection):
    b=Builder('SM_HelmetNight')
    front_bracket(b)
    b.box((16.5,0,.5),(2.4,10.5,1.5),'Charcoal','binocular bridge',.24)
    for side in (-1,1):
        y=side*3.6
        b.tube((13.7,y,-1.2),(20.7,y,-1.2),2.05,'Charcoal','night tube',12)
        b.tube((16,y,-1.2),(18.7,y,-1.2),2.23,'Olive','tube housing',12)
        for x in (14.2,19.8):
            b.tube((x,y,-1.2),(x+.5,y,-1.2),2.3,'Hardware','focus collar',12)
        b.tube((20.6,y,-1.2),(21.4,y,-1.2),2.22,'Charcoal','open objective bezel',12,1.7)
        b.tube((21.02,y,-1.2),(21.09,y,-1.2),1.68,'Lens','recessed objective',12)
        b.tube((13.1,y,-1.2),(13.8,y,-1.2),2.15,'Webbing','eyecup',12,1.55)
        b.tube((13.25,y,-1.2),(13.33,y,-1.2),1.5,'Lens','rear optic',12)
    b.box((17,0,1.55),(2.4,2.5,.8),'Olive','control housing',.15)
    b.tube((17,0,1.8),(17,0,2.4),.7,'Hardware','power dial',10)
    b.finish(collection,'Dual twelve-sided night optics with recessed teal lenses and shared folding front mount.',[12,0,5])
    b=Builder('SM_HelmetThermal')
    front_bracket(b)
    b.box((17.9,0,-.8),(7.4,5.9,5.5),'Olive','single thermal sensor housing',.52)
    b.box((18,0,1.95),(4.5,3.7,.65),'Charcoal','thermal top cover',.18)
    b.tube((21.1,0,-.8),(22.0,0,-.8),2.65,'Charcoal','sensor bezel',12,2.05)
    b.tube((21.6,0,-.8),(21.7,0,-.8),2.0,'Lens','thermal sensor window',12)
    b.tube((13.6,0,-.8),(14.6,0,-.8),2.1,'Webbing','monocular eyecup',12,1.55)
    b.tube((13.9,0,-.8),(14.0,0,-.8),1.5,'Lens','rear optic',12)
    for x in (16,17.2,18.4,19.6):
        b.box((x,3.0,-.9),(.42,.28,3.1),'Charcoal','cooling fin',.05)
    b.finish(collection,'Single box sensor, faceted objective and cooling fins; same helmet-local front mount.',[12,0,5])
    b=Builder('SM_HelmetLamp')
    b.box((0,11.5,3),(4.2,1.6,1.6),'Hardware','right rail clamp',.2)
    b.box((0,12.7,3),(2.5,1.2,2.1),'Charcoal','lamp saddle',.18)
    b.tube((-4.1,14,3),(3.4,14,3),1.5,'Charcoal','flashlight body',12)
    for x in (-2.7,-1.8,-.9):
        b.tube((x,14,3),(x+.32,14,3),1.65,'Olive','grip rib',12)
    b.tube((3.0,14,3),(5.6,14,3),1.95,'Olive','flashlight head',12)
    b.tube((5.3,14,3),(6,14,3),2.05,'Hardware','open lamp bezel',12,1.57)
    b.tube((5.62,14,3),(5.7,14,3),1.53,'LampGlass','recessed lamp glass',12)
    b.tube((-4.8,14,3),(-4,14,3),1.25,'Webbing','tail switch',12)
    b.finish(collection,'Right-side rail flashlight; X+ beam axis, authored around Y+11 Z+3 attachment.',[0,11,3])


def verify_openings():
    checks={}
    for name,obj in MODELS.items():
        bm=bmesh.new(); bm.from_mesh(obj.data)
        tree=BVHTree.FromBMesh(bm)
        if 'Vest' in name:
            # Central neck-to-waist line must stay empty, with distinct front/back walls.
            assert tree.ray_cast(Vector((0,0,-40)),Vector((0,0,1)),80)[0] is None,name
            for xdir in (-1,1):
                hit=tree.ray_cast(Vector((0,0,0)),Vector((xdir,0,0)),30)[0]
                assert hit is not None and abs(hit.x)>9,name
            for side in (-1,1):
                assert tree.ray_cast(Vector((-30,side*19,10)),Vector((1,0,0)),60)[0] is None,name
            checks[name]={'neck_and_waist_open':True,'front_back_panels':True,
                          'upper_arm_clearance_y19_z10':True}
        elif name=='SM_Helmet':
            for direction in ((0,0,-1),(1,0,0)):
                assert tree.ray_cast(Vector((0,0,0)),Vector(direction),35)[0] is None,name
            hit=tree.ray_cast(Vector((0,0,0)),Vector((0,0,1)),30)[0]
            assert hit is not None and abs(hit.z-13.2)<.001,name
            checks[name]={'open_bottom':True,'open_face_x_positive':True,
                          'inner_crown_height_cm':13.2,'shell_thickness_cm':.8}
        bm.free()
    return checks


def fbx_export_and_verify(obj, row):
    from io_scene_fbx import parse_fbx
    from bpy_extras.io_utils import axis_conversion
    assert axis_conversion(to_forward='Y',to_up='Z').to_4x4()==Matrix.Identity(4)
    # FBX is right-handed. UE ConvertPos always negates Y, even KEEP_XYZ_AXES.
    # Reflect a COPY and correct winding; editable source retains requested Y+ right.
    copy=obj.copy(); copy.data=obj.data.copy()
    bpy.context.scene.collection.objects.link(copy)
    copy.data.transform(Matrix.Diagonal((1,-1,1,1)))
    bm=bmesh.new(); bm.from_mesh(copy.data)
    bmesh.ops.reverse_faces(bm,faces=list(bm.faces));bm.to_mesh(copy.data);bm.free()
    bpy.ops.object.select_all(action='DESELECT')
    copy.select_set(True);bpy.context.view_layer.objects.active=copy
    path=ROOT/row['fbx']
    bpy.ops.export_scene.fbx(filepath=str(path),use_selection=True,object_types={'MESH'},
        global_scale=1,apply_unit_scale=True,apply_scale_options='FBX_SCALE_ALL',
        axis_forward='Y',axis_up='Z',bake_space_transform=False,
        use_mesh_modifiers=True,mesh_smooth_type='FACE',use_triangles=True,
        use_tspace=True,add_leaf_bones=False,bake_anim=False,path_mode='AUTO')
    meshcopy=copy.data
    bpy.data.objects.remove(copy,do_unlink=True);bpy.data.meshes.remove(meshcopy)
    # Read the actual binary FBX. Identity transforms and centimeter metadata mean
    # raw (X,-Y,Z) FBX points must match the source vertices point-for-point.
    root,version=parse_fbx.parse(str(path))
    def children(node,key): return [c for c in node.elems if c.id==key]
    globals_=children(root,b'GlobalSettings')[0]
    props=children(globals_,b'Properties70')[0]
    values={p.props[0]:p.props[-1] for p in props.elems if p.id==b'P'}
    assert abs(values[b'UnitScaleFactor']-1)<1.e-6,values
    assert values[b'UpAxis']==2 and values[b'UpAxisSign']==1,values
    objects=children(root,b'Objects')[0]
    geoms=children(objects,b'Geometry')
    assert len(geoms)==1
    raw=children(geoms[0],b'Vertices')[0].props[0]
    assert len(raw)==len(obj.data.vertices)*3
    error=max(abs((raw[i*3+k]*(-1 if k==1 else 1))-v.co[k])
              for i,v in enumerate(obj.data.vertices) for k in range(3))
    assert error<.0001,(obj.name,error)
    for model in children(objects,b'Model'):
        for group in children(model,b'Properties70'):
            for prop in group.elems:
                if prop.id==b'P' and prop.props[0] in (b'Lcl Translation',b'Lcl Rotation',b'Lcl Scaling'):
                    expected=1 if prop.props[0]==b'Lcl Scaling' else 0
                    assert all(abs(v-expected)<1.e-6 for v in prop.props[-3:]),prop.props
    assert len(children(geoms[0],b'LayerElementUV'))>=1
    assert len(children(geoms[0],b'LayerElementNormal'))>=1
    row['fbx_sha256']=hashlib.sha256(path.read_bytes()).hexdigest()
    row['export_validation']={'fbx_version':version,'unit_scale_cm':1,
        'raw_vertex_ue_mapping_max_error_cm':error,'identity_node_transforms':True,
        'normals_and_uvs':True,'y_handedness_compensated':True}


def emission_material(name,color):
    mat=bpy.data.materials.new(name);mat.use_nodes=True
    nodes=mat.node_tree.nodes;nodes.clear()
    output=nodes.new('ShaderNodeOutputMaterial');em=nodes.new('ShaderNodeEmission')
    em.inputs['Color'].default_value=(*color,1)
    mat.node_tree.links.new(em.outputs[0],output.inputs['Surface'])
    return mat


def render_gallery(source_scene):
    scene=bpy.data.scenes.new('Equipment | contact sheet')
    bpy.context.window.scene=scene
    scene.render.engine='CYCLES';scene.cycles.samples=24
    scene.cycles.use_denoising=True
    scene.world=bpy.data.worlds.new('Studio world');scene.world.use_nodes=True
    scene.world.node_tree.nodes['Background'].inputs[0].default_value=(.45,.5,.55,1)
    scene.world.node_tree.nodes['Background'].inputs[1].default_value=.6
    scene.view_settings.view_transform='AgX'
    scene.render.image_settings.file_format='PNG'
    scene.render.resolution_x=2400;scene.render.resolution_y=2430
    scene.render.resolution_percentage=100
    bg=emission_material('Display | background',(.79,.81,.78))
    ink=emission_material('Display | ink',(.022,.04,.035))
    muted=emission_material('Display | muted',(.14,.20,.17))
    accent=emission_material('Display | accent',(.18,.32,.25))
    def text(body,x,z,size,mat):
        curve=bpy.data.curves.new(body,'FONT');curve.body=body;curve.size=size
        ob=bpy.data.objects.new(body,curve);scene.collection.objects.link(ob)
        ob.location=(x,-70,z);ob.rotation_euler=(math.pi/2,0,0);curve.materials.append(mat)
    def display(names,center,view,scale=1):
        points=[v.co for name in names for v in MODELS[name].data.vertices]
        low=Vector([min(p[i] for p in points) for i in range(3)])
        high=Vector([max(p[i] for p in points) for i in range(3)])
        target=(low+high)/2
        # A camera's screen basis mapped to the fixed gallery camera basis.
        viewrot=(-Vector(view)).to_track_quat('-Z','Y').to_matrix()
        galleryrot=Matrix.Rotation(math.pi/2,3,'X')
        rotation=galleryrot@viewrot.transposed()
        for name in names:
            ob=bpy.data.objects.new('Display | '+name,MODELS[name].data)
            scene.collection.objects.link(ob)
            ob.matrix_world=Matrix.Translation(Vector(center))@rotation.to_4x4()@Matrix.Scale(scale,4)@Matrix.Translation(-target)
    panels=[
        ('01  /  LIGHT','Twin bands / minimum coverage',['SM_VestLight'],(1.4,1,.7),1),
        ('02  /  MEDIUM','Plate carrier / utility pockets',['SM_VestMedium'],(1.4,1,.7),1),
        ('03  /  HEAVY','Broad torso / clear shoulders',['SM_VestHeavy'],(1.4,1,.7),1),
        ('LIGHT  /  REAR','Hollow torso / narrow shoulder webbing',['SM_VestLight'],(-1.4,-1,.8),1),
        ('MEDIUM  /  OPEN TOP','Back plate / soft side band',['SM_VestMedium'],(-1.4,1,1),1),
        ('HEAVY  /  REAR','Lateral armor stays below the arms',['SM_VestHeavy'],(-1.4,1,.8),1),
        ('04  /  HELMET','Open face / front mount / side rails',['SM_Helmet'],(1.6,1,.65),1.4),
        ('HELMET  /  INTERIOR','Real inner shell / no bottom cap',['SM_Helmet'],(1.5,1,-1),1.4),
        ('05  /  NIGHT','Dual tubes / shared head-center origin',['SM_Helmet','SM_HelmetNight'],(1.8,1,.7),1.3),
        ('06  /  THERMAL','Single sensor / folded bridge',['SM_Helmet','SM_HelmetThermal'],(1.8,1,.7),1.3),
        ('07  /  LAMP','Right rail Y+ / flashlight faces X+',['SM_Helmet','SM_HelmetLamp'],(1.1,1.7,.8),1.4),
        ('ACCESSORIES  /  DETAIL','Interchangeable front optics + side lamp',['SM_HelmetNight'],(1.8,1,.8),2.0),
    ]
    for i,(label,sub,names,view,scale) in enumerate(panels):
        col,row=i%3,i//3
        x=(col-1)*62;z=83-row*56
        display(names,(x,0,z),view,scale)
        text(label,x-28,z-24,2.1,ink)
        text(sub,x-28,z-27.6,1.25,muted)
    text('IMPACT  /  FIELD EQUIPMENT',-90,119,5,ink)
    text('ORIGINAL LOW-POLY ASSETS     /     OLIVE + CHARCOAL     /     CENTIMETERS',-90,112,1.8,accent)
    text('X FORWARD   Y RIGHT   Z UP     |     7 STATIC MESHES     |     NO COLLISION GEOMETRY',-90,-119,1.7,muted)
    bpy.ops.mesh.primitive_plane_add(size=2000,location=(0,45,0),rotation=(math.pi/2,0,0))
    bpy.context.object.data.materials.append(bg)
    for location,power,size in [((-80,-100,180),1100000,120),((110,-80,45),700000,100),((0,35,120),1000000,70)]:
        data=bpy.data.lights.new('Studio softbox','AREA');data.energy=power;data.shape='DISK';data.size=size
        ob=bpy.data.objects.new('Studio softbox',data);scene.collection.objects.link(ob)
        ob.location=location;ob.rotation_euler=(-Vector(location)).to_track_quat('-Z','Y').to_euler()
    data=bpy.data.cameras.new('Gallery camera');cam=bpy.data.objects.new('Gallery camera',data)
    scene.collection.objects.link(cam);cam.location=(0,-400,0);cam.rotation_euler=(math.pi/2,0,0)
    data.type='ORTHO';data.ortho_scale=246;scene.camera=cam
    scene.render.filepath=str(QA/'contact_sheet.png')
    bpy.ops.render.render(write_still=True)
    # Useful close-ups use the same render scene and show actual surfaces, no cutaway fabrication.
    for title,names,view,scale,path in [
        ('HELMET / OPEN INTERIOR',['SM_Helmet'],(1.4,.8,-1.1),2.5,'helmet_cavity.png'),
        ('HEAVY / ARM AND NECK CLEARANCE',['SM_VestHeavy'],(1.1,1,1.5),1.6,'vest_clearance.png')]:
        for ob in scene.objects:
            if ob.name.startswith('Display |') or ob.type=='FONT':ob.hide_render=True
        display(names,(0,0,0),view,scale)
        text(title,-43,-43,2,ink)
        data.ortho_scale=100
        scene.render.resolution_x=1200;scene.render.resolution_y=1200
        scene.render.filepath=str(QA/path)
        bpy.ops.render.render(write_still=True)
    bpy.context.window.scene=source_scene
    return scene


def write_notes(manifest):
    lines=['# Impact equipment models','',
           'Original procedural models made in Blender 5.2. No downloaded meshes or textures.',
           '', '## Delivery and status','',
           '- Source: `ArtSource/Equipment/Equipment.blend`; seven separate mesh objects in the Assets scene.',
           '- FBXs: `ArtSource/Equipment/FBX/`; one complete static mesh per file.',
           '- Manifest: `ArtSource/Equipment/manifest.json` (bounds, hashes, materials, validation, parts).',
           '- Preview: `Saved/Equipment/contact_sheet.png`, `helmet_cavity.png`, `vest_clearance.png`.',
           '- Unreal was NOT launched or executed. Engine-side import, bounds checks and saves are pending parent execution.',
           '- No C++, config, runtime integration or other-agent files changed by these tools.',
           '', '## Coordinates / runtime attachment','',
           'All source meshes use numeric centimeters: X forward, Y right, Z up. Object transforms are identity.',
           'Vest origins are midchest (0,0,0). Helmet and accessories share head-center (0,0,0).',
           'Attach the helmet and accessories with the SAME transform; do not translate each accessory to its mount again.',
           'Night and thermal are alternative front attachments at (12,0,5); the lamp is independent at (0,11,3).',
           'The shell has inner head radii X=12.5, Y=10.5 and inner crown Z=13.2 cm; outer crown is Z=14.',
           'The chin harness extends below Z=0; the bowl itself is entirely Z=0..14 (rim binding may extend 0.1 cm below).',
           'Vests have a real empty torso volume, uncapped neck and waist, front/back panels, soft side and shoulder straps.',
           'Heavy extends coverage over the lower flanks and abdomen; no shoulder caps or upper-arm plates.',
           'These are rigid static visuals: the parent must check the target skeleton and animation fit and disable collision on components.',
           '', '## Verified export / import contract','',
           'Blender metric scale_length=0.01. Export axis_forward=Y, axis_up=Z, FBX_SCALE_ALL and apply_unit_scale=True.',
           'Each exported copy has baked Y reflection and reversed winding, leaving the editable source unchanged.',
           'UE legacy KEEP_XYZ_AXES still negates Y for right-to-left handed conversion. This compensation keeps the lamp on Y+.',
           'The binary FBXs were parsed: centimeter units, identity node transforms, normals/UVs and every mapped vertex checked.',
           'Official reference: https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/CoordinateSystemPolicy?application_version=5.7',
           'Local verification: UE_5.7/Engine/Source/Editor/UnrealEd/Private/Fbx/FbxUtilsImport.cpp, ConvertPos.',
           'ImportEquipment.py uses legacy FbxFactory, convert_scene=False, force_front_x_axis=False, convert_scene_unit=True.',
           'It creates only missing assets, refuses to replace pre-existing unowned assets, binds authored materials,',
           'removes simple collisions, disables body collision/cooked collision data and validates bounds before saving.',
           'The transient legacy-FBX console setting is restored; the editor is never started or quit by the script.',
           '', '## Bounds (cm, X/Y/Z)','',
           '| Mesh | Minimum | Maximum | Size (depth / width / height) | Triangles |',
           '|---|---|---|---|---:|']
    fmt=lambda xs:', '.join(f'{v:.2f}' for v in xs)
    for r in RECORDS:
        lines.append(f"| {r['name']} | {fmt(r['bounds_cm'][0])} | {fmt(r['bounds_cm'][1])} | {fmt(r['dimensions_xyz_cm'])} | {r['triangles']} |")
    lines+=['','## Exact intended Unreal object paths','']
    lines += [f"- `{r['object_path']}`" for r in RECORDS]
    lines+=['','## Materials','',
            'Seven shared opaque PBR materials use constant linear colors, metallic and roughness. Lenses are opaque tinted surfaces; no external textures.']
    lines += [f"- `{DEST}/{m.name}.{m.name}`" for m in MATERIALS.values()]
    lines+=['','## Run','',
            'Build in a hidden Blender process with `--background --factory-startup --python Tools/Equipment/BuildEquipment.py`.',
            'In the existing Unreal editor Python environment, run `Tools/Equipment/ImportEquipment.py`.',
            'The importer appends its actual result here and never reports an import before the editor verifies it.',
            'Previous generated source/FBX/preview outputs are preserved under ArtSource/Equipment/History on rebuild.',
            '', '## Geometry QA','',
            'All render components are closed manifold surfaces with positive triangle area and UVs. Hollow shells have modeled inner walls.',
            'Ray checks prove vest neck/waist clearance, separate front/back walls, upper-arm clearance, and helmet open face/bottom.',
            'Source and binary FBX checks passed. No UCX/UBX/USP meshes, armatures, physics objects or downloaded dependencies.',
            'Contact-sheet display duplicates are render-only objects in a separate scene; only the seven originals are exported.',
            '', '## Limitations','',
            'No gameplay code, animated cloth, skeletal binding, LOD chain or target-character fitting is included.',
            'Imported static mesh .uassets will exist only after the parent runs the importer in Unreal.', '']
    (QA/'models_notes.md').write_text('\n'.join(lines),encoding='utf-8')


def main():
    archive_outputs()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.preferences.filepaths.save_version=0
    scene=bpy.context.scene;scene.name='Equipment | Assets'
    scene.unit_settings.system='METRIC';scene.unit_settings.scale_length=.01
    collection=bpy.data.collections.new('Equipment | export these seven meshes')
    scene.collection.children.link(collection)
    create_materials()
    for level in range(3):vest(level,collection)
    helmet(collection);accessories(collection)
    checks=verify_openings()
    for row in RECORDS:fbx_export_and_verify(MODELS[row['name']],row)
    manifest={'schema':1,'generator':'Tools/Equipment/BuildEquipment.py',
              'destination':DEST,'units':'cm','axes':{'X':'forward','Y':'right','Z':'up'},
              'source':'ArtSource/Equipment/Equipment.blend','meshes':RECORDS,
              'geometry_checks':checks,'unreal_import_executed':False,
              'materials':[{'name':MATERIALS[key].name,'asset_path':DEST+'/'+MATERIALS[key].name,
                            'base_color_linear':color,'metallic':metallic,'roughness':roughness}
                           for key,(color,metallic,roughness) in PALETTE.items()],
              'import':{'factory':'FbxFactory','coordinate_system_policy':'KEEP_XYZ_AXES',
                        'convert_scene':False,'convert_scene_unit':True,'force_front_x_axis':False,
                        'auto_generate_collision':False,'axis_forward':'Y','axis_up':'Z',
                        'fbx_vertex_transform_from_source':[1,-1,1]}}
    (OUT/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    write_notes(manifest)
    # Save deliverable before rendering too; an interrupted render cannot lose the models.
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'Equipment.blend'))
    render_gallery(scene)
    # Keep originals individually discoverable; the render scene stores the assembled previews.
    for obj in MODELS.values():obj.hide_set(True)
    MODELS['SM_VestMedium'].hide_set(False)
    bpy.ops.object.select_all(action='DESELECT')
    MODELS['SM_VestMedium'].select_set(True);bpy.context.view_layer.objects.active=MODELS['SM_VestMedium']
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'Equipment.blend'))
    print('EQUIPMENT_BUILD_OK '+json.dumps([{k:r[k] for k in ('name','bounds_cm','triangles')} for r in RECORDS]))


if __name__=='__main__':
    main()
