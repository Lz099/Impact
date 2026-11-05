"""Blender background: --python export_batch.py -- --blend FILE --out FILE.fbx --report FILE.json [--loop] [--feet-fixed] [--ground] [--root-motion]."""
import bpy, os, sys, json, math, argparse, struct, hashlib, array
from mathutils import Matrix, Euler
from io_scene_fbx import parse_fbx, export_fbx_bin, fbx_utils

TICKS = 46186158000
SOURCE = r'D:\UEProjets\Impact\ArtSource\Animations\Reference\SKM_Als.fbx'

def parse(path):
    tree, version = parse_fbx.parse(path)
    nodes, cons, globals, stacks, curves, takes, poses, custom = {}, [], {}, [], [], [], [], []
    geometries = 0
    for e in tree.elems:
        if e.id == b'Objects':
            for ob in e.elems:
                if ob.id == b'Model' and ob.props[2] != b'Mesh':
                    props = {}
                    for c in ob.elems:
                        if c.id == b'Properties70':
                            props = {p.props[0].decode(): list(p.props[4:]) for p in c.elems}
                            custom += [(ob.props[1].split(b'\0')[0].decode(), p.props[0].decode()) for p in c.elems if b'U' in p.props[3]]
                    nodes[ob.props[0]] = {'name': ob.props[1].split(b'\0')[0].decode(), 'type': ob.props[2].decode(), 'props': props}
                elif ob.id == b'AnimationStack':
                    props = {}
                    for c in ob.elems:
                        if c.id == b'Properties70': props = {p.props[0].decode(): list(p.props[4:]) for p in c.elems}
                    stacks.append({'name': ob.props[1].split(b'\0')[0].decode(), 'props': props})
                elif ob.id == b'AnimationCurve':
                    times = next(c.props[0] for c in ob.elems if c.id == b'KeyTime')
                    values = next(c.props[0] for c in ob.elems if c.id == b'KeyValueFloat')
                    curves.append({'id': ob.props[0], 'count': len(times), 'first': times[0], 'last': times[-1], 'times': list(times), 'range': max(values)-min(values)})
                elif ob.id == b'Geometry': geometries += 1
                elif ob.id == b'Pose': poses.append(ob.props[2].decode())
        elif e.id == b'Connections': cons = [list(c.props) for c in e.elems if c.id == b'C']
        elif e.id == b'GlobalSettings':
            for c in e.elems:
                if c.id == b'Properties70': globals = {p.props[0].decode(): list(p.props[4:]) for p in c.elems}
        elif e.id == b'Takes':
            for take in e.elems:
                if take.id == b'Take':
                    for c in take.elems:
                        if c.id in (b'LocalTime', b'ReferenceTime'): takes.append({'take': take.props[0].decode(), 'kind': c.id.decode(), 'ticks': list(c.props)})
    animated_model_properties = []
    for c in cons:
        if c[0] == b'OO' and c[1] in nodes and (c[2] in nodes or c[2] == 0): nodes[c[1]]['parent'] = nodes.get(c[2], {}).get('name')
        if c[0] == b'OP' and c[2] in nodes: animated_model_properties.append(c[3].decode())
    return {'tree': tree, 'version': version, 'models': {v['name']: v for v in nodes.values()}, 'global': globals, 'stacks': stacks, 'curves': curves, 'geometries': geometries, 'poses': poses, 'takes': takes, 'custom_properties': custom, 'animated_model_properties': sorted(set(animated_model_properties))}

def fingerprint(node):
    h = hashlib.sha256()
    def step(n):
        h.update(n.id); h.update(bytes(n.props_type))
        for p in n.props: h.update(p.tobytes() if isinstance(p, array.array) else repr(p).encode())
        for e in n.elems: step(e)
    step(node)
    return h.hexdigest()

def normalize_timing(path, start_tick, end_tick):
    before = open(path, 'rb').read(); data = bytearray(before)
    tree_before, version = parse_fbx.parse(path)
    assert version == 7400
    changes = []
    def properties(start, count):
        result, pos = [], start
        for i in range(count):
            typ = chr(data[pos]); pos += 1; value_start = pos
            if typ in 'YCFDIL':
                fmt = {'Y':'h', 'C':'?', 'F':'f', 'D':'d', 'I':'i', 'L':'q'}[typ]
                val = struct.unpack_from('<'+fmt, data, pos)[0]; pos += struct.calcsize('<'+fmt)
            elif typ in 'SR':
                size = struct.unpack_from('<I', data, pos)[0]; pos += 4; val = bytes(data[pos:pos+size]); pos += size
            elif typ in 'fdlibc':
                count2, encoding, size = struct.unpack_from('<III', data, pos); pos += 12; val = None; pos += size
            else: raise ValueError(typ)
            result.append((typ, value_start, val))
        return result, pos
    def change(prop, desired, field):
        typ, at, previous = prop; assert typ == 'L'
        if previous != desired:
            struct.pack_into('<q', data, at, desired)
            changes.append({'field':field, 'offset':at, 'before':previous, 'after':desired})
    def walk(offset, parent):
        end, count, length, nlen = struct.unpack_from('<IIIB', data, offset)
        if end == 0: return offset+13
        name = bytes(data[offset+13:offset+13+nlen]); path_nodes = parent+[name]
        pstart = offset+13+nlen; props, pstop = properties(pstart, count)
        assert pstop == pstart+length
        if path_nodes == [b'GlobalSettings', b'Properties70', b'P'] and props and props[0][2] in (b'TimeSpanStart', b'TimeSpanStop'):
            field = props[0][2]; change(props[4], start_tick if field == b'TimeSpanStart' else end_tick, field.decode())
        if len(path_nodes) == 3 and path_nodes[:2] == [b'Takes', b'Take'] and name in (b'LocalTime', b'ReferenceTime'):
            assert props[0][2] == start_tick and props[1][2] == end_tick, ('Unexpected Take timing', props)
        child = pstop
        while child < end-13: child = walk(child, path_nodes)
        return end
    pos = 27
    while pos < len(data)-13:
        if struct.unpack_from('<I', data, pos)[0] == 0: break
        pos = walk(pos, [])
    allowed = {i for c in changes for i in range(c['offset'], c['offset']+8)}
    changed = [i for i, (a,b) in enumerate(zip(before,data)) if a != b]
    assert set(changed) <= allowed and len(before) == len(data)
    if changes:
        with open(path, 'wb') as f: f.write(data)
    tree_after, version_after = parse_fbx.parse(path)
    stable = {}
    for key in (b'Objects', b'Connections', b'Takes'):
        n1 = next(e for e in tree_before.elems if e.id == key)
        n2 = next(e for e in tree_after.elems if e.id == key)
        stable[key.decode()] = fingerprint(n1) == fingerprint(n2)
    assert all(stable.values()) and version_after == version
    return {'changes':changes, 'bytes_changed':len(changed), 'file_size_unchanged':True, 'unchanged_sections':stable, 'sha256_final':hashlib.sha256(data).hexdigest()}

def lmat(p):
    return Matrix.Translation(p.get('Lcl Translation',[0,0,0])) @ Euler([math.radians(v) for v in p.get('Lcl Rotation',[0,0,0])], 'XYZ').to_matrix().to_4x4() @ Matrix.Diagonal([*p.get('Lcl Scaling',[1,1,1]),1])
def err(a,b): return max(abs(v) for row in a-b for v in row)
def bounds(points): return [[min(p[i] for p in points) for i in range(3)], [max(p[i] for p in points) for i in range(3)]]
def geometry(o):
    oe = o.evaluated_get(bpy.context.evaluated_depsgraph_get()); me = oe.to_mesh()
    points = [oe.matrix_world @ v.co for v in me.vertices]; oe.to_mesh_clear()
    return points

def export_and_validate(args):
    path = os.path.abspath(args.out); report_path = os.path.abspath(args.report)
    os.makedirs(os.path.dirname(path), exist_ok=True); os.makedirs(os.path.dirname(report_path), exist_ok=True)
    source_hash = hashlib.sha256(open(args.blend,'rb').read()).hexdigest()
    bpy.ops.wm.open_mainfile(filepath=os.path.abspath(args.blend))
    s = bpy.context.scene; start, end = s.frame_start, s.frame_end
    assert start == 0 and end > start, 'Expected clip frames to begin at 0'
    s.render.fps = 30; s.render.fps_base = 1; s.name = os.path.splitext(os.path.basename(path))[0]
    a = bpy.data.objects['root']; assert a.type == 'ARMATURE' and a.parent.name == 'SKM_Als'
    source_bones = {b.name: {'parent':b.parent.name if b.parent else None, 'rest':b.matrix_local.copy()} for b in a.data.bones}
    mesh = next(o for o in s.objects if o.type == 'MESH' and any(m.type == 'ARMATURE' and m.object == a for m in o.modifiers))
    s.frame_set(start); root_world = a.matrix_world.copy(); mesh_world = mesh.matrix_world.copy()
    bpy.ops.object.select_all(action='DESELECT'); a.select_set(True); a.parent.select_set(True); bpy.context.view_layer.objects.active = a
    settings = dict(use_selection=True, object_types={'ARMATURE','EMPTY'}, global_scale=1, apply_unit_scale=True,
        apply_scale_options='FBX_SCALE_NONE', use_space_transform=True, bake_space_transform=False,
        axis_forward='Y', axis_up='Z', add_leaf_bones=False, primary_bone_axis='Y', secondary_bone_axis='X',
        use_armature_deform_only=False, armature_nodetype='ROOT', use_custom_props=False,
        bake_anim=True, bake_anim_use_all_bones=True, bake_anim_use_nla_strips=False, bake_anim_use_all_actions=False,
        bake_anim_force_startend_keying=True, bake_anim_step=1, bake_anim_simplify_factor=0)
    original_model_writer = export_fbx_bin.fbx_data_object_elements
    original_tx = fbx_utils.ObjectWrapper.fbx_object_tx
    def rest_model_tx(self,scene_data,rest=False,rot_euler_compat=None):
        return original_tx(self,scene_data,rest=True if self.is_bone else rest,rot_euler_compat=rot_euler_compat)
    def write_model_at_rest(root,ob_obj,scene_data):
        fbx_utils.ObjectWrapper.fbx_object_tx = rest_model_tx
        try: return original_model_writer(root,ob_obj,scene_data)
        finally: fbx_utils.ObjectWrapper.fbx_object_tx = original_tx
    export_fbx_bin.fbx_data_object_elements = write_model_at_rest
    try: bpy.ops.export_scene.fbx(filepath=path, **settings)
    finally: export_fbx_bin.fbx_data_object_elements = original_model_writer
    start_tick, end_tick = round(start*TICKS/30), round(end*TICKS/30)
    timing = normalize_timing(path,start_tick,end_tick)
    source_raw, out_raw = parse(SOURCE), parse(path)
    missing = set(source_raw['models'])-set(out_raw['models'])
    extra = set(out_raw['models'])-set(source_raw['models'])
    rest_errors = {name:err(lmat(ob['props']),lmat(source_raw['models'][name]['props'])) for name,ob in out_raw['models'].items() if name in source_raw['models']}
    old_objects = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path,automatic_bone_orientation=False,use_anim=True,anim_offset=0)
    imported = set(bpy.data.objects)-old_objects; a2 = next(o for o in imported if o.type == 'ARMATURE')
    copy_mesh = mesh.copy(); copy_mesh.data = mesh.data.copy(); copy_mesh.name = 'QA_Export_Reimport_Mesh'
    s.collection.objects.link(copy_mesh); copy_mesh.parent = None; copy_mesh.matrix_world = mesh_world
    for mod in copy_mesh.modifiers:
        if mod.type == 'ARMATURE' and mod.object == a: mod.object = a2
    frames, first_poses, first_vertices = [], {}, None
    reference_feet, reference_root = {}, None
    for frame in range(start,end+1):
        s.frame_set(frame); bpy.context.view_layer.update()
        poses = {name:a2.pose.bones[name].matrix.copy() for name in source_bones}
        pose_errors = {name:err(a.pose.bones[name].matrix,poses[name]) for name in source_bones}
        head_errors = {name:((a.matrix_world @ a.pose.bones[name].matrix.translation)-(a2.matrix_world @ poses[name].translation)).length for name in source_bones}
        feet = {name:a2.matrix_world @ poses[name] for name in ['foot_l','foot_r','ball_l','ball_r']}
        verts, verts2 = geometry(mesh), geometry(copy_mesh)
        if frame == start:
            first_poses = poses; first_vertices = verts2; reference_feet = feet; reference_root = a2.matrix_world.copy()
        pelvis_world = a2.matrix_world @ poses['pelvis'].translation
        spine_world = a2.matrix_world @ poses['spine_03'].translation
        torso = spine_world-pelvis_world
        frames.append({'frame':frame,'pose_matrix_max_error':max(pose_errors.values()),'pose_matrix_worst_bone':max(pose_errors,key=pose_errors.get),
            'world_bone_head_max_error_m':max(head_errors.values()),'root_world_matrix_change':err(reference_root,a2.matrix_world),
            'root_world_matrix_authored_error':err(a.matrix_world,a2.matrix_world),
            'root_world_cm':[v*100 for v in a2.matrix_world.translation],
            'feet_world_matrix_change':{name:err(reference_feet[name],mat) for name,mat in feet.items()},
            'feet_displacement_cm':{name:(mat.translation-reference_feet[name].translation).length*100 for name,mat in feet.items()},
            'pelvis_world_cm':[v*100 for v in pelvis_world], 'torso_vertical_fraction':abs(torso.z)/max(torso.length,1e-12),
            'mesh_world_bounds_m':bounds(verts2),'authored_mesh_world_bounds_m':bounds(verts),
            'mesh_vertex_world_max_error_m':max((v-v2).length for v,v2 in zip(verts,verts2)),
            'vertex_count_matches':len(verts)==len(verts2)})
        if frame == end:
            seam = {'pose_matrix_max_error':max(err(first_poses[name],poses[name]) for name in source_bones),
                'bone_head_max_error_cm':max((poses[name].translation-first_poses[name].translation).length for name in source_bones),
                'world_vertex_max_error_cm':100*max((v-v2).length for v,v2 in zip(first_vertices,verts2))}
    reimport_rest = {name:err(source_bones[name]['rest'],a2.data.bones[name].matrix_local) for name in source_bones}
    duration = (end-start)/30
    summary = {'clip':s.name,'output_path':path,'binary_header_valid':open(path,'rb').read(23)==b'Kaydara FBX Binary  \x00\x1a\x00',
        'fbx_version':out_raw['version'],'stack_count':len(out_raw['stacks']),'stack_names':[p['name'] for p in out_raw['stacks']],
        'fps_reimport':s.render.fps,'fps_base_reimport':s.render.fps_base,'unit_scale_factor_cm':out_raw['global']['UnitScaleFactor'][0],
        'frame_start':start,'frame_end':end,'sample_count':end-start+1,'duration_seconds':duration,
        'key_counts':sorted(set(c['count'] for c in out_raw['curves'])),'curve_count':len(out_raw['curves']),
        'curve_times_seconds':sorted(set((c['first']/TICKS,c['last']/TICKS) for c in out_raw['curves'])),
        'all_curve_times_at_30_fps':all(abs(t/TICKS*30-round(t/TICKS*30))<1e-6 for c in out_raw['curves'] for t in c['times']),
        'global_times_seconds':[out_raw['global']['TimeSpanStart'][0]/TICKS,out_raw['global']['TimeSpanStop'][0]/TICKS],
        'takes_times_seconds':[[v/TICKS for v in take['ticks']] for take in out_raw['takes']],
        'source_skeleton_node_count':len(source_raw['models']),'output_skeleton_node_count':len(out_raw['models']),
        'bone_count_authored':len(source_bones),'bone_count_reimported':len(a2.data.bones),
        'skeleton_names_exact':not missing and not extra,'missing_nodes':sorted(missing),'extra_nodes':sorted(extra),
        'skeleton_hierarchy_exact':all(ob.get('parent')==source_raw['models'].get(name,{}).get('parent') for name,ob in out_raw['models'].items()),
        'skeleton_node_types_exact':all(ob['type']==source_raw['models'].get(name,{}).get('type') for name,ob in out_raw['models'].items()),
        'raw_rest_local_matrix_max_error':max(rest_errors.values()),'reimport_rest_matrix_max_error':max(reimport_rest.values()),
        'geometry_count':out_raw['geometries'],'bind_pose_objects':out_raw['poses'],'custom_properties':out_raw['custom_properties'],
        'animated_model_properties':out_raw['animated_model_properties'],
        'all_frames_root_world_matrix_authored_error':max(f['root_world_matrix_authored_error'] for f in frames),
        'root_motion_requested':args.root_motion,
        'root_trajectory_cm':[[f['frame'],*f['root_world_cm']] for f in frames],
        'root_endpoint_displacement_cm':math.dist(frames[0]['root_world_cm'],frames[-1]['root_world_cm']),
        'root_total_path_length_cm':sum(math.dist(frames[i-1]['root_world_cm'],frames[i]['root_world_cm']) for i in range(1,len(frames))),
        'root_stationary_max_matrix_change':max(f['root_world_matrix_change'] for f in frames),
        'feet_stationary_max_displacement_cm':max(v for f in frames for v in f['feet_displacement_cm'].values()),
        'all_frames_pose_matrix_max_error':max(f['pose_matrix_max_error'] for f in frames),
        'all_frames_world_bone_head_error_cm':100*max(f['world_bone_head_max_error_m'] for f in frames),
        'all_frames_world_vertex_error_cm':100*max(f['mesh_vertex_world_max_error_m'] for f in frames),
        'ground_pose_at_frame0_requested':args.ground,'ground_initial_pelvis_world_cm':frames[0]['pelvis_world_cm'],
        'ground_initial_torso_vertical_fraction':frames[0]['torso_vertical_fraction'],
        'ground_initial_mesh_bounds_m':frames[0]['mesh_world_bounds_m'],
        'ground_pelvis_max_distance_from_origin_cm':max(math.sqrt(sum(v*v for v in f['pelvis_world_cm'])) for f in frames),
        'loop_requested':args.loop,'loop_seam':seam,'feet_fixed_requested':args.feet_fixed,
        'source_blend_untouched':source_hash==hashlib.sha256(open(args.blend,'rb').read()).hexdigest(), 'sha256':timing['sha256_final']}
    checks = {'binary':summary['binary_header_valid'],'one_stack':summary['stack_count']==1,'fps_30':summary['fps_reimport']==30,
        'centimetres':summary['unit_scale_factor_cm']==1,'all_samples':summary['key_counts']==[end-start+1],
        'sample_times':summary['all_curve_times_at_30_fps'],'clip_times':summary['curve_times_seconds']==[(start/30,end/30)],
        'global_times':summary['global_times_seconds']==[start/30,end/30],
        'take_times':bool(out_raw['takes']) and all(t['ticks']==[start_tick,end_tick] for t in out_raw['takes']),
        'names':summary['skeleton_names_exact'],'hierarchy':summary['skeleton_hierarchy_exact'],'types':summary['skeleton_node_types_exact'],
        'no_mesh':summary['geometry_count']==0,'rest_source':summary['raw_rest_local_matrix_max_error']<.001,
        'rest_reimport':summary['reimport_rest_matrix_max_error']<.001,'root_matches_authored':summary['all_frames_root_world_matrix_authored_error']<1e-6,
        'pose_matches':summary['all_frames_world_bone_head_error_cm']<.01,'mesh_matches':summary['all_frames_world_vertex_error_cm']<.01,
        'mesh_vertex_counts':all(f['vertex_count_matches'] for f in frames),'no_custom_properties':not out_raw['custom_properties'],
        'only_transform_animation':set(out_raw['animated_model_properties'])=={'Lcl Translation','Lcl Rotation','Lcl Scaling'},
        'source_untouched':summary['source_blend_untouched']}
    if args.feet_fixed: checks['feet_fixed'] = summary['feet_stationary_max_displacement_cm']<.01
    if args.root_motion: checks['root_motion_present'] = summary['root_total_path_length_cm']>.01
    else: checks['root_stationary'] = summary['root_stationary_max_matrix_change']<1e-6
    if args.loop: checks['loop_seam'] = seam['pose_matrix_max_error']<.001 and seam['world_vertex_max_error_cm']<.01
    if args.ground:
        checks['ground_pelvis_origin_all_frames'] = summary['ground_pelvis_max_distance_from_origin_cm']<.01
        checks['ground_horizontal_at_frame0'] = frames[0]['torso_vertical_fraction']<.6
    summary['checks']=checks; summary['passed']=all(checks.values())
    result = {'summary':summary,'settings':{k:list(v) if isinstance(v,set) else v for k,v in settings.items()},
        'metadata_timing_cleanup':timing,'raw_rest_local_errors':rest_errors,'reimport_rest_errors':reimport_rest,
        'source_global':source_raw['global'],'export_global':out_raw['global'],'animation_stacks':out_raw['stacks'],'takes':out_raw['takes'],
        'frames':frames,'notes':['Export is skeleton and one animation only, with original names and reference pose.',
        'Bone Model defaults use reference transforms; independently baked animated transforms are unchanged.',
        'All frames were reimported into Blender with anim_offset=0 and compared against the authored skeleton and original mesh weights.',
        'Numeric tolerances are floating point roundtrip precision. Unreal import is not part of this validation.',
        'Ground validation requires pelvis at world (0,0,0) on every frame and an already horizontal torso at frame 0.',
        'Only a clip explicitly exported with --root-motion may move its root. Its full world trajectory is recorded and compared to the authored root.']}
    with open(report_path,'w') as f:json.dump(result,f,indent=2,default=lambda v:v.decode('utf-8','replace') if isinstance(v,bytes) else str(v))
    print('BATCH_EXPORT_VALIDATION',json.dumps(summary))
    if not summary['passed']: raise RuntimeError('Export validation failed: '+', '.join(k for k,v in checks.items() if not v))

if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--blend',required=True);parser.add_argument('--out',required=True);parser.add_argument('--report',required=True)
    parser.add_argument('--loop',action='store_true');parser.add_argument('--feet-fixed',action='store_true');parser.add_argument('--ground',action='store_true');parser.add_argument('--root-motion',action='store_true')
    export_and_validate(parser.parse_args(sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else []))
